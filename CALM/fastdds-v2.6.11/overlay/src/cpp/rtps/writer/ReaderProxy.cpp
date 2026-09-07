// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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
 * @file ReaderProxy.cpp
 *
 */


#include <fastdds/dds/log/Log.hpp>
#include <fastdds/rtps/history/WriterHistory.h>
#include <fastdds/rtps/writer/ReaderProxy.h>
#include <fastdds/rtps/writer/StatefulWriter.h>
#include <fastdds/rtps/resources/TimedEvent.h>
#include <fastrtps/utils/TimeConversion.h>
#include <fastdds/rtps/common/LocatorListComparisons.hpp>

#include <rtps/participant/RTPSParticipantImpl.h>
#include <rtps/history/HistoryAttributesExtension.hpp>

#include "rtps/messages/RTPSGapBuilder.hpp"
#include <rtps/DataSharing/DataSharingNotifier.hpp>

#include <mutex>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <cstdint>

namespace eprosima {
namespace fastrtps {
namespace rtps {

namespace
{

std::mutex& calm_metrics_mutex()
{
    static std::mutex metrics_mutex;
    return metrics_mutex;
}

std::ofstream& calm_backlog_csv()
{
    static std::ofstream stream;
    return stream;
}

std::ofstream& calm_budget_csv()
{
    static std::ofstream stream;
    return stream;
}

std::ofstream& calm_observer_csv()
{
    static std::ofstream stream;
    return stream;
}

std::ofstream& storm_observer_csv()
{
    static std::ofstream stream;
    return stream;
}

uint32_t& storm_rows_since_flush()
{
    static uint32_t rows = 0;
    return rows;
}

uint32_t& calm_rows_since_flush()
{
    static uint32_t rows = 0;
    return rows;
}

void calm_flush_metrics_periodically_locked()
{
    if (++calm_rows_since_flush() < 64)
    {
        return;
    }

    calm_backlog_csv().flush();
    calm_budget_csv().flush();
    calm_observer_csv().flush();
    calm_rows_since_flush() = 0;
}

bool calm_env_flag_enabled(
        const char* name,
        bool fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return fallback;
    }

    return 0 != std::strcmp(value, "0") &&
           0 != std::strcmp(value, "false") &&
           0 != std::strcmp(value, "off");
}

bool calm_metrics_enabled()
{
    return calm_env_flag_enabled("FASTDDS_CALM_ENABLED", false) &&
           std::getenv("FASTDDS_CALM_LOG_DIR") != nullptr;
}

bool calm_observer_enabled()
{
    return calm_env_flag_enabled("FASTDDS_CALM_OBSERVER_ENABLED", false);
}

bool calm_observer_metrics_enabled()
{
    return calm_observer_enabled() &&
           std::getenv("FASTDDS_CALM_LOG_DIR") != nullptr;
}

bool storm_observer_enabled()
{
    return calm_env_flag_enabled("FASTDDS_STORM_OBSERVER_ENABLED", false) &&
           std::getenv("FASTDDS_STORM_LOG_FILE") != nullptr;
}

uint64_t calm_now_ns()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string calm_reader_id(
        const GUID_t& guid)
{
    std::ostringstream out;
    out << guid;
    return out.str();
}

bool calm_file_is_empty(
        const std::string& path)
{
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    return !stream.good() || stream.peek() == std::ifstream::traits_type::eof();
}

void calm_open_metrics_locked()
{
    const char* log_dir = std::getenv("FASTDDS_CALM_LOG_DIR");
    if (log_dir == nullptr)
    {
        return;
    }

    std::ofstream& backlog = calm_backlog_csv();
    if (!backlog.is_open())
    {
        const std::string backlog_path = std::string(log_dir) + "/calm_backlog.csv";
        const bool write_header = calm_file_is_empty(backlog_path);
        backlog.open(backlog_path, std::ios::out | std::ios::app);
        if (write_header)
        {
            backlog << "time_ns,reader_guid,rho_bytes,requested_bytes,scheduled_repair_bytes,"
                    << "requested_changes,held_new_bytes,queued_new_bytes,delta_bytes,"
                    << "path_rate_mbps,acked_rate_mbps,control_state,failed_feedback_rounds,"
                    << "repair_round_armed,congestion_window_bytes\n";
        }
    }

    std::ofstream& budget = calm_budget_csv();
    if (!budget.is_open())
    {
        const std::string budget_path = std::string(log_dir) + "/calm_budget.csv";
        const bool write_header = calm_file_is_empty(budget_path);
        budget.open(budget_path, std::ios::out | std::ios::app);
        if (write_header)
        {
            budget << "time_ns,reader_guid,control_epoch,phase,budget_bytes,released_bytes,"
                   << "requested_remaining_bytes,rho_bytes,delta_bytes,action,budget_ratio,"
                   << "previous_rho_bytes,gamma,alpha,q_min,pacing_period_us,"
                   << "scheduled_repair_bytes,held_new_bytes,nack_delta,repair_rate_mbps,controller,"
                   << "queued_new_bytes,path_rate_mbps,acked_rate_mbps,control_state,"
                   << "failed_feedback_rounds,repair_round,repair_round_armed,release_credit,"
                   << "feedback_rtt_ewma_ms,ack_stall_threshold_ms,feedback_guard_ms,"
                   << "heartbeat_period_ms,average_sample_bytes,offered_rate_ewma_mbps,"
                   << "active_rate_reference_mbps,delivery_rate_sample_mbps,"
                   << "transport_rate_mbps,service_rate_mbps,service_app_limited,"
                   << "dynamic_rate_min_mbps,dynamic_rate_initial_mbps,dynamic_rate_max_mbps,"
                   << "dynamic_rate_ai_mbps,transport_bytes_total,acknowledged_bytes_total,"
                   << "service_success_rounds,service_failure_rounds,service_window_ms,"
                   << "service_window_acked_bytes,service_window_recovered_bytes\n";
        }
    }

    std::ofstream& observer = calm_observer_csv();
    if (!observer.is_open())
    {
        const std::string observer_path = std::string(log_dir) + "/calm_observer.csv";
        const bool write_header = calm_file_is_empty(observer_path);
        observer.open(observer_path, std::ios::out | std::ios::app);
        if (write_header)
        {
            observer << "time_ns,reader_guid,phase,rho_bytes,requested_bytes,"
                     << "requested_changes,nack_events_total,nack_bytes_total,"
                     << "unique_repair_bytes_total,repeated_nack_events_total,"
                     << "repeated_nack_bytes_total,repair_release_events_total,"
                     << "repair_release_bytes_total,released_bytes\n";
        }
    }
}

void storm_open_metrics_locked()
{
    const char* path = std::getenv("FASTDDS_STORM_LOG_FILE");
    if (path == nullptr)
    {
        return;
    }

    std::ofstream& observer = storm_observer_csv();
    if (!observer.is_open())
    {
        const bool write_header = calm_file_is_empty(path);
        observer.open(path, std::ios::out | std::ios::app);
        if (write_header)
        {
            observer << "time_ns,writer_guid,reader_guid,event,event_bytes,rho_bytes,oldest_repair_age_ms,"
                     << "requested_bytes,requested_changes,repair_unsent_bytes,"
                     << "repair_underway_bytes,repair_unacknowledged_bytes,"
                     << "nack_events_total,nack_bytes_total,unique_repair_bytes_total,"
                     << "repeated_nack_events_total,repeated_nack_bytes_total,"
                     << "tnr_release_events_total,tnr_requested_changes_total,tnr_requested_bytes_total,"
                     << "flow_enqueue_accepted_changes_total,flow_enqueue_accepted_bytes_total,"
                     << "flow_enqueue_rejected_changes_total,flow_enqueue_rejected_bytes_total,"
                     << "transport_repair_attempt_events_total,transport_repair_attempt_bytes_total,"
                     << "ack_recovered_bytes_total,calm_state,failed_feedback_rounds,repair_round,"
                     << "repair_round_armed,calm_window_bytes,release_credit,"
                     << "event_sample_seq,event_sample_retransmit_count,oldest_repair_seq,"
                     << "oldest_repair_retransmit_count,oldest_no_progress_rounds,"
                     << "reader_acked_high_seq,active_repair_samples,max_repair_retransmit_count,"
                     << "detector_nonshrinking_rounds,ack_progress_age_ms,admitted_bytes,"
                     << "admission_window_bytes,event_sample_failed_repair_count,"
                     << "oldest_repair_failed_repair_count,max_failed_repair_count,"
                     << "detector_retry_signal,failure_severity,"
                     << "effective_window_gamma,effective_rate_gamma,"
                     << "calm4_delta_u_bytes,calm4_delta_oldest_failed,"
                     << "calm4_failure_fraction,calm4_progress_fraction,"
                     << "calm4_fixed_pacing_ms,calm41_feedback_timeout_ms\n";
        }
    }
}

uint64_t calm_requested_change_bytes(
        const ChangeForReader_t& reader_change)
{
    const CacheChange_t* change = reader_change.getChange();
    if (change == nullptr)
    {
        return 0;
    }

    const uint64_t payload_length = change->serializedPayload.length;
    const uint32_t fragment_size = change->getFragmentSize();
    const uint32_t fragment_count = change->getFragmentCount();

    if (fragment_size == 0 || fragment_count == 0)
    {
        return payload_length;
    }

    uint64_t bytes = 0;
    const FragmentNumberSet_t unsent_fragments = reader_change.getUnsentFragments();
    unsent_fragments.for_each(
        [&](FragmentNumber_t fragment)
        {
            if (fragment == 0 || fragment > fragment_count)
            {
                return;
            }

            const uint64_t offset = static_cast<uint64_t>(fragment - 1) * fragment_size;
            if (offset >= payload_length)
            {
                return;
            }

            const uint64_t remaining = payload_length - offset;
            bytes += std::min<uint64_t>(fragment_size, remaining);
        });

    return bytes;
}

uint64_t calm_fragment_set_bytes(
        const ChangeForReader_t& reader_change,
        const FragmentNumberSet_t& fragments)
{
    const CacheChange_t* change = reader_change.getChange();
    if (change == nullptr)
    {
        return 0;
    }

    const uint64_t payload_length = change->serializedPayload.length;
    const uint32_t fragment_size = change->getFragmentSize();
    const uint32_t fragment_count = change->getFragmentCount();
    uint64_t bytes = 0;
    fragments.for_each(
        [&](FragmentNumber_t fragment)
        {
            if (fragment == 0 || fragment_size == 0 || fragment > fragment_count)
            {
                return;
            }

            const uint64_t offset = static_cast<uint64_t>(fragment - 1) * fragment_size;
            if (offset < payload_length)
            {
                bytes += std::min<uint64_t>(fragment_size, payload_length - offset);
            }
        });
    return bytes;
}

double calm_env_double(
        const char* name,
        double fallback,
        double minimum,
        double maximum)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return fallback;
    }

    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || parsed < minimum || parsed > maximum)
    {
        return fallback;
    }

    return parsed;
}

uint64_t calm_env_uint64(
        const char* name,
        uint64_t fallback,
        uint64_t minimum,
        uint64_t maximum = (std::numeric_limits<uint64_t>::max)())
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed < minimum || parsed > maximum)
    {
        return fallback;
    }

    return static_cast<uint64_t>(parsed);
}

bool calm_env_enabled()
{
    return calm_env_flag_enabled("FASTDDS_CALM_ENABLED", false);
}

uint64_t calm_hold_mode()
{
    return calm_env_uint64("FASTDDS_CALM_HOLD_MODE", 1, 0, 2);
}

const char* calm_controller()
{
    const char* controller = std::getenv("FASTDDS_CALM_CONTROLLER");
    return controller == nullptr || controller[0] == '\0' ? "calm4" : controller;
}

bool calm4_controller()
{
    return 0 == std::strcmp(calm_controller(), "calm4");
}

bool calm41_controller()
{
    return 0 == std::strcmp(calm_controller(), "calm41");
}

bool calm4_family_controller()
{
    return calm4_controller() || calm41_controller();
}

bool calm41_entry_ack_pacing_enabled()
{
    const char* mode = std::getenv("FASTDDS_CALM41_PACING_MODE");
    return mode != nullptr &&
           (0 == std::strcmp(mode, "entry_ack") ||
           0 == std::strcmp(mode, "entry_service"));
}

double calm41_entry_pacing_period_ms(
        uint64_t budget_bytes,
        double entry_service_rate_mbps,
        double eta,
        double fallback_ms)
{
    if (budget_bytes == 0 || entry_service_rate_mbps <= 0.0)
    {
        return fallback_ms;
    }

    // Mbps is 10^6 bit/s, so 8*bytes/(Mbps*1000) yields milliseconds.
    return eta * static_cast<double>(budget_bytes) * 8.0 /
           (entry_service_rate_mbps * 1000.0);
}

double calm_batch_interval_ms(
        uint64_t batch_bytes,
        double release_rate_mbps,
        double pacing_floor_ms,
        double observed_batch_drain_ms,
        double drain_guard)
{
    const double serialization_ms = release_rate_mbps > 0.0 ?
            static_cast<double>(batch_bytes) * 8.0 / (release_rate_mbps * 1000.0) :
            0.0;
    return std::max(
        pacing_floor_ms,
        std::max(serialization_ms, observed_batch_drain_ms * drain_guard));
}

}  // namespace

ReaderProxy::ReaderProxy(
        const WriterTimes& times,
        const RemoteLocatorsAllocationAttributes& loc_alloc,
        StatefulWriter* writer)
    : is_active_(false)
    , locator_info_(
        writer, loc_alloc.max_unicast_locators,
        loc_alloc.max_multicast_locators)
    , durability_kind_(VOLATILE)
    , expects_inline_qos_(false)
    , is_reliable_(false)
    , disable_positive_acks_(false)
    , writer_(writer)
    , changes_for_reader_(resource_limits_from_history(writer->mp_history->m_att, 0))
    , nack_supression_event_(nullptr)
    , initial_heartbeat_event_(nullptr)
    , timers_enabled_(false)
    , next_expected_acknack_count_(0)
    , last_nackfrag_count_(0)
{
    auto participant = writer_->getRTPSParticipant();
    if (nullptr != participant)
    {
        nack_supression_event_ = new TimedEvent(participant->getEventResource(),
                        [&]() -> bool
                        {
                            writer_->perform_nack_supression(guid());
                            return false;
                        },
                        TimeConv::Time_t2MilliSecondsDouble(times.nackSupressionDuration));

        initial_heartbeat_event_ = new TimedEvent(participant->getEventResource(),
                        [&]() -> bool
                        {
                            writer_->intraprocess_heartbeat(this);
                            return false;
                        }, 0);
    }

    stop();
}

bool ReaderProxy::rtps_is_relevant(
        CacheChange_t* change) const
{
    auto filter = writer_->reader_data_filter();
    if (nullptr != filter)
    {
        bool ret = filter->is_relevant(*change, guid());
        logInfo(RTPS_READER_PROXY,
                "Change " << change->instanceHandle << " is relevant for reader " << guid() << "? " << ret);
        return ret;
    }
    return true;
}

ReaderProxy::~ReaderProxy()
{
    if (nack_supression_event_)
    {
        delete(nack_supression_event_);
        nack_supression_event_ = nullptr;
    }

    if (initial_heartbeat_event_)
    {
        delete(initial_heartbeat_event_);
        initial_heartbeat_event_ = nullptr;
    }
}

void ReaderProxy::start(
        const ReaderProxyData& reader_attributes,
        bool is_datasharing)
{
    locator_info_.start(
        reader_attributes.guid(),
        reader_attributes.remote_locators().unicast,
        reader_attributes.remote_locators().multicast,
        reader_attributes.m_expectsInlineQos,
        is_datasharing);

    is_active_ = true;
    durability_kind_ = reader_attributes.m_qos.m_durability.durabilityKind();
    expects_inline_qos_ = reader_attributes.m_expectsInlineQos;
    is_reliable_ = reader_attributes.m_qos.m_reliability.kind != BEST_EFFORT_RELIABILITY_QOS;
    disable_positive_acks_ = reader_attributes.disable_positive_acks();
    if (durability_kind_ == DurabilityKind_t::VOLATILE)
    {
        SequenceNumber_t min_sequence = writer_->get_seq_num_min();
        changes_low_mark_ = (min_sequence == SequenceNumber_t::unknown()) ?
                writer_->next_sequence_number() - 1 : min_sequence - 1;
    }
    else
    {
        acked_changes_set(SequenceNumber_t());  // Simulate initial acknack to set low mark
    }

    timers_enabled_.store(is_remote_and_reliable());
    if (is_local_reader() && initial_heartbeat_event_)
    {
        initial_heartbeat_event_->restart_timer();
    }

    logInfo(RTPS_READER_PROXY, "Reader Proxy started");
}

bool ReaderProxy::update(
        const ReaderProxyData& reader_attributes)
{
    durability_kind_ = reader_attributes.m_qos.m_durability.durabilityKind();
    expects_inline_qos_ = reader_attributes.m_expectsInlineQos;
    is_reliable_ = reader_attributes.m_qos.m_reliability.kind != BEST_EFFORT_RELIABILITY_QOS;
    disable_positive_acks_ = reader_attributes.disable_positive_acks();

    locator_info_.update(
        reader_attributes.remote_locators().unicast,
        reader_attributes.remote_locators().multicast,
        reader_attributes.m_expectsInlineQos);

    return true;
}

void ReaderProxy::stop()
{
    locator_info_.stop();
    is_active_ = false;
    disable_timers();

    changes_for_reader_.clear();
    calm_ = CalmState();
    next_expected_acknack_count_ = 0;
    last_nackfrag_count_ = 0;
    changes_low_mark_ = SequenceNumber_t();
}

void ReaderProxy::disable_timers()
{
    if (timers_enabled_.exchange(false) && nack_supression_event_)
    {
        nack_supression_event_->cancel_timer();
    }
    if (initial_heartbeat_event_)
    {
        initial_heartbeat_event_->cancel_timer();
    }
}

void ReaderProxy::update_nack_supression_interval(
        const Duration_t& interval)
{
    if (nack_supression_event_)
    {
        nack_supression_event_->update_interval(interval);
    }
}

void ReaderProxy::add_change(
        const ChangeForReader_t& change,
        bool is_relevant,
        bool restart_nack_supression)
{
    if (restart_nack_supression && timers_enabled_.load() && nack_supression_event_)
    {
        nack_supression_event_->restart_timer();
    }

    add_change(change, is_relevant);
}

void ReaderProxy::add_change(
        const ChangeForReader_t& change,
        bool is_relevant,
        bool restart_nack_supression,
        const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time)
{
    if (restart_nack_supression && timers_enabled_ && nack_supression_event_)
    {
        nack_supression_event_->restart_timer(max_blocking_time);
    }

    add_change(change, is_relevant);
}

void ReaderProxy::add_change(
        const ChangeForReader_t& change,
        bool is_relevant)
{
    assert(change.getSequenceNumber() > changes_low_mark_);
    assert(changes_for_reader_.empty() ? true :
            change.getSequenceNumber() > changes_for_reader_.back().getSequenceNumber());

    // Irrelevant changes are not added to the collection
    if (!is_relevant)
    {
        if ( !is_reliable_ &&
                changes_low_mark_ + 1 == change.getSequenceNumber())
        {
            changes_low_mark_ = change.getSequenceNumber();
        }
        return;
    }

    // Observe before push_back: callers may pass a reference into a container
    // whose storage can be invalidated when the proxy vector grows.
    calm_observe_new_change(change);

    if (changes_for_reader_.push_back(change) == nullptr)
    {
        // This should never happen
        logError(RTPS_READER_PROXY, "Error adding change " << change.getSequenceNumber()
                                                           << " to reader proxy " << guid());
        eprosima::fastdds::dds::Log::Flush();
        assert(false);
    }

}

bool ReaderProxy::has_changes() const
{
    return !changes_for_reader_.empty();
}

bool ReaderProxy::change_is_acked(
        const SequenceNumber_t& seq_num) const
{
    if (seq_num <= changes_low_mark_ || changes_for_reader_.empty())
    {
        return true;
    }

    ChangeConstIterator chit = find_change(seq_num);
    if (chit == changes_for_reader_.end())
    {
        // There is a hole in changes_for_reader_
        // This means a change was removed, or was not relevant.
        return true;
    }

    return chit->getStatus() == ACKNOWLEDGED;
}

bool ReaderProxy::change_is_unsent(
        const SequenceNumber_t& seq_num,
        FragmentNumber_t& next_unsent_frag,
        SequenceNumber_t& gap_seq,
        const SequenceNumber_t& min_seq,
        bool& need_reactivate_periodic_heartbeat) const
{
    if (seq_num <= changes_low_mark_ || changes_for_reader_.empty())
    {
        return false;
    }

    ChangeConstIterator chit = find_change(seq_num);
    if (chit == changes_for_reader_.end())
    {
        // There is a hole in changes_for_reader_
        // This means a change was removed.
        return false;
    }

    if (chit->calm_new_held())
    {
        return false;
    }

    bool returned_value = chit->getStatus() == UNSENT;

    if (returned_value)
    {
        next_unsent_frag = chit->get_next_unsent_fragment();
        gap_seq = SequenceNumber_t::unknown();

        if (is_reliable_ && !chit->has_been_delivered())
        {
            need_reactivate_periodic_heartbeat |= true;
            SequenceNumber_t prev =
                    (changes_for_reader_.begin() != chit ?
                    std::prev(chit)->getSequenceNumber() :
                    changes_low_mark_
                    ) + 1;

            if (prev != chit->getSequenceNumber())
            {
                gap_seq = prev;

                // Verify the calculated gap_seq in ReaderProxy is a real hole in the history.
                if (gap_seq < min_seq) // Several samples of the hole are not really already available.
                {
                    if (min_seq < seq_num)
                    {
                        gap_seq = min_seq;
                    }
                    else
                    {
                        gap_seq = SequenceNumber_t::unknown();
                    }
                }
            }
        }
    }

    return returned_value;
}

void ReaderProxy::acked_changes_set(
        const SequenceNumber_t& seq_num)
{
    const SequenceNumber_t previous_low_mark = changes_low_mark_;
    SequenceNumber_t future_low_mark = seq_num;
    uint64_t acknowledged_bytes = 0;
    uint64_t recovered_repair_bytes = 0;

    if (seq_num > changes_low_mark_)
    {
        ChangeIterator chit = find_change(seq_num, false);
        // continue advancing until next change is not acknowledged
        while (chit != changes_for_reader_.end()
                && chit->getSequenceNumber() == future_low_mark
                && chit->getStatus() == ACKNOWLEDGED)
        {
            ++chit;
            ++future_low_mark;
        }
        for (auto acknowledged = changes_for_reader_.begin(); acknowledged != chit; ++acknowledged)
        {
            if (acknowledged->getChange() != nullptr)
            {
                acknowledged_bytes += acknowledged->getChange()->serializedPayload.length;
            }
            if (acknowledged->calm_repair_pending())
            {
                recovered_repair_bytes += acknowledged->calm_repair_pending_bytes();
            }
        }
        changes_for_reader_.erase(changes_for_reader_.begin(), chit);
    }
    else
    {
        future_low_mark = changes_low_mark_ + 1;

        if (seq_num == SequenceNumber_t() && durability_kind_ != DurabilityKind_t::VOLATILE)
        {
            // Special case. Currently only used on Builtin StatefulWriters
            // after losing lease duration, and on late joiners to set
            // changes_low_mark_ to match that of the writer.
            SequenceNumber_t min_sequence = writer_->get_seq_num_min();
            if (min_sequence != SequenceNumber_t::unknown())
            {
                SequenceNumber_t current_sequence = seq_num;
                if (seq_num < min_sequence)
                {
                    current_sequence = min_sequence;
                }
                future_low_mark = current_sequence;

                bool should_sort = false;
                for (; current_sequence <= changes_low_mark_; ++current_sequence)
                {
                    // Skip all consecutive changes already in the collection
                    ChangeConstIterator it = find_change(current_sequence);
                    while ( it != changes_for_reader_.end() &&
                            current_sequence <= changes_low_mark_ &&
                            it->getSequenceNumber() == current_sequence)
                    {
                        ++current_sequence;
                        ++it;
                    }

                    if (current_sequence <= changes_low_mark_)
                    {
                        CacheChange_t* change = nullptr;
                        if (writer_->mp_history->get_change(current_sequence, writer_->getGuid(), &change))
                        {
                            should_sort = true;
                            ChangeForReader_t cr(change);
                            cr.setStatus(UNACKNOWLEDGED);
                            changes_for_reader_.push_back(cr);
                        }
                    }
                }
                // Keep changes sorted by sequence number
                if (should_sort)
                {
                    std::sort(changes_for_reader_.begin(), changes_for_reader_.end(), ChangeForReaderCmp());
                }
            }
            else if (!is_local_reader())
            {
                future_low_mark = writer_->next_sequence_number();
            }
        }
    }
    changes_low_mark_ = future_low_mark - 1;
    if (changes_low_mark_ > previous_low_mark)
    {
        calm_.storm_oldest_no_progress_seq = SequenceNumber_t::unknown();
        calm_.storm_oldest_no_progress_rounds = 0;
    }
    calm_.storm_ack_recovered_bytes_total += recovered_repair_bytes;
    calm_note_ack(acknowledged_bytes, recovered_repair_bytes);
    if (acknowledged_bytes > 0)
    {
        calm_log_observer_snapshot("ack", 0);
        storm_log_snapshot("ack", recovered_repair_bytes);
    }
    calm_reset_control_if_recovered();
}

bool ReaderProxy::requested_changes_set(
        const SequenceNumberSet_t& seq_num_set,
        RTPSGapBuilder& gap_builder,
        const SequenceNumber_t& min_seq_in_history)
{
    bool isSomeoneWasSetRequested = false;
    bool observed_nack = false;
    bool observed_repeated_nack = false;
    const double feedback_guard_ms = calm_feedback_guard_ms();

    if (SequenceNumber_t::unknown() != min_seq_in_history)
    {
        seq_num_set.for_each([&](SequenceNumber_t sit)
                {
                    ChangeIterator chit = find_change(sit, true);
                    if (chit != changes_for_reader_.end())
                    {
                        if (UNACKNOWLEDGED == chit->getStatus())
                        {
                            if (calm_tracking_enabled())
                            {
                                const uint64_t previous_bytes = chit->calm_repair_pending_bytes();
                                const bool failed_repair_feedback =
                                        chit->calm_note_whole_nack_after_repair(
                                            feedback_guard_ms);
                                const bool repeated = chit->calm_nack_count() > 0 ||
                                        chit->calm_retransmit_count() > 0;
                                chit->calm_mark_whole_repair_pending();
                                const uint64_t current_bytes = chit->calm_repair_pending_bytes();
                                calm_record_nack(
                                    current_bytes,
                                    current_bytes > previous_bytes ? current_bytes - previous_bytes : 0,
                                    repeated,
                                    failed_repair_feedback,
                                    chit->calm_last_failed_repair_severity());
                                observed_nack = true;
                                observed_repeated_nack |= repeated;
                            }
                            if (calm_controls_repair())
                            {
                                calm_note_nack();
                                if (!chit->calm_retry_ready(
                                            std::chrono::steady_clock::now(),
                                            calm_.retry_cooldown_ms))
                                {
                                    return;
                                }
                            }
                            chit->setStatus(REQUESTED);
                            chit->markAllFragmentsAsUnsent();
                            isSomeoneWasSetRequested = true;
                        }
                        else if (REQUESTED == chit->getStatus())
                        {
                            // Keep the outstanding debt unique, but retain repeated NACK information.
                            if (calm_tracking_enabled())
                            {
                                const uint64_t previous_bytes = chit->calm_repair_pending_bytes();
                                const bool failed_repair_feedback =
                                        chit->calm_note_whole_nack_after_repair(
                                            feedback_guard_ms);
                                const bool repeated = chit->calm_nack_count() > 0 ||
                                        chit->calm_retransmit_count() > 0;
                                chit->calm_mark_whole_repair_pending();
                                const uint64_t current_bytes = chit->calm_repair_pending_bytes();
                                calm_record_nack(
                                    current_bytes,
                                    current_bytes > previous_bytes ? current_bytes - previous_bytes : 0,
                                    repeated,
                                    failed_repair_feedback,
                                    chit->calm_last_failed_repair_severity());
                                observed_nack = true;
                                observed_repeated_nack |= repeated;
                            }
                            isSomeoneWasSetRequested = true;
                        }
                        else if (calm_tracking_enabled() &&
                                chit->calm_repair_pending() &&
                                (UNSENT == chit->getStatus() || UNDERWAY == chit->getStatus()))
                        {
                            // Feedback may arrive while an older repair attempt is queued or in flight.
                            // Observe it without changing the protocol status.  A fresh UNSENT
                            // change may be held new data that has never reached this reader, so
                            // stale ACKNACK feedback must not promote it into repair debt.
                            const uint64_t previous_bytes = chit->calm_repair_pending_bytes();
                            chit->calm_note_repeated_nack();
                            const uint64_t current_bytes = chit->calm_repair_pending_bytes();
                            calm_record_nack(
                                current_bytes,
                                current_bytes > previous_bytes ? current_bytes - previous_bytes : 0,
                                true,
                                chit->calm_retransmitted_since(calm_.repair_round_start_time),
                                1.0);
                            observed_nack = true;
                            observed_repeated_nack = true;
                        }
                    }
                    else if ((sit >= min_seq_in_history) && (sit > changes_low_mark_))
                    {
                        gap_builder.add(sit);
                    }
                });
    }

    if (observed_nack)
    {
        calm_log_observer_snapshot(
            observed_repeated_nack ? "nack_repeated" : "nack", 0);
        storm_log_snapshot(
            observed_repeated_nack ? "nack_repeated" : "nack", 0);
    }

    if (isSomeoneWasSetRequested && calm_controls_repair())
    {
        calm_note_request_ready();
        calm_hold_unsent_new_changes();
        logInfo(RTPS_READER_PROXY, "Requested Changes: " << seq_num_set);
    }

    return isSomeoneWasSetRequested;
}

bool ReaderProxy::process_initial_acknack(
        const std::function<void(ChangeForReader_t& change)>& func)
{
    if (is_local_reader())
    {
        return 0 != convert_status_on_all_changes(UNACKNOWLEDGED, UNSENT, func);
    }

    return true;
}

void ReaderProxy::from_unsent_to_status(
        const SequenceNumber_t& seq_num,
        ChangeForReaderStatus_t status,
        bool restart_nack_supression,
        bool delivered)
{
    // This function must not be called by a best-effort reader.
    // It will use acked_changes_set().
    assert(is_reliable_);

    if (restart_nack_supression && is_remote_and_reliable() && nack_supression_event_)
    {
        assert(timers_enabled_.load());
        nack_supression_event_->restart_timer();
    }

    // Called when delivering an UNSENT sample, the seq_number must exists in the ReaderProxy.
    assert(seq_num > changes_low_mark_);
    ChangeIterator it = find_change(seq_num, true);
    assert(changes_for_reader_.end() != it);
    assert(UNSENT == it->getStatus());
    assert(UNSENT != status);

    if (ACKNOWLEDGED == status && seq_num == changes_low_mark_ + 1)
    {
        assert(changes_for_reader_.begin() == it);
        changes_for_reader_.erase(it);
        acked_changes_set(seq_num + 1);
        return;
    }

    it->setStatus(status);

    if (delivered)
    {
        it->set_delivered();
    }

    if (calm_tracking_enabled() && UNDERWAY == status && it->calm_repair_pending())
    {
        calm_note_repair_attempt();
        storm_note_sample_retransmit(seq_num);
        storm_log_snapshot("repair_underway", it->calm_repair_pending_bytes(), seq_num);
        if (calm_controls_repair() && calm_requested_size() > 0)
        {
            // The selected batch has left the FlowController. Allow the pacing
            // event to admit the next oldest REQUESTED batch without waiting
            // for another HEARTBEAT/NACK cycle.
            calm_note_request_ready();
            calm_pacing_active(true);
        }
    }
}

bool ReaderProxy::mark_fragment_as_sent_for_change(
        const SequenceNumber_t& seq_num,
        FragmentNumber_t frag_num,
        bool& was_last_fragment)
{
    was_last_fragment = false;

    if (seq_num <= changes_low_mark_)
    {
        return false;
    }

    bool change_found = false;
    ChangeIterator it = find_change(seq_num, true);

    if (it != changes_for_reader_.end())
    {
        change_found = true;
        it->markFragmentsAsSent(frag_num);
        if (it->calm_consume_new_release_credit(frag_num) &&
                !it->getUnsentFragments().empty())
        {
            // The held-new byte credit for this tick is exhausted. Leave the
            // remaining fragments in WHC and let the next pacing tick requeue
            // this same Change.
            it->calm_hold_new();
            calm_pacing_active(true);
        }
        was_last_fragment = it->getUnsentFragments().empty();
        if (was_last_fragment && calm_controls_repair() &&
                it->calm_activate_deferred_fragments())
        {
            // The selected byte window has left the FlowController, but this
            // Change still owns deferred fragments.  Keep it REQUESTED so the
            // pacing event releases the next window; it is not UNDERWAY yet.
            was_last_fragment = false;
            calm_note_request_ready();
            calm_pacing_active(true);
        }
    }

    return change_found;
}

bool ReaderProxy::perform_nack_supression()
{
    return 0 != convert_status_on_all_changes(UNDERWAY, UNACKNOWLEDGED);
}

uint32_t ReaderProxy::perform_acknack_response(
        const std::function<void(ChangeForReader_t& change)>& func)
{
    return convert_status_on_all_changes(REQUESTED, UNSENT, func);
}

uint64_t ReaderProxy::calm_backlog_size() const
{
    uint64_t rho = 0;

    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_repair_pending())
        {
            rho += change.calm_repair_pending_bytes();
        }
    }

    return rho;
}

uint64_t ReaderProxy::calm_average_sample_bytes() const
{
    uint64_t bytes = 0;
    uint64_t samples = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.getChange() != nullptr)
        {
            bytes += change.getChange()->serializedPayload.length;
            ++samples;
        }
    }

    return samples == 0 ? 0 : bytes / samples;
}

void ReaderProxy::calm_observe_new_change(
        const ChangeForReader_t& change)
{
    if (!calm_enabled() || change.getChange() == nullptr)
    {
        return;
    }

    const uint64_t transport_atom_bytes = change.getChange()->getFragmentSize() == 0 ?
            change.getChange()->serializedPayload.length :
            change.getChange()->getFragmentSize();
    if (transport_atom_bytes > 0)
    {
        calm_.minimum_transport_atom_bytes = calm_.minimum_transport_atom_bytes == 0 ?
                transport_atom_bytes :
                std::min(calm_.minimum_transport_atom_bytes, transport_atom_bytes);
    }

    const auto now = std::chrono::steady_clock::now();
    if (calm_.first_new_change_time == std::chrono::steady_clock::time_point::min())
    {
        calm_.first_new_change_time = now;
    }
    if (calm_.last_new_change_time != std::chrono::steady_clock::time_point::min())
    {
        const double interval_seconds = std::chrono::duration<double>(
            now - calm_.last_new_change_time).count();
        if (interval_seconds >= 0.0001 && interval_seconds <= 10.0)
        {
            calm_.offered_rate_alpha = calm_env_double(
                "FASTDDS_CALM_OFFERED_RATE_EWMA_ALPHA",
                calm_.offered_rate_alpha, 0.001, 1.0);
            const double sample_rate_mbps = std::min(
                100000.0,
                static_cast<double>(change.getChange()->serializedPayload.length) * 8.0 /
                (interval_seconds * 1000000.0));
            calm_.offered_rate_ewma_mbps = calm_.offered_rate_ewma_mbps == 0.0 ?
                    sample_rate_mbps :
                    (1.0 - calm_.offered_rate_alpha) * calm_.offered_rate_ewma_mbps +
                    calm_.offered_rate_alpha * sample_rate_mbps;
        }
    }
    calm_.last_new_change_time = now;
}

void ReaderProxy::calm_refresh_budget_parameters()
{
    calm_.minimum_budget_sample_multiplier = calm_env_double(
        "FASTDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER",
        calm_.minimum_budget_sample_multiplier, 0.0, 1000000.0);
    calm_.initial_budget_sample_multiplier = calm_env_double(
        "FASTDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER",
        calm_.initial_budget_sample_multiplier, 0.0, 1000000.0);
    calm_.maximum_budget_sample_multiplier = calm_env_double(
        "FASTDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER",
        calm_.maximum_budget_sample_multiplier, 0.0, 1000000.0);
    calm_.additive_increase_sample_multiplier = calm_env_double(
        "FASTDDS_CALM_AI_B_SAMPLE_MULTIPLIER",
        calm_.additive_increase_sample_multiplier, 0.0, 1000000.0);

    const uint64_t average_sample_bytes = calm_average_sample_bytes();
    const auto scaled_bytes = [average_sample_bytes](double multiplier) -> uint64_t
            {
                if (average_sample_bytes == 0 || multiplier <= 0.0)
                {
                    return 0;
                }
                const long double value = std::ceil(
                    static_cast<long double>(average_sample_bytes) * multiplier);
                return value >= static_cast<long double>((std::numeric_limits<uint64_t>::max)()) ?
                       (std::numeric_limits<uint64_t>::max)() :
                       std::max<uint64_t>(1, static_cast<uint64_t>(value));
            };

    const char* scaling_mode = std::getenv("FASTDDS_CALM_BUDGET_SCALING_MODE");
    const bool floor_mode = scaling_mode == nullptr ||
            0 == std::strcmp(scaling_mode, "floor") ||
            0 == std::strcmp(scaling_mode, "hybrid");
    const auto apply_scaled = [floor_mode](uint64_t current, uint64_t scaled) -> uint64_t
            {
                if (scaled == 0)
                {
                    return current;
                }
                return floor_mode ? std::max(current, scaled) : scaled;
            };

    const uint64_t scaled_max = scaled_bytes(calm_.maximum_budget_sample_multiplier);
    if (scaled_max > 0)
    {
        calm_.max_batch_bytes = apply_scaled(calm_.max_batch_bytes, scaled_max);
    }
    const uint64_t scaled_initial = scaled_bytes(calm_.initial_budget_sample_multiplier);
    if (scaled_initial > 0)
    {
        calm_.initial_budget_bytes = apply_scaled(calm_.initial_budget_bytes, scaled_initial);
    }
    const uint64_t scaled_min = scaled_bytes(calm_.minimum_budget_sample_multiplier);
    if (scaled_min > 0)
    {
        calm_.minimum_budget_bytes = apply_scaled(calm_.minimum_budget_bytes, scaled_min);
    }
    const uint64_t scaled_ai = scaled_bytes(calm_.additive_increase_sample_multiplier);
    if (scaled_ai > 0)
    {
        calm_.additive_increase_bytes = apply_scaled(calm_.additive_increase_bytes, scaled_ai);
    }

    calm_.initial_budget_bytes = std::max<uint64_t>(
        1, std::min(calm_.initial_budget_bytes, calm_.max_batch_bytes));
    calm_.minimum_budget_bytes = std::max<uint64_t>(
        1, std::min(calm_.minimum_budget_bytes, calm_.initial_budget_bytes));
    calm_.max_scheduled_bytes = std::max(
        calm_.max_scheduled_bytes, calm_.max_batch_bytes);
}

void ReaderProxy::calm_refresh_rate_parameters()
{
    calm_.max_repair_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MAX_REPAIR_RATE_MBPS",
        calm_.max_repair_rate_mbps, 1.0, 100000.0);
    calm_.min_repair_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MIN_REPAIR_RATE_MBPS",
        calm_.min_repair_rate_mbps, 1.0, calm_.max_repair_rate_mbps);
    calm_.initial_repair_rate_mbps = calm_env_double(
        "FASTDDS_CALM_INITIAL_REPAIR_RATE_MBPS",
        calm_.initial_repair_rate_mbps,
        calm_.min_repair_rate_mbps,
        calm_.max_repair_rate_mbps);
    calm_.storm_rate_increase_mbps = calm_env_double(
        "FASTDDS_CALM_STORM_RATE_AI_MBPS",
        calm_.storm_rate_increase_mbps, 0.0, 10000.0);

    calm_.min_rate_offered_multiplier = calm_env_double(
        "FASTDDS_CALM_MIN_RATE_OFFERED_MULTIPLIER", 0.0, 0.0, 1000000.0);
    calm_.initial_rate_offered_multiplier = calm_env_double(
        "FASTDDS_CALM_INITIAL_RATE_OFFERED_MULTIPLIER", 0.0, 0.0, 1000000.0);
    calm_.max_rate_offered_multiplier = calm_env_double(
        "FASTDDS_CALM_MAX_RATE_OFFERED_MULTIPLIER", 0.0, 0.0, 1000000.0);
    calm_.rate_increase_offered_multiplier = calm_env_double(
        "FASTDDS_CALM_AI_V_OFFERED_MULTIPLIER", 0.0, 0.0, 1000000.0);

    double reference_rate_mbps = calm_.active_rate_reference_mbps > 0.0 ?
            calm_.active_rate_reference_mbps : calm_.offered_rate_ewma_mbps;
    if (reference_rate_mbps <= 0.0)
    {
        const double heartbeat_ms = calm_heartbeat_period_ms();
        const uint64_t average_sample_bytes = calm_average_sample_bytes();
        if (heartbeat_ms > 0.0 && average_sample_bytes > 0)
        {
            reference_rate_mbps = static_cast<double>(average_sample_bytes) * 8.0 /
                    (2.0 * heartbeat_ms * 1000.0);
        }
    }
    if (calm_service_rate_enabled())
    {
        calm_refresh_service_rate_bounds();
        return;
    }
    if (reference_rate_mbps <= 0.0)
    {
        return;
    }

    if (calm_.max_rate_offered_multiplier > 0.0)
    {
        calm_.max_repair_rate_mbps = std::max(
            1.0, reference_rate_mbps * calm_.max_rate_offered_multiplier);
    }
    if (calm_.min_rate_offered_multiplier > 0.0)
    {
        calm_.min_repair_rate_mbps = std::max(
            1.0, reference_rate_mbps * calm_.min_rate_offered_multiplier);
    }
    calm_.min_repair_rate_mbps = std::min(
        calm_.min_repair_rate_mbps, calm_.max_repair_rate_mbps);
    if (calm_.initial_rate_offered_multiplier > 0.0)
    {
        calm_.initial_repair_rate_mbps =
                reference_rate_mbps * calm_.initial_rate_offered_multiplier;
    }
    calm_.initial_repair_rate_mbps = std::max(
        calm_.min_repair_rate_mbps,
        std::min(calm_.initial_repair_rate_mbps, calm_.max_repair_rate_mbps));
    if (calm_.rate_increase_offered_multiplier > 0.0)
    {
        calm_.storm_rate_increase_mbps =
                reference_rate_mbps * calm_.rate_increase_offered_multiplier;
    }
}

bool ReaderProxy::calm_service_rate_enabled() const
{
    const char* mode = std::getenv("FASTDDS_CALM_RELEASE_RATE_MODE");
    return mode != nullptr &&
           (0 == std::strcmp(mode, "dds_service") ||
           0 == std::strcmp(mode, "adaptive") ||
           0 == std::strcmp(mode, "service_rate"));
}

uint64_t ReaderProxy::calm_minimum_transport_atom_bytes() const
{
    if (calm_.minimum_transport_atom_bytes > 0)
    {
        return calm_.minimum_transport_atom_bytes;
    }

    uint64_t minimum = (std::numeric_limits<uint64_t>::max)();
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.getChange() == nullptr)
        {
            continue;
        }

        const uint64_t atom = change.getChange()->getFragmentSize() == 0 ?
                change.getChange()->serializedPayload.length :
                change.getChange()->getFragmentSize();
        if (atom > 0)
        {
            minimum = std::min(minimum, atom);
        }
    }
    return minimum == (std::numeric_limits<uint64_t>::max)() ? 1 : minimum;
}

void ReaderProxy::calm_refresh_service_rate_bounds()
{
    calm_.service_rate_min_multiplier = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_MIN_MULTIPLIER",
        calm_.service_rate_min_multiplier, 0.001, 1000.0);
    calm_.service_rate_initial_multiplier = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_INITIAL_MULTIPLIER",
        calm_.service_rate_initial_multiplier,
        calm_.service_rate_min_multiplier, 1000.0);
    calm_.service_rate_max_multiplier = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_MAX_MULTIPLIER",
        calm_.service_rate_max_multiplier,
        calm_.service_rate_initial_multiplier, 1000.0);
    calm_.service_rate_ai_multiplier = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_AI_MULTIPLIER",
        calm_.service_rate_ai_multiplier, 0.0001, 1000.0);
    calm_.service_rate_alpha_up = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_ALPHA_UP",
        calm_.service_rate_alpha_up, 0.001, 1.0);
    calm_.service_rate_alpha_down = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_ALPHA_DOWN",
        calm_.service_rate_alpha_down, 0.001, 1.0);
    calm_.service_rate_failure_gamma = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_FAILURE_GAMMA",
        calm_.service_rate_failure_gamma, 0.01, 1.0);
    calm_.service_rate_ack_cap_multiplier = calm_env_double(
        "FASTDDS_CALM_SERVICE_RATE_ACK_CAP_MULTIPLIER",
        calm_.service_rate_ack_cap_multiplier, 1.0, 1000.0);

    double reference_rate_mbps = calm_.service_rate_mbps;
    if (calm_.control_state == CalmControlState::NORMAL)
    {
        // With cumulative ACK progress and no CALM backlog, the Writer is
        // application-limited: the observed generation rate is a DDS-visible
        // lower bound on service capacity, not an estimate of the physical
        // link maximum.  Keeping that lower bound prevents an early, sparse
        // ACK sample from permanently throttling a healthy flow.
        reference_rate_mbps = std::max(
            reference_rate_mbps,
            std::max(calm_.acked_rate_mbps, calm_.offered_rate_ewma_mbps));
    }
    else if (reference_rate_mbps <= 0.0)
    {
        reference_rate_mbps = std::max(
            calm_.acked_rate_mbps, calm_.offered_rate_ewma_mbps);
    }

    const double feedback_horizon_ms = std::max(
        1.0,
        std::max(calm_heartbeat_period_ms(), calm_.effective_feedback_guard_ms));
    const double liveness_rate_mbps =
            static_cast<double>(calm_minimum_transport_atom_bytes()) * 8.0 /
            (feedback_horizon_ms * 1000.0);
    reference_rate_mbps = std::max(liveness_rate_mbps, reference_rate_mbps);
    if (calm_.service_rate_mbps <= 0.0 ||
            (calm_.control_state == CalmControlState::NORMAL &&
            reference_rate_mbps > calm_.service_rate_mbps))
    {
        calm_.service_rate_mbps = reference_rate_mbps;
    }

    const double probe_reference_mbps =
            calm_.control_state == CalmControlState::ACTIVE ?
            std::max(reference_rate_mbps, calm_.active_rate_reference_mbps) :
            reference_rate_mbps;
    const double initial_reference_mbps =
            calm_.control_state == CalmControlState::ACTIVE &&
            calm_.active_rate_reference_mbps > 0.0 ?
            calm_.active_rate_reference_mbps : reference_rate_mbps;

    calm_.min_repair_rate_mbps = std::max(
        liveness_rate_mbps,
        reference_rate_mbps * calm_.service_rate_min_multiplier);
    calm_.initial_repair_rate_mbps = std::max(
        calm_.min_repair_rate_mbps,
        initial_reference_mbps * calm_.service_rate_initial_multiplier);
    calm_.max_repair_rate_mbps = std::max(
        calm_.initial_repair_rate_mbps,
        probe_reference_mbps * calm_.service_rate_max_multiplier);
    calm_.dynamic_rate_ai_mbps = std::max(
        liveness_rate_mbps,
        probe_reference_mbps * calm_.service_rate_ai_multiplier);
    calm_.storm_rate_increase_mbps = calm_.dynamic_rate_ai_mbps;
}

uint64_t ReaderProxy::calm_detector_rho_threshold_bytes() const
{
    const uint64_t fixed_threshold = calm_env_uint64(
        "FASTDDS_CALM_DETECTOR_RHO_BYTES",
        calm_env_uint64(
            "FASTDDS_CALM_DETECTOR_MIN_RHO_BYTES",
            calm_.detector_min_rho_bytes, 1),
        1);
    const double sample_multiplier = calm_env_double(
        "FASTDDS_CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER", 2.0, 0.0, 1000000.0);
    const uint64_t average_sample_bytes = calm_average_sample_bytes();
    if (sample_multiplier <= 0.0 || average_sample_bytes == 0)
    {
        return fixed_threshold;
    }

    const long double threshold = std::ceil(
        static_cast<long double>(average_sample_bytes) * sample_multiplier);
    return threshold >= static_cast<long double>((std::numeric_limits<uint64_t>::max)()) ?
           (std::numeric_limits<uint64_t>::max)() :
           std::max<uint64_t>(1, static_cast<uint64_t>(threshold));
}

uint64_t ReaderProxy::calm_requested_bytes() const
{
    uint64_t requested_bytes = 0;

    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (REQUESTED == change.getStatus())
        {
            requested_bytes += calm_requested_change_bytes(change);
        }
    }

    return requested_bytes;
}

uint32_t ReaderProxy::calm_requested_size() const
{
    uint32_t requested = 0;

    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (REQUESTED == change.getStatus())
        {
            ++requested;
        }
    }

    return requested;
}

uint64_t ReaderProxy::calm_scheduled_repair_bytes() const
{
    uint64_t scheduled_bytes = 0;

    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_repair_pending() && UNSENT == change.getStatus())
        {
            scheduled_bytes += calm_requested_change_bytes(change);
        }
    }

    return scheduled_bytes;
}

uint64_t ReaderProxy::calm_total_nack_count() const
{
    uint64_t count = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        count += change.calm_nack_count();
    }
    return count;
}

uint32_t ReaderProxy::calm_active_repair_count() const
{
    uint32_t count = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_repair_pending())
        {
            ++count;
        }
    }
    return count;
}

uint32_t ReaderProxy::calm_max_retransmit_count() const
{
    uint32_t maximum = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_repair_pending())
        {
            maximum = std::max(maximum, change.calm_retransmit_count());
        }
    }
    return maximum;
}

uint32_t ReaderProxy::calm_oldest_retransmit_count() const
{
    const ChangeForReader_t* oldest = nullptr;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_repair_pending() &&
                (oldest == nullptr || change.getSequenceNumber() < oldest->getSequenceNumber()))
        {
            oldest = &change;
        }
    }
    return oldest == nullptr ? 0 : oldest->calm_retransmit_count();
}

uint32_t ReaderProxy::calm_max_failed_repair_count() const
{
    uint32_t maximum = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_repair_pending())
        {
            maximum = std::max(maximum, change.calm_failed_repair_count());
        }
    }
    return maximum;
}

uint32_t ReaderProxy::calm_oldest_failed_repair_count() const
{
    const ChangeForReader_t* oldest = nullptr;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_repair_pending() &&
                (oldest == nullptr || change.getSequenceNumber() < oldest->getSequenceNumber()))
        {
            oldest = &change;
        }
    }
    return oldest == nullptr ? 0 : oldest->calm_failed_repair_count();
}

uint32_t ReaderProxy::calm_held_new_size() const
{
    uint32_t count = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_new_held())
        {
            ++count;
        }
    }
    return count;
}

uint64_t ReaderProxy::calm_held_new_bytes() const
{
    uint64_t bytes = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_new_held() && change.getChange() != nullptr)
        {
            bytes += change.getChange()->serializedPayload.length;
        }
    }
    return bytes;
}

uint64_t ReaderProxy::calm_queued_new_bytes() const
{
    uint64_t bytes = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_new_queued() && change.getChange() != nullptr)
        {
            bytes += change.getChange()->serializedPayload.length;
        }
    }
    return bytes;
}

uint64_t ReaderProxy::calm_update_budget()
{
    if (!calm_controls_repair())
    {
        calm_.budget_bytes = 0;
        return 0;
    }

    calm_.max_batch_bytes = calm_env_uint64(
        "FASTDDS_CALM_MAX_BUDGET_BYTES", calm_.max_batch_bytes, 1);
    calm_.initial_budget_bytes = calm_env_uint64(
        "FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES",
        calm_.initial_budget_bytes, 1, calm_.max_batch_bytes);
    calm_.minimum_budget_bytes = calm_env_uint64(
        "FASTDDS_CALM_STORM_MIN_BUDGET_BYTES",
        calm_.minimum_budget_bytes, 1, calm_.initial_budget_bytes);
    calm_.additive_increase_bytes = calm_env_uint64(
        "FASTDDS_CALM_STORM_AI_BYTES", calm_.additive_increase_bytes, 1);
    calm_.storm_decrease_gamma = calm_env_double(
        "FASTDDS_CALM_STORM_GAMMA", calm_.storm_decrease_gamma, 0.01, 1.0);
    calm_.storm_rate_decrease_gamma = calm_env_double(
        "FASTDDS_CALM_STORM_RATE_GAMMA",
        calm_.storm_rate_decrease_gamma, 0.01, 1.0);
    calm_.storm_rate_increase_mbps = calm_env_double(
        "FASTDDS_CALM_STORM_RATE_AI_MBPS",
        calm_.storm_rate_increase_mbps, 0.0, 10000.0);
    calm_.feedback_min_age_ms = calm_env_double(
        "FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS",
        calm_.feedback_min_age_ms, 0.0, 60000.0);
    calm_.pacing_floor_ms = calm_env_double(
        "FASTDDS_CALM_PACING_MS", calm_.pacing_floor_ms, 0.1, 10000.0);
    calm_.pacing_drain_guard = calm_env_double(
        "FASTDDS_CALM_PACING_DRAIN_GUARD",
        calm_.pacing_drain_guard, 1.0, 10.0);
    calm_.budget_horizon_ms = calm_env_double(
        "FASTDDS_CALM_BUDGET_HORIZON_MS", calm_.budget_horizon_ms, 0.1, 10000.0);
    calm_.max_scheduled_bytes = calm_env_uint64(
        "FASTDDS_CALM_MAX_SCHEDULED_BYTES", calm_.max_scheduled_bytes, 1);
    calm_.max_scheduled_bytes = std::max(
        calm_.max_scheduled_bytes, calm_.max_batch_bytes);
    calm_.retry_cooldown_ms = calm_env_double(
        "FASTDDS_CALM_RETRY_COOLDOWN_MS", calm_.retry_cooldown_ms, 0.0, 10000.0);

    // The byte settings are stable safety floors. Sample-relative terms let
    // the window grow for larger topics without sacrificing small-topic
    // behavior established by the fixed-parameter experiments.
    calm_refresh_budget_parameters();
    if (calm4_family_controller())
    {
        calm_.pacing_floor_ms = calm_env_double(
            "FASTDDS_CALM_PACING_MS", calm_.pacing_floor_ms, 0.1, 10000.0);
        if (!calm_.entry_ack_pacing_applied)
        {
            calm_.last_pacing_period_ms = calm_.pacing_floor_ms;
        }
        calm_.congestion_window_bytes = std::max(
            calm_.minimum_budget_bytes,
            std::min(calm_.congestion_window_bytes, calm_.max_batch_bytes));
        calm_.budget_bytes = std::min(
            calm_requested_bytes(), calm_.congestion_window_bytes);
        ++calm_.control_epoch;
        return calm_.budget_bytes;
    }

    calm_refresh_rate_parameters();

    if (0 == std::strcmp(calm_controller(), "storm_aimd"))
    {
        if (calm_.congestion_window_bytes == 0 || calm_.control_epoch == 0)
        {
            calm_.congestion_window_bytes = calm_.initial_budget_bytes;
            calm_.repair_rate_mbps = std::max(
                calm_.min_repair_rate_mbps,
                std::min(
                    calm_.max_repair_rate_mbps,
                    static_cast<double>(calm_.congestion_window_bytes) * 8.0 /
                    (calm_.budget_horizon_ms * 1000.0)));
        }
        calm_.congestion_window_bytes = std::max(
            calm_.minimum_budget_bytes,
            std::min(calm_.congestion_window_bytes, calm_.max_batch_bytes));

        const uint64_t rho = calm_backlog_size();
        const uint64_t requested_bytes = calm_requested_bytes();
        const uint64_t scheduled_bytes = calm_scheduled_repair_bytes();
        const uint64_t held_new_bytes = calm_held_new_bytes();
        const uint64_t previous_rho = calm_.rho_prev;
        const double delta = static_cast<double>(rho) - static_cast<double>(previous_rho);
        const uint64_t nack_count = calm_total_nack_count();
        const uint64_t nack_delta = nack_count >= calm_.nack_count_prev ?
                nack_count - calm_.nack_count_prev : nack_count;

        calm_.budget_bytes = requested_bytes == 0 ? 0 :
                std::min(requested_bytes, calm_.congestion_window_bytes);
        calm_.budget_ratio = static_cast<double>(calm_.congestion_window_bytes) /
                static_cast<double>(std::max<uint64_t>(1, calm_.max_batch_bytes));
        calm_.repair_rate_mbps = std::max(
            calm_.min_repair_rate_mbps,
            std::min(calm_.repair_rate_mbps, calm_.max_repair_rate_mbps));
        calm_.last_pacing_period_ms = calm_batch_interval_ms(
            calm_.budget_bytes,
            calm_.repair_rate_mbps,
            calm_.pacing_floor_ms,
            calm_.observed_batch_drain_ms,
            calm_.pacing_drain_guard);
        calm_.rho_prev = rho;
        calm_.nack_count_prev = nack_count;
        ++calm_.control_epoch;

        if (calm_metrics_enabled())
        {
            std::lock_guard<std::mutex> guard(calm_metrics_mutex());
            calm_open_metrics_locked();
            const uint64_t timestamp = calm_now_ns();
            const std::string reader = calm_reader_id(guid());
            calm_backlog_csv() << timestamp << ',' << reader << ','
                               << rho << ',' << requested_bytes << ',' << scheduled_bytes << ','
                               << calm_requested_size() << ',' << held_new_bytes << ','
                               << calm_queued_new_bytes() << ',' << delta << ','
                               << calm_.path_rate_mbps << ',' << calm_.acked_rate_mbps << ','
                               << calm_control_state_name() << ',' << calm_.failed_feedback_rounds << ','
                               << (calm_.repair_round_armed ? 1 : 0) << ','
                               << calm_.congestion_window_bytes << '\n';

            calm_budget_csv() << timestamp << ',' << reader << ','
                              << calm_.control_epoch << ",control,"
                              << calm_.budget_bytes << ",0,"
                              << requested_bytes << ',' << rho << ',' << delta << ','
                              << "active_budget," << calm_.budget_ratio << ','
                              << previous_rho << ',' << calm_.storm_decrease_gamma << ','
                              << calm_.additive_increase_bytes << ',' << calm_.minimum_budget_bytes << ','
                              << static_cast<uint64_t>(calm_.last_pacing_period_ms * 1000.0) << ','
                              << scheduled_bytes << ',' << held_new_bytes << ',' << nack_delta << ','
                              << calm_.repair_rate_mbps << ",storm_aimd,"
                              << calm_queued_new_bytes() << ',' << calm_.path_rate_mbps << ','
                              << calm_.acked_rate_mbps << ',' << calm_control_state_name() << ','
                              << calm_.failed_feedback_rounds << ',' << calm_.repair_round << ','
                              << (calm_.repair_round_armed ? 1 : 0) << ','
                              << (calm_.release_credit_available ? 1 : 0) << ','
                              << calm_.feedback_rtt_ewma_ms << ','
                              << calm_.effective_ack_stall_ms << ','
                              << calm_.effective_feedback_guard_ms << ','
                              << calm_heartbeat_period_ms() << ','
                              << calm_average_sample_bytes() << ','
                              << calm_.offered_rate_ewma_mbps << ','
                              << calm_.active_rate_reference_mbps << ','
                              << calm_.delivery_rate_sample_mbps << ','
                              << calm_.transport_rate_mbps << ','
                              << calm_.service_rate_mbps << ','
                              << (calm_.last_delivery_sample_app_limited ? 1 : 0) << ','
                              << calm_.min_repair_rate_mbps << ','
                              << calm_.initial_repair_rate_mbps << ','
                              << calm_.max_repair_rate_mbps << ','
                              << calm_.dynamic_rate_ai_mbps << ','
                              << calm_.transport_bytes_total << ','
                              << calm_.acknowledged_bytes_total << ','
                              << calm_.service_success_rounds << ','
                              << calm_.service_failure_rounds << ','
                              << calm_.last_service_window_ms << ','
                              << calm_.last_service_window_acknowledged_bytes << ','
                              << calm_.last_service_window_recovered_repair_bytes << '\n';
            calm_flush_metrics_periodically_locked();
        }

        return calm_.budget_bytes;
    }

    calm_.decrease_gamma = calm_env_double("FASTDDS_CALM_GAMMA", calm_.decrease_gamma, 0.0, 1.0);
    calm_.increase_alpha = calm_env_double("FASTDDS_CALM_ALPHA", calm_.increase_alpha, 0.0, 1000.0);
    calm_.budget_ratio_floor = calm_env_double("FASTDDS_CALM_Q_MIN", calm_.budget_ratio_floor, 0.0, 1.0);
    calm_.max_repair_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MAX_REPAIR_RATE_MBPS", calm_.max_repair_rate_mbps, 1.0, 100000.0);
    calm_.min_repair_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MIN_REPAIR_RATE_MBPS",
        calm_.min_repair_rate_mbps,
        1.0,
        calm_.max_repair_rate_mbps);
    calm_.initial_repair_rate_mbps = calm_env_double(
        "FASTDDS_CALM_INITIAL_REPAIR_RATE_MBPS",
        calm_.initial_repair_rate_mbps,
        calm_.min_repair_rate_mbps,
        calm_.max_repair_rate_mbps);
    calm_.max_path_rate_mbps = calm_env_double(
        "FASTDDS_CALM_HELD_NEW_RATE_MBPS", calm_.max_path_rate_mbps, 1.0, 100000.0);
    calm_.max_path_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MAX_PATH_RATE_MBPS", calm_.max_path_rate_mbps, 1.0, 100000.0);
    calm_.min_path_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MIN_PATH_RATE_MBPS",
        calm_.min_path_rate_mbps,
        1.0,
        calm_.max_path_rate_mbps);
    calm_.path_decrease_gamma = calm_env_double(
        "FASTDDS_CALM_PATH_GAMMA", calm_.path_decrease_gamma, 0.05, 1.0);
    calm_.path_increase_alpha = calm_env_double(
        "FASTDDS_CALM_PATH_ALPHA", calm_.path_increase_alpha, 0.0, 10000.0);
    calm_.pacing_floor_ms = calm_env_double(
        "FASTDDS_CALM_PACING_MS", calm_.pacing_floor_ms, 0.1, 10000.0);
    calm_.budget_horizon_ms = calm_env_double(
        "FASTDDS_CALM_BUDGET_HORIZON_MS", calm_.budget_horizon_ms, 0.1, 10000.0);
    calm_.max_batch_bytes = calm_env_uint64(
        "FASTDDS_CALM_MAX_BUDGET_BYTES", calm_.max_batch_bytes, 1);
    calm_.max_scheduled_bytes = calm_env_uint64(
        "FASTDDS_CALM_MAX_SCHEDULED_BYTES", calm_.max_scheduled_bytes, 1);
    calm_.delta_threshold_bytes = calm_env_uint64(
        "FASTDDS_CALM_DELTA_THRESHOLD_BYTES", calm_.delta_threshold_bytes, 0);
    calm_.onset_threshold_bytes = calm_env_uint64(
        "FASTDDS_CALM_ONSET_THRESHOLD_BYTES", calm_.onset_threshold_bytes, 1);
    calm_.post_repair_guard_ms = calm_env_double(
        "FASTDDS_CALM_POST_REPAIR_GUARD_MS", calm_.post_repair_guard_ms, 0.0, 60000.0);
    calm_.recovery_probe_delay_ms = calm_env_double(
        "FASTDDS_CALM_RECOVERY_PROBE_DELAY_MS", calm_.recovery_probe_delay_ms, 0.0, 10000.0);
    if (calm_.control_epoch == 0)
    {
        calm_.repair_rate_mbps = calm_.initial_repair_rate_mbps;
    }
    calm_.repair_rate_mbps = std::max(
        calm_.min_repair_rate_mbps,
        std::min(calm_.repair_rate_mbps, calm_.max_repair_rate_mbps));
    calm_.path_rate_mbps = std::max(
        calm_.min_path_rate_mbps,
        std::min(calm_.path_rate_mbps, calm_.max_path_rate_mbps));

    const uint64_t rho = calm_backlog_size();
    const uint64_t requested_bytes = calm_requested_bytes();
    const uint64_t previous_rho = calm_.rho_prev;
    const double delta = static_cast<double>(rho) - static_cast<double>(previous_rho);
    const uint64_t nack_count = calm_total_nack_count();
    const uint64_t nack_delta = nack_count >= calm_.nack_count_prev ?
            nack_count - calm_.nack_count_prev : nack_count;
    const uint64_t scheduled_bytes = calm_scheduled_repair_bytes();
    const uint64_t held_new_bytes = calm_held_new_bytes();
    const double normalization = static_cast<double>(std::max<uint64_t>(1, calm_.max_batch_bytes));
    const double normalized_delta = delta / normalization;
    const double positive_delta = std::max(0.0, normalized_delta);
    const bool backlog_grew = delta > static_cast<double>(calm_.delta_threshold_bytes);
    const bool backlog_shrank = -delta > static_cast<double>(calm_.delta_threshold_bytes);
    const bool severe_onset = previous_rho == 0 && rho >= calm_.onset_threshold_bytes;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed_seconds = calm_.last_control_time == std::chrono::steady_clock::time_point::min() ?
            0.0 :
            std::min(1.0, std::chrono::duration<double>(now - calm_.last_control_time).count());
    calm_.last_control_time = now;
    if (backlog_grew)
    {
        calm_.last_growth_time = now;
    }
    const bool path_congestion = severe_onset || (previous_rho > 0 && backlog_grew);
    const bool path_decrease_ready =
            calm_.last_path_decrease_time == std::chrono::steady_clock::time_point::min() ||
            std::chrono::duration<double, std::milli>(
        now - calm_.last_path_decrease_time).count() >= 50.0;
    if (path_congestion && path_decrease_ready)
    {
        calm_.path_rate_mbps = std::max(
            calm_.min_path_rate_mbps,
            calm_.path_rate_mbps * calm_.path_decrease_gamma);
        calm_.last_path_decrease_time = now;
    }
    const bool growth_is_quiet =
            calm_.last_growth_time == std::chrono::steady_clock::time_point::min() ||
            std::chrono::duration<double, std::milli>(
        now - calm_.last_growth_time).count() >= calm_.recovery_probe_delay_ms;
    const double nominal_horizon_seconds = calm_.budget_horizon_ms / 1000.0;
    const double control_step_ratio = std::max(
        0.25, elapsed_seconds / std::max(0.001, nominal_horizon_seconds));
    const double previous_positive_delta = std::max(
        0.0, calm_.previous_normalized_delta);
    const double positive_acceleration = std::max(
        0.0, (positive_delta - previous_positive_delta) / control_step_ratio);
    const char* controller = calm_controller();
    const char* action = "hold";

    if (0 == std::strcmp(controller, "pi") || 0 == std::strcmp(controller, "pid"))
    {
        const bool is_pid = 0 == std::strcmp(controller, "pid");
        const double kp = calm_env_double(
            "FASTDDS_CALM_KP", is_pid ? 0.45 : 0.30, 0.0, 1000.0);
        const double ki = calm_env_double(
            "FASTDDS_CALM_KI", is_pid ? 0.03 : 0.08, 0.0, 1000.0);
        const double kd = is_pid ?
                calm_env_double("FASTDDS_CALM_KD", 0.25, 0.0, 1000.0) : 0.0;
        if (severe_onset)
        {
            calm_.repair_rate_mbps *= calm_.decrease_gamma;
            action = kd > 0.0 ? "pid_onset_decrease" : "pi_onset_decrease";
        }
        else if (backlog_grew && previous_rho > 0)
        {
            calm_.integral_error = std::min(
                100.0, calm_.integral_error + positive_delta * control_step_ratio);
            const double commanded_rate = calm_.max_repair_rate_mbps /
                    (1.0 + kp * positive_delta + ki * calm_.integral_error +
                    kd * positive_acceleration);
            calm_.repair_rate_mbps = std::min(calm_.repair_rate_mbps, commanded_rate);
            action = kd > 0.0 ? "pid_decrease" : "pi_decrease";
        }
        else if (backlog_shrank)
        {
            calm_.integral_error *= 0.5;
            calm_.repair_rate_mbps += calm_.increase_alpha * elapsed_seconds;
            action = kd > 0.0 ? "pid_increase" : "pi_increase";
        }
        else if (rho > 0 && previous_rho == 0)
        {
            action = kd > 0.0 ? "pid_onset" : "pi_onset";
        }
        else
        {
            calm_.integral_error *= std::pow(0.95, control_step_ratio);
            if (rho > 0 && growth_is_quiet)
            {
                calm_.repair_rate_mbps += calm_.increase_alpha * elapsed_seconds;
                action = kd > 0.0 ? "pid_probe_increase" : "pi_probe_increase";
            }
            else
            {
                action = kd > 0.0 ? "pid_hold" : "pi_hold";
            }
        }
    }
    else if (0 == std::strcmp(controller, "pd"))
    {
        const double kp = calm_env_double("FASTDDS_CALM_KP", 0.70, 0.0, 1000.0);
        const double kd = calm_env_double("FASTDDS_CALM_KD", 1.50, 0.0, 1000.0);
        if (severe_onset)
        {
            calm_.repair_rate_mbps *= calm_.decrease_gamma;
            action = "pd_onset_decrease";
        }
        else if (backlog_grew && previous_rho > 0)
        {
            const double commanded_rate = calm_.max_repair_rate_mbps /
                    (1.0 + kp * positive_delta + kd * positive_acceleration);
            calm_.repair_rate_mbps = std::min(calm_.repair_rate_mbps, commanded_rate);
            action = "pd_decrease";
        }
        else if (backlog_shrank)
        {
            calm_.repair_rate_mbps += calm_.increase_alpha * elapsed_seconds;
            action = "pd_increase";
        }
        else if (rho > 0 && previous_rho == 0)
        {
            action = "pd_onset";
        }
        else
        {
            if (rho > 0 && growth_is_quiet)
            {
                calm_.repair_rate_mbps += calm_.increase_alpha * elapsed_seconds;
                action = "pd_probe_increase";
            }
            else
            {
                action = "pd_hold";
            }
        }
    }
    else
    {
        if (severe_onset)
        {
            calm_.repair_rate_mbps *= calm_.decrease_gamma;
            action = "onset_decrease";
        }
        else if (backlog_grew && previous_rho > 0)
        {
            calm_.repair_rate_mbps *= calm_.decrease_gamma;
            action = "multiplicative_decrease";
        }
        else if (backlog_shrank)
        {
            calm_.repair_rate_mbps += calm_.increase_alpha * elapsed_seconds;
            action = "additive_increase";
        }
        else if (rho > 0 && previous_rho == 0)
        {
            action = "onset";
        }
        else if (rho > 0 && growth_is_quiet)
        {
            calm_.repair_rate_mbps += calm_.increase_alpha * elapsed_seconds;
            action = "probe_increase";
        }
    }

    calm_.repair_rate_mbps = std::max(
        calm_.min_repair_rate_mbps,
        std::min(calm_.repair_rate_mbps, calm_.max_repair_rate_mbps));
    const double effective_repair_rate = std::min(
        calm_.repair_rate_mbps, calm_.path_rate_mbps);
    calm_.budget_ratio = effective_repair_rate / calm_.max_repair_rate_mbps;

    if (requested_bytes == 0)
    {
        calm_.budget_bytes = 0;
        action = "idle";
    }
    else
    {
        const uint64_t rate_budget = static_cast<uint64_t>(std::ceil(
                    effective_repair_rate * calm_.budget_horizon_ms * 125.0));
        calm_.budget_bytes = std::min(
            requested_bytes,
            std::min(calm_.max_batch_bytes, std::max<uint64_t>(1, rate_budget)));
    }

    calm_.rho_prev = rho;
    calm_.nack_count_prev = nack_count;
    calm_.previous_normalized_delta = normalized_delta;
    ++calm_.control_epoch;

    if (calm_metrics_enabled())
    {
        std::lock_guard<std::mutex> guard(calm_metrics_mutex());
        calm_open_metrics_locked();

        const uint64_t timestamp = calm_now_ns();
        const std::string reader = calm_reader_id(guid());
        calm_backlog_csv() << timestamp << ',' << reader << ','
                           << rho << ',' << requested_bytes << ',' << scheduled_bytes << ','
                           << calm_requested_size() << ',' << held_new_bytes << ','
                           << calm_queued_new_bytes() << ',' << delta << ','
                           << calm_.path_rate_mbps << ',' << calm_.acked_rate_mbps << ','
                           << calm_control_state_name() << ',' << calm_.failed_feedback_rounds << ','
                           << (calm_.repair_round_armed ? 1 : 0) << ','
                           << calm_.congestion_window_bytes << '\n';

        calm_budget_csv() << timestamp << ',' << reader << ','
                          << calm_.control_epoch << ",control,"
                          << calm_.budget_bytes << ",0,"
                          << requested_bytes << ',' << rho << ',' << delta << ','
                          << action << ',' << calm_.budget_ratio << ','
                          << previous_rho << ',' << calm_.decrease_gamma << ','
                          << calm_.increase_alpha << ',' << calm_.budget_ratio_floor << ','
                          << static_cast<uint64_t>(calm_.last_pacing_period_ms * 1000.0) << ','
                          << scheduled_bytes << ',' << held_new_bytes << ',' << nack_delta << ','
                          << calm_.repair_rate_mbps << ',' << controller << ','
                          << calm_queued_new_bytes() << ',' << calm_.path_rate_mbps << ','
                          << calm_.acked_rate_mbps << ',' << calm_control_state_name() << ','
                          << calm_.failed_feedback_rounds << ',' << calm_.repair_round << ','
                          << (calm_.repair_round_armed ? 1 : 0) << ','
                          << (calm_.release_credit_available ? 1 : 0) << ','
                          << calm_.feedback_rtt_ewma_ms << ','
                          << calm_.effective_ack_stall_ms << ','
                          << calm_.effective_feedback_guard_ms << ','
                          << calm_heartbeat_period_ms() << ','
                          << calm_average_sample_bytes() << ','
                          << calm_.offered_rate_ewma_mbps << ','
                          << calm_.active_rate_reference_mbps << ','
                          << calm_.delivery_rate_sample_mbps << ','
                          << calm_.transport_rate_mbps << ','
                          << calm_.service_rate_mbps << ','
                          << (calm_.last_delivery_sample_app_limited ? 1 : 0) << ','
                          << calm_.min_repair_rate_mbps << ','
                          << calm_.initial_repair_rate_mbps << ','
                          << calm_.max_repair_rate_mbps << ','
                          << calm_.dynamic_rate_ai_mbps << ','
                          << calm_.transport_bytes_total << ','
                          << calm_.acknowledged_bytes_total << ','
                          << calm_.service_success_rounds << ','
                          << calm_.service_failure_rounds << ','
                          << calm_.last_service_window_ms << ','
                          << calm_.last_service_window_acknowledged_bytes << ','
                          << calm_.last_service_window_recovered_repair_bytes << '\n';
        calm_flush_metrics_periodically_locked();
    }

    return calm_.budget_bytes;
}

uint32_t ReaderProxy::perform_acknack_response_limited(
        uint64_t budget_bytes,
        const std::function<void(ChangeForReader_t& change)>& func,
        uint64_t& released_bytes)
{
    uint32_t changed = 0;
    released_bytes = 0;
    const auto now = std::chrono::steady_clock::now();

    for (ChangeForReader_t& change : changes_for_reader_)
    {
        if (REQUESTED == change.getStatus())
        {
            // Preserve oldest-first repair ordering while waiting for receiver feedback.
            if (!change.calm_retry_ready(now, calm_.retry_cooldown_ms))
            {
                break;
            }

            if (released_bytes >= budget_bytes)
            {
                break;
            }

            const uint64_t change_bytes = change.calm_limit_unsent_fragments(
                budget_bytes - released_bytes);
            if (change_bytes == 0)
            {
                // A repeated NACK may leave a REQUESTED change whose fragment
                // window was already consumed by an in-flight repair.  It has
                // nothing to enqueue and must not starve newer valid requests.
                change.setStatus(UNACKNOWLEDGED);
                continue;
            }

            ++changed;
            released_bytes += change_bytes;
            change.setStatus(UNSENT);

            if (func)
            {
                func(change);
            }

            // Continue in sequence order while budget remains. This keeps
            // oldest-first repair without degenerating into stop-and-wait.
        }
    }

    return changed;
}

uint64_t ReaderProxy::calm_budget_bytes() const
{
    return calm_.budget_bytes;
}

uint64_t ReaderProxy::calm_total_release_budget_bytes() const
{
    if (!calm_controls_repair())
    {
        return calm_.max_batch_bytes;
    }

    return std::max(
        calm_.minimum_budget_bytes,
        std::min(calm_.congestion_window_bytes, calm_.max_batch_bytes));
}

uint64_t ReaderProxy::calm_max_batch_bytes() const
{
    return calm_.max_batch_bytes;
}

double ReaderProxy::calm_pacing_period_ms() const
{
    return calm_.last_pacing_period_ms;
}

double ReaderProxy::calm_release_rate_mbps() const
{
    return calm_.repair_rate_mbps;
}

double ReaderProxy::calm_service_rate_mbps() const
{
    return calm_.service_rate_mbps;
}

bool ReaderProxy::calm_enabled() const
{
    return calm_env_enabled();
}

bool ReaderProxy::calm_controls_repair() const
{
    return calm_enabled() && calm_.control_state == CalmControlState::ACTIVE;
}

bool ReaderProxy::calm_tracking_enabled() const
{
    return calm_enabled() || calm_observer_enabled() || storm_observer_enabled();
}

void ReaderProxy::calm_enter_active()
{
    calm_.max_batch_bytes = calm_env_uint64(
        "FASTDDS_CALM_MAX_BUDGET_BYTES", calm_.max_batch_bytes, 1);
    calm_.initial_budget_bytes = calm_env_uint64(
        "FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES",
        calm_.initial_budget_bytes, 1, calm_.max_batch_bytes);
    calm_.minimum_budget_bytes = calm_env_uint64(
        "FASTDDS_CALM_STORM_MIN_BUDGET_BYTES",
        calm_.minimum_budget_bytes, 1, calm_.initial_budget_bytes);
    calm_refresh_budget_parameters();
    if (calm4_family_controller())
    {
        calm_.pacing_floor_ms = calm_env_double(
            "FASTDDS_CALM_PACING_MS", calm_.pacing_floor_ms, 0.1, 10000.0);
        calm_.entry_pacing_eta = calm_env_double(
            "FASTDDS_CALM41_PACING_ETA", calm_.entry_pacing_eta, 1.0, 10.0);
        calm_.entry_ack_pacing_enabled = calm41_controller() &&
                calm41_entry_ack_pacing_enabled();
        calm_.calm4_decrease_gain = calm_env_double(
            "FASTDDS_CALM4_K_DEC", calm_.calm4_decrease_gain, 0.0, 1.0);
        calm_.calm4_increase_gain = calm_env_double(
            "FASTDDS_CALM4_K_INC", calm_.calm4_increase_gain, 0.0, 1000.0);
        calm_.congestion_window_bytes = std::max(
            calm_.minimum_budget_bytes,
            std::min(calm_.initial_budget_bytes, calm_.max_batch_bytes));
        calm_.budget_bytes = calm_.congestion_window_bytes;
        calm_.entry_service_rate_mbps = calm_.entry_ack_pacing_enabled ?
                calm_.acked_rate_mbps : 0.0;
        calm_.entry_ack_pacing_applied =
                calm_.entry_ack_pacing_enabled && calm_.entry_service_rate_mbps > 0.0;
        calm_.active_rate_reference_mbps = calm_.entry_service_rate_mbps;
        calm_.last_pacing_period_ms = calm41_entry_pacing_period_ms(
            calm_.congestion_window_bytes,
            calm_.entry_service_rate_mbps,
            calm_.entry_pacing_eta,
            calm_.pacing_floor_ms);
        calm_.control_state = CalmControlState::ACTIVE;
        calm_.failed_feedback_rounds = 0;
        calm_.release_credit_available = calm_requested_size() > 0;
        calm_.next_release_time = std::chrono::steady_clock::time_point::min();
        calm_.repair_not_before = std::chrono::steady_clock::time_point::min();
        calm_.pacing_active = true;
        calm_hold_unsent_new_changes();
        return;
    }

    calm_refresh_rate_parameters();
    calm_.active_rate_reference_mbps = calm_service_rate_enabled() ?
            calm_.service_rate_mbps : calm_.offered_rate_ewma_mbps;
    calm_refresh_rate_parameters();
    calm_.storm_decrease_gamma = calm_env_double(
        "FASTDDS_CALM_STORM_GAMMA", calm_.storm_decrease_gamma, 0.01, 1.0);
    calm_.storm_rate_decrease_gamma = calm_env_double(
        "FASTDDS_CALM_STORM_RATE_GAMMA",
        calm_.storm_rate_decrease_gamma, 0.01, 1.0);
    calm_.pacing_floor_ms = calm_env_double(
        "FASTDDS_CALM_PACING_MS", calm_.pacing_floor_ms, 0.1, 10000.0);
    calm_.pacing_drain_guard = calm_env_double(
        "FASTDDS_CALM_PACING_DRAIN_GUARD",
        calm_.pacing_drain_guard, 1.0, 10.0);
    calm_.budget_horizon_ms = calm_env_double(
        "FASTDDS_CALM_BUDGET_HORIZON_MS", calm_.budget_horizon_ms, 0.1, 10000.0);
    calm_.congestion_window_bytes = calm_.initial_budget_bytes;
    calm_.budget_bytes = calm_.congestion_window_bytes;
    const bool configured_initial_rate =
            std::getenv("FASTDDS_CALM_INITIAL_REPAIR_RATE_MBPS") != nullptr ||
            calm_.initial_rate_offered_multiplier > 0.0;
    calm_.repair_rate_mbps = calm_service_rate_enabled() ?
            calm_.initial_repair_rate_mbps :
            (configured_initial_rate ?
            calm_.initial_repair_rate_mbps :
            std::max(
                calm_.min_repair_rate_mbps,
                std::min(
                    calm_.max_repair_rate_mbps,
                    static_cast<double>(calm_.congestion_window_bytes) * 8.0 /
                    (calm_.budget_horizon_ms * 1000.0))));
    calm_.last_pacing_period_ms = calm_batch_interval_ms(
        calm_.congestion_window_bytes,
        calm_.repair_rate_mbps,
        calm_.pacing_floor_ms,
        calm_.observed_batch_drain_ms,
        calm_.pacing_drain_guard);
    calm_.control_state = CalmControlState::ACTIVE;
    calm_.failed_feedback_rounds = 0;
    calm_.repair_round_armed = false;
    calm_.next_release_time = std::chrono::steady_clock::time_point::min();
    calm_.batch_release_time = std::chrono::steady_clock::time_point::min();
    calm_.repair_not_before = std::chrono::steady_clock::time_point::min();
    calm_.release_credit_available = calm_requested_size() > 0;
    calm_.ack_increase_credit_bytes = 0;
    calm_.last_release_was_repair = false;
    calm_.pacing_active = true;
    calm_hold_unsent_new_changes();
}

void ReaderProxy::calm_evaluate_before_tnr()
{
    if (!calm_enabled() || calm_controls_repair() || calm_requested_size() == 0 ||
            calm4_family_controller())
    {
        return;
    }

    calm_.detector_min_rho_bytes = calm_detector_rho_threshold_bytes();
    calm_.detector_soft_retry_count = static_cast<uint32_t>(calm_env_uint64(
                "FASTDDS_CALM_DETECTOR_RETRY_COUNT",
                calm_env_uint64(
                    "FASTDDS_CALM_DETECTOR_SOFT_RETRY_COUNT",
                    calm_.detector_soft_retry_count, 1,
                    (std::numeric_limits<uint32_t>::max)()),
                1,
                (std::numeric_limits<uint32_t>::max)()));
    calm_.detector_failed_repair_count = static_cast<uint32_t>(calm_env_uint64(
                "FASTDDS_CALM_DETECTOR_FAILED_REPAIR_COUNT",
                calm_.detector_failed_repair_count, 1,
                (std::numeric_limits<uint32_t>::max)()));
    calm_.detector_growth_rounds = static_cast<uint32_t>(calm_env_uint64(
                "FASTDDS_CALM_DETECTOR_GROWTH_ROUNDS",
                calm_.detector_growth_rounds, 1, (std::numeric_limits<uint32_t>::max)()));
    const double ack_stall_threshold_ms = calm_ack_stall_threshold_ms();

    const uint64_t rho = calm_backlog_size();
    if (rho < calm_.detector_min_rho_bytes)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    double ack_progress_age_ms = 0.0;
    if (calm_.last_ack_time == std::chrono::steady_clock::time_point::min())
    {
        for (const ChangeForReader_t& change : changes_for_reader_)
        {
            ack_progress_age_ms = std::max(
                ack_progress_age_ms, change.calm_repair_age_ms(now));
        }
    }
    else
    {
        ack_progress_age_ms = std::chrono::duration<double, std::milli>(
            now - calm_.last_ack_time).count();
    }

    const uint32_t oldest_retry_count = calm_oldest_retransmit_count();
    const uint32_t oldest_failed_repair_count = calm_oldest_failed_repair_count();
    const bool nonshrinking =
            calm_.detector_nonshrinking_rounds >= calm_.detector_growth_rounds;
    const bool ack_progress_stalled = ack_progress_age_ms >= ack_stall_threshold_ms;

    // Re-evaluate the selected N_i or F_i activation signal immediately before
    // T_NR releases the requested burst.
    if (calm_detector_attempt_pressure(
                oldest_retry_count, oldest_failed_repair_count) &&
            (nonshrinking || ack_progress_stalled))
    {
        calm_enter_active();
        storm_log_snapshot("calm_active_tnr_stall", rho);
    }
}

void ReaderProxy::calm_observe_default_release(
        uint64_t released_bytes)
{
    if (!calm_tracking_enabled())
    {
        return;
    }

    if (released_bytes > 0)
    {
        ++calm_.observer_repair_release_events_total;
        calm_.observer_repair_release_bytes_total += released_bytes;
    }
    calm_log_observer_snapshot("default_release", released_bytes);
}

uint64_t ReaderProxy::calm_change_requested_bytes(
        const ChangeForReader_t& change) const
{
    return calm_requested_change_bytes(change);
}

void ReaderProxy::storm_observe_tnr_release(
        uint64_t requested_bytes,
        uint32_t requested_changes,
        uint64_t accepted_bytes,
        uint32_t accepted_changes,
        uint64_t rejected_bytes,
        uint32_t rejected_changes)
{
    if (!storm_observer_enabled())
    {
        return;
    }

    ++calm_.storm_tnr_release_events_total;
    calm_.storm_tnr_requested_changes_total += requested_changes;
    calm_.storm_tnr_requested_bytes_total += requested_bytes;
    calm_.storm_flow_enqueue_accepted_changes_total += accepted_changes;
    calm_.storm_flow_enqueue_accepted_bytes_total += accepted_bytes;
    calm_.storm_flow_enqueue_rejected_changes_total += rejected_changes;
    calm_.storm_flow_enqueue_rejected_bytes_total += rejected_bytes;
    storm_log_snapshot(
        rejected_changes > 0 ? "tnr_release_enqueue_rejected" : "tnr_release",
        requested_bytes);
}

void ReaderProxy::storm_observe_transport_repair(
        const SequenceNumber_t& seq_num,
        FragmentNumber_t fragment)
{
    if (!calm_tracking_enabled())
    {
        return;
    }

    ChangeIterator change = find_change(seq_num, true);
    if (change == changes_for_reader_.end())
    {
        return;
    }

    if (calm_enabled() && calm_service_rate_enabled())
    {
        const uint64_t transmitted_bytes = change->calm_fragment_payload_size(fragment);
        const uint64_t total_room =
                (std::numeric_limits<uint64_t>::max)() - calm_.transport_bytes_total;
        calm_.transport_bytes_total += std::min(transmitted_bytes, total_room);
        const uint64_t interval_room =
                (std::numeric_limits<uint64_t>::max)() - calm_.transport_bytes_since_ack;
        calm_.transport_bytes_since_ack += std::min(transmitted_bytes, interval_room);
        calm_.transport_interval_backlogged =
                calm_.transport_interval_backlogged ||
                (calm_controls_repair() &&
                (calm_backlog_size() > 0 || calm_held_new_size() > 0));
    }

    if (!change->calm_repair_pending())
    {
        return;
    }

    if (calm_enabled() && calm4_family_controller())
    {
        calm_note_repair_attempt();
        const uint64_t transmitted_bytes = change->calm_fragment_payload_size(fragment);
        const uint64_t room = (std::numeric_limits<uint64_t>::max)() -
                calm_.feedback_round_released_bytes;
        calm_.feedback_round_released_bytes += std::min(transmitted_bytes, room);
    }
    change->calm_mark_repair_transmitted(fragment);

    if (storm_observer_enabled())
    {
        ++calm_.storm_transport_repair_attempt_events_total;
        calm_.storm_transport_repair_attempt_bytes_total +=
                change->calm_fragment_payload_size(fragment);
    }
}

bool ReaderProxy::calm_can_release_repair() const
{
    if (!calm_controls_repair() || !calm_.release_credit_available ||
            calm_requested_size() == 0 ||
            std::chrono::steady_clock::now() < calm_.next_release_time ||
            std::chrono::steady_clock::now() < calm_.repair_not_before)
    {
        return false;
    }

    // Do not enqueue a new pacing batch while a previous repair or held-new
    // batch is still pending in the DDS send path. This prevents adjacent
    // pacing ticks from merging into one FlowController burst.
    if (calm_scheduled_repair_bytes() != 0 || calm_queued_new_bytes() != 0)
    {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (REQUESTED == change.getStatus())
        {
            return change.calm_retry_ready(now, calm_.retry_cooldown_ms);
        }
    }

    return false;
}

bool ReaderProxy::calm_can_release_held_new(
        bool alongside_repair) const
{
    const auto now = std::chrono::steady_clock::now();
    const bool fills_older_gap = calm_oldest_held_new_precedes_repair();
    const bool repair_drained = calm_requested_size() == 0 &&
            calm_scheduled_repair_bytes() == 0 &&
            calm_backlog_size() == 0;
    const bool paced_new_slot = calm_controls_repair() &&
            calm_requested_size() == 0;
    return calm_enabled() &&
           (fills_older_gap || repair_drained || paced_new_slot) &&
           (alongside_repair || calm_scheduled_repair_bytes() == 0) &&
           calm_held_new_size() > 0 &&
           calm_queued_new_bytes() == 0 &&
           (alongside_repair || now >= calm_.next_release_time) &&
           (alongside_repair || fills_older_gap || now >= calm_.post_repair_guard_until);
}

bool ReaderProxy::calm_has_pacing_work() const
{
    return calm_enabled() &&
           ((calm_controls_repair() &&
           ((calm_.release_credit_available && calm_requested_size() > 0) ||
           calm_scheduled_repair_bytes() > 0)) ||
           calm_held_new_size() > 0 ||
           calm_queued_new_bytes() > 0);
}

void ReaderProxy::calm_note_release(
        uint64_t released_bytes,
        bool repair)
{
    calm_note_shared_release(released_bytes, repair ? released_bytes : 0);
}

void ReaderProxy::calm_note_shared_release(
        uint64_t released_bytes,
        uint64_t repair_bytes)
{
    if (released_bytes == 0)
    {
        return;
    }

    if (repair_bytes > 0 && calm_tracking_enabled())
    {
        ++calm_.observer_repair_release_events_total;
        calm_.observer_repair_release_bytes_total += repair_bytes;
        calm_log_observer_snapshot("calm_release", repair_bytes);
    }
    if (repair_bytes > 0)
    {
        calm_.release_credit_available = false;
        calm_.repair_not_before = std::chrono::steady_clock::time_point::min();
        calm_.batch_release_time = std::chrono::steady_clock::now();
    }
    calm_.last_release_was_repair = repair_bytes > 0;
    if (calm4_family_controller())
    {
        if (!calm_.entry_ack_pacing_applied)
        {
            calm_.last_pacing_period_ms = calm_env_double(
                "FASTDDS_CALM_PACING_MS", calm_.pacing_floor_ms, 0.1, 10000.0);
        }
        const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(calm_.last_pacing_period_ms));
        calm_.next_release_time = std::chrono::steady_clock::now() + interval;
        return;
    }

    const bool storm_aimd = 0 == std::strcmp(calm_controller(), "storm_aimd");
    const bool shared_calm_batch = calm_controls_repair() || calm_held_new_size() > 0;
    const double pacing_rate_mbps = storm_aimd && shared_calm_batch ?
            calm_.repair_rate_mbps :
            (repair_bytes > 0 ?
            std::min(calm_.repair_rate_mbps, calm_.path_rate_mbps) :
            calm_.path_rate_mbps);
    calm_.last_pacing_period_ms = calm_batch_interval_ms(
        released_bytes,
        pacing_rate_mbps,
        calm_.pacing_floor_ms,
        calm_.observed_batch_drain_ms,
        calm_.pacing_drain_guard);
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(calm_.last_pacing_period_ms));
    calm_.next_release_time = std::chrono::steady_clock::now() + interval;
}

const char* ReaderProxy::calm_control_state_name() const
{
    switch (calm_.control_state)
    {
        case CalmControlState::ACTIVE:
            return "CALM_ACTIVE";
        case CalmControlState::NORMAL:
        default:
            return "NORMAL";
    }
}

bool ReaderProxy::calm_detector_attempt_pressure(
        uint32_t oldest_retransmit_count,
        uint32_t oldest_failed_repair_count) const
{
    const char* signal = std::getenv("FASTDDS_CALM_DETECTOR_RETRY_SIGNAL");
    if (signal == nullptr ||
            (0 == std::strcmp(signal, "rho") ||
            0 == std::strcmp(signal, "rho_progress")))
    {
        return true;
    }
    if (0 == std::strcmp(signal, "f") ||
            0 == std::strcmp(signal, "failed_feedback"))
    {
        return oldest_failed_repair_count >= calm_.detector_failed_repair_count;
    }
    return oldest_retransmit_count >= calm_.detector_soft_retry_count;
}

double ReaderProxy::calm_heartbeat_period_ms() const
{
    return writer_ == nullptr ?
           0.0 :
           TimeConv::Time_t2MilliSecondsDouble(writer_->m_times.heartbeatPeriod);
}

void ReaderProxy::calm_note_feedback_rtt(
        const std::chrono::steady_clock::time_point& now)
{
    if (!calm_.repair_round_armed ||
            calm_.repair_round_start_time == std::chrono::steady_clock::time_point::min())
    {
        return;
    }

    const double sample_ms = std::chrono::duration<double, std::milli>(
        now - calm_.repair_round_start_time).count();
    if (sample_ms <= 0.0 || sample_ms > 60000.0)
    {
        return;
    }

    calm_.feedback_rtt_alpha = calm_env_double(
        "FASTDDS_CALM_FEEDBACK_RTT_ALPHA",
        calm_.feedback_rtt_alpha, 0.01, 1.0);
    calm_.feedback_rtt_ewma_ms = calm_.feedback_rtt_ewma_ms == 0.0 ?
            sample_ms :
            (1.0 - calm_.feedback_rtt_alpha) * calm_.feedback_rtt_ewma_ms +
            calm_.feedback_rtt_alpha * sample_ms;
}

double ReaderProxy::calm_ack_stall_threshold_ms()
{
    calm_.detector_ack_stall_ms = calm_env_double(
        "FASTDDS_CALM_DETECTOR_ACK_STALL_MS",
        calm_.detector_ack_stall_ms, 0.0, 60000.0);
    calm_.effective_ack_stall_ms = calm_.detector_ack_stall_ms;

    const char* mode = std::getenv("FASTDDS_CALM_ACK_STALL_MODE");
    if (mode == nullptr || 0 != std::strcmp(mode, "dynamic") ||
            calm_.feedback_rtt_ewma_ms <= 0.0)
    {
        return calm_.effective_ack_stall_ms;
    }

    calm_.ack_stall_rtt_multiplier = calm_env_double(
        "FASTDDS_CALM_ACK_STALL_RTT_MULTIPLIER",
        calm_.ack_stall_rtt_multiplier, 0.0, 100.0);
    calm_.ack_stall_hb_multiplier = calm_env_double(
        "FASTDDS_CALM_ACK_STALL_HB_MULTIPLIER",
        calm_.ack_stall_hb_multiplier, 0.0, 100.0);
    calm_.ack_stall_min_ms = calm_env_double(
        "FASTDDS_CALM_ACK_STALL_MIN_MS",
        calm_.ack_stall_min_ms, 0.0, 60000.0);
    calm_.ack_stall_max_ms = calm_env_double(
        "FASTDDS_CALM_ACK_STALL_MAX_MS",
        calm_.ack_stall_max_ms, calm_.ack_stall_min_ms, 60000.0);

    const double rtt_term =
            calm_.ack_stall_rtt_multiplier * calm_.feedback_rtt_ewma_ms;
    const double heartbeat_term =
            calm_.ack_stall_hb_multiplier * calm_heartbeat_period_ms();
    calm_.effective_ack_stall_ms = std::max(
        calm_.ack_stall_min_ms,
        std::min(calm_.ack_stall_max_ms, std::max(rtt_term, heartbeat_term)));
    return calm_.effective_ack_stall_ms;
}

double ReaderProxy::calm_feedback_guard_ms()
{
    calm_.feedback_min_age_ms = calm_env_double(
        "FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS",
        calm_.feedback_min_age_ms, 0.0, 60000.0);
    calm_.effective_feedback_guard_ms = calm_.feedback_min_age_ms;

    const char* mode = std::getenv("FASTDDS_CALM_FEEDBACK_GUARD_MODE");
    if (mode == nullptr || 0 != std::strcmp(mode, "dynamic") ||
            calm_.feedback_rtt_ewma_ms <= 0.0)
    {
        return calm_.effective_feedback_guard_ms;
    }

    calm_.feedback_guard_rtt_multiplier = calm_env_double(
        "FASTDDS_CALM_FEEDBACK_GUARD_RTT_MULTIPLIER",
        calm_.feedback_guard_rtt_multiplier, 0.0, 100.0);
    calm_.feedback_guard_min_ms = calm_env_double(
        "FASTDDS_CALM_FEEDBACK_GUARD_MIN_MS",
        calm_.feedback_guard_min_ms, 0.0, 60000.0);
    calm_.feedback_guard_max_ms = calm_env_double(
        "FASTDDS_CALM_FEEDBACK_GUARD_MAX_MS",
        calm_.feedback_guard_max_ms, calm_.feedback_guard_min_ms, 60000.0);

    const double rtt_term =
            calm_.feedback_guard_rtt_multiplier * calm_.feedback_rtt_ewma_ms;
    calm_.effective_feedback_guard_ms = std::max(
        calm_.feedback_guard_min_ms,
        std::min(
            calm_.feedback_guard_max_ms,
            std::max(rtt_term, calm_heartbeat_period_ms())));
    return calm_.effective_feedback_guard_ms;
}

double ReaderProxy::calm41_feedback_timeout_ms()
{
    calm_.calm41_timeout_rtt_multiplier = calm_env_double(
        "FASTDDS_CALM41_TIMEOUT_RTT_MULTIPLIER",
        calm_.calm41_timeout_rtt_multiplier, 0.1, 1000.0);
    const double feedback_reference_ms = calm_.feedback_rtt_ewma_ms > 0.0 ?
            calm_.feedback_rtt_ewma_ms : calm_heartbeat_period_ms();
    calm_.calm41_feedback_timeout_ms = calm_.calm41_timeout_rtt_multiplier *
            std::max(0.001, feedback_reference_ms);
    return calm_.calm41_feedback_timeout_ms;
}

void ReaderProxy::calm_note_repair_attempt()
{
    if (!calm_enabled())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!calm_.repair_round_armed)
    {
        ++calm_.repair_round;
        calm_.repair_round_armed = true;
        calm_.repair_round_start_time = now;
        calm_.last_repair_attempt_time = calm_.repair_round_start_time;
        calm_.feedback_round_start_u_bytes = calm_backlog_size();
        calm_.feedback_round_released_bytes = 0;
        calm_.feedback_round_start_oldest_failed_count =
                calm_oldest_failed_repair_count();
    }
    // One T_NR response can release several Changes. Measure the feedback age
    // from the first actual repair in that response so congestion feedback for
    // an older Change is not hidden by a newer Change leaving the same batch.

    if (calm_.batch_release_time != std::chrono::steady_clock::time_point::min() &&
            calm_scheduled_repair_bytes() == 0)
    {
        const double sample_ms = std::chrono::duration<double, std::milli>(
            now - calm_.batch_release_time).count();
        calm_.observed_batch_drain_ms = calm_.observed_batch_drain_ms == 0.0 ?
                sample_ms :
                0.8 * calm_.observed_batch_drain_ms + 0.2 * sample_ms;
        calm_.batch_release_time = std::chrono::steady_clock::time_point::min();
    }
}

void ReaderProxy::calm_observe_repeated_feedback(
        double failure_severity)
{
    if (!calm_enabled())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!calm_.repair_round_armed ||
            calm_.last_repair_attempt_time == std::chrono::steady_clock::time_point::min())
    {
        return;
    }

    const double feedback_guard_ms = calm_feedback_guard_ms();
    if (std::chrono::duration<double, std::milli>(
                now - calm_.last_repair_attempt_time).count() < feedback_guard_ms)
    {
        return;
    }

    // Only the first repeated feedback after an actual repair closes a round.
    // Additional NACK_FRAG submessages in the same response cannot count again.
    calm_note_feedback_rtt(now);
    calm_.repair_round_armed = false;
    ++calm_.failed_feedback_rounds;
    calm_.last_failure_severity = std::max(
        0.0, std::min(1.0, failure_severity));

    if (calm4_family_controller())
    {
        calm_refresh_budget_parameters();
        calm_.calm4_decrease_gain = calm_env_double(
            "FASTDDS_CALM4_K_DEC", calm_.calm4_decrease_gain, 0.0, 1.0);
        calm_.calm4_increase_gain = calm_env_double(
            "FASTDDS_CALM4_K_INC", calm_.calm4_increase_gain, 0.0, 1000.0);

        const uint64_t current_u = calm_backlog_size();
        const uint32_t current_oldest_failed = calm_oldest_failed_repair_count();
        const int64_t delta_u = current_u >= calm_.feedback_round_start_u_bytes ?
                static_cast<int64_t>(current_u - calm_.feedback_round_start_u_bytes) :
                -static_cast<int64_t>(calm_.feedback_round_start_u_bytes - current_u);
        const uint32_t delta_oldest_failed =
                current_oldest_failed >= calm_.feedback_round_start_oldest_failed_count ?
                current_oldest_failed - calm_.feedback_round_start_oldest_failed_count : 0;
        const double ack_progress_age_ms = calm_.last_ack_time ==
                std::chrono::steady_clock::time_point::min() ?
                std::chrono::duration<double, std::milli>(
                    now - calm_.repair_round_start_time).count() :
                std::chrono::duration<double, std::milli>(
                    now - calm_.last_ack_time).count();
        bool activated_calm41 = false;
        if (calm41_controller() && !calm_controls_repair() && current_oldest_failed >= 1)
        {
            calm_enter_active();
            activated_calm41 = true;
        }
        const double feedback_timeout_ms = calm41_controller() ?
                calm41_feedback_timeout_ms() : calm_ack_stall_threshold_ms();
        const bool stalled = ack_progress_age_ms >= feedback_timeout_ms &&
                (calm41_controller() || calm_.last_failure_severity >= 1.0);
        const bool decrease = delta_oldest_failed >= 1 &&
                (delta_u >= 0 || stalled);

        calm_.last_feedback_delta_u_bytes = delta_u;
        calm_.last_feedback_delta_oldest_failed = delta_oldest_failed;
        calm_.last_feedback_failure_fraction = calm_.last_failure_severity;
        calm_.last_feedback_progress_fraction = 1.0 - calm_.last_failure_severity;

        if (decrease)
        {
            if (!calm_controls_repair())
            {
                calm_enter_active();
            }
            const double factor = std::max(
                0.0, 1.0 - calm_.calm4_decrease_gain * calm_.last_failure_severity);
            calm_.congestion_window_bytes = std::max(
                calm_.minimum_budget_bytes,
                static_cast<uint64_t>(std::floor(
                    static_cast<double>(calm_.congestion_window_bytes) * factor)));
            calm_.budget_bytes = std::min(
                calm_requested_bytes(), calm_.congestion_window_bytes);
            calm_.ack_increase_credit_bytes = 0;
            storm_log_snapshot(
                calm41_controller() ? "calm41_decrease" : "calm4_decrease",
                calm_.congestion_window_bytes);
        }
        else if (activated_calm41)
        {
            storm_log_snapshot("calm41_activate", current_u);
        }
        else
        {
            storm_log_snapshot(
                calm41_controller() ? "calm41_hold_feedback" : "calm4_hold_feedback",
                current_u);
        }

        calm_.failed_feedback_rounds = 0;
        calm_.repair_round_armed = false;
        calm_.feedback_round_start_u_bytes = current_u;
        calm_.feedback_round_released_bytes = 0;
        calm_.feedback_round_start_oldest_failed_count = current_oldest_failed;
        return;
    }

    calm_.detector_min_rho_bytes = calm_detector_rho_threshold_bytes();
    calm_.detector_soft_retry_count = static_cast<uint32_t>(calm_env_uint64(
                "FASTDDS_CALM_DETECTOR_RETRY_COUNT",
                calm_env_uint64(
                    "FASTDDS_CALM_DETECTOR_SOFT_RETRY_COUNT",
                    calm_.detector_soft_retry_count, 1,
                    (std::numeric_limits<uint32_t>::max)()),
                1,
                (std::numeric_limits<uint32_t>::max)()));
    calm_.detector_failed_repair_count = static_cast<uint32_t>(calm_env_uint64(
                "FASTDDS_CALM_DETECTOR_FAILED_REPAIR_COUNT",
                calm_.detector_failed_repair_count, 1,
                (std::numeric_limits<uint32_t>::max)()));
    calm_.detector_growth_rounds = static_cast<uint32_t>(calm_env_uint64(
                "FASTDDS_CALM_DETECTOR_GROWTH_ROUNDS",
                calm_.detector_growth_rounds, 1, (std::numeric_limits<uint32_t>::max)()));
    const double ack_stall_threshold_ms = calm_ack_stall_threshold_ms();

    const uint64_t rho = calm_backlog_size();
    const uint32_t oldest_retry_count = calm_oldest_retransmit_count();
    const uint32_t oldest_failed_repair_count = calm_oldest_failed_repair_count();
    double ack_progress_age_ms = 0.0;
    if (calm_.last_ack_time == std::chrono::steady_clock::time_point::min())
    {
        for (const ChangeForReader_t& change : changes_for_reader_)
        {
            ack_progress_age_ms = std::max(
                ack_progress_age_ms, change.calm_repair_age_ms(now));
        }
    }
    else
    {
        ack_progress_age_ms = std::chrono::duration<double, std::milli>(
            now - calm_.last_ack_time).count();
    }
    const bool ack_progress_stalled = ack_progress_age_ms >= ack_stall_threshold_ms;
    if (rho > 0 && rho >= calm_.detector_previous_rho_bytes)
    {
        ++calm_.detector_nonshrinking_rounds;
    }
    else
    {
        calm_.detector_nonshrinking_rounds = 0;
    }
    calm_.detector_previous_rho_bytes = rho;

    // The experiment can select actual repair rounds N_i or confirmed failed
    // repair feedback F_i. Both remain gated by debt and lack of progress.
    const bool failed_repair_pressure =
            rho >= calm_.detector_min_rho_bytes &&
            calm_detector_attempt_pressure(
                oldest_retry_count, oldest_failed_repair_count) &&
            (calm_.detector_nonshrinking_rounds >= calm_.detector_growth_rounds ||
            ack_progress_stalled);

    const char* transition_event = "calm_active_failed_round";
    if (calm_.control_state != CalmControlState::ACTIVE)
    {
        if (failed_repair_pressure)
        {
            calm_enter_active();
            transition_event = "calm_active_rho_retry";
        }
        else
        {
            storm_log_snapshot("calm_detector_observe", rho);
            return;
        }
    }
    else
    {
        calm_.failed_rounds_before_decrease = static_cast<uint32_t>(calm_env_uint64(
                    "FASTDDS_CALM_FAILED_ROUNDS_BEFORE_DECREASE",
                    calm_.failed_rounds_before_decrease, 1,
                    (std::numeric_limits<uint32_t>::max)()));
        if (calm_.failed_feedback_rounds < calm_.failed_rounds_before_decrease)
        {
            storm_log_snapshot("calm_active_wait_ack", rho);
            return;
        }
        calm_.storm_decrease_gamma = calm_env_double(
            "FASTDDS_CALM_STORM_GAMMA", calm_.storm_decrease_gamma, 0.01, 1.0);
        calm_.storm_rate_decrease_gamma = calm_env_double(
            "FASTDDS_CALM_STORM_RATE_GAMMA",
            calm_.storm_rate_decrease_gamma, 0.01, 1.0);
        calm_.first_failure_decrease_gamma = calm_env_double(
            "FASTDDS_CALM_FIRST_FAILURE_GAMMA",
            calm_.first_failure_decrease_gamma, 0.01, 1.0);
        calm_.first_failure_rate_decrease_gamma = calm_env_double(
            "FASTDDS_CALM_FIRST_FAILURE_RATE_GAMMA",
            calm_.first_failure_rate_decrease_gamma, 0.01, 1.0);
        calm_refresh_budget_parameters();
        calm_refresh_rate_parameters();
        const bool first_failure = oldest_failed_repair_count <= 1;
        const double selected_window_gamma = first_failure ?
                calm_.first_failure_decrease_gamma : calm_.storm_decrease_gamma;
        const double selected_rate_gamma = first_failure ?
                calm_.first_failure_rate_decrease_gamma : calm_.storm_rate_decrease_gamma;
        const double failure_severity = calm_env_flag_enabled(
            "FASTDDS_CALM_FEEDBACK_BYTE_PROGRESS", false) ?
                calm_.last_failure_severity : 1.0;
        const double effective_window_gamma =
                1.0 - failure_severity * (1.0 - selected_window_gamma);
        const double effective_selected_rate_gamma =
                1.0 - failure_severity * (1.0 - selected_rate_gamma);
        const double effective_service_gamma =
                1.0 - failure_severity * (1.0 - calm_.service_rate_failure_gamma);
        calm_.last_effective_window_gamma = effective_window_gamma;
        calm_.last_effective_rate_gamma = effective_selected_rate_gamma;
        const uint64_t previous_window = calm_.congestion_window_bytes;
        const double previous_rate = calm_.repair_rate_mbps;
        if (calm_service_rate_enabled() && calm_.service_rate_mbps > 0.0)
        {
            // v already receives the controller's selected multiplicative
            // decrease below.  mu_hat is a slower path-service estimate, so
            // applying the same decrease to both would count one failed
            // feedback round twice and collapse the dynamic lower bound.
            calm_.service_rate_mbps *= effective_service_gamma;
            ++calm_.service_failure_rounds;
            calm_refresh_service_rate_bounds();
        }
        calm_.congestion_window_bytes = std::max(
            calm_.minimum_budget_bytes,
            static_cast<uint64_t>(std::ceil(
                static_cast<double>(calm_.congestion_window_bytes) * effective_window_gamma)));
        calm_.ack_increase_credit_bytes = 0;
        if (calm_.congestion_window_bytes < previous_window)
        {
            const double window_ratio = static_cast<double>(calm_.congestion_window_bytes) /
                    static_cast<double>(previous_window);
            // gamma_B < gamma_R < 1 makes the batch interval shorter while the
            // permitted average release rate still decreases.
            const double minimum_rate_gamma = 0.5 * (1.0 + window_ratio);
            const double effective_rate_gamma = std::min(
                0.999,
                std::max(effective_selected_rate_gamma, minimum_rate_gamma));
            calm_.repair_rate_mbps = std::max(
                calm_.min_repair_rate_mbps,
                previous_rate * effective_rate_gamma);
        }
        calm_.budget_bytes = std::min(
            calm_requested_bytes(), calm_.congestion_window_bytes);
        calm_.last_pacing_period_ms = calm_batch_interval_ms(
            calm_.congestion_window_bytes,
            calm_.repair_rate_mbps,
            calm_.pacing_floor_ms,
            calm_.observed_batch_drain_ms,
            calm_.pacing_drain_guard);
        calm_.failed_feedback_rounds = 0;
        transition_event = first_failure ?
                "calm_active_first_failure_soft" :
                "calm_active_repeated_failure_hard";
    }

    storm_log_snapshot(transition_event, calm_.congestion_window_bytes);
}

void ReaderProxy::calm_note_request_ready()
{
    if (calm_controls_repair())
    {
        calm_.release_credit_available = true;
        calm_.pacing_active = true;
    }
}

bool ReaderProxy::calm_oldest_held_new_precedes_repair() const
{
    SequenceNumber_t oldest_held = SequenceNumber_t::unknown();
    SequenceNumber_t oldest_repair = SequenceNumber_t::unknown();
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_new_held() &&
                (oldest_held == SequenceNumber_t::unknown() ||
                change.getSequenceNumber() < oldest_held))
        {
            oldest_held = change.getSequenceNumber();
        }
        if (change.calm_repair_pending() &&
                (oldest_repair == SequenceNumber_t::unknown() ||
                change.getSequenceNumber() < oldest_repair))
        {
            oldest_repair = change.getSequenceNumber();
        }
    }

    return oldest_held != SequenceNumber_t::unknown() &&
           (oldest_repair == SequenceNumber_t::unknown() || oldest_held < oldest_repair);
}

uint64_t ReaderProxy::calm_active_admitted_bytes() const
{
    uint64_t bytes = 0;
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (!change.calm_new_held() && change.getStatus() != ACKNOWLEDGED &&
                change.getChange() != nullptr)
        {
            bytes += change.getChange()->serializedPayload.length;
        }
    }
    return bytes;
}

uint64_t ReaderProxy::calm_admission_window_bytes() const
{
    const uint64_t configured_max = calm_env_uint64(
        "FASTDDS_CALM_MAX_ADMISSION_WINDOW_BYTES",
        calm_.max_admission_window_bytes, 1);
    const uint64_t configured_min = calm_env_uint64(
        "FASTDDS_CALM_MIN_ADMISSION_WINDOW_BYTES",
        calm_.min_admission_window_bytes, 1, configured_max);
    const double ratio = static_cast<double>(calm_.congestion_window_bytes) /
            static_cast<double>(std::max<uint64_t>(1, calm_.max_batch_bytes));
    return std::max(
        configured_min,
        static_cast<uint64_t>(std::ceil(
            static_cast<double>(configured_max) *
            std::max(0.0, std::min(1.0, ratio)))));
}

bool ReaderProxy::calm_active_window_has_slot() const
{
    return calm_controls_repair() &&
           calm_active_admitted_bytes() < calm_admission_window_bytes();
}

void ReaderProxy::calm_reset_control_if_recovered()
{
    if (calm_backlog_size() != 0)
    {
        return;
    }

    const uint64_t previous_rho = calm_.rho_prev;
    if (previous_rho > 0 && calm_metrics_enabled())
    {
        std::lock_guard<std::mutex> guard(calm_metrics_mutex());
        calm_open_metrics_locked();
        const uint64_t timestamp = calm_now_ns();
        const std::string reader = calm_reader_id(guid());
        calm_backlog_csv() << timestamp << ',' << reader << ",0,0,"
                           << calm_scheduled_repair_bytes() << ",0,"
                           << calm_held_new_bytes() << ',' << calm_queued_new_bytes() << ','
                           << -static_cast<double>(previous_rho) << ','
                           << calm_.path_rate_mbps << ',' << calm_.acked_rate_mbps << ','
                           << "NORMAL,0,0," << calm_.initial_budget_bytes << '\n';

        calm_budget_csv() << timestamp << ',' << reader << ','
                          << calm_.control_epoch << ",recovered,0,0,0,0,"
                          << -static_cast<double>(previous_rho)
                          << ",recovered,"
                          << calm_.initial_repair_rate_mbps / calm_.max_repair_rate_mbps
                          << ',' << previous_rho << ','
                          << calm_.decrease_gamma << ',' << calm_.increase_alpha << ','
                          << calm_.budget_ratio_floor << ','
                          << static_cast<uint64_t>(calm_.last_pacing_period_ms * 1000.0) << ','
                          << calm_scheduled_repair_bytes() << ',' << calm_held_new_bytes()
                          << ",0," << calm_.initial_repair_rate_mbps << ','
                          << calm_controller() << ',' << calm_queued_new_bytes() << ','
                          << calm_.path_rate_mbps << ',' << calm_.acked_rate_mbps << ','
                          << "NORMAL,0," << calm_.repair_round << ",0,0,"
                          << calm_.feedback_rtt_ewma_ms << ','
                          << calm_.effective_ack_stall_ms << ','
                          << calm_.effective_feedback_guard_ms << ','
                          << calm_heartbeat_period_ms() << ','
                          << calm_average_sample_bytes() << ','
                          << calm_.offered_rate_ewma_mbps << ','
                          << calm_.active_rate_reference_mbps << ','
                          << calm_.delivery_rate_sample_mbps << ','
                          << calm_.transport_rate_mbps << ','
                          << calm_.service_rate_mbps << ','
                          << (calm_.last_delivery_sample_app_limited ? 1 : 0) << ','
                          << calm_.min_repair_rate_mbps << ','
                          << calm_.initial_repair_rate_mbps << ','
                          << calm_.max_repair_rate_mbps << ','
                          << calm_.dynamic_rate_ai_mbps << ','
                          << calm_.transport_bytes_total << ','
                          << calm_.acknowledged_bytes_total << ','
                          << calm_.service_success_rounds << ','
                          << calm_.service_failure_rounds << ','
                          << calm_.last_service_window_ms << ','
                          << calm_.last_service_window_acknowledged_bytes << ','
                          << calm_.last_service_window_recovered_repair_bytes << '\n';
        calm_flush_metrics_periodically_locked();
    }

    calm_.rho_prev = 0;
    calm_.nack_count_prev = calm_total_nack_count();
    calm_.integral_error = 0.0;
    calm_.previous_normalized_delta = 0.0;
    calm_.last_control_time = std::chrono::steady_clock::time_point::min();
    calm_.last_growth_time = std::chrono::steady_clock::time_point::min();
    calm_.active_rate_reference_mbps = 0.0;
    calm_.entry_service_rate_mbps = 0.0;
    calm_.entry_ack_pacing_applied = false;
    if (calm_service_rate_enabled())
    {
        calm_refresh_service_rate_bounds();
    }
    calm_.repair_rate_mbps = calm_.initial_repair_rate_mbps;
    calm_.budget_ratio = std::min(
        calm_.repair_rate_mbps, calm_.path_rate_mbps) / calm_.max_repair_rate_mbps;
    calm_.budget_bytes = 0;
    calm_.congestion_window_bytes = calm_.initial_budget_bytes;
    calm_.ack_increase_credit_bytes = 0;
    calm_.control_state = CalmControlState::NORMAL;
    calm_.failed_feedback_rounds = 0;
    calm_.repair_round_armed = false;
    calm_.feedback_round_start_u_bytes = 0;
    calm_.feedback_round_released_bytes = 0;
    calm_.feedback_round_start_oldest_failed_count = 0;
    calm_.last_feedback_delta_u_bytes = 0;
    calm_.last_feedback_delta_oldest_failed = 0;
    calm_.last_feedback_failure_fraction = 0.0;
    calm_.last_feedback_progress_fraction = 0.0;
    calm_.calm41_feedback_timeout_ms = 0.0;
    calm_.release_credit_available = false;
    calm_.last_repair_attempt_time = std::chrono::steady_clock::time_point::min();
    calm_.repair_round_start_time = std::chrono::steady_clock::time_point::min();
    calm_.batch_release_time = std::chrono::steady_clock::time_point::min();
    calm_.last_release_was_repair = false;
    calm_.detector_nonshrinking_rounds = 0;
    calm_.detector_previous_rho_bytes = 0;
}

bool ReaderProxy::calm_pacing_active() const
{
    return calm_.pacing_active;
}

void ReaderProxy::calm_pacing_active(
        bool active)
{
    calm_.pacing_active = active;
}

bool ReaderProxy::calm_should_hold_new() const
{
    if (!calm_controls_repair() || 0 == calm_hold_mode())
    {
        return false;
    }

    if (calm_backlog_size() > 0)
    {
        return true;
    }

    return calm_requested_size() > 0 ||
           calm_scheduled_repair_bytes() > 0 ||
           calm_held_new_size() > 0 ||
           std::chrono::steady_clock::now() < calm_.post_repair_guard_until;
}

void ReaderProxy::calm_note_nack()
{
    calm_.max_path_rate_mbps = calm_env_double(
        "FASTDDS_CALM_HELD_NEW_RATE_MBPS", calm_.max_path_rate_mbps, 1.0, 100000.0);
    calm_.max_path_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MAX_PATH_RATE_MBPS", calm_.max_path_rate_mbps, 1.0, 100000.0);
    calm_.min_path_rate_mbps = calm_env_double(
        "FASTDDS_CALM_MIN_PATH_RATE_MBPS",
        calm_.min_path_rate_mbps,
        1.0,
        calm_.max_path_rate_mbps);
    calm_.path_decrease_gamma = calm_env_double(
        "FASTDDS_CALM_PATH_GAMMA", calm_.path_decrease_gamma, 0.05, 1.0);
    calm_.path_increase_alpha = calm_env_double(
        "FASTDDS_CALM_PATH_ALPHA", calm_.path_increase_alpha, 0.0, 10000.0);
    calm_.first_repair_delay_ms = calm_env_double(
        "FASTDDS_CALM_FIRST_REPAIR_DELAY_MS", calm_.first_repair_delay_ms, 0.0, 10000.0);
    calm_.post_repair_guard_ms = calm_env_double(
        "FASTDDS_CALM_POST_REPAIR_GUARD_MS", calm_.post_repair_guard_ms, 0.0, 60000.0);
    const auto guard = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(calm_.post_repair_guard_ms));
    const auto now = std::chrono::steady_clock::now();
    calm_.post_repair_guard_until = now + guard;
    // The first-repair delay is a fixed deadline for this release credit.
    // Repeated NACKs must not slide it forward indefinitely under congestion.
    if (calm_.repair_not_before == std::chrono::steady_clock::time_point::min())
    {
        const auto repair_delay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(calm_.first_repair_delay_ms));
        calm_.repair_not_before = now + repair_delay;
    }
}

void ReaderProxy::calm_record_nack(
        uint64_t nack_bytes,
        uint64_t unique_repair_bytes,
        bool repeated,
        bool failed_repair_feedback,
        double failure_severity)
{
    if (!calm_tracking_enabled())
    {
        return;
    }

    ++calm_.observer_nack_events_total;
    calm_.observer_nack_bytes_total += nack_bytes;
    calm_.observer_unique_repair_bytes_total += unique_repair_bytes;
    if (repeated)
    {
        ++calm_.observer_repeated_nack_events_total;
        calm_.observer_repeated_nack_bytes_total += nack_bytes;
        if (failed_repair_feedback)
        {
            calm_observe_repeated_feedback(failure_severity);
        }
    }
}

void ReaderProxy::calm_log_observer_snapshot(
        const char* phase,
        uint64_t released_bytes) const
{
    if (!calm_observer_metrics_enabled())
    {
        return;
    }

    std::lock_guard<std::mutex> guard(calm_metrics_mutex());
    calm_open_metrics_locked();
    calm_observer_csv() << calm_now_ns() << ',' << calm_reader_id(guid()) << ','
                        << phase << ',' << calm_backlog_size() << ','
                        << calm_requested_bytes() << ',' << calm_requested_size() << ','
                        << calm_.observer_nack_events_total << ','
                        << calm_.observer_nack_bytes_total << ','
                        << calm_.observer_unique_repair_bytes_total << ','
                        << calm_.observer_repeated_nack_events_total << ','
                        << calm_.observer_repeated_nack_bytes_total << ','
                        << calm_.observer_repair_release_events_total << ','
                        << calm_.observer_repair_release_bytes_total << ','
                        << released_bytes << '\n';
    calm_flush_metrics_periodically_locked();
}

void ReaderProxy::storm_note_sample_retransmit(
        const SequenceNumber_t& seq_num)
{
    if (!storm_observer_enabled())
    {
        return;
    }

    ChangeConstIterator oldest = changes_for_reader_.end();
    for (ChangeConstIterator it = changes_for_reader_.begin();
            it != changes_for_reader_.end(); ++it)
    {
        if (it->calm_repair_pending() &&
                (oldest == changes_for_reader_.end() ||
                it->getSequenceNumber() < oldest->getSequenceNumber()))
        {
            oldest = it;
        }
    }

    if (oldest == changes_for_reader_.end() || oldest->getSequenceNumber() != seq_num)
    {
        return;
    }

    if (calm_.storm_oldest_no_progress_seq != seq_num)
    {
        calm_.storm_oldest_no_progress_seq = seq_num;
        calm_.storm_oldest_no_progress_rounds = 0;
    }
    ++calm_.storm_oldest_no_progress_rounds;
}

void ReaderProxy::storm_log_snapshot(
        const char* event,
        uint64_t event_bytes,
        const SequenceNumber_t& event_sample_seq) const
{
    if (!storm_observer_enabled())
    {
        return;
    }

    uint64_t repair_unsent_bytes = 0;
    uint64_t repair_underway_bytes = 0;
    uint64_t repair_unacknowledged_bytes = 0;
    double oldest_repair_age_ms = 0.0;
    double ack_progress_age_ms = 0.0;
    const ChangeForReader_t* oldest_repair = nullptr;
    const ChangeForReader_t* event_sample = nullptr;
    const auto now = std::chrono::steady_clock::now();
    for (const ChangeForReader_t& change : changes_for_reader_)
    {
        if (event_sample_seq != SequenceNumber_t::unknown() &&
                change.getSequenceNumber() == event_sample_seq)
        {
            event_sample = &change;
        }
        if (!change.calm_repair_pending())
        {
            continue;
        }

        if (oldest_repair == nullptr ||
                change.getSequenceNumber() < oldest_repair->getSequenceNumber())
        {
            oldest_repair = &change;
        }

        oldest_repair_age_ms = std::max(
            oldest_repair_age_ms, change.calm_repair_age_ms(now));
        switch (change.getStatus())
        {
            case UNSENT:
                repair_unsent_bytes += change.calm_repair_pending_bytes();
                break;
            case UNDERWAY:
                repair_underway_bytes += change.calm_repair_pending_bytes();
                break;
            case UNACKNOWLEDGED:
                repair_unacknowledged_bytes += change.calm_repair_pending_bytes();
                break;
            default:
                break;
        }
    }
    ack_progress_age_ms = calm_.last_ack_time == std::chrono::steady_clock::time_point::min() ?
            oldest_repair_age_ms :
            std::chrono::duration<double, std::milli>(now - calm_.last_ack_time).count();

    std::lock_guard<std::mutex> guard(calm_metrics_mutex());
    storm_open_metrics_locked();
    std::ostringstream row;
    row << calm_now_ns() << ',' << calm_reader_id(writer_->getGuid()) << ','
        << calm_reader_id(guid()) << ','
        << event << ',' << event_bytes << ',' << calm_backlog_size() << ','
        << oldest_repair_age_ms << ',' << calm_requested_bytes() << ','
        << calm_requested_size() << ',' << repair_unsent_bytes << ','
        << repair_underway_bytes << ',' << repair_unacknowledged_bytes << ','
        << calm_.observer_nack_events_total << ','
        << calm_.observer_nack_bytes_total << ','
        << calm_.observer_unique_repair_bytes_total << ','
        << calm_.observer_repeated_nack_events_total << ','
        << calm_.observer_repeated_nack_bytes_total << ','
        << calm_.storm_tnr_release_events_total << ','
        << calm_.storm_tnr_requested_changes_total << ','
        << calm_.storm_tnr_requested_bytes_total << ','
        << calm_.storm_flow_enqueue_accepted_changes_total << ','
        << calm_.storm_flow_enqueue_accepted_bytes_total << ','
        << calm_.storm_flow_enqueue_rejected_changes_total << ','
        << calm_.storm_flow_enqueue_rejected_bytes_total << ','
        << calm_.storm_transport_repair_attempt_events_total << ','
        << calm_.storm_transport_repair_attempt_bytes_total << ','
        << calm_.storm_ack_recovered_bytes_total << ','
        << calm_control_state_name() << ',' << calm_.failed_feedback_rounds << ','
        << calm_.repair_round << ',' << (calm_.repair_round_armed ? 1 : 0) << ','
        << calm_.congestion_window_bytes << ','
        << (calm_.release_credit_available ? 1 : 0) << ','
        << (event_sample_seq == SequenceNumber_t::unknown() ? 0 : event_sample_seq.to64long()) << ','
        << (event_sample == nullptr ? 0 : event_sample->calm_retransmit_count()) << ','
        << (oldest_repair == nullptr ? 0 : oldest_repair->getSequenceNumber().to64long()) << ','
        << (oldest_repair == nullptr ? 0 : oldest_repair->calm_retransmit_count()) << ','
        << calm_.storm_oldest_no_progress_rounds << ','
        << changes_low_mark_.to64long() << ','
        << calm_active_repair_count() << ','
        << calm_max_retransmit_count() << ','
        << calm_.detector_nonshrinking_rounds << ','
        << ack_progress_age_ms << ','
        << calm_active_admitted_bytes() << ','
        << calm_admission_window_bytes() << ','
        << (event_sample == nullptr ? 0 : event_sample->calm_failed_repair_count()) << ','
        << (oldest_repair == nullptr ? 0 : oldest_repair->calm_failed_repair_count()) << ','
        << calm_max_failed_repair_count() << ','
        << ((std::getenv("FASTDDS_CALM_DETECTOR_RETRY_SIGNAL") != nullptr) ?
        std::getenv("FASTDDS_CALM_DETECTOR_RETRY_SIGNAL") : "rho_progress") << ','
        << calm_.last_failure_severity << ','
        << calm_.last_effective_window_gamma << ','
        << calm_.last_effective_rate_gamma << ','
        << calm_.last_feedback_delta_u_bytes << ','
        << calm_.last_feedback_delta_oldest_failed << ','
        << calm_.last_feedback_failure_fraction << ','
        << calm_.last_feedback_progress_fraction << ','
        << calm_.pacing_floor_ms << ','
        << calm_.calm41_feedback_timeout_ms << '\n';
    const std::string complete_row = row.str();
    storm_observer_csv().write(complete_row.data(), complete_row.size());
    if (++storm_rows_since_flush() >= 64)
    {
        storm_observer_csv().flush();
        storm_rows_since_flush() = 0;
    }
}

bool ReaderProxy::calm_update_service_rate_on_ack(
        uint64_t acknowledged_bytes,
        uint64_t recovered_repair_bytes,
        const std::chrono::steady_clock::time_point& now)
{
    if (!calm_service_rate_enabled())
    {
        return false;
    }

    calm_refresh_service_rate_bounds();
    const uint64_t acknowledged_room =
            (std::numeric_limits<uint64_t>::max)() - calm_.acknowledged_bytes_total;
    calm_.acknowledged_bytes_total += std::min(acknowledged_bytes, acknowledged_room);
    const uint64_t window_ack_room =
            (std::numeric_limits<uint64_t>::max)() - calm_.service_window_acknowledged_bytes;
    calm_.service_window_acknowledged_bytes += std::min(acknowledged_bytes, window_ack_room);
    const uint64_t window_repair_room =
            (std::numeric_limits<uint64_t>::max)() - calm_.service_window_recovered_repair_bytes;
    calm_.service_window_recovered_repair_bytes += std::min(
        recovered_repair_bytes, window_repair_room);

    if (calm_.service_window_start_time == std::chrono::steady_clock::time_point::min())
    {
        calm_.service_window_start_time = now;
        return false;
    }

    const double elapsed_seconds = std::chrono::duration<double>(
        now - calm_.service_window_start_time).count();
    const double service_window_floor_ms = std::max(
        4.0 * calm_.pacing_floor_ms,
        std::max(
            2.0 * calm_heartbeat_period_ms(),
            4.0 * calm_.feedback_rtt_ewma_ms));
    if (elapsed_seconds * 1000.0 < std::max(1.0, service_window_floor_ms))
    {
        return false;
    }

    calm_.last_service_window_ms = elapsed_seconds * 1000.0;
    calm_.last_service_window_acknowledged_bytes =
            calm_.service_window_acknowledged_bytes;
    calm_.last_service_window_recovered_repair_bytes =
            calm_.service_window_recovered_repair_bytes;
    calm_.last_service_window_repair_progress =
            calm_.service_window_recovered_repair_bytes > 0;
    calm_.last_delivery_sample_app_limited = !calm_.transport_interval_backlogged;

    const double raw_ack_rate_mbps =
            static_cast<double>(calm_.service_window_acknowledged_bytes) * 8.0 /
            (elapsed_seconds * 1000000.0);
    calm_.transport_rate_mbps =
            static_cast<double>(calm_.transport_bytes_since_ack) * 8.0 /
            (elapsed_seconds * 1000000.0);
    // A transport enqueue burst is not link service: the DDS send path can
    // accept bytes much faster than the socket/NIC drains them.  Bound ACK
    // compression by the causal DDS release command instead.
    double observed_send_rate_mbps = calm_.offered_rate_ewma_mbps;
    if (calm_.control_state == CalmControlState::ACTIVE)
    {
        observed_send_rate_mbps = std::max(
            observed_send_rate_mbps, calm_.repair_rate_mbps);
    }
    if (observed_send_rate_mbps <= 0.0)
    {
        observed_send_rate_mbps = calm_.transport_rate_mbps;
    }
    const double ack_cap_mbps = observed_send_rate_mbps > 0.0 ?
            observed_send_rate_mbps * calm_.service_rate_ack_cap_multiplier :
            raw_ack_rate_mbps;
    calm_.delivery_rate_sample_mbps = std::min(raw_ack_rate_mbps, ack_cap_mbps);
    calm_.acked_rate_mbps = calm_.acked_rate_mbps == 0.0 ?
            calm_.delivery_rate_sample_mbps :
            0.8 * calm_.acked_rate_mbps + 0.2 * calm_.delivery_rate_sample_mbps;

    double observed_rate_mbps = calm_.delivery_rate_sample_mbps;
    if (observed_rate_mbps <= 0.0)
    {
        observed_rate_mbps = std::max(
            calm_.acked_rate_mbps, calm_.offered_rate_ewma_mbps);
    }
    if (calm_.service_rate_mbps <= 0.0)
    {
        calm_.service_rate_mbps = observed_rate_mbps;
    }
    else if (calm_.control_state == CalmControlState::NORMAL)
    {
        // NORMAL is usually application-limited. It establishes a safe lower
        // bound but cannot prove that the link capacity fell below the offered
        // rate, so only successful upward observations change the estimate.
        if (observed_rate_mbps > calm_.service_rate_mbps)
        {
            calm_.service_rate_mbps += calm_.service_rate_alpha_up *
                    (observed_rate_mbps - calm_.service_rate_mbps);
        }
    }
    else if (calm_.last_service_window_repair_progress && observed_rate_mbps > 0.0)
    {
        // A cumulative ACK that retires repair debt is a successful service
        // sample. Upward learning is deliberately faster than downward
        // learning because isolated ACK spacing can under-report goodput.
        const double alpha = observed_rate_mbps >= calm_.service_rate_mbps ?
                calm_.service_rate_alpha_up : calm_.service_rate_alpha_down;
        calm_.service_rate_mbps += alpha *
                (observed_rate_mbps - calm_.service_rate_mbps);
        ++calm_.service_success_rounds;
    }

    calm_.transport_bytes_since_ack = 0;
    calm_.transport_interval_backlogged = false;
    calm_.service_window_acknowledged_bytes = 0;
    calm_.service_window_recovered_repair_bytes = 0;
    calm_.service_window_start_time = now;
    calm_refresh_service_rate_bounds();
    return true;
}

void ReaderProxy::calm_note_ack(
        uint64_t acknowledged_bytes,
        uint64_t recovered_repair_bytes)
{
    if (!calm_enabled() || acknowledged_bytes == 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (recovered_repair_bytes > 0)
    {
        calm_note_feedback_rtt(now);
    }
    bool service_window_ready = false;
    if (calm_service_rate_enabled())
    {
        service_window_ready = calm_update_service_rate_on_ack(
            acknowledged_bytes, recovered_repair_bytes, now);
    }
    else
    {
        const auto rate_start = calm_.last_ack_time !=
                std::chrono::steady_clock::time_point::min() ?
                calm_.last_ack_time : calm_.first_new_change_time;
        const double elapsed_seconds = rate_start ==
                std::chrono::steady_clock::time_point::min() ? 0.0 :
                std::chrono::duration<double>(now - rate_start).count();
        if (elapsed_seconds >= 0.001)
        {
            const double sample_rate_mbps = std::min(
                calm_.max_path_rate_mbps * 2.0,
                static_cast<double>(acknowledged_bytes) * 8.0 /
                (elapsed_seconds * 1000000.0));
            calm_.acked_rate_mbps = calm_.acked_rate_mbps == 0.0 ?
                    sample_rate_mbps :
                    0.8 * calm_.acked_rate_mbps + 0.2 * sample_rate_mbps;
        }
    }

    if (calm4_family_controller() &&
            recovered_repair_bytes > 0 &&
            calm_.repair_round_armed)
    {
        calm_refresh_budget_parameters();
        calm_.calm4_increase_gain = calm_env_double(
            "FASTDDS_CALM4_K_INC", calm_.calm4_increase_gain, 0.0, 1000.0);
        const uint64_t current_u = calm_backlog_size();
        const int64_t delta_u = current_u >= calm_.feedback_round_start_u_bytes ?
                static_cast<int64_t>(current_u - calm_.feedback_round_start_u_bytes) :
                -static_cast<int64_t>(calm_.feedback_round_start_u_bytes - current_u);
        const uint64_t reference_bytes = std::max<uint64_t>(
            1, calm_.feedback_round_released_bytes);
        const double progress_fraction = std::min(
            1.0,
            static_cast<double>(recovered_repair_bytes) /
            static_cast<double>(reference_bytes));

        calm_.last_feedback_delta_u_bytes = delta_u;
        calm_.last_feedback_delta_oldest_failed = 0;
        calm_.last_feedback_failure_fraction = 1.0 - progress_fraction;
        calm_.last_feedback_progress_fraction = progress_fraction;
        if (calm_.control_state == CalmControlState::ACTIVE && delta_u < 0)
        {
            const uint64_t average_sample_bytes = std::max<uint64_t>(
                1, calm_average_sample_bytes());
            const long double increase = std::ceil(
                static_cast<long double>(calm_.calm4_increase_gain) *
                static_cast<long double>(average_sample_bytes) * progress_fraction);
            const uint64_t increase_bytes = increase >=
                    static_cast<long double>((std::numeric_limits<uint64_t>::max)()) ?
                    (std::numeric_limits<uint64_t>::max)() :
                    static_cast<uint64_t>(increase);
            calm_.congestion_window_bytes =
                    calm_.max_batch_bytes - calm_.congestion_window_bytes < increase_bytes ?
                    calm_.max_batch_bytes : calm_.congestion_window_bytes + increase_bytes;
            storm_log_snapshot(
                calm41_controller() ? "calm41_increase" : "calm4_increase",
                calm_.congestion_window_bytes);
        }
        else
        {
            storm_log_snapshot(
                calm41_controller() ? "calm41_hold_ack" : "calm4_hold_ack",
                current_u);
        }
        calm_.budget_bytes = std::min(
            calm_requested_bytes(), calm_.congestion_window_bytes);
        calm_.feedback_round_start_u_bytes = current_u;
        calm_.feedback_round_released_bytes = 0;
        calm_.feedback_round_start_oldest_failed_count =
                calm_oldest_failed_repair_count();
        calm_.repair_round_armed = false;
    }

    if (!calm4_family_controller() && calm_.control_state == CalmControlState::ACTIVE)
    {
        calm_refresh_budget_parameters();
        calm_refresh_rate_parameters();
        calm_.pacing_floor_ms = calm_env_double(
            "FASTDDS_CALM_PACING_MS", calm_.pacing_floor_ms, 0.1, 10000.0);
        calm_.pacing_drain_guard = calm_env_double(
            "FASTDDS_CALM_PACING_DRAIN_GUARD",
            calm_.pacing_drain_guard, 1.0, 10.0);

        const char* increase_mode = std::getenv("FASTDDS_CALM_ACK_INCREASE_MODE");
        if (increase_mode != nullptr &&
                (0 == std::strcmp(increase_mode, "byte_normalized") ||
                0 == std::strcmp(increase_mode, "bytes")))
        {
            const uint64_t available_credit =
                    (std::numeric_limits<uint64_t>::max)() - calm_.ack_increase_credit_bytes;
            calm_.ack_increase_credit_bytes += std::min(acknowledged_bytes, available_credit);
            uint32_t growth_steps = 0;
            while (calm_.congestion_window_bytes < calm_.max_batch_bytes &&
                    calm_.ack_increase_credit_bytes >= calm_.congestion_window_bytes &&
                    growth_steps < 64)
            {
                calm_.ack_increase_credit_bytes -= calm_.congestion_window_bytes;
                calm_.congestion_window_bytes = std::min(
                    calm_.max_batch_bytes,
                    calm_.congestion_window_bytes + calm_.additive_increase_bytes);
                ++growth_steps;
            }
        }
        else
        {
            calm_.congestion_window_bytes = std::min(
                calm_.max_batch_bytes,
                calm_.congestion_window_bytes + calm_.additive_increase_bytes);
        }

        const char* rate_mode = std::getenv("FASTDDS_CALM_RELEASE_RATE_MODE");
        if (calm_service_rate_enabled())
        {
            // Only ACK progress that retires NACK-induced debt confirms that
            // the current paced service rate was useful. The increment is a
            // dimensionless fraction of the Writer-observed service estimate.
            if (service_window_ready && calm_.last_service_window_repair_progress)
            {
                calm_.repair_rate_mbps = std::min(
                    calm_.max_repair_rate_mbps,
                    calm_.repair_rate_mbps + calm_.dynamic_rate_ai_mbps);
            }
        }
        else if (rate_mode != nullptr &&
                (0 == std::strcmp(rate_mode, "ack_goodput") ||
                0 == std::strcmp(rate_mode, "goodput")))
        {
            calm_.ack_goodput_headroom = calm_env_double(
                "FASTDDS_CALM_ACK_GOODPUT_HEADROOM",
                calm_.ack_goodput_headroom, 1.0, 4.0);
            const double goodput_target = std::max(
                calm_.min_repair_rate_mbps,
                std::min(
                    calm_.max_repair_rate_mbps,
                    calm_.acked_rate_mbps * calm_.ack_goodput_headroom));
            if (goodput_target > calm_.repair_rate_mbps)
            {
                calm_.repair_rate_mbps = std::min(
                    goodput_target,
                    calm_.repair_rate_mbps + calm_.storm_rate_increase_mbps);
            }
        }
        else
        {
            calm_.repair_rate_mbps = std::min(
                calm_.max_repair_rate_mbps,
                calm_.repair_rate_mbps + calm_.storm_rate_increase_mbps);
        }
        calm_.budget_bytes = std::min(
            calm_requested_bytes(), calm_.congestion_window_bytes);
        calm_.last_pacing_period_ms = calm_batch_interval_ms(
            calm_.congestion_window_bytes,
            calm_.repair_rate_mbps,
            calm_.pacing_floor_ms,
            calm_.observed_batch_drain_ms,
            calm_.pacing_drain_guard);
    }
    calm_.failed_feedback_rounds = 0;
    calm_.repair_round_armed = false;

    if (calm_.control_state == CalmControlState::ACTIVE)
    {
        storm_log_snapshot("calm_ack_increase", calm_.congestion_window_bytes);
    }

    if (calm_.last_ack_time != std::chrono::steady_clock::time_point::min())
    {
        const double elapsed_seconds = std::chrono::duration<double>(
            now - calm_.last_ack_time).count();
        if (elapsed_seconds >= 0.001)
        {
            const bool recovery_ready =
                    calm_.last_path_decrease_time == std::chrono::steady_clock::time_point::min() ||
                    std::chrono::duration<double, std::milli>(
                now - calm_.last_path_decrease_time).count() >= calm_.recovery_probe_delay_ms;
            if (recovery_ready)
            {
                const double additive_probe =
                        calm_.path_increase_alpha * std::min(1.0, elapsed_seconds);
                const double measured_headroom = std::max(
                    0.0, 1.05 * calm_.acked_rate_mbps - calm_.path_rate_mbps);
                calm_.path_rate_mbps = std::min(
                    calm_.max_path_rate_mbps,
                    calm_.path_rate_mbps + additive_probe +
                    std::min(additive_probe, 0.1 * measured_headroom));
            }
        }
    }
    calm_.last_ack_time = now;
}

void ReaderProxy::calm_hold_unsent_new_changes()
{
    if (!calm_should_hold_new())
    {
        return;
    }

    for (ChangeForReader_t& change : changes_for_reader_)
    {
        if (UNSENT == change.getStatus() &&
                !change.calm_repair_pending() &&
                !change.calm_new_queued() &&
                !change.has_been_delivered())
        {
            change.calm_hold_new();
        }
    }
}

uint32_t ReaderProxy::calm_release_held_new(
        uint64_t budget_bytes,
        const std::function<void(ChangeForReader_t& change)>& func,
        uint64_t& released_bytes,
        bool alongside_repair)
{
    released_bytes = 0;
    const bool fills_older_gap = calm_oldest_held_new_precedes_repair();
    const bool repair_drained = calm_requested_size() == 0 &&
            calm_scheduled_repair_bytes() == 0 &&
            calm_backlog_size() == 0;
    const bool paced_new_slot = calm_controls_repair() &&
            calm_requested_size() == 0;
    if (!calm_enabled() ||
            (!fills_older_gap && !repair_drained && !paced_new_slot) ||
            (!alongside_repair && calm_scheduled_repair_bytes() > 0) ||
            (!alongside_repair && !fills_older_gap &&
            std::chrono::steady_clock::now() < calm_.post_repair_guard_until))
    {
        return 0;
    }

    const uint64_t queued_bytes = calm_queued_new_bytes();
    if (queued_bytes != 0)
    {
        return 0;
    }
    budget_bytes = std::min(budget_bytes, calm_.max_scheduled_bytes);

    uint32_t released = 0;
    for (ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.calm_new_held())
        {
            const uint64_t change_bytes = change.getChange() == nullptr ?
                    0 : change.getChange()->serializedPayload.length;
            const uint64_t remaining = budget_bytes - std::min(budget_bytes, released_bytes);
            if (remaining == 0)
            {
                break;
            }

            uint64_t admitted_bytes = change_bytes;
            if (change.getChange() != nullptr &&
                    change.getChange()->getFragmentSize() != 0 &&
                    change.getChange()->getFragmentCount() != 0)
            {
                // Byte pacing is quantized to one RTPS fragment. The final
                // fragment may exceed the remaining budget by at most one
                // fragment, which avoids deadlock on a sub-fragment remainder.
                const uint64_t next_fragment_bytes = change.calm_fragment_payload_size(
                    change.get_next_unsent_fragment());
                admitted_bytes = std::min(change_bytes,
                                std::max(remaining, next_fragment_bytes));
            }
            else if ((alongside_repair || released > 0) && change_bytes > remaining)
            {
                // An unfragmented Change is the minimum transmission atom.
                break;
            }

            change.calm_release_new(admitted_bytes);
            ++released;
            released_bytes += admitted_bytes;
            if (func)
            {
                func(change);
            }
            if (released_bytes >= budget_bytes)
            {
                break;
            }
        }
    }

    return released;
}

void ReaderProxy::calm_log_release(
        const char* phase,
        uint64_t released_bytes) const
{
    if (!calm_metrics_enabled())
    {
        return;
    }

    std::lock_guard<std::mutex> guard(calm_metrics_mutex());
    calm_open_metrics_locked();
    const char* controller = calm_controller();

    calm_budget_csv() << calm_now_ns() << ',' << calm_reader_id(guid()) << ','
                      << calm_.control_epoch << ',' << phase << ','
                      << calm_.budget_bytes << ',' << released_bytes << ','
                      << calm_requested_bytes() << ',' << calm_backlog_size() << ','
                      << "0,release," << calm_.budget_ratio << ','
                      << calm_.rho_prev << ',' << calm_.decrease_gamma << ','
                      << calm_.increase_alpha << ',' << calm_.budget_ratio_floor << ','
                      << static_cast<uint64_t>(calm_.last_pacing_period_ms * 1000.0) << ','
                      << calm_scheduled_repair_bytes() << ',' << calm_held_new_bytes() << ",0,"
                      << calm_.repair_rate_mbps << ',' << controller << ','
                      << calm_queued_new_bytes() << ',' << calm_.path_rate_mbps << ','
                      << calm_.acked_rate_mbps << ',' << calm_control_state_name() << ','
                      << calm_.failed_feedback_rounds << ',' << calm_.repair_round << ','
                      << (calm_.repair_round_armed ? 1 : 0) << ','
                      << (calm_.release_credit_available ? 1 : 0) << ','
                      << calm_.feedback_rtt_ewma_ms << ','
                      << calm_.effective_ack_stall_ms << ','
                      << calm_.effective_feedback_guard_ms << ','
                      << calm_heartbeat_period_ms() << ','
                      << calm_average_sample_bytes() << ','
                      << calm_.offered_rate_ewma_mbps << ','
                      << calm_.active_rate_reference_mbps << ','
                      << calm_.delivery_rate_sample_mbps << ','
                      << calm_.transport_rate_mbps << ','
                      << calm_.service_rate_mbps << ','
                      << (calm_.last_delivery_sample_app_limited ? 1 : 0) << ','
                      << calm_.min_repair_rate_mbps << ','
                      << calm_.initial_repair_rate_mbps << ','
                      << calm_.max_repair_rate_mbps << ','
                      << calm_.dynamic_rate_ai_mbps << ','
                      << calm_.transport_bytes_total << ','
                      << calm_.acknowledged_bytes_total << ','
                      << calm_.service_success_rounds << ','
                      << calm_.service_failure_rounds << ','
                      << calm_.last_service_window_ms << ','
                      << calm_.last_service_window_acknowledged_bytes << ','
                      << calm_.last_service_window_recovered_repair_bytes << '\n';
    calm_flush_metrics_periodically_locked();
}

uint32_t ReaderProxy::convert_status_on_all_changes(
        ChangeForReaderStatus_t previous,
        ChangeForReaderStatus_t next,
        const std::function<void(ChangeForReader_t& change)>& func)
{
    assert(previous > next);

    // NOTE: This is only called for REQUESTED=>UNSENT (acknack response) or
    //       UNDERWAY=>UNACKNOWLEDGED (nack supression)

    uint32_t changed = 0;
    for (ChangeForReader_t& change : changes_for_reader_)
    {
        if (change.getStatus() == previous)
        {
            ++changed;
            change.setStatus(next);

            if (func)
            {
                func(change);
            }
        }
    }

    return changed;
}

void ReaderProxy::change_has_been_removed(
        const SequenceNumber_t& seq_num)
{
    // Check sequence number is in the container, because it was not clean up.
    if (changes_for_reader_.empty() || seq_num < changes_for_reader_.begin()->getSequenceNumber())
    {
        return;
    }

    auto chit = find_change(seq_num);

    if (chit == this->changes_for_reader_.end())
    {
        // No change for this sequence number
        return;
    }

    // In intraprocess, if there is an UNACKNOWLEDGED, a GAP has to be send because there is no reliable mechanism.
    if (is_local_reader() && ACKNOWLEDGED > chit->getStatus())
    {
        writer_->intraprocess_gap(this, seq_num);
    }

    // Element may not be in the container when marked as irrelevant.
    changes_for_reader_.erase(chit);

    // When removing the next-to-be-acknowledged, we should auto-acknowledge it.
    if ((changes_low_mark_ + 1) == seq_num)
    {
        acked_changes_set(seq_num + 1);
    }

    calm_reset_control_if_recovered();
}

bool ReaderProxy::has_unacknowledged(
        const SequenceNumber_t& first_seq_in_history) const
{
    if (first_seq_in_history > changes_low_mark_)
    {
        return true;
    }

    for (const ChangeForReader_t& it : changes_for_reader_)
    {
        if (it.getStatus() == UNACKNOWLEDGED)
        {
            return true;
        }
    }

    return false;
}

bool ReaderProxy::requested_fragment_set(
        const SequenceNumber_t& seq_num,
        const FragmentNumberSet_t& frag_set)
{
    // Locate the outbound change referenced by the NACK_FRAG
    ChangeIterator changeIter = find_change(seq_num, true);
    if (changeIter == changes_for_reader_.end())
    {
        return false;
    }

    if (!changeIter->has_been_delivered())
    {
        return false;
    }

    if (calm_tracking_enabled())
    {
        const double feedback_guard_ms = calm_feedback_guard_ms();
        const uint64_t previous_bytes = changeIter->calm_repair_pending_bytes();
        const bool feedback_can_close_round =
                changeIter->getStatus() != UNSENT && changeIter->getStatus() != UNDERWAY;
        const bool failed_repair_feedback = feedback_can_close_round &&
                changeIter->calm_note_fragment_nack_after_repair(
                    frag_set,
                    feedback_guard_ms,
                    calm_env_flag_enabled(
                        "FASTDDS_CALM_FEEDBACK_BYTE_PROGRESS", calm4_family_controller()));
        const bool repeated = changeIter->calm_nack_count() > 0 ||
                changeIter->calm_retransmit_count() > 0;
        const uint64_t nack_bytes = calm_fragment_set_bytes(*changeIter, frag_set);
        changeIter->calm_mark_fragment_repair_pending(frag_set);
        const uint64_t current_bytes = changeIter->calm_repair_pending_bytes();
        calm_record_nack(
            nack_bytes,
            current_bytes > previous_bytes ? current_bytes - previous_bytes : 0,
            repeated,
            failed_repair_feedback,
            changeIter->calm_last_failed_repair_severity());
        calm_log_observer_snapshot(
            repeated ? "nackfrag_repeated" : "nackfrag", 0);
        storm_log_snapshot(
            repeated ? "nackfrag_repeated" : "nackfrag", nack_bytes, seq_num);
    }
    if (calm_controls_repair())
    {
        calm_note_nack();
        // Feedback received while this repair attempt is still being sent is stale
        // for scheduling purposes. Keep it in the NACK metrics, but do not append
        // already-sent fragments back into the active UNSENT fragment window.
        if (UNSENT == changeIter->getStatus() || UNDERWAY == changeIter->getStatus())
        {
            return false;
        }
        if (!changeIter->calm_retry_ready(
                    std::chrono::steady_clock::now(),
                    calm_.retry_cooldown_ms))
        {
            return false;
        }
    }
    changeIter->markFragmentsAsUnsent(frag_set);

    // If it was UNSENT, we shouldn't switch back to REQUESTED to prevent stalling.
    if (changeIter->getStatus() != UNSENT)
    {
        changeIter->setStatus(REQUESTED);
    }

    if (calm_controls_repair())
    {
        calm_note_request_ready();
        calm_hold_unsent_new_changes();
    }
    return true;
}

bool ReaderProxy::process_nack_frag(
        const GUID_t& reader_guid,
        uint32_t nack_count,
        const SequenceNumber_t& seq_num,
        const FragmentNumberSet_t& fragments_state)
{
    if (guid() == reader_guid)
    {
        if (last_nackfrag_count_ < nack_count)
        {
            last_nackfrag_count_ = nack_count;
            if (requested_fragment_set(seq_num, fragments_state))
            {
                return true;
            }
        }
    }

    return false;
}

static bool change_less_than_sequence(
        const ChangeForReader_t& change,
        const SequenceNumber_t& seq_num)
{
    return change.getSequenceNumber() < seq_num;
}

ReaderProxy::ChangeIterator ReaderProxy::find_change(
        const SequenceNumber_t& seq_num,
        bool exact)
{
    ReaderProxy::ChangeIterator it;
    ReaderProxy::ChangeIterator end = changes_for_reader_.end();
    it = std::lower_bound(changes_for_reader_.begin(), end, seq_num, change_less_than_sequence);

    return (!exact)
           ? it
           : it == end
           ? it
           : it->getSequenceNumber() == seq_num ? it : end;
}

ReaderProxy::ChangeConstIterator ReaderProxy::find_change(
        const SequenceNumber_t& seq_num) const
{
    ReaderProxy::ChangeConstIterator it;
    ReaderProxy::ChangeConstIterator end = changes_for_reader_.end();
    it = std::lower_bound(changes_for_reader_.begin(), end, seq_num, change_less_than_sequence);

    return it == end
           ? it
           : it->getSequenceNumber() == seq_num ? it : end;
}

}   // namespace rtps
}   // namespace fastrtps
}   // namespace eprosima
