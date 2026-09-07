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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <chrono>
#include <initializer_list>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <fastrtps/rtps/writer/ReaderProxy.h>
#include <fastrtps/rtps/writer/StatefulWriter.h>
#include <rtps/messages/RTPSGapBuilder.hpp>

//using namespace eprosima::fastrtps::rtps;
namespace eprosima {
namespace fastrtps {
namespace rtps {

namespace
{

class ScopedEnvironment
{
public:

    ScopedEnvironment(
            std::initializer_list<std::pair<const char*, const char*>> values)
    {
        for (const auto& value : values)
        {
            const char* previous = std::getenv(value.first);
            previous_.emplace_back(
                value.first,
                previous != nullptr,
                previous == nullptr ? "" : previous);
            setenv(value.first, value.second, 1);
        }
    }

    ~ScopedEnvironment()
    {
        for (const auto& value : previous_)
        {
            if (std::get<1>(value))
            {
                setenv(std::get<0>(value).c_str(), std::get<2>(value).c_str(), 1);
            }
            else
            {
                unsetenv(std::get<0>(value).c_str());
            }
        }
    }

private:

    std::vector<std::tuple<std::string, bool, std::string>> previous_;
};

}  // namespace

TEST(ReaderProxyTests, find_change_test)
{
    //RemoteReaderAttributes rattr;
    StatefulWriter writerMock;
    WriterTimes wTimes;
    RemoteLocatorsAllocationAttributes alloc;
    ReaderProxy rproxy(wTimes, alloc, &writerMock);
    CacheChange_t seq1; seq1.sequenceNumber = {0, 1};
    CacheChange_t seq2; seq2.sequenceNumber = {0, 2};
    CacheChange_t seq3; seq3.sequenceNumber = {0, 3};
    CacheChange_t seq6; seq6.sequenceNumber = {0, 6};
    CacheChange_t seq7; seq7.sequenceNumber = {0, 7};

    rproxy.add_change(ChangeForReader_t(&seq1), true, false);
    rproxy.add_change(ChangeForReader_t(&seq2), true, false);
    rproxy.add_change(ChangeForReader_t(&seq3), true, false);
    //rproxy.add_change(ChangeForReader_t(&seq4), false); // GAP
    //rproxy.add_change(ChangeForReader_t(&seq5), false); // GAP
    rproxy.add_change(ChangeForReader_t(&seq6), true, false);
    rproxy.add_change(ChangeForReader_t(&seq7), true, false);

    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 1)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 2)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 3)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 4)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 5)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 6)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 7)));

    rproxy.acked_changes_set(SequenceNumber_t(0, 3));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 1)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 2)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 3)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 4)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 5)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 6)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 7)));

    rproxy.acked_changes_set(SequenceNumber_t(0, 3)); // AGAIN SAME ACK
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 1)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 2)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 3)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 4)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 5)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 6)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 7)));

    rproxy.acked_changes_set(SequenceNumber_t(0, 5)); // AGAIN SAME ACK
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 1)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 2)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 3)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 4)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 5)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 6)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 7)));
}

TEST(ReaderProxyTests, find_change_removed_test)
{
    //RemoteReaderAttributes rattr;
    StatefulWriter writerMock;
    WriterTimes wTimes;
    RemoteLocatorsAllocationAttributes alloc;
    ReaderProxy rproxy(wTimes, alloc, &writerMock);
    CacheChange_t seq1; seq1.sequenceNumber = {0, 1};
    CacheChange_t seq2; seq2.sequenceNumber = {0, 2};
    CacheChange_t seq3; seq3.sequenceNumber = {0, 3};
    CacheChange_t seq4; seq4.sequenceNumber = {0, 4};

    rproxy.add_change(ChangeForReader_t(&seq1), true, false);
    rproxy.add_change(ChangeForReader_t(&seq2), true, false);
    rproxy.change_has_been_removed(SequenceNumber_t(0, 1));
    rproxy.add_change(ChangeForReader_t(&seq3), true, false);
    rproxy.change_has_been_removed(SequenceNumber_t(0, 2));
    rproxy.add_change(ChangeForReader_t(&seq4), true, false);

    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 1)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 2)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 3)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 4)));

    rproxy.acked_changes_set(SequenceNumber_t(0, 2));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 1)));
    ASSERT_TRUE(rproxy.change_is_acked(SequenceNumber_t(0, 2)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 3)));
    ASSERT_FALSE(rproxy.change_is_acked(SequenceNumber_t(0, 4)));
}

// Regression test for #13556 (Github #2423)
TEST(ReaderProxyTests, requested_changes_set_test)
{
    StatefulWriter writerMock;
    WriterTimes wTimes;
    RemoteLocatorsAllocationAttributes alloc;
    ReaderProxy rproxy(wTimes, alloc, &writerMock);
    CacheChange_t seq1; seq1.sequenceNumber = {0, 1};
    CacheChange_t seq2; seq2.sequenceNumber = {0, 2};
    CacheChange_t seq3; seq3.sequenceNumber = {0, 3};
    CacheChange_t seq4; seq4.sequenceNumber = {0, 4};
    RTPSMessageGroup message_group(nullptr, false);
    RTPSGapBuilder gap_builder(message_group);

    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    rproxy.start(reader_attributes);


    rproxy.add_change(ChangeForReader_t(&seq1), false, false);
    rproxy.add_change(ChangeForReader_t(&seq2), true, false);
    rproxy.add_change(ChangeForReader_t(&seq3), true, false);
    rproxy.add_change(ChangeForReader_t(&seq4), false, false);

    SequenceNumberSet_t set({0, 1});
    set.add({0, 1});
    set.add({0, 2});
    set.add({0, 3});
    set.add({0, 4});

    EXPECT_CALL(gap_builder, add(SequenceNumber_t(0, 1))).Times(1).WillOnce(testing::Return(true));
    EXPECT_CALL(gap_builder, add(SequenceNumber_t(0, 4))).Times(1).WillOnce(testing::Return(true));

    rproxy.requested_changes_set(set, gap_builder, {0, 1});
}

TEST(ReaderProxyTests, calm_failed_feedback_reduces_batch_interval_and_release_rate)
{
    ScopedEnvironment environment({
                {"FASTDDS_CALM_ENABLED", "1"},
                {"FASTDDS_CALM_CONTROLLER", "storm_aimd"},
                {"FASTDDS_CALM_DETECTOR_RHO_BYTES", "1"},
                {"FASTDDS_CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM_DETECTOR_RETRY_SIGNAL", "n"},
                {"FASTDDS_CALM_DETECTOR_RETRY_COUNT", "1"},
                {"FASTDDS_CALM_DETECTOR_ACK_STALL_MS", "0"},
                {"FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS", "0"},
                {"FASTDDS_CALM_RETRY_COOLDOWN_MS", "0"},
                {"FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_STORM_MIN_BUDGET_BYTES", "1000"},
                {"FASTDDS_CALM_MAX_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_BUDGET_HORIZON_MS", "20"},
                {"FASTDDS_CALM_STORM_GAMMA", "0.5"},
                {"FASTDDS_CALM_STORM_RATE_GAMMA", "0.75"},
                {"FASTDDS_CALM_MIN_REPAIR_RATE_MBPS", "1"},
                {"FASTDDS_CALM_MAX_REPAIR_RATE_MBPS", "1000"},
                {"FASTDDS_CALM_PACING_MS", "0.1"},
                {"FASTDDS_CALM_PACING_DRAIN_GUARD", "1.0"}
            });

    StatefulWriter writer_mock;
    WriterTimes writer_times;
    RemoteLocatorsAllocationAttributes allocation;
    ReaderProxy reader(writer_times, allocation, &writer_mock);
    CacheChange_t sample;
    sample.sequenceNumber = {0, 1};
    sample.serializedPayload.length = 100000;

    RTPSMessageGroup message_group(nullptr, false);
    RTPSGapBuilder gap_builder(message_group);
    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    reader.start(reader_attributes);
    reader.add_change(ChangeForReader_t(&sample), true, false);
    reader.from_unsent_to_status(sample.sequenceNumber, UNACKNOWLEDGED, false);

    SequenceNumberSet_t request({0, 1});
    request.add({0, 1});
    ASSERT_TRUE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));
    reader.calm_evaluate_before_tnr();
    ASSERT_FALSE(reader.calm_controls_repair());

    // This test isolates the controller decrease after activation. Production
    // defaults use N_oldest=2; the detector threshold is lowered to one here.
    uint64_t released_bytes = 0;
    ASSERT_EQ(reader.perform_acknack_response_limited(
                100000u, nullptr, released_bytes), 1u);
    ASSERT_EQ(released_bytes, 100000u);
    reader.storm_observe_transport_repair(sample.sequenceNumber, 0);
    reader.from_unsent_to_status(sample.sequenceNumber, UNDERWAY, false);
    ASSERT_TRUE(reader.perform_nack_supression());
    ASSERT_FALSE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));

    ASSERT_TRUE(reader.calm_controls_repair());
    ASSERT_EQ(reader.calm_update_budget(), 0u);
    ASSERT_TRUE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));
    ASSERT_EQ(reader.calm_update_budget(), 100000u);

    const uint64_t previous_budget = reader.calm_total_release_budget_bytes();
    const double previous_period_ms = reader.calm_pacing_period_ms();
    const double previous_rate_mbps = reader.calm_release_rate_mbps();
    EXPECT_EQ(previous_budget, 100000u);
    EXPECT_NEAR(previous_period_ms, 20.0, 0.01);
    EXPECT_NEAR(previous_rate_mbps, 40.0, 0.01);

    released_bytes = 0;
    ASSERT_EQ(reader.perform_acknack_response_limited(
                previous_budget, nullptr, released_bytes), 1u);
    ASSERT_EQ(released_bytes, 100000u);
    reader.calm_note_release(released_bytes, true);
    reader.storm_observe_transport_repair(sample.sequenceNumber, 0);
    reader.from_unsent_to_status(sample.sequenceNumber, UNDERWAY, false);
    ASSERT_EQ(reader.calm_oldest_retransmit_count(), 2u);
    ASSERT_EQ(reader.calm_oldest_failed_repair_count(), 1u);
    ASSERT_TRUE(reader.perform_nack_supression());

    ASSERT_TRUE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));
    ASSERT_EQ(reader.calm_oldest_failed_repair_count(), 2u);
    const uint64_t reduced_budget = reader.calm_total_release_budget_bytes();
    const double reduced_period_ms = reader.calm_pacing_period_ms();
    const double reduced_rate_mbps = reader.calm_release_rate_mbps();

    EXPECT_EQ(reduced_budget, 50000u);
    EXPECT_NEAR(reduced_period_ms, 100.0 / 7.5, 0.05);
    EXPECT_NEAR(reduced_rate_mbps, 30.0, 0.01);
    EXPECT_LT(reduced_period_ms, previous_period_ms);
    EXPECT_LT(reduced_rate_mbps, previous_rate_mbps);
    EXPECT_LT(
        static_cast<double>(reduced_budget) / reduced_period_ms,
        static_cast<double>(previous_budget) / previous_period_ms);
}

TEST(ReaderProxyTests, calm4_failed_feedback_reduces_only_budget_with_fixed_pacing)
{
    ScopedEnvironment environment({
                {"FASTDDS_CALM_ENABLED", "1"},
                {"FASTDDS_CALM_CONTROLLER", "calm4"},
                {"FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS", "0"},
                {"FASTDDS_CALM_RETRY_COOLDOWN_MS", "0"},
                {"FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_STORM_MIN_BUDGET_BYTES", "1000"},
                {"FASTDDS_CALM_MAX_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM4_K_DEC", "0.25"},
                {"FASTDDS_CALM_PACING_MS", "50"}
            });

    StatefulWriter writer_mock;
    WriterTimes writer_times;
    RemoteLocatorsAllocationAttributes allocation;
    ReaderProxy reader(writer_times, allocation, &writer_mock);
    CacheChange_t sample;
    sample.sequenceNumber = {0, 1};
    sample.serializedPayload.length = 100000;

    RTPSMessageGroup message_group(nullptr, false);
    RTPSGapBuilder gap_builder(message_group);
    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    reader.start(reader_attributes);
    reader.add_change(ChangeForReader_t(&sample), true, false);
    reader.from_unsent_to_status(sample.sequenceNumber, UNACKNOWLEDGED, false);

    SequenceNumberSet_t request({0, 1});
    request.add({0, 1});
    ASSERT_TRUE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));
    ASSERT_FALSE(reader.calm_controls_repair());

    uint64_t released_bytes = 0;
    ASSERT_EQ(reader.perform_acknack_response_limited(
                100000u, nullptr, released_bytes), 1u);
    ASSERT_EQ(released_bytes, 100000u);
    reader.storm_observe_transport_repair(sample.sequenceNumber, 0);
    reader.from_unsent_to_status(sample.sequenceNumber, UNDERWAY, false);
    ASSERT_TRUE(reader.perform_nack_supression());

    // The feedback that activates CALM closes the failed round but is not
    // re-enqueued in the same call; pacing handles the retained repair debt.
    ASSERT_FALSE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));
    EXPECT_TRUE(reader.calm_controls_repair());
    EXPECT_EQ(reader.calm_total_release_budget_bytes(), 75000u);
    EXPECT_NEAR(reader.calm_pacing_period_ms(), 50.0, 0.01);
}

TEST(ReaderProxyTests, calm41_first_failed_feedback_activates_with_fixed_pacing)
{
    ScopedEnvironment environment({
                {"FASTDDS_CALM_ENABLED", "1"},
                {"FASTDDS_CALM_CONTROLLER", "calm41"},
                {"FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS", "0"},
                {"FASTDDS_CALM_RETRY_COOLDOWN_MS", "0"},
                {"FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_STORM_MIN_BUDGET_BYTES", "1000"},
                {"FASTDDS_CALM_MAX_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM4_K_DEC", "0.25"},
                {"FASTDDS_CALM41_TIMEOUT_RTT_MULTIPLIER", "4"},
                {"FASTDDS_CALM_PACING_MS", "50"}
            });

    StatefulWriter writer_mock;
    WriterTimes writer_times;
    RemoteLocatorsAllocationAttributes allocation;
    ReaderProxy reader(writer_times, allocation, &writer_mock);
    CacheChange_t sample;
    sample.sequenceNumber = {0, 1};
    sample.serializedPayload.length = 100000;

    RTPSMessageGroup message_group(nullptr, false);
    RTPSGapBuilder gap_builder(message_group);
    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    reader.start(reader_attributes);
    reader.add_change(ChangeForReader_t(&sample), true, false);
    reader.from_unsent_to_status(sample.sequenceNumber, UNACKNOWLEDGED, false);

    SequenceNumberSet_t request({0, 1});
    request.add({0, 1});
    ASSERT_TRUE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));
    ASSERT_FALSE(reader.calm_controls_repair());

    uint64_t released_bytes = 0;
    ASSERT_EQ(reader.perform_acknack_response_limited(
                100000u, nullptr, released_bytes), 1u);
    ASSERT_EQ(released_bytes, 100000u);
    reader.storm_observe_transport_repair(sample.sequenceNumber, 0);
    reader.from_unsent_to_status(sample.sequenceNumber, UNDERWAY, false);
    ASSERT_TRUE(reader.perform_nack_supression());

    ASSERT_FALSE(reader.requested_changes_set(request, gap_builder, sample.sequenceNumber));
    EXPECT_TRUE(reader.calm_controls_repair());
    EXPECT_EQ(reader.calm_oldest_failed_repair_count(), 1u);
    EXPECT_EQ(reader.calm_total_release_budget_bytes(), 75000u);
    EXPECT_NEAR(reader.calm_pacing_period_ms(), 50.0, 0.01);
}

TEST(ReaderProxyTests, calm41_entry_ack_pacing_uses_pre_entry_ack_goodput)
{
    ScopedEnvironment environment({
                {"FASTDDS_CALM_ENABLED", "1"},
                {"FASTDDS_CALM_CONTROLLER", "calm41"},
                {"FASTDDS_CALM41_PACING_MODE", "entry_ack"},
                {"FASTDDS_CALM41_PACING_ETA", "1"},
                {"FASTDDS_CALM_STORM_FEEDBACK_MIN_AGE_MS", "0"},
                {"FASTDDS_CALM_RETRY_COOLDOWN_MS", "0"},
                {"FASTDDS_CALM_STORM_INITIAL_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_STORM_MIN_BUDGET_BYTES", "1000"},
                {"FASTDDS_CALM_MAX_BUDGET_BYTES", "100000"},
                {"FASTDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER", "0"},
                {"FASTDDS_CALM4_K_DEC", "0.25"},
                {"FASTDDS_CALM_PACING_MS", "50"}
            });

    StatefulWriter writer_mock;
    WriterTimes writer_times;
    RemoteLocatorsAllocationAttributes allocation;
    ReaderProxy reader(writer_times, allocation, &writer_mock);
    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    reader.start(reader_attributes);

    CacheChange_t first;
    CacheChange_t second;
    CacheChange_t repair;
    first.sequenceNumber = {0, 1};
    second.sequenceNumber = {0, 2};
    repair.sequenceNumber = {0, 3};
    first.serializedPayload.length = 100000;
    second.serializedPayload.length = 100000;
    repair.serializedPayload.length = 100000;
    reader.add_change(ChangeForReader_t(&first), true, false);
    reader.add_change(ChangeForReader_t(&second), true, false);
    reader.add_change(ChangeForReader_t(&repair), true, false);

    reader.acked_changes_set({0, 2});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    reader.acked_changes_set({0, 3});
    reader.from_unsent_to_status(repair.sequenceNumber, UNACKNOWLEDGED, false);

    RTPSMessageGroup message_group(nullptr, false);
    RTPSGapBuilder gap_builder(message_group);
    SequenceNumberSet_t request(repair.sequenceNumber);
    request.add(repair.sequenceNumber);
    ASSERT_TRUE(reader.requested_changes_set(request, gap_builder, repair.sequenceNumber));

    uint64_t released_bytes = 0;
    ASSERT_EQ(reader.perform_acknack_response_limited(
                100000u, nullptr, released_bytes), 1u);
    reader.storm_observe_transport_repair(repair.sequenceNumber, 0);
    reader.from_unsent_to_status(repair.sequenceNumber, UNDERWAY, false);
    ASSERT_TRUE(reader.perform_nack_supression());
    ASSERT_FALSE(reader.requested_changes_set(request, gap_builder, repair.sequenceNumber));

    EXPECT_TRUE(reader.calm_controls_repair());
    reader.calm_update_budget();
    EXPECT_LT(reader.calm_pacing_period_ms(), 30.0);
    EXPECT_GT(reader.calm_pacing_period_ms(), 5.0);
}

TEST(ReaderProxyTests, calm_detector_rho_threshold_uses_reader_sample_average)
{
    ScopedEnvironment environment({
                {"FASTDDS_CALM_DETECTOR_RHO_BYTES", "999"},
                {"FASTDDS_CALM_DETECTOR_RHO_SAMPLE_MULTIPLIER", "4"}
            });

    StatefulWriter writer_mock;
    WriterTimes writer_times;
    RemoteLocatorsAllocationAttributes allocation;
    ReaderProxy reader(writer_times, allocation, &writer_mock);
    CacheChange_t first;
    first.sequenceNumber = {0, 1};
    first.serializedPayload.length = 1000;
    CacheChange_t second;
    second.sequenceNumber = {0, 2};
    second.serializedPayload.length = 3000;

    reader.add_change(ChangeForReader_t(&first), true, false);
    reader.add_change(ChangeForReader_t(&second), true, false);

    EXPECT_EQ(reader.calm_average_sample_bytes(), 2000u);
    EXPECT_EQ(reader.calm_detector_rho_threshold_bytes(), 8000u);
}

FragmentNumber_t mark_next_fragment_sent(
        ReaderProxy& rproxy,
        SequenceNumber_t sequence_number,
        FragmentNumber_t expected_fragment)
{
    FragmentNumber_t next_fragment{expected_fragment};
    SequenceNumber_t gap_seq;
    SequenceNumber_t min_seq;
    bool need_reactivate_periodic_heartbeat;
    rproxy.change_is_unsent(sequence_number, next_fragment, gap_seq, min_seq, need_reactivate_periodic_heartbeat);
    if (next_fragment != expected_fragment)
    {
        return next_fragment;
    }

    bool was_last_fragment;
    rproxy.mark_fragment_as_sent_for_change(sequence_number, next_fragment, was_last_fragment);
    return next_fragment;
}

TEST(ChangeForReaderTests, calm_byte_budget_keeps_oldest_fragments)
{
    CacheChange_t cache_change;
    cache_change.sequenceNumber = {0, 1};
    cache_change.serializedPayload.length = 1040;
    cache_change.setFragmentSize(100);

    ChangeForReader_t change(&cache_change);
    change.set_delivered();
    ASSERT_EQ(change.calm_limit_unsent_fragments(350), 300u);

    const FragmentNumberSet_t selected = change.getUnsentFragments();
    ASSERT_TRUE(selected.is_set(1));
    ASSERT_TRUE(selected.is_set(2));
    ASSERT_TRUE(selected.is_set(3));
    ASSERT_FALSE(selected.is_set(4));
    ASSERT_FALSE(selected.is_set(11));
}

TEST(ChangeForReaderTests, calm_deferred_fragments_resume_in_next_window)
{
    CacheChange_t cache_change;
    cache_change.sequenceNumber = {0, 1};
    cache_change.serializedPayload.length = 1040;
    cache_change.setFragmentSize(100);

    ChangeForReader_t change(&cache_change);
    change.set_delivered();
    ASSERT_EQ(change.calm_limit_unsent_fragments(350), 300u);
    change.markFragmentsAsSent(1);
    change.markFragmentsAsSent(2);
    change.markFragmentsAsSent(3);

    ASSERT_TRUE(change.calm_activate_deferred_fragments());
    ASSERT_EQ(change.getStatus(), REQUESTED);
    ASSERT_EQ(change.get_next_unsent_fragment(), 4u);

}

TEST(ChangeForReaderTests, calm_counts_actual_repair_epochs_and_failed_feedback)
{
    CacheChange_t cache_change;
    cache_change.sequenceNumber = {0, 1};
    cache_change.serializedPayload.length = 1000;
    cache_change.setFragmentSize(100);

    ChangeForReader_t change(&cache_change);
    change.set_delivered();
    FragmentNumberSet_t fragment_four;
    fragment_four.base(1);
    fragment_four.add(4);
    change.calm_mark_fragment_repair_pending(fragment_four);

    change.calm_mark_repair_transmitted(4);
    change.calm_mark_repair_transmitted(5);
    ASSERT_EQ(change.calm_retransmit_count(), 1u);
    ASSERT_EQ(change.calm_failed_repair_count(), 0u);

    ASSERT_TRUE(change.calm_note_fragment_nack_after_repair(fragment_four));
    ASSERT_FALSE(change.calm_note_fragment_nack_after_repair(fragment_four));
    ASSERT_EQ(change.calm_failed_repair_count(), 1u);

    change.calm_mark_repair_transmitted(4);
    ASSERT_EQ(change.calm_retransmit_count(), 2u);

    FragmentNumberSet_t different_fragment;
    different_fragment.base(1);
    different_fragment.add(7);
    ASSERT_FALSE(change.calm_note_fragment_nack_after_repair(different_fragment));
    ASSERT_EQ(change.calm_failed_repair_count(), 1u);

    ASSERT_TRUE(change.calm_note_fragment_nack_after_repair(fragment_four));
    ASSERT_EQ(change.calm_failed_repair_count(), 2u);
}

TEST(ReaderProxyTests, process_nack_frag_single_fragment_different_windows_test)
{
    constexpr FragmentNumber_t TOTAL_NUMBER_OF_FRAGMENTS = 400;
    constexpr uint16_t FRAGMENT_SIZE = 100;

    StatefulWriter writerMock;
    WriterTimes wTimes;
    RemoteLocatorsAllocationAttributes alloc;
    ReaderProxy rproxy(wTimes, alloc, &writerMock);
    CacheChange_t seq;
    seq.sequenceNumber = {0, 1};
    seq.serializedPayload.length = TOTAL_NUMBER_OF_FRAGMENTS * FRAGMENT_SIZE;
    seq.setFragmentSize(FRAGMENT_SIZE);

    RTPSMessageGroup message_group(nullptr, false);
    RTPSGapBuilder gap_builder(message_group);

    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    rproxy.start(reader_attributes);

    ChangeForReader_t change(&seq);
    rproxy.add_change(change, true, false);

    SequenceNumberSet_t sequence_number_set({0, 1});
    sequence_number_set.add({0, 1});
    rproxy.from_unsent_to_status(seq.sequenceNumber, UNACKNOWLEDGED, false, false);
    rproxy.requested_changes_set(sequence_number_set, gap_builder, seq.sequenceNumber);

    // The number of sent fragments should be higher than the FragmentNumberSet_t size.
    constexpr FragmentNumber_t NUMBER_OF_SENT_FRAGMENTS = 259;

    for (auto i = 1u; i <= NUMBER_OF_SENT_FRAGMENTS; ++i)
    {
        ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber, i), i);
    }

    // Set the change status to UNSENT.
    rproxy.perform_acknack_response(nullptr);

    // The difference between the latest sent fragment and an undelivered fragment should also be higher than
    // the FragmentNumberSet_t size.
    constexpr FragmentNumber_t UNDELIVERED_FRAGMENT = 3;
    FragmentNumberSet_t undelivered_fragment_set(UNDELIVERED_FRAGMENT);
    undelivered_fragment_set.add(UNDELIVERED_FRAGMENT);

    rproxy.process_nack_frag({}, 1, seq.sequenceNumber, undelivered_fragment_set);

    // Nack data should be ignored: first, complete the sequential delivering of the remaining fragments.
    for (auto i = NUMBER_OF_SENT_FRAGMENTS + 1u; i <= TOTAL_NUMBER_OF_FRAGMENTS; ++i)
    {
        ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber, i), i);
    }

    // Mark the change as sent, i.e. all fragments were sent once.
    rproxy.from_unsent_to_status(seq.sequenceNumber, UNACKNOWLEDGED, false, true);

    // After the change is marked as delivered, nack data can be processed.
    rproxy.process_nack_frag({}, 2, seq.sequenceNumber, undelivered_fragment_set);

    // Now, send the fragments that were reported as undelivered.
    ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber,
            UNDELIVERED_FRAGMENT), UNDELIVERED_FRAGMENT);

    // All fragments are marked as sent.
    ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber,
            TOTAL_NUMBER_OF_FRAGMENTS + 1u), TOTAL_NUMBER_OF_FRAGMENTS + 1u);
}

TEST(ReaderProxyTests, process_nack_frag_multiple_fragments_different_windows_test)
{
    constexpr FragmentNumber_t TOTAL_NUMBER_OF_FRAGMENTS = 400;
    constexpr uint16_t FRAGMENT_SIZE = 100;

    StatefulWriter writerMock;
    WriterTimes wTimes;
    RemoteLocatorsAllocationAttributes alloc;
    ReaderProxy rproxy(wTimes, alloc, &writerMock);
    CacheChange_t seq;
    seq.sequenceNumber = {0, 1};
    seq.serializedPayload.length = TOTAL_NUMBER_OF_FRAGMENTS * FRAGMENT_SIZE;
    seq.setFragmentSize(FRAGMENT_SIZE);

    RTPSMessageGroup message_group(nullptr, false);
    RTPSGapBuilder gap_builder(message_group);

    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    rproxy.start(reader_attributes);

    ChangeForReader_t change(&seq);
    rproxy.add_change(change, true, false);

    SequenceNumberSet_t sequence_number_set({0, 1});
    sequence_number_set.add({0, 1});
    rproxy.from_unsent_to_status(seq.sequenceNumber, UNACKNOWLEDGED, false, false);
    rproxy.requested_changes_set(sequence_number_set, gap_builder, seq.sequenceNumber);

    // The number of sent fragments should be higher than the FragmentNumberSet_t size.
    constexpr FragmentNumber_t NUMBER_OF_SENT_FRAGMENTS = 259;

    for (auto i = 1u; i <= NUMBER_OF_SENT_FRAGMENTS; ++i)
    {
        ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber, i), i);
    }

    // Set the change status to UNSENT.
    rproxy.perform_acknack_response(nullptr);

    // Handle the first portion of undelivered fragments.
    {
        std::vector<FragmentNumber_t> undelivered_fragments = {3, 6, 8};
        FragmentNumberSet_t undelivered_fragment_set(undelivered_fragments.front());
        for (auto fragment: undelivered_fragments)
        {
            undelivered_fragment_set.add(fragment);
        }
        rproxy.process_nack_frag({}, 1, seq.sequenceNumber, undelivered_fragment_set);

        // Nack data should be ignored: first, complete the sequential delivering of the remaining fragments.
        for (auto i = NUMBER_OF_SENT_FRAGMENTS + 1u; i <= TOTAL_NUMBER_OF_FRAGMENTS; ++i)
        {
            ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber, i), i);
        }

        // Mark the change as sent, i.e. all fragments were sent once.
        rproxy.from_unsent_to_status(seq.sequenceNumber, UNACKNOWLEDGED, false, true);

        rproxy.process_nack_frag({}, 2, seq.sequenceNumber, undelivered_fragment_set);

        // After the change is marked as delivered, nack data can be processed.
        for (auto fragment: undelivered_fragments)
        {
            ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber, fragment), fragment);
        }
    }

    // All fragments are marked as sent.
    ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber,
            TOTAL_NUMBER_OF_FRAGMENTS + 1u), TOTAL_NUMBER_OF_FRAGMENTS + 1u);

    // Handle undelivered fragments that are from a different window.
    {
        std::vector<FragmentNumber_t> undelivered_fragments = {301, 399};
        FragmentNumberSet_t undelivered_fragment_set(undelivered_fragments.front());
        for (auto fragment: undelivered_fragments)
        {
            undelivered_fragment_set.add(fragment);
        }
        rproxy.process_nack_frag({}, 3, seq.sequenceNumber, undelivered_fragment_set);

        // After the change is marked as delivered, nack data can be processed.
        for (auto fragment: undelivered_fragments)
        {
            ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber, fragment), fragment);
        }
    }

    // All fragments are marked as sent.
    ASSERT_EQ(mark_next_fragment_sent(rproxy, seq.sequenceNumber,
            TOTAL_NUMBER_OF_FRAGMENTS + 1u), TOTAL_NUMBER_OF_FRAGMENTS + 1u);
}

// Test expectations regarding acknack count.
// Serves as a regression test for redmine issue #20729.
TEST(ReaderProxyTests, acknack_count)
{
    StatefulWriter writer_mock;
    WriterTimes w_times;
    RemoteLocatorsAllocationAttributes alloc;
    ReaderProxy rproxy(w_times, alloc, &writer_mock);

    ReaderProxyData reader_attributes(0, 0);
    reader_attributes.m_qos.m_reliability.kind = RELIABLE_RELIABILITY_QOS;
    rproxy.start(reader_attributes);

    // Check that the initial acknack count is 0.
    EXPECT_TRUE(rproxy.check_and_set_acknack_count(0u));
    // Check that it is not accepted twice.
    EXPECT_FALSE(rproxy.check_and_set_acknack_count(0u));
    // Check that it is accepted if it is incremented.
    EXPECT_TRUE(rproxy.check_and_set_acknack_count(1u));
    // Check that it is not accepted twice.
    EXPECT_FALSE(rproxy.check_and_set_acknack_count(1u));
    // Check that it is not accepted if it is decremented.
    EXPECT_FALSE(rproxy.check_and_set_acknack_count(0u));
    // Check that it is accepted if it has a big increment.
    EXPECT_TRUE(rproxy.check_and_set_acknack_count(100u));
    // Check that previous values are rejected.
    for (uint32_t i = 0; i <= 100u; ++i)
    {
        EXPECT_FALSE(rproxy.check_and_set_acknack_count(i));
    }
}

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima

int main(
        int argc,
        char** argv)
{
    testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}
