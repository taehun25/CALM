// Copyright 2016-2019 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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
 * @file ReaderProxy.h
 */
#ifndef _FASTDDS_RTPS_WRITER_READERPROXY_H_
#define _FASTDDS_RTPS_WRITER_READERPROXY_H_

#ifndef DOXYGEN_SHOULD_SKIP_THIS_PUBLIC

#include <fastdds/rtps/attributes/WriterAttributes.h>
#include <fastdds/rtps/attributes/RTPSParticipantAllocationAttributes.hpp>

#include <fastdds/rtps/builtin/data/ReaderProxyData.h>

#include <fastdds/rtps/common/Types.h>
#include <fastdds/rtps/common/Locator.h>
#include <fastdds/rtps/common/SequenceNumber.h>
#include <fastdds/rtps/common/CacheChange.h>
#include <fastdds/rtps/common/FragmentNumber.h>

#include <fastdds/rtps/writer/ChangeForReader.h>
#include <fastdds/rtps/writer/ReaderLocator.h>

#include <fastrtps/utils/collections/ResourceLimitedVector.hpp>

#include <algorithm>
#include <mutex>
#include <set>
#include <atomic>

namespace eprosima {
namespace fastrtps {
namespace rtps {

class StatefulWriter;
class TimedEvent;
class RTPSReader;
class IDataSharingNotifier;
class RTPSGapBuilder;

/**
 * ReaderProxy class that helps to keep the state of a specific Reader with respect to the RTPSWriter.
 * @ingroup WRITER_MODULE
 */
class ReaderProxy
{
public:

    ~ReaderProxy();

    /**
     * Constructor.
     * @param times WriterTimes to use in the ReaderProxy.
     * @param loc_alloc Maximum number of remote locators to keep in the ReaderProxy.
     * @param writer Pointer to the StatefulWriter creating the reader proxy.
     */
    ReaderProxy(
            const WriterTimes& times,
            const RemoteLocatorsAllocationAttributes& loc_alloc,
            StatefulWriter* writer);

    /**
     * Activate this proxy associating it to a remote reader.
     * @param reader_attributes ReaderProxyData of the reader for which to keep state.
     * @param is_datasharing whether the reader is datasharing compatible with the writer or not.
     */
    void start(
            const ReaderProxyData& reader_attributes,
            bool is_datasharing = false);

    /**
     * Update information about the remote reader.
     * @param reader_attributes ReaderProxyData with updated information of the reader.
     * @return true if data was modified, false otherwise.
     */
    bool update(
            const ReaderProxyData& reader_attributes);

    /**
     * Disable this proxy.
     */
    void stop();

    /**
     * Called when a change is added to the writer's history.
     * @param change Information regarding the change added.
     * @param is_relevant Specify if change is relevant for this remote reader.
     * @param restart_nack_supression Whether nack-supression event should be restarted.
     */
    void add_change(
            const ChangeForReader_t& change,
            bool is_relevant,
            bool restart_nack_supression);

    void add_change(
            const ChangeForReader_t& change,
            bool is_relevant,
            bool restart_nack_supression,
            const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time);

    /**
     * Check if there are changes pending for this reader.
     * @return true when there are pending changes, false otherwise.
     */
    bool has_changes() const;

    /**
     * Check if a specific change has been already acknowledged for this reader.
     * @param seq_num Sequence number of the change to be checked.
     * @return true when the change is irrelevant or has been already acknowledged, false otherwise.
     */
    bool change_is_acked(
            const SequenceNumber_t& seq_num) const;

    /**
     * Check if a specific change is marked to be sent to this reader.
     *
     * @param[in]  seq_num Sequence number of the change to be checked.
     * @param[out] next_unsent_frag Return next fragment to be sent.
     * @param[out] gap_seq Return, when it is its first delivery (should be relevant seq_num), the sequence number of
     * the first sequence of the gap [first, seq_num). Otherwise return SequenceNumber_t::unknown().
     * @param[in]  min_seq Minimum sequence number managed by the History. It could be SequenceNumber_t::unknown() if
     * history is empty.
     * @param[out] need_reactivate_periodic_heartbeat Indicates if the heartbeat period event has to be restarted.
     *
     * @return true if the change is marked to be sent. False otherwise.
     */
    bool change_is_unsent(
            const SequenceNumber_t& seq_num,
            FragmentNumber_t& next_unsent_frag,
            SequenceNumber_t& gap_seq,
            const SequenceNumber_t& min_seq,
            bool& need_reactivate_periodic_heartbeat) const;

    /**
     * Mark all changes up to the one indicated by seq_num as Acknowledged.
     * For instance, when seq_num is 30, changes 1-29 are marked as acknowledged.
     * @param seq_num Sequence number of the first change not to be marked as acknowledged.
     */
    void acked_changes_set(
            const SequenceNumber_t& seq_num);

    /**
     * Mark all changes in the vector as requested.
     * @param seq_num_set Bitmap of sequence numbers.
     * @param gap_builder RTPSGapBuilder reference uses for adding  each requested change that is irrelevant for the
     * requester.
     * @param[in] min_seq_in_history Minimum SequenceNumber_t in the writer's history. If writer's history is empty,
     * SequenceNumber_t::unknown() is expected.
     * @return true if at least one change has been marked as REQUESTED, false otherwise.
     */
    bool requested_changes_set(
            const SequenceNumberSet_t& seq_num_set,
            RTPSGapBuilder& gap_builder,
            const SequenceNumber_t& min_seq_in_history);

    /**
     * Performs processing of preemptive acknack
     * @param func functor called, if the requester is a local reader, for each changes moved to UNSENT status.
     * @return true if a heartbeat should be sent, false otherwise.
     */
    bool process_initial_acknack(
            const std::function<void(ChangeForReader_t& change)>& func);

    /*!
     * @brief Sets a change to a particular status (if present in the ReaderProxy)
     * @param seq_num Sequence number of the change to update.
     * @param status Status to apply.
     * @param restart_nack_supression Whether nack supression event should be restarted or not.
     * @param delivered true if change was able to be delivered to its addressees. false otherwise.
     */
    void from_unsent_to_status(
            const SequenceNumber_t& seq_num,
            ChangeForReaderStatus_t status,
            bool restart_nack_supression,
            bool delivered = true);

    /**
     * @brief Mark a particular fragment as sent.
     * @param[in]  seq_num Sequence number of the change to update.
     * @param[in]  frag_num Fragment number to mark as sent.
     * @param[out] was_last_fragment Indicates if the fragment was the last one pending.
     * @return true when the change was found, false otherwise.
     */
    bool mark_fragment_as_sent_for_change(
            const SequenceNumber_t& seq_num,
            FragmentNumber_t frag_num,
            bool& was_last_fragment);

    /**
     * Turns all UNDERWAY changes into UNACKNOWLEDGED.
     *
     * @return true if at least one change changed its status, false otherwise.
     */
    bool perform_nack_supression();

    /**
     * Turns all REQUESTED changes into UNSENT.
     *
     * @param func Function executed for each change which changes its status.
     * @return the number of changes that changed its status.
     */
    uint32_t perform_acknack_response(
            const std::function<void(ChangeForReader_t& change)>& func);

    /**
     * Counts bytes that still contribute to this reader's retransmission backlog.
     *
     * @return byte-weighted retransmission backlog for this reader.
     */
    uint64_t calm_backlog_size() const;

    //! Average serialized sample size currently represented by this ReaderProxy.
    uint64_t calm_average_sample_bytes() const;

    //! Effective fixed-byte or sample-relative rho threshold used by the detector.
    uint64_t calm_detector_rho_threshold_bytes() const;

    /**
     * Counts bytes still in REQUESTED and not released by CALM pacing.
     *
     * @return byte-weighted REQUESTED queue for this reader.
     */
    uint64_t calm_requested_bytes() const;

    /**
     * Counts changes explicitly requested by this reader through ACKNACK.
     *
     * @return number of changes currently marked REQUESTED.
     */
    uint32_t calm_requested_size() const;

    /**
     * Counts repair bytes currently released as UNSENT and awaiting network delivery.
     *
     * Unlike the outstanding repair backlog, this value falls as fragments leave
     * the writer and does not wait for a cumulative ACK.
     */
    uint64_t calm_scheduled_repair_bytes() const;

    uint64_t calm_total_nack_count() const;

    //! Number of reader changes with unresolved NACK-induced repair debt.
    uint32_t calm_active_repair_count() const;

    //! Largest repair-release count among unresolved reader changes.
    uint32_t calm_max_retransmit_count() const;

    //! Repair-release count of the oldest unresolved reader change.
    uint32_t calm_oldest_retransmit_count() const;

    //! Largest failed repair-feedback count among unresolved reader changes.
    uint32_t calm_max_failed_repair_count() const;

    //! Failed repair-feedback count of the oldest unresolved reader change.
    uint32_t calm_oldest_failed_repair_count() const;

    uint32_t calm_held_new_size() const;

    uint64_t calm_held_new_bytes() const;

    uint64_t calm_queued_new_bytes() const;

    /**
     * Updates and returns the CALM retransmission budget for this reader.
     *
     * @return maximum number of REQUESTED payload bytes to enqueue this control epoch.
     */
    uint64_t calm_update_budget();

    /**
     * Turns oldest REQUESTED changes into UNSENT up to a byte budget.
     *
     * @param budget_bytes Maximum requested payload bytes to process.
     * @param func Function executed for each change which changes its status.
     * @param released_bytes Payload bytes released by this call.
     * @return the number of changes that changed its status.
     */
    uint32_t perform_acknack_response_limited(
            uint64_t budget_bytes,
            const std::function<void(ChangeForReader_t& change)>& func,
            uint64_t& released_bytes);

    uint64_t calm_budget_bytes() const;

    //! Byte budget shared by repair and held-new releases while CALM is active.
    uint64_t calm_total_release_budget_bytes() const;

    uint64_t calm_max_batch_bytes() const;

    double calm_pacing_period_ms() const;

    double calm_release_rate_mbps() const;

    //! Writer-observed DDS delivery capacity used by the adaptive release-rate mode.
    double calm_service_rate_mbps() const;

    bool calm_enabled() const;

    /**
     * Returns whether storm mitigation currently controls repair release.
     * NORMAL intentionally retains the stock Fast DDS behavior.
     */
    bool calm_controls_repair() const;

    bool calm_tracking_enabled() const;

    //! Re-evaluate preemptive CALM activation immediately before a T_NR release.
    void calm_evaluate_before_tnr();

    void calm_observe_default_release(
            uint64_t released_bytes);

    uint64_t calm_change_requested_bytes(
            const ChangeForReader_t& change) const;

    void storm_observe_tnr_release(
            uint64_t requested_bytes,
            uint32_t requested_changes,
            uint64_t accepted_bytes,
            uint32_t accepted_changes,
            uint64_t rejected_bytes,
            uint32_t rejected_changes);

    void storm_observe_transport_repair(
            const SequenceNumber_t& seq_num,
            FragmentNumber_t fragment);

    bool calm_can_release_repair() const;

    bool calm_can_release_held_new(
            bool alongside_repair = false) const;

    bool calm_has_pacing_work() const;

    void calm_note_release(
            uint64_t released_bytes,
            bool repair);

    void calm_note_shared_release(
            uint64_t released_bytes,
            uint64_t repair_bytes);

    bool calm_pacing_active() const;

    void calm_pacing_active(
            bool active);

    bool calm_should_hold_new() const;

    void calm_hold_unsent_new_changes();

    uint32_t calm_release_held_new(
            uint64_t budget_bytes,
            const std::function<void(ChangeForReader_t& change)>& func,
            uint64_t& released_bytes,
            bool alongside_repair = false);

    void calm_log_release(
            const char* phase,
            uint64_t released_bytes) const;

    /**
     * Call this to inform a change was removed from history.
     * @param seq_num Sequence number of the removed change.
     */
    void change_has_been_removed(
            const SequenceNumber_t& seq_num);

    /*!
     * @brief Returns there is some UNACKNOWLEDGED change.
     * @param first_seq_in_history Minimum sequence number in the writer history.
     * @return There is some UNACKNOWLEDGED change.
     */
    bool has_unacknowledged(
            const SequenceNumber_t& first_seq_in_history) const;

    /**
     * Get the GUID of the reader represented by this proxy.
     * @return the GUID of the reader represented by this proxy.
     */
    inline const GUID_t& guid() const
    {
        return locator_info_.remote_guid();
    }

    /**
     * Get the durability of the reader represented by this proxy.
     * @return the durability of the reader represented by this proxy.
     */
    inline DurabilityKind_t durability_kind() const
    {
        return durability_kind_;
    }

    /**
     * Check if the reader represented by this proxy expexts inline QOS to be received.
     * @return true if the reader represented by this proxy expexts inline QOS to be received.
     */
    inline bool expects_inline_qos() const
    {
        return expects_inline_qos_;
    }

    /**
     * Check if the reader represented by this proxy is reliable.
     * @return true if the reader represented by this proxy is reliable.
     */
    inline bool is_reliable() const
    {
        return is_reliable_;
    }

    inline bool disable_positive_acks() const
    {
        return disable_positive_acks_;
    }

    /**
     * Check if the reader represented by this proxy is remote and reliable.
     * @return true if the reader represented by this proxy is remote and reliable.
     */
    inline bool is_remote_and_reliable() const
    {
        return !locator_info_.is_local_reader() && !locator_info_.is_datasharing_reader() && is_reliable_;
    }

    /**
     * Check if the reader is on the same process.
     * @return true if the reader is no the same process.
     */
    inline bool is_local_reader()
    {
        return locator_info_.is_local_reader();
    }

    /**
     * Get the local reader on the same process (if any).
     * @return The local reader on the same process.
     */
    inline RTPSReader* local_reader()
    {
        return locator_info_.local_reader();
    }

    /**
     * Called when an ACKNACK is received to set a new value for the minimum count accepted for following received
     * ACKNACKs.
     *
     * @param acknack_count The count of the received ACKNACK.
     * @return true if internal count changed (i.e. received ACKNACK is accepted)
     */
    bool check_and_set_acknack_count(
            uint32_t acknack_count)
    {
        if (acknack_count >= next_expected_acknack_count_)
        {
            next_expected_acknack_count_ = acknack_count;
            ++next_expected_acknack_count_;
            return true;
        }

        return false;
    }

    /**
     * Process an incoming NACKFRAG submessage.
     * @param reader_guid Destination guid of the submessage.
     * @param nack_count Counter field of the submessage.
     * @param seq_num Sequence number field of the submessage.
     * @param fragments_state Bitmap indicating the requested fragments.
     * @return true if a change was modified, false otherwise.
     */
    bool process_nack_frag(
            const GUID_t& reader_guid,
            uint32_t nack_count,
            const SequenceNumber_t& seq_num,
            const FragmentNumberSet_t& fragments_state);

    /**
     * Filter a CacheChange_t using the StatefulWriter's IReaderDataFilter.
     * @param change
     * @return true if the change is relevant, false otherwise.
     */
    bool rtps_is_relevant(
            CacheChange_t* change) const;

    /**
     * Get the highest fully acknowledged sequence number.
     * @return the highest fully acknowledged sequence number.
     */
    SequenceNumber_t changes_low_mark() const
    {
        return changes_low_mark_;
    }

    /**
     * Change the interval of nack-supression event.
     * @param interval Time from data sending to acknack processing.
     */
    void update_nack_supression_interval(
            const Duration_t& interval);

    LocatorSelectorEntry* locator_selector_entry()
    {
        return locator_info_.locator_selector_entry();
    }

    RTPSMessageSenderInterface* message_sender()
    {
        return &locator_info_;
    }

    bool is_datasharing_reader() const
    {
        return locator_info_.is_datasharing_reader();
    }

    IDataSharingNotifier* datasharing_notifier()
    {
        return locator_info_.datasharing_notifier();
    }

    const IDataSharingNotifier* datasharing_notifier() const
    {
        return locator_info_.datasharing_notifier();
    }

    void datasharing_notify()
    {
        locator_info_.datasharing_notify();
    }

    size_t locators_size() const
    {
        return locator_info_.locators_size();
    }

    bool active() const
    {
        return active_;
    }

    void active(
            bool active)
    {
        active_ = active;
    }

private:

    enum class CalmControlState : uint8_t
    {
        NORMAL,
        ACTIVE
    };

    struct CalmState
    {
        uint64_t rho_prev = 0;
        double budget_ratio = 1.0;
        double budget_ratio_floor = 0.125;
        uint64_t budget_bytes = 0;
        uint64_t congestion_window_bytes = 0;
        uint64_t initial_budget_bytes = 512u * 1024u;
        uint64_t minimum_budget_bytes = 128u * 1024u;
        uint64_t additive_increase_bytes = 256u * 1024u;
        double minimum_budget_sample_multiplier = 0.25;
        double initial_budget_sample_multiplier = 1.0;
        double maximum_budget_sample_multiplier = 2.0;
        double additive_increase_sample_multiplier = 0.25;
        double storm_decrease_gamma = 0.75;
        double storm_rate_decrease_gamma = 0.875;
        double first_failure_decrease_gamma = 0.80;
        double first_failure_rate_decrease_gamma = 0.90;
        double storm_rate_increase_mbps = 16.0;
        uint32_t failed_rounds_before_decrease = 1;
        double feedback_min_age_ms = 100.0;
        double effective_feedback_guard_ms = 100.0;
        double feedback_guard_rtt_multiplier = 1.0;
        double feedback_guard_min_ms = 10.0;
        double feedback_guard_max_ms = 2000.0;
        double feedback_rtt_ewma_ms = 0.0;
        double feedback_rtt_alpha = 0.2;
        double decrease_gamma = 0.65;
        double increase_alpha = 64.0;
        double repair_rate_mbps = 192.0;
        double initial_repair_rate_mbps = 192.0;
        double min_repair_rate_mbps = 64.0;
        double max_repair_rate_mbps = 192.0;
        double offered_rate_ewma_mbps = 0.0;
        double active_rate_reference_mbps = 0.0;
        double offered_rate_alpha = 0.2;
        double min_rate_offered_multiplier = 0.0;
        double initial_rate_offered_multiplier = 0.0;
        double max_rate_offered_multiplier = 0.0;
        double rate_increase_offered_multiplier = 0.0;
        double max_path_rate_mbps = 300.0;
        double path_rate_mbps = 96.0;
        double min_path_rate_mbps = 16.0;
        double path_decrease_gamma = 0.75;
        double path_increase_alpha = 64.0;
        double acked_rate_mbps = 0.0;
        double transport_rate_mbps = 0.0;
        double delivery_rate_sample_mbps = 0.0;
        double service_rate_mbps = 0.0;
        double service_rate_alpha_up = 0.25;
        double service_rate_alpha_down = 0.10;
        double service_rate_failure_gamma = 0.98;
        double service_rate_min_multiplier = 0.25;
        double service_rate_initial_multiplier = 1.0;
        double service_rate_max_multiplier = 2.0;
        double service_rate_ai_multiplier = 0.25;
        double service_rate_ack_cap_multiplier = 1.05;
        double dynamic_rate_ai_mbps = 0.0;
        uint64_t transport_bytes_total = 0;
        uint64_t transport_bytes_since_ack = 0;
        uint64_t minimum_transport_atom_bytes = 0;
        uint64_t acknowledged_bytes_total = 0;
        uint64_t service_window_acknowledged_bytes = 0;
        uint64_t service_window_recovered_repair_bytes = 0;
        uint64_t last_service_window_acknowledged_bytes = 0;
        uint64_t last_service_window_recovered_repair_bytes = 0;
        double last_service_window_ms = 0.0;
        uint64_t service_success_rounds = 0;
        uint64_t service_failure_rounds = 0;
        bool transport_interval_backlogged = false;
        bool last_delivery_sample_app_limited = true;
        bool last_service_window_repair_progress = false;
        uint64_t ack_increase_credit_bytes = 0;
        double ack_goodput_headroom = 1.05;
        // CALM 4 uses an episode-fixed pacing period. CALM 4.1 may derive it
        // once from pre-entry ACK goodput; neither mode follows later B changes.
        double pacing_floor_ms = 50.0;
        double entry_service_rate_mbps = 0.0;
        double entry_pacing_eta = 1.10;
        bool entry_ack_pacing_enabled = false;
        bool entry_ack_pacing_applied = false;
        double pacing_drain_guard = 1.10;
        double budget_horizon_ms = 20.0;
        uint64_t max_batch_bytes = 2u * 1024u * 1024u;
        uint64_t max_scheduled_bytes = 2u * 1024u * 1024u;
        uint64_t max_admission_window_bytes = 8u * 1024u * 1024u;
        uint64_t min_admission_window_bytes = 2u * 1024u * 1024u;
        uint64_t delta_threshold_bytes = 16u * 1024u;
        uint64_t onset_threshold_bytes = 2u * 1024u * 1024u;
        uint64_t detector_min_rho_bytes = 4u * 1024u * 1024u;
        uint32_t detector_soft_retry_count = 2;
        uint32_t detector_failed_repair_count = 2;
        uint32_t detector_growth_rounds = 2;
        double detector_ack_stall_ms = 500.0;
        double effective_ack_stall_ms = 500.0;
        double ack_stall_rtt_multiplier = 4.0;
        double ack_stall_hb_multiplier = 2.0;
        double ack_stall_min_ms = 50.0;
        double ack_stall_max_ms = 2000.0;
        uint32_t detector_nonshrinking_rounds = 0;
        uint64_t detector_previous_rho_bytes = 0;
        double retry_cooldown_ms = 100.0;
        double first_repair_delay_ms = 25.0;
        double post_repair_guard_ms = 100.0;
        double recovery_probe_delay_ms = 150.0;
        uint64_t nack_count_prev = 0;
        uint64_t observer_nack_events_total = 0;
        uint64_t observer_nack_bytes_total = 0;
        uint64_t observer_unique_repair_bytes_total = 0;
        uint64_t observer_repeated_nack_events_total = 0;
        uint64_t observer_repeated_nack_bytes_total = 0;
        uint64_t observer_repair_release_events_total = 0;
        uint64_t observer_repair_release_bytes_total = 0;
        uint64_t storm_tnr_release_events_total = 0;
        uint64_t storm_tnr_requested_changes_total = 0;
        uint64_t storm_tnr_requested_bytes_total = 0;
        uint64_t storm_flow_enqueue_accepted_changes_total = 0;
        uint64_t storm_flow_enqueue_accepted_bytes_total = 0;
        uint64_t storm_flow_enqueue_rejected_changes_total = 0;
        uint64_t storm_flow_enqueue_rejected_bytes_total = 0;
        uint64_t storm_transport_repair_attempt_events_total = 0;
        uint64_t storm_transport_repair_attempt_bytes_total = 0;
        uint64_t storm_ack_recovered_bytes_total = 0;
        SequenceNumber_t storm_oldest_no_progress_seq = SequenceNumber_t::unknown();
        uint32_t storm_oldest_no_progress_rounds = 0;
        double integral_error = 0.0;
        double previous_normalized_delta = 0.0;
        double last_pacing_period_ms = 1.0;
        double observed_batch_drain_ms = 0.0;
        std::chrono::steady_clock::time_point next_release_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point batch_release_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point repair_not_before =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point post_repair_guard_until =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_control_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_growth_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_path_decrease_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_ack_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point first_new_change_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point service_window_start_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_new_change_time =
                std::chrono::steady_clock::time_point::min();
        bool pacing_active = false;
        CalmControlState control_state = CalmControlState::NORMAL;
        uint32_t failed_feedback_rounds = 0;
        uint64_t repair_round = 0;
        bool repair_round_armed = false;
        uint64_t feedback_round_start_u_bytes = 0;
        uint64_t feedback_round_released_bytes = 0;
        uint32_t feedback_round_start_oldest_failed_count = 0;
        int64_t last_feedback_delta_u_bytes = 0;
        uint32_t last_feedback_delta_oldest_failed = 0;
        double last_feedback_failure_fraction = 0.0;
        double last_feedback_progress_fraction = 0.0;
        double calm4_decrease_gain = 0.25;
        double calm4_increase_gain = 0.25;
        double calm41_timeout_rtt_multiplier = 4.0;
        double calm41_feedback_timeout_ms = 0.0;
        double last_failure_severity = 1.0;
        double last_effective_window_gamma = 1.0;
        double last_effective_rate_gamma = 1.0;
        bool release_credit_available = false;
        bool last_release_was_repair = false;
        std::chrono::steady_clock::time_point last_repair_attempt_time =
                std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point repair_round_start_time =
                std::chrono::steady_clock::time_point::min();
        uint64_t control_epoch = 0;
    };

    void calm_note_ack(
            uint64_t acknowledged_bytes,
            uint64_t recovered_repair_bytes);

    void calm_note_repair_attempt();

    void calm_observe_repeated_feedback(
            double failure_severity);

    void calm_note_feedback_rtt(
            const std::chrono::steady_clock::time_point& now);

    double calm_heartbeat_period_ms() const;

    double calm_ack_stall_threshold_ms();

    double calm_feedback_guard_ms();

    double calm41_feedback_timeout_ms();

    void calm_enter_active();

    void calm_refresh_budget_parameters();

    void calm_refresh_rate_parameters();

    bool calm_service_rate_enabled() const;

    void calm_refresh_service_rate_bounds();

    bool calm_update_service_rate_on_ack(
            uint64_t acknowledged_bytes,
            uint64_t recovered_repair_bytes,
            const std::chrono::steady_clock::time_point& now);

    uint64_t calm_minimum_transport_atom_bytes() const;

    void calm_observe_new_change(
            const ChangeForReader_t& change);

    void calm_note_request_ready();

    bool calm_oldest_held_new_precedes_repair() const;

    uint64_t calm_active_admitted_bytes() const;

    uint64_t calm_admission_window_bytes() const;

    bool calm_active_window_has_slot() const;

    const char* calm_control_state_name() const;

    bool calm_detector_attempt_pressure(
            uint32_t oldest_retransmit_count,
            uint32_t oldest_failed_repair_count) const;

    //!Is this proxy active? I.e. does it have a remote reader associated?
    bool is_active_;
    //!Reader locator information
    ReaderLocator locator_info_;
    //!Taken from QoS
    DurabilityKind_t durability_kind_;
    //!Taken from QoS
    bool expects_inline_qos_;
    //!Taken from QoS
    bool is_reliable_;
    //!Taken from QoS
    bool disable_positive_acks_;
    //!Pointer to the associated StatefulWriter.
    StatefulWriter* writer_;
    //!Set of the changes and its state.
    ResourceLimitedVector<ChangeForReader_t, std::true_type> changes_for_reader_;
    //! CALM per-reader retransmission control state.
    CalmState calm_;
    //! Timed Event to manage the delay to mark a change as UNACKED after sending it.
    TimedEvent* nack_supression_event_;
    TimedEvent* initial_heartbeat_event_;
    //! Are timed events enabled?
    std::atomic_bool timers_enabled_;
    //! Next expected ack/nack count
    uint32_t next_expected_acknack_count_;
    //! Last  NACKFRAG count.
    uint32_t last_nackfrag_count_;

    SequenceNumber_t changes_low_mark_;

    bool active_ = false;

    using ChangeIterator = ResourceLimitedVector<ChangeForReader_t, std::true_type>::iterator;
    using ChangeConstIterator = ResourceLimitedVector<ChangeForReader_t, std::true_type>::const_iterator;

    void disable_timers();

    void calm_reset_control_if_recovered();

    void calm_note_nack();

    void calm_record_nack(
            uint64_t nack_bytes,
            uint64_t unique_repair_bytes,
            bool repeated,
            bool failed_repair_feedback,
            double failure_severity);

    void calm_log_observer_snapshot(
            const char* phase,
            uint64_t released_bytes) const;

    void storm_log_snapshot(
            const char* event,
            uint64_t event_bytes,
            const SequenceNumber_t& event_sample_seq = SequenceNumber_t::unknown()) const;

    void storm_note_sample_retransmit(
            const SequenceNumber_t& seq_num);

    /*
     * Converts all changes with a given status to a different status.
     * @param previous Status to change.
     * @param next Status to adopt.
     * @param func Function executed for each change which changes its status.
     * @return the number of changes that have been modified.
     */
    uint32_t convert_status_on_all_changes(
            ChangeForReaderStatus_t previous,
            ChangeForReaderStatus_t next,
            const std::function<void(ChangeForReader_t& change)>& func = {});

    /*!
     * @brief Adds requested fragments. These fragments will be sent in next NackResponseDelay.
     * @param[in] seq_num Sequence number to be paired with the requested fragments.
     * @param[in] frag_set set containing the requested fragments to be sent.
     * @return True if there is at least one requested fragment. False in other case.
     */
    bool requested_fragment_set(
            const SequenceNumber_t& seq_num,
            const FragmentNumberSet_t& frag_set);

    void add_change(
            const ChangeForReader_t& change,
            bool is_relevant);

    /**
     * @brief Find a change with the specified sequence number.
     * @param seq_num Sequence number to find.
     * @param exact When false, the first change with a sequence number not less than seq_num will be returned.
     * When true, the change with a sequence number value of seq_num will be returned.
     * @return Iterator pointing to the change, changes_for_reader_.end() if not found.
     */
    ChangeIterator find_change(
            const SequenceNumber_t& seq_num,
            bool exact);

    /**
     * @brief Find a change with the specified sequence number.
     * @param seq_num Sequence number to find.
     * @return Iterator pointing to the change, changes_for_reader_.end() if not found.
     */
    ChangeConstIterator find_change(
            const SequenceNumber_t& seq_num) const;
};

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */

#endif // ifndef DOXYGEN_SHOULD_SKIP_THIS_PUBLIC
#endif /* _FASTDDS_RTPS_WRITER_READERPROXY_H_ */
