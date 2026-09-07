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
 * @file ChangeForReader.h
 */

#ifndef _FASTDDS_RTPS_CHANGEFORREADER_H_
#define _FASTDDS_RTPS_CHANGEFORREADER_H_

#include <fastdds/rtps/common/CacheChange.h>
#include <fastdds/rtps/common/FragmentNumber.h>
#include <fastdds/rtps/common/SequenceNumber.h>

#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>

namespace eprosima {
namespace fastrtps {
namespace rtps {

#ifndef DOXYGEN_SHOULD_SKIP_THIS_PUBLIC

/**
 * Enum ChangeForReaderStatus_t, possible states for a CacheChange_t in a ReaderProxy.
 *  @ingroup COMMON_MODULE
 */
enum ChangeForReaderStatus_t
{
    UNSENT = 0,                    //!< UNSENT
    REQUESTED = 1,                 //!< REQUESTED
    UNACKNOWLEDGED = 2,            //!< UNACKNOWLEDGED
    ACKNOWLEDGED = 3,              //!< ACKNOWLEDGED
    UNDERWAY = 4                   //!< UNDERWAY
};

/**
 * Struct ChangeForReader_t used to represent the state of a specific change with respect to a specific reader, as well as its relevance.
 *  @ingroup COMMON_MODULE
 */
class ChangeForReader_t
{
    friend struct ChangeForReaderCmp;

public:

    explicit ChangeForReader_t(
            CacheChange_t* change)
        : status_(UNSENT)
        , seq_num_(change->sequenceNumber)
        , change_(change)
    {
        if (change->getFragmentSize() != 0)
        {
            unsent_fragments_.base(1u);
            unsent_fragments_.add_range(1u, change->getFragmentCount() + 1u);
        }
    }

    /**
     * Get the cache change
     * @return Cache change
     */
    CacheChange_t* getChange() const
    {
        return change_;
    }

    void setStatus(
            const ChangeForReaderStatus_t status)
    {
        status_ = status;
        if (UNSENT != status)
        {
            calm_new_queued_ = false;
            calm_new_credit_limited_ = false;
            calm_new_release_credit_bytes_ = 0;
        }
    }

    ChangeForReaderStatus_t getStatus() const
    {
        return status_;
    }

    const SequenceNumber_t getSequenceNumber() const
    {
        return seq_num_;
    }

    FragmentNumber_t get_next_unsent_fragment() const
    {
        if (unsent_fragments_.empty())
        {
            return change_->getFragmentCount() + 1;
        }

        return unsent_fragments_.min();
    }

    FragmentNumberSet_t getUnsentFragments() const
    {
        return unsent_fragments_;
    }

    void markAllFragmentsAsUnsent()
    {
        assert(nullptr != change_);

        if (change_->getFragmentSize() != 0)
        {
            unsent_fragments_.base(1u);
            unsent_fragments_.add_range(1u, change_->getFragmentCount() + 1u);
        }
    }

    void markFragmentsAsSent(
            const FragmentNumber_t& sentFragment)
    {
        unsent_fragments_.remove(sentFragment);

        // We only use the running window mechanism during the first stage, until all fragments have been delivered
        // once, and we consider the whole change as delivered.
        if (!delivered_ && !unsent_fragments_.empty() && (unsent_fragments_.max() < change_->getFragmentCount()))
        {
            FragmentNumber_t base = unsent_fragments_.base();
            FragmentNumber_t max = unsent_fragments_.max();
            assert(!unsent_fragments_.is_set(base));

            // Update base to first bit set
            base = unsent_fragments_.min();
            unsent_fragments_.base_update(base);

            // Add all possible fragments
            unsent_fragments_.add_range(max + 1u, change_->getFragmentCount() + 1u);
        }
    }

    void markFragmentsAsUnsent(
            const FragmentNumberSet_t& unsentFragments)
    {
        // Ignore NACK_FRAG messages during the first stage, until all fragments have been delivered once, and we
        // consider the whole change as delivered.
        if (delivered_)
        {
            if (unsent_fragments_.empty())
            {
                // Current window is empty, so we can set it to the received one.
                unsent_fragments_ = unsentFragments;
            }
            else
            {
                // Update window to send the lowest possible requested fragments first.
                FragmentNumber_t other_base = unsentFragments.base();
                if (other_base < unsent_fragments_.base())
                {
                    unsent_fragments_.base_update(other_base);
                }
                unsentFragments.for_each(
                    [this](
                        FragmentNumber_t element)
                    {
                        unsent_fragments_.add(element);
                    });
            }
        }
    }

    bool has_been_delivered() const
    {
        return delivered_;
    }

    void set_delivered()
    {
        delivered_ = true;
    }

    void calm_mark_whole_repair_pending()
    {
        if (!calm_repair_pending_)
        {
            calm_first_nack_time_ = std::chrono::steady_clock::now();
        }
        ++calm_nack_count_;
        calm_repair_pending_ = true;
        calm_whole_sample_repair_ = true;
        calm_new_held_ = false;
        calm_new_queued_ = false;
        calm_repair_fragments_.clear();
        calm_repair_pending_bytes_ = change_ == nullptr ? 0 : change_->serializedPayload.length;
    }

    void calm_mark_fragment_repair_pending(
            const FragmentNumberSet_t& fragments)
    {
        if (!calm_repair_pending_)
        {
            calm_first_nack_time_ = std::chrono::steady_clock::now();
        }
        ++calm_nack_count_;
        calm_repair_pending_ = true;
        calm_new_held_ = false;
        calm_new_queued_ = false;

        if (calm_whole_sample_repair_ || change_ == nullptr)
        {
            return;
        }

        fragments.for_each(
            [this](
                FragmentNumber_t fragment)
            {
                if (calm_repair_fragments_.insert(fragment).second)
                {
                    calm_repair_pending_bytes_ += calm_fragment_size(fragment);
                }
            });
    }

    bool calm_repair_pending() const
    {
        return calm_repair_pending_;
    }

    uint64_t calm_repair_pending_bytes() const
    {
        return calm_repair_pending_bytes_;
    }

    uint32_t calm_nack_count() const
    {
        return calm_nack_count_;
    }

    void calm_note_repeated_nack()
    {
        ++calm_nack_count_;
    }

    double calm_repair_age_ms(
            const std::chrono::steady_clock::time_point& now) const
    {
        if (!calm_repair_pending_ ||
                calm_first_nack_time_ == std::chrono::steady_clock::time_point::min())
        {
            return 0.0;
        }

        return std::chrono::duration<double, std::milli>(
            now - calm_first_nack_time_).count();
    }

    uint64_t calm_fragment_payload_size(
            FragmentNumber_t fragment) const
    {
        if (fragment == 0)
        {
            return change_ == nullptr ? 0 : change_->serializedPayload.length;
        }
        return calm_fragment_size(fragment);
    }

    /**
     * Keep only the oldest currently requested fragments that fit a byte budget.
     *
     * The fragments omitted from this attempt remain part of the receiver-side
     * loss state and will be reported by a later NACK_FRAG.  This lets CALM
     * enforce a real byte window without changing the RTPS wire protocol.
     * An unfragmented sample, or a single fragment larger than the budget, is
     * kept whole because it cannot be split at this layer.
     */
    uint64_t calm_limit_unsent_fragments(
            uint64_t budget_bytes)
    {
        if (change_ == nullptr || budget_bytes == 0)
        {
            return 0;
        }

        if (change_->getFragmentSize() == 0 || change_->getFragmentCount() == 0)
        {
            return change_->serializedPayload.length;
        }

        FragmentNumberSet_t selected;
        selected.base(unsent_fragments_.empty() ? 1u : unsent_fragments_.base());
        FragmentNumberSet_t deferred;
        deferred.base(unsent_fragments_.empty() ? 1u : unsent_fragments_.base());
        uint64_t selected_bytes = 0;
        bool budget_full = false;
        unsent_fragments_.for_each(
            [&](FragmentNumber_t fragment)
            {
                if (budget_full)
                {
                    deferred.add(fragment);
                    return;
                }
                const uint64_t fragment_bytes = calm_fragment_size(fragment);
                if (fragment_bytes == 0)
                {
                    return;
                }

                const uint64_t remaining = budget_bytes > selected_bytes ?
                        budget_bytes - selected_bytes : 0;
                if (selected_bytes == 0 || fragment_bytes <= remaining)
                {
                    selected.add(fragment);
                    selected_bytes += fragment_bytes;
                }
                else
                {
                    budget_full = true;
                    deferred.add(fragment);
                }
            });

        if (selected_bytes > 0)
        {
            unsent_fragments_ = selected;
            calm_deferred_fragments_ = deferred;
        }
        return selected_bytes;
    }

    bool calm_activate_deferred_fragments()
    {
        if (!unsent_fragments_.empty() || calm_deferred_fragments_.empty())
        {
            return false;
        }

        unsent_fragments_ = calm_deferred_fragments_;
        calm_deferred_fragments_ = FragmentNumberSet_t();
        status_ = REQUESTED;
        return true;
    }

    void calm_mark_retransmit_sent()
    {
        calm_mark_repair_transmitted(0);
    }

    /**
     * Record an actual repair transmission without counting each paced
     * fragment batch as a separate retry. A new N_i round starts only for the
     * first repair, or after feedback proved that the previous round failed.
     */
    void calm_mark_repair_transmitted(
            FragmentNumber_t fragment)
    {
        if (!calm_repair_pending_)
        {
            return;
        }

        if (calm_retransmit_count_ == 0 || calm_next_repair_round_pending_)
        {
            ++calm_retransmit_count_;
            calm_next_repair_round_pending_ = false;
            calm_awaiting_repair_feedback_ = true;
            calm_failed_feedback_recorded_ = false;
            calm_transmitted_whole_sample_ = false;
            calm_transmitted_repair_fragments_.clear();
        }

        if (fragment == 0)
        {
            calm_transmitted_whole_sample_ = true;
        }
        else
        {
            calm_transmitted_repair_fragments_.insert(fragment);
        }
        calm_last_retransmit_time_ = std::chrono::steady_clock::now();
    }

    /**
     * Count one failed repair feedback round when a whole-sample request
     * arrives after this sample was actually retransmitted.
     */
    bool calm_note_whole_nack_after_repair(
            double minimum_feedback_age_ms = 0.0)
    {
        return calm_note_failed_repair_feedback(
            true, nullptr, minimum_feedback_age_ms);
    }

    /**
     * Count one failed repair feedback round only when the NACK_FRAG overlaps
     * fragments actually sent in the latest repair epoch.
     */
    bool calm_note_fragment_nack_after_repair(
            const FragmentNumberSet_t& fragments,
            double minimum_feedback_age_ms = 0.0,
            bool require_full_stall = false)
    {
        return calm_note_failed_repair_feedback(
            false, &fragments, minimum_feedback_age_ms, require_full_stall);
    }

    uint32_t calm_failed_repair_count() const
    {
        return calm_failed_repair_count_;
    }

    double calm_last_failed_repair_severity() const
    {
        return calm_last_failed_repair_severity_;
    }

    bool calm_retry_ready(
            const std::chrono::steady_clock::time_point& now,
            double cooldown_ms) const
    {
        if (calm_retransmit_count_ == 0)
        {
            return true;
        }

        return std::chrono::duration<double, std::milli>(
            now - calm_last_retransmit_time_).count() >= cooldown_ms;
    }

    uint32_t calm_retransmit_count() const
    {
        return calm_retransmit_count_;
    }

    bool calm_retransmitted_since(
            const std::chrono::steady_clock::time_point& since) const
    {
        return calm_retransmit_count_ > 0 &&
               since != std::chrono::steady_clock::time_point::min() &&
               calm_last_retransmit_time_ >= since;
    }

    bool calm_note_failed_repair_feedback(
            bool whole_sample_request,
            const FragmentNumberSet_t* fragments,
            double minimum_feedback_age_ms,
            bool require_full_stall = false)
    {
        if (!calm_awaiting_repair_feedback_ || calm_failed_feedback_recorded_)
        {
            return false;
        }

        if (calm_last_retransmit_time_ != std::chrono::steady_clock::time_point::min() &&
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - calm_last_retransmit_time_).count() <
                minimum_feedback_age_ms)
        {
            return false;
        }

        bool overlaps_latest_repair = whole_sample_request;
        bool feedback_made_progress = false;
        double failure_severity = 1.0;
        if (!whole_sample_request && fragments != nullptr)
        {
            FragmentNumberSet_t::bitmap_type bitmap;
            uint32_t scope_bits = 0;
            uint32_t bitmap_longs = 0;
            fragments->bitmap_get(scope_bits, bitmap, bitmap_longs);
            const FragmentNumber_t scope_begin = fragments->base();
            const FragmentNumber_t scope_end = scope_begin + scope_bits;
            uint64_t retransmitted_scope_bytes = 0;
            uint64_t rerequested_bytes = 0;

            if (calm_transmitted_whole_sample_ && change_ != nullptr)
            {
                for (FragmentNumber_t fragment = scope_begin;
                        fragment < scope_end && fragment <= change_->getFragmentCount();
                        ++fragment)
                {
                    const uint64_t bytes = calm_fragment_size(fragment);
                    retransmitted_scope_bytes += bytes;
                    if (fragments->is_set(fragment))
                    {
                        rerequested_bytes += bytes;
                    }
                }
            }
            else
            {
                for (FragmentNumber_t fragment : calm_transmitted_repair_fragments_)
                {
                    if (fragment >= scope_begin && fragment < scope_end)
                    {
                        const uint64_t bytes = calm_fragment_size(fragment);
                        retransmitted_scope_bytes += bytes;
                        if (fragments->is_set(fragment))
                        {
                            rerequested_bytes += bytes;
                        }
                    }
                }
            }
            overlaps_latest_repair = rerequested_bytes > 0;
            if (retransmitted_scope_bytes > 0)
            {
                feedback_made_progress = rerequested_bytes < retransmitted_scope_bytes;
                failure_severity = std::min(
                    1.0,
                    static_cast<double>(rerequested_bytes) /
                    static_cast<double>(retransmitted_scope_bytes));
            }
        }
        if (!overlaps_latest_repair)
        {
            return false;
        }
        if (require_full_stall && feedback_made_progress)
        {
            calm_last_failed_repair_severity_ = failure_severity;
            return false;
        }

        ++calm_failed_repair_count_;
        calm_last_failed_repair_severity_ = failure_severity;
        calm_failed_feedback_recorded_ = true;
        calm_awaiting_repair_feedback_ = false;
        calm_next_repair_round_pending_ = true;
        return true;
    }

    void calm_hold_new()
    {
        calm_new_held_ = true;
        calm_new_queued_ = false;
    }

    void calm_release_new(
            uint64_t credit_bytes)
    {
        calm_new_held_ = false;
        calm_new_queued_ = true;
        calm_new_credit_limited_ = true;
        calm_new_release_credit_bytes_ = credit_bytes;
    }

    bool calm_new_held() const
    {
        return calm_new_held_;
    }

    void calm_mark_new_queued()
    {
        calm_new_held_ = false;
        calm_new_queued_ = true;
        calm_new_credit_limited_ = false;
        calm_new_release_credit_bytes_ = 0;
    }

    bool calm_new_queued() const
    {
        return calm_new_queued_;
    }

    bool calm_consume_new_release_credit(
            FragmentNumber_t fragment)
    {
        if (!calm_new_queued_ || !calm_new_credit_limited_)
        {
            return false;
        }

        const uint64_t fragment_bytes = calm_fragment_size(fragment);
        calm_new_release_credit_bytes_ =
                fragment_bytes >= calm_new_release_credit_bytes_ ?
                0 : calm_new_release_credit_bytes_ - fragment_bytes;
        return calm_new_release_credit_bytes_ == 0;
    }

private:

    uint64_t calm_fragment_size(
            FragmentNumber_t fragment) const
    {
        const uint64_t payload_length = change_->serializedPayload.length;
        const uint32_t fragment_size = change_->getFragmentSize();
        const uint32_t fragment_count = change_->getFragmentCount();

        if (fragment == 0 || fragment_size == 0 || fragment > fragment_count)
        {
            return 0;
        }

        const uint64_t offset = static_cast<uint64_t>(fragment - 1) * fragment_size;
        if (offset >= payload_length)
        {
            return 0;
        }

        return std::min<uint64_t>(fragment_size, payload_length - offset);
    }

    //!Status
    ChangeForReaderStatus_t status_;

    //!Sequence number
    SequenceNumber_t seq_num_;

    CacheChange_t* change_;

    FragmentNumberSet_t unsent_fragments_;

    //! Requested fragments deferred to a later CALM pacing batch.
    FragmentNumberSet_t calm_deferred_fragments_;

    //! Indicates if was delivered at least once.
    bool delivered_ = false;

    //! True after this reader NACKs the change and until the change is cumulatively ACKed.
    bool calm_repair_pending_ = false;

    //! True when an ACKNACK requested the whole sample instead of specific fragments.
    bool calm_whole_sample_repair_ = false;

    //! Payload bytes that have entered the NACK-induced repair lifecycle.
    uint64_t calm_repair_pending_bytes_ = 0;

    //! Fragments already accounted in calm_repair_pending_bytes_.
    std::set<FragmentNumber_t> calm_repair_fragments_;

    //! Number of accepted NACK events observed for this reader/change pair.
    uint32_t calm_nack_count_ = 0;

    //! Time when this unresolved repair debt first entered the NACK lifecycle.
    std::chrono::steady_clock::time_point calm_first_nack_time_ =
            std::chrono::steady_clock::time_point::min();

    //! Number of completed repair releases for this reader/change pair.
    uint32_t calm_retransmit_count_ = 0;

    //! Number of repair epochs followed by feedback requesting the same bytes.
    uint32_t calm_failed_repair_count_ = 0;

    //! Fraction of bytes sent in the feedback scope that were requested again.
    double calm_last_failed_repair_severity_ = 1.0;

    //! True after a repair epoch until matching ACK/NACK feedback is observed.
    bool calm_awaiting_repair_feedback_ = false;

    //! Prevents duplicate NACK submessages from counting twice for one epoch.
    bool calm_failed_feedback_recorded_ = false;

    //! Arms the next N_i increment after a failed repair feedback round.
    bool calm_next_repair_round_pending_ = false;

    //! Latest repair epoch sent the entire unfragmented sample.
    bool calm_transmitted_whole_sample_ = false;

    //! Fragment numbers actually transmitted in the latest repair epoch.
    std::set<FragmentNumber_t> calm_transmitted_repair_fragments_;

    //! Completion time of the latest repair release, used to await receiver feedback.
    std::chrono::steady_clock::time_point calm_last_retransmit_time_ =
            std::chrono::steady_clock::time_point::min();

    //! New data retained in history for CALM's repair-first shared byte budget.
    bool calm_new_held_ = false;

    //! New data has already been handed to the FlowController and must not be held again.
    bool calm_new_queued_ = false;

    //! A paced held-new release may send only this many payload bytes.
    uint64_t calm_new_release_credit_bytes_ = 0;

    //! False for ordinary default-path new data, true for a CALM paced release.
    bool calm_new_credit_limited_ = false;
};

struct ChangeForReaderCmp
{
    bool operator ()(
            const ChangeForReader_t& a,
            const ChangeForReader_t& b) const
    {
        return a.seq_num_ < b.seq_num_;
    }

};

#endif // ifndef DOXYGEN_SHOULD_SKIP_THIS_PUBLIC

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima

#endif /* _FASTDDS_RTPS_CHANGEFORREADER_H_ */
