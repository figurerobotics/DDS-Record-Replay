// Copyright 2023 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file McapHandler.cpp
 */

#define MCAP_IMPLEMENTATION  // Define this in exactly one .cpp file

#include <cstdio>
#include <mcap/reader.hpp>

#include <yaml-cpp/yaml.h>

#include <cpp_utils/exception/InitializationException.hpp>
#include <cpp_utils/exception/InconsistencyException.hpp>
#include <cpp_utils/utils.hpp>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <fastcdr/FastCdr.h>
#include <fastdds/rtps/common/CDRMessage_t.h>
#include <fastdds/rtps/common/SerializedPayload.h>
#include <fastrtps/types/DynamicType.h>
#include <fastrtps/types/TypeObjectFactory.h>

#include <ddspipe_core/types/dynamic_types/schema.hpp>

#include <ddsrecorder_participants/constants.hpp>

#include <ddsrecorder_participants/recorder/mcap/McapHandler.hpp>

namespace eprosima {
namespace ddsrecorder {
namespace participants {

using namespace eprosima::ddspipe::core::types;

Message::Message(
        const Message& msg)
    : mcap::Message(msg)
{
    this->payload_owner = msg.payload_owner;
    auto payload_owner_ =
            const_cast<eprosima::fastrtps::rtps::IPayloadPool*>((eprosima::fastrtps::rtps::IPayloadPool*)msg.
                    payload_owner);
    this->payload_owner->get_payload(
        msg.payload,
        payload_owner_,
        this->payload);
}

Message::~Message()
{
    // If payload owner exists and payload has size, release it correctly in pool
    if (payload_owner && payload.length > 0)
    {
        payload_owner->release_payload(payload);
    }
}

McapHandler::McapHandler(
        const McapHandlerConfiguration& config,
        const std::shared_ptr<ddspipe::core::PayloadPool>& payload_pool,
        const McapHandlerStateCode& init_state /* = McapHandlerStateCode::RUNNING */)
    : configuration_(config)
    , payload_pool_(payload_pool)
    , state_(McapHandlerStateCode::STOPPED)
{
    std::string tmp_filename = tmp_filename_(config.file_name);
    auto status = mcap_writer_.open(tmp_filename.c_str(), mcap::McapWriterOptions("ros2"));
    if (!status.ok())
    {
        throw utils::InitializationException(
                  STR_ENTRY << "Failed to open MCAP file " << tmp_filename << " for writing: " << status.message);
    }

    logInfo(DDSRECORDER_MCAP_HANDLER,
            "MCAP file <" << config.file_name << "> .");

    if (init_state == McapHandlerStateCode::RUNNING)
    {
        start();
    }
    else if (init_state == McapHandlerStateCode::PAUSED)
    {
        pause();
    }
}

McapHandler::~McapHandler()
{
    logInfo(DDSRECORDER_MCAP_HANDLER, "Destroying handler.");

    // Stop handler prior to destruction
    stop();

    // Serialize and store dynamic types associated to all added schemas
    store_dynamic_types_();

    // Close writer and output file
    mcap_writer_.close();

    // Rename temp file to configuration file_name
    std::string tmp_filename = tmp_filename_(configuration_.file_name);
    if (std::rename(tmp_filename.c_str(), configuration_.file_name.c_str()))
    {
        logError(
            DDSRECORDER_MCAP_HANDLER,
            "Failed to rename " << tmp_filename << " into " << configuration_.file_name << " on handler destruction.");
    }
}

void McapHandler::add_schema(
        const fastrtps::types::DynamicType_ptr& dynamic_type)
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (state_ == McapHandlerStateCode::STOPPED)
    {
        throw utils::InconsistencyException(
                  STR_ENTRY << "Attempting to add schema through a stopped handler, dropping..."
                  );
    }

    assert(nullptr != dynamic_type);
    std::string type_name = dynamic_type->get_name();

    // Check if it exists already
    if (received_types_.find(type_name) != received_types_.end())
    {
        return;
    }

    // Schema not found, generate from dynamic type and store
    std::string schema_text = generate_ros2_schema(dynamic_type);

    logInfo(DDSRECORDER_MCAP_HANDLER, "\nAdding schema with name " << type_name << " :\n" << schema_text << "\n");

    // Create schema and add it to writer and to schemas map
    mcap::Schema new_schema(type_name, "ros2msg", schema_text);
    // WARNING: passing as non-const to MCAP library
    mcap_writer_.addSchema(new_schema);

    logInfo(DDSRECORDER_MCAP_HANDLER, "Schema created: " << type_name << ".");

    auto it = schemas_.find(type_name);
    if (it != schemas_.end())
    {
        // Update channels previously created with blank schema
        update_channels_nts_(it->second.id, new_schema.id);
    }
    schemas_[type_name] = std::move(new_schema);
    received_types_.insert(type_name);

    // Check if there are any pending samples for this new schema. If so, add them.
    if ((pending_samples_.find(type_name) != pending_samples_.end()) ||
            (state_ == McapHandlerStateCode::PAUSED &&
            (pending_samples_paused_.find(type_name) != pending_samples_paused_.end())))
    {
        add_pending_samples_nts_(type_name);
    }
}

void McapHandler::add_data(
        const DdsTopic& topic,
        RtpsPayloadData& data)
{
    std::lock_guard<std::mutex> lock(mtx_);

    logInfo(
        DDSRECORDER_MCAP_HANDLER,
        "Adding data in topic " << topic);

    if (state_ == McapHandlerStateCode::STOPPED)
    {
        throw utils::InconsistencyException(
                  STR_ENTRY << "Attempting to add sample through a stopped handler, dropping..."
                  );
    }

    // Add data to channel
    Message msg;
    msg.sequence = unique_sequence_number_++;
    msg.publishTime = fastdds_timestamp_to_mcap_timestamp(data.source_timestamp);
    if (configuration_.log_publishTime)
    {
        msg.logTime = msg.publishTime;
    }
    else
    {
        msg.logTime = now();
    }
    msg.dataSize = data.payload.length;

    if (data.payload.length > 0)
    {
        auto payload_owner =
                const_cast<eprosima::fastrtps::rtps::IPayloadPool*>((eprosima::fastrtps::rtps::IPayloadPool*)data.
                        payload_owner);

        if (payload_owner)
        {
            payload_pool_->get_payload(
                data.payload,
                payload_owner,
                msg.payload);

            msg.payload_owner = payload_pool_.get();
            msg.data = reinterpret_cast<std::byte*>(msg.payload.data);
        }
        else
        {
            throw utils::InconsistencyException(
                      STR_ENTRY << "Payload owner not found in data received."
                      );
        }
    }
    else
    {
        throw utils::InconsistencyException(
                  STR_ENTRY << "Received sample with no payload."
                  );
    }

    if (received_types_.count(topic.type_name) != 0)
    {
        // Schema available -> add to buffer
        add_data_nts_(msg, topic);
    }
    else
    {
        if (state_ == McapHandlerStateCode::RUNNING)
        {
            if (configuration_.max_pending_samples == 0)
            {
                if (configuration_.only_with_schema)
                {
                    // No schema available + no pending samples + only_with_schema -> Discard message
                    return;
                }
                else
                {
                    // No schema available + no pending samples -> Add to buffer with blank schema
                    add_data_nts_(msg, topic);
                }
            }
            else
            {
                logInfo(
                    DDSRECORDER_MCAP_HANDLER,
                    "Schema for topic " << topic << " not yet available, inserting to pending samples queue.");

                add_to_pending_nts_(msg, topic);
            }
        }
        else if (state_ == McapHandlerStateCode::PAUSED)
        {
            logInfo(
                DDSRECORDER_MCAP_HANDLER,
                "Schema for topic " << topic << " not yet available, inserting to (paused) pending samples queue.");

            pending_samples_paused_[topic.type_name].push_back({topic, msg});
        }
        else
        {
            // Should not happen, protected with mutex and state verified at beginning
            utils::tsnh(
                utils::Formatter() << "Trying to add sample from a stopped instance.");
        }
    }
}

void McapHandler::start()
{
    std::lock_guard<std::mutex> lock(mtx_);

    // Store previous state to act differently depending on its value
    McapHandlerStateCode prev_state = state_;
    state_ = McapHandlerStateCode::RUNNING;

    if (prev_state == McapHandlerStateCode::RUNNING)
    {
        logWarning(
            DDSRECORDER_MCAP_HANDLER,
            "Ignoring start command, instance already started.");
    }
    else
    {
        logInfo(
            DDSRECORDER_MCAP_HANDLER,
            "Starting handler.");

        if (prev_state == McapHandlerStateCode::PAUSED)
        {
            // Stop event routine (cleans buffers)
            stop_event_thread_nts_();
        }
    }
}

void McapHandler::stop()
{
    std::lock_guard<std::mutex> lock(mtx_);

    // Store previous state to act differently depending on its value
    McapHandlerStateCode prev_state = state_;
    state_ = McapHandlerStateCode::STOPPED;

    if (prev_state == McapHandlerStateCode::STOPPED)
    {
        logWarning(
            DDSRECORDER_MCAP_HANDLER,
            "Ignoring stop command, instance already stopped.");
    }
    else
    {
        logInfo(
            DDSRECORDER_MCAP_HANDLER,
            "Stopping handler.");

        if (prev_state == McapHandlerStateCode::PAUSED)
        {
            // Stop event routine (cleans buffers)
            stop_event_thread_nts_();
        }

        if (!configuration_.only_with_schema)
        {
            // Adds to buffer samples whose schema was not received while running
            add_pending_samples_nts_();
        }
        else
        {
            // Free memory resources
            pending_samples_.clear();
        }
        dump_data_nts_();  // if prev_state == RUNNING -> writes buffer + added pending samples (if !only_with_schema)
                           // if prev_state == PAUSED  -> writes added pending samples (if !only_with_schema)
    }
}

void McapHandler::pause()
{
    std::lock_guard<std::mutex> lock(mtx_);

    // Store previous state to act differently depending on its value
    McapHandlerStateCode prev_state = state_;
    state_ = McapHandlerStateCode::PAUSED;

    if (prev_state == McapHandlerStateCode::PAUSED)
    {
        logWarning(
            DDSRECORDER_MCAP_HANDLER,
            "Ignoring pause command, instance already paused.");
    }
    else
    {
        logInfo(
            DDSRECORDER_MCAP_HANDLER,
            "Pausing handler.");

        if (prev_state == McapHandlerStateCode::RUNNING)
        {
            // Write data stored in buffer
            dump_data_nts_();
            // Clear buffer
            // NOTE: not really needed, dump_data_nts_ already writes and pops all samples
            samples_buffer_.clear();
        }

        // Launch event thread routine
        event_flag_ = EventCode::untriggered;  // No need to take event mutex (protected by mtx_)
        event_thread_ = std::thread(&McapHandler::event_thread_routine_, this);
    }
}

void McapHandler::trigger_event()
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (state_ != McapHandlerStateCode::PAUSED)
    {
        logWarning(
            DDSRECORDER_MCAP_HANDLER,
            "Ignoring trigger event command, instance is not paused.");
    }
    else
    {
        logInfo(
            DDSRECORDER_MCAP_HANDLER,
            "Triggering event.");
        {
            std::lock_guard<std::mutex> lock(event_cv_mutex_);
            event_flag_ = EventCode::triggered;
        }
        event_cv_.notify_one();
    }
}

mcap::Timestamp McapHandler::fastdds_timestamp_to_mcap_timestamp(
        const DataTime& time)
{
    uint64_t mcap_time = time.seconds();
    mcap_time *= 1000000000;
    return mcap_time + time.nanosec();
}

mcap::Timestamp McapHandler::std_timepoint_to_mcap_timestamp(
        const utils::Timestamp& time)
{
    return mcap::Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

mcap::Timestamp McapHandler::now()
{
    return std_timepoint_to_mcap_timestamp(utils::now());
}

void McapHandler::add_data_nts_(
        const Message& msg,
        bool direct_write /* false */)
{
    if (direct_write)
    {
        try
        {
            // Write to MCAP file
            write_message_nts_(msg);
        }
        catch (const utils::InconsistencyException& e)
        {
            logError(DDSRECORDER_MCAP_HANDLER, "Error writting message in channel " << msg.channelId << ". Error message:\n " <<
                    e.what());
        }
    }
    else
    {
        samples_buffer_.push_back(msg);
        if (state_ == McapHandlerStateCode::RUNNING && samples_buffer_.size() == configuration_.buffer_size)
        {
            logInfo(DDSRECORDER_MCAP_HANDLER, "Full buffer, writting to disk...");
            dump_data_nts_();
        }
    }
}

void McapHandler::add_data_nts_(
        Message& msg,
        const DdsTopic& topic,
        bool direct_write /* false */)
{
    try
    {
        auto channel_id = get_channel_id_nts_(topic);
        msg.channelId = channel_id;
    }
    catch (const utils::InconsistencyException& e)
    {
        logWarning(DDSRECORDER_MCAP_HANDLER, "Error adding message in topic " << topic << ". Error message:\n " <<
                e.what());
        return;
    }
    add_data_nts_(msg, direct_write);
}

void McapHandler::write_message_nts_(
        const Message& msg)
{
    auto status = mcap_writer_.write(msg);
    if (!status.ok())
    {
        throw utils::InconsistencyException(
                  STR_ENTRY << "Error writting in MCAP, error message: " << status.message
                  );
    }
}

void McapHandler::add_to_pending_nts_(
        Message& msg,
        const DdsTopic& topic)
{
    assert(configuration_.max_pending_samples != 0);
    if (configuration_.max_pending_samples > 0 &&
            pending_samples_[topic.type_name].size() == static_cast<unsigned int>(configuration_.max_pending_samples))
    {
        if (configuration_.only_with_schema)
        {
            // Discard oldest message in pending samples
            logWarning(DDSRECORDER_MCAP_HANDLER,
                    "Dropping pending sample in type " << topic.type_name << ": buffer limit (" << configuration_.max_pending_samples <<
                    ") reached.");
        }
        else
        {
            logInfo(DDSRECORDER_MCAP_HANDLER,
                    "Buffer limit (" << configuration_.max_pending_samples <<  ") reached for type " << topic.type_name <<
                    ": writing oldest sample without schema.");

            // Write oldest message without schema
            auto& oldest_sample = pending_samples_[topic.type_name].front();
            add_data_nts_(oldest_sample.second, oldest_sample.first);
        }
        pending_samples_[topic.type_name].pop_front();
    }
    pending_samples_[topic.type_name].push_back({topic, msg});
}

void McapHandler::add_pending_samples_nts_(
        const std::string& schema_name)
{
    logInfo(DDSRECORDER_MCAP_HANDLER, "Adding pending samples for type: " << schema_name << ".");

    if (pending_samples_.find(schema_name) != pending_samples_.end())
    {
        // Move samples from pending_samples to buffer
        // NOTE: directly write to MCAP when PAUSED, as these correspond to samples that were previously received in
        // RUNNING state (hence to avoid being cleaned by event thread)
        add_pending_samples_nts_(pending_samples_[schema_name], state_ == McapHandlerStateCode::PAUSED);
        pending_samples_.erase(schema_name);
    }

    if (state_ == McapHandlerStateCode::PAUSED &&
            (pending_samples_paused_.find(schema_name) != pending_samples_paused_.end()))
    {
        // Move samples from pending_samples_paused to buffer, from where they will be written if event received
        add_pending_samples_nts_(pending_samples_paused_[schema_name]);
        pending_samples_paused_.erase(schema_name);
    }
}

void McapHandler::add_pending_samples_nts_(
        pending_list& pending_samples,
        bool direct_write /* false */)
{
    while (!pending_samples.empty())
    {
        // Move samples from pending list to buffer, or write them directly to MCAP file
        auto& sample = pending_samples.front();
        add_data_nts_(sample.second, sample.first, direct_write);
        pending_samples.pop_front();
    }
}

void McapHandler::add_pending_samples_nts_()
{
    logInfo(DDSRECORDER_MCAP_HANDLER, "Adding pending samples for all types.");

    auto pending_types = utils::get_keys(pending_samples_);
    for (auto& pending_type: pending_types)
    {
        add_pending_samples_nts_(pending_type);
    }
}

void McapHandler::event_thread_routine_()
{
    while (true)
    {
        bool timeout;
        {
            auto exit_time = std::chrono::time_point<std::chrono::system_clock>::max();
            auto cleanup_period_ = std::chrono::seconds(configuration_.cleanup_period);
            if (cleanup_period_ < std::chrono::seconds::max())
            {
                auto now = std::chrono::system_clock::now();
                exit_time = now + cleanup_period_;
            }

            std::unique_lock<std::mutex> lock(event_cv_mutex_);

            if (event_flag_ != EventCode::untriggered)
            {
                // Flag set before taking mutex, no need to wait
                timeout = false;
            }
            else
            {
                timeout = !event_cv_.wait_until(
                    lock,
                    exit_time,
                    [&]
                    {
                        return event_flag_ != EventCode::untriggered;
                    });
            }

            if (event_flag_ == EventCode::stopped)
            {
                logInfo(DDSRECORDER_MCAP_HANDLER, "Finishing event thread routine.");
                return;
            }
            else
            {
                // Reset and wait for next event
                event_flag_ = EventCode::untriggered;
            }
        }

        std::lock_guard<std::mutex> lock(mtx_);

        // Delete outdated samples if timeout, and also before dumping (event triggered case)
        remove_outdated_samples_nts_();

        if (timeout)
        {
            logInfo(DDSRECORDER_MCAP_HANDLER, "Event thread timeout.");
        }
        else
        {
            logInfo(DDSRECORDER_MCAP_HANDLER, "Event triggered: dumping buffered data.");

            if (!(configuration_.max_pending_samples == 0 && configuration_.only_with_schema))
            {
                // Move (paused) pending samples to buffer (or pending samples) prior to dumping
                for (auto& pending_type : pending_samples_paused_)
                {
                    auto type_name = pending_type.first;
                    auto& pending_list = pending_type.second;
                    while (!pending_list.empty())
                    {
                        auto& sample = pending_list.front();
                        if (configuration_.max_pending_samples == 0)
                        {
                            if (configuration_.only_with_schema)
                            {
                                // Cannot happen (outter if)
                            }
                            else
                            {
                                // Add to buffer with blank schema
                                add_data_nts_(sample.second, sample.first);
                            }
                        }
                        else
                        {
                            add_to_pending_nts_(sample.second, sample.first);
                        }
                        pending_list.pop_front();
                    }
                }
            }
            dump_data_nts_();
        }
    }
}

void McapHandler::remove_outdated_samples_nts_()
{
    logInfo(DDSRECORDER_MCAP_HANDLER, "Removing outdated samples.");

    auto threshold = std_timepoint_to_mcap_timestamp(utils::now() - std::chrono::seconds(configuration_.event_window));
    samples_buffer_.remove_if([&](auto& sample)
            {
                return sample.logTime < threshold;
            });

    for (auto& pending_type : pending_samples_paused_)
    {
        pending_type.second.remove_if([&](auto& sample)
                {
                    return sample.second.logTime < threshold;
                });
    }
}

void McapHandler::stop_event_thread_nts_()
{
    // WARNING: state must have been set different to PAUSED before calling this method
    assert(state_ != McapHandlerStateCode::PAUSED);

    logInfo(DDSRECORDER_MCAP_HANDLER, "Stopping event thread.");
    if (event_thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(event_cv_mutex_);
            event_flag_ = EventCode::stopped;
        }
        event_cv_.notify_one();
        event_thread_.join();
    }
    samples_buffer_.clear();
    pending_samples_paused_.clear();
}

void McapHandler::dump_data_nts_()
{
    logInfo(DDSRECORDER_MCAP_HANDLER, "Writing data stored in buffer.");

    while (!samples_buffer_.empty())
    {
        auto& sample = samples_buffer_.front();
        try
        {
            // Write to MCAP file
            write_message_nts_(sample);
        }
        catch (const utils::InconsistencyException& e)
        {
            logError(DDSRECORDER_MCAP_HANDLER, "Error writting message in channel " << sample.channelId << ". Error message:\n " <<
                    e.what());
        }

        // Pop written sample (even if exception thrown)
        samples_buffer_.pop_front();
    }
}

mcap::ChannelId McapHandler::create_channel_id_nts_(
        const DdsTopic& topic)
{
    // Find schema
    mcap::SchemaId schema_id;
    try
    {
        schema_id = get_schema_id_nts_(topic.type_name);
    }
    catch (const utils::InconsistencyException&)
    {
        if (!configuration_.only_with_schema)
        {
            logInfo(DDSRECORDER_MCAP_HANDLER,
                    "Schema not found for type: " << topic.type_name << ". Creating blank schema...");

            mcap::Schema blank_schema(topic.type_name, "ros2msg", "");
            mcap_writer_.addSchema(blank_schema);
            schemas_.insert({topic.type_name, std::move(blank_schema)});

            schema_id = blank_schema.id;
        }
        else
        {
            // Propagate exception
            throw;
        }
    }

    // Create new channel
    mcap::KeyValueMap metadata = {};
    metadata[QOS_SERIALIZATION_QOS] = serialize_qos_(topic.topic_qos);
    mcap::Channel new_channel(topic.m_topic_name, "cdr", schema_id, metadata);
    mcap_writer_.addChannel(new_channel);
    auto channel_id = new_channel.id;
    channels_.insert({topic, std::move(new_channel)});
    logInfo(DDSRECORDER_MCAP_HANDLER, "Channel created: " << topic << ".");

    return channel_id;
}

mcap::ChannelId McapHandler::get_channel_id_nts_(
        const DdsTopic& topic)
{
    auto it = channels_.find(topic);
    if (it != channels_.end())
    {
        return it->second.id;
    }

    // If it does not exist yet, create it (call it with mutex taken)
    return create_channel_id_nts_(topic);
}

void McapHandler::update_channels_nts_(
        const mcap::SchemaId& old_schema_id,
        const mcap::SchemaId& new_schema_id)
{
    for (auto& channel : channels_)
    {
        if (channel.second.schemaId == old_schema_id)
        {
            logInfo(DDSRECORDER_MCAP_HANDLER, "Updating channel in topic " << channel.first.m_topic_name << ".");

            assert(channel.first.m_topic_name == channel.second.topic);
            mcap::Channel new_channel(channel.second.topic, "cdr", new_schema_id, channel.second.metadata);
            mcap_writer_.addChannel(new_channel);
            channel.second = std::move(new_channel);
        }
    }
}

mcap::SchemaId McapHandler::get_schema_id_nts_(
        const std::string& schema_name)
{
    auto it = schemas_.find(schema_name);
    if (it != schemas_.end())
    {
        return it->second.id;
    }
    else
    {
        throw utils::InconsistencyException(
                  STR_ENTRY << "Schema " << schema_name << " is not registered.");
    }
}

void McapHandler::store_dynamic_types_()
{
    auto type_names = utils::get_keys(schemas_);
    mcap::KeyValueMap dynamic_types{};

    for (auto& type_name: type_names)
    {
        const eprosima::fastrtps::types::TypeIdentifier* type_identifier = nullptr;
        const eprosima::fastrtps::types::TypeObject* type_object = nullptr;
        const eprosima::fastrtps::types::TypeInformation* type_information = nullptr;

        type_information =
                eprosima::fastrtps::types::TypeObjectFactory::get_instance()->get_type_information(type_name);
        if (type_information != nullptr)
        {
            auto dependencies = type_information->complete().dependent_typeids();
            std::string dependency_name;
            unsigned int dependency_index = 0;
            for (auto dependency: dependencies)
            {
                type_identifier = &dependency.type_id();
                type_object = eprosima::fastrtps::types::TypeObjectFactory::get_instance()->get_type_object(
                    type_identifier);
                dependency_name = type_name + "_" + std::to_string(dependency_index);

                // Serialize dynamic type in a string and store in map
                store_dynamic_type_(type_identifier, type_object, dependency_name, dynamic_types);

                // Increment suffix counter
                dependency_index++;
            }
        }
        type_identifier = nullptr;
        type_object = nullptr;

        type_identifier = eprosima::fastrtps::types::TypeObjectFactory::get_instance()->get_type_identifier(type_name,
                        true);
        if (type_identifier)
        {
            type_object =
                    eprosima::fastrtps::types::TypeObjectFactory::get_instance()->get_type_object(type_name, true);
        }

        // If complete not found, try with minimal
        if (!type_object)
        {
            type_identifier = eprosima::fastrtps::types::TypeObjectFactory::get_instance()->get_type_identifier(
                type_name, false);
            if (type_identifier)
            {
                type_object = eprosima::fastrtps::types::TypeObjectFactory::get_instance()->get_type_object(type_name,
                                false);
            }
        }

        // Serialize dynamic type in a string and store in map
        store_dynamic_type_(type_identifier, type_object, type_name, dynamic_types);
    }

    // Store dynamic types as metadata in MCAP file
    mcap::Metadata dynamic_metadata;
    dynamic_metadata.name = METADATA_DYNAMIC_TYPES;
    dynamic_metadata.metadata = dynamic_types;
    auto status = mcap_writer_.write(dynamic_metadata);

    return;
}

void McapHandler::store_dynamic_type_(
        const eprosima::fastrtps::types::TypeIdentifier* type_identifier,
        const eprosima::fastrtps::types::TypeObject* type_object,
        const std::string& type_name,
        mcap::KeyValueMap& dynamic_types)
{
    if (type_identifier != nullptr && type_object != nullptr)
    {
        auto typeid_str = serialize_type_identifier_(type_identifier);
        auto typeobj_str = serialize_type_object_(type_object);

        dynamic_types[type_name] = typeid_str + TYPES_SERIALIZATION_DELIMITER + typeobj_str;
    }
}

std::string McapHandler::tmp_filename_(
        const std::string& filename)
{
    static const std::string TMP_SUFFIX = ".tmp~";
    return filename + TMP_SUFFIX;
}

std::string McapHandler::serialize_qos_(
        const TopicQoS& qos)
{
    // TODO: Reuse code from ddspipe_yaml

    YAML::Node qos_yaml;

    // Reliability tag
    YAML::Node reliability_tag = qos_yaml[QOS_SERIALIZATION_RELIABILITY];
    if (qos.is_reliable())
    {
        reliability_tag = true;
    }
    else
    {
        reliability_tag = false;
    }

    // Durability tag
    YAML::Node durability_tag = qos_yaml[QOS_SERIALIZATION_DURABILITY];
    if (qos.is_transient_local())
    {
        durability_tag = true;
    }
    else
    {
        durability_tag = false;
    }

    // Ownership tag
    YAML::Node ownership_tag = qos_yaml[QOS_SERIALIZATION_OWNERSHIP];
    if (qos.has_ownership())
    {
        ownership_tag = true;
    }
    else
    {
        ownership_tag = false;
    }

    // Keyed tag
    YAML::Node keyed_tag = qos_yaml[QOS_SERIALIZATION_KEYED];
    if (qos.keyed)
    {
        keyed_tag = true;
    }
    else
    {
        keyed_tag = false;
    }

    return YAML::Dump(qos_yaml);
}

std::string McapHandler::serialize_type_identifier_(
        const eprosima::fastrtps::types::TypeIdentifier* type_identifier)
{
    // Reserve payload and create buffer
    size_t size = fastrtps::types::TypeIdentifier::getCdrSerializedSize(*type_identifier) +
            eprosima::fastrtps::rtps::SerializedPayload_t::representation_header_size;
    fastrtps::rtps::SerializedPayload_t payload(static_cast<uint32_t>(size));
    eprosima::fastcdr::FastBuffer fastbuffer((char*) payload.data, payload.max_size);

    // Create CDR serializer
    eprosima::fastcdr::Cdr ser(fastbuffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
    payload.encapsulation = ser.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS ? CDR_BE : CDR_LE;

    // Serialize
    type_identifier->serialize(ser);
    payload.length = (uint32_t)ser.getSerializedDataLength();
    size = (ser.getSerializedDataLength() + 3) & ~3;

    // Create CDR message
    // NOTE: Use 0 length to avoid allocation (memory already reserved in payload creation)
    eprosima::fastrtps::rtps::CDRMessage_t* cdr_message = new eprosima::fastrtps::rtps::CDRMessage_t(0);
    cdr_message->buffer = payload.data;
    cdr_message->max_size = payload.max_size;
    cdr_message->length = payload.length;
#if __BIG_ENDIAN__
    cdr_message->msg_endian = eprosima::fastrtps::rtps::BIGEND;
#else
    cdr_message->msg_endian = eprosima::fastrtps::rtps::LITTLEEND;
#endif // if __BIG_ENDIAN__

    // Add data
    bool valid = fastrtps::rtps::CDRMessage::addData(cdr_message, payload.data, payload.length);
    for (uint32_t count = payload.length; count < size; ++count)
    {
        valid &= fastrtps::rtps::CDRMessage::addOctet(cdr_message, 0);
    }
    // Copy buffer to string
    std::string typeid_str(reinterpret_cast<char const*>(cdr_message->buffer), size);

    // Delete CDR message
    // NOTE: set wraps attribute to avoid double free (buffer released by payload on destruction)
    cdr_message->wraps = true;
    delete cdr_message;

    return typeid_str;
}

std::string McapHandler::serialize_type_object_(
        const eprosima::fastrtps::types::TypeObject* type_object)
{
    // Reserve payload and create buffer
    size_t size = fastrtps::types::TypeObject::getCdrSerializedSize(*type_object) +
            eprosima::fastrtps::rtps::SerializedPayload_t::representation_header_size;
    fastrtps::rtps::SerializedPayload_t payload(static_cast<uint32_t>(size));
    eprosima::fastcdr::FastBuffer fastbuffer((char*) payload.data, payload.max_size);

    // Create CDR serializer
    eprosima::fastcdr::Cdr ser(fastbuffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
    payload.encapsulation = ser.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS ? CDR_BE : CDR_LE;

    // Serialize
    type_object->serialize(ser);
    payload.length = (uint32_t)ser.getSerializedDataLength();
    size = (ser.getSerializedDataLength() + 3) & ~3;

    // Create CDR message
    // NOTE: Use 0 length to avoid allocation (memory already reserved in payload creation)
    eprosima::fastrtps::rtps::CDRMessage_t* cdr_message = new eprosima::fastrtps::rtps::CDRMessage_t(0);
    cdr_message->buffer = payload.data;
    cdr_message->max_size = payload.max_size;
    cdr_message->length = payload.length;
#if __BIG_ENDIAN__
    cdr_message->msg_endian = eprosima::fastrtps::rtps::BIGEND;
#else
    cdr_message->msg_endian = eprosima::fastrtps::rtps::LITTLEEND;
#endif // if __BIG_ENDIAN__

    // Add data
    bool valid = fastrtps::rtps::CDRMessage::addData(cdr_message, payload.data, payload.length);
    for (uint32_t count = payload.length; count < size; ++count)
    {
        valid &= fastrtps::rtps::CDRMessage::addOctet(cdr_message, 0);
    }
    // Copy buffer to string
    std::string typeobj_str(reinterpret_cast<char const*>(cdr_message->buffer), size);

    // Delete CDR message
    // NOTE: set wraps attribute to avoid double free (buffer released by payload on destruction)
    cdr_message->wraps = true;
    delete cdr_message;

    return typeobj_str;
}

} /* namespace participants */
} /* namespace ddsrecorder */
} /* namespace eprosima */