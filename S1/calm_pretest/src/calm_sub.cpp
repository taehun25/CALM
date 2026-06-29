#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <sys/time.h>
#include <unordered_set>
#include <vector>

using UInt8MultiArray = std_msgs::msg::UInt8MultiArray;
using namespace std::chrono_literals;

namespace
{

double now_seconds()
{
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1000000.0;
}

std::string arg_value(int argc, char** argv, const std::string& key, const std::string& fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (argv[i] == key)
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

int int_arg(int argc, char** argv, const std::string& key, int fallback)
{
    return std::stoi(arg_value(argc, argv, key, std::to_string(fallback)));
}

double double_arg(int argc, char** argv, const std::string& key, double fallback)
{
    return std::stod(arg_value(argc, argv, key, std::to_string(fallback)));
}

rclcpp::QoS data_qos(const std::string& reliability)
{
    rclcpp::QoS qos{rclcpp::KeepAll()};
    if (reliability == "best_effort")
    {
        qos.best_effort();
    }
    else
    {
        qos.reliable();
    }
    qos.durability_volatile();
    return qos;
}

rclcpp::QoS sync_qos()
{
    rclcpp::QoS qos{rclcpp::KeepAll()};
    qos.reliable();
    qos.transient_local();
    return qos;
}

std::string sanitize_topic_token(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        if (std::isalnum(c))
        {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        else
        {
            out.push_back('_');
        }
    }
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out.front())))
    {
        out = "run_" + out;
    }
    return out;
}

std::string run_topic(const std::string& run_id, const std::string& suffix)
{
    return "calm_pretest_" + sanitize_topic_token(run_id) + "_" + suffix;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const double index = (p / 100.0) * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(index));
    const size_t hi = static_cast<size_t>(std::ceil(index));
    if (lo == hi)
    {
        return values[lo];
    }
    const double frac = index - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

}  // namespace

class CalmSubscriber : public rclcpp::Node
{
public:
    CalmSubscriber(
            int payload_bytes,
            double target_hz,
            uint64_t expected_count,
            const std::string& reliability,
            const std::string& run_id,
            const std::string& csv_dir,
            double idle_timeout_s,
            double total_timeout_s)
        : Node("calm_pretest_sub")
        , payload_bytes_(payload_bytes)
        , target_hz_(target_hz)
        , expected_count_(expected_count)
        , reliability_(reliability)
        , run_id_(run_id)
        , idle_timeout_s_(idle_timeout_s)
        , total_timeout_s_(total_timeout_s)
    {
        std::filesystem::create_directories(csv_dir);
        sample_csv_path_ = csv_dir + "/" + run_id_ + "_sub_samples.csv";
        summary_csv_path_ = csv_dir + "/" + run_id_ + "_sub_summary.csv";

        sample_csv_.open(sample_csv_path_, std::ios::out);
        sample_csv_ << "seq,send_time_s,recv_time_s,delay_ms,payload_bytes,reliability,target_hz\n";

        data_topic_ = run_topic(run_id_, "data");
        ready_topic_ = run_topic(run_id_, "sub_ready");
        done_topic_ = run_topic(run_id_, "sub_done");

        RCLCPP_INFO(
            get_logger(),
            "Using topics: data=%s ready=%s done=%s",
            data_topic_.c_str(),
            ready_topic_.c_str(),
            done_topic_.c_str());

        data_sub_ = create_subscription<UInt8MultiArray>(
            data_topic_,
            data_qos(reliability_),
            std::bind(&CalmSubscriber::on_data, this, std::placeholders::_1));

        ready_pub_ = create_publisher<UInt8MultiArray>(ready_topic_, sync_qos());
        done_pub_ = create_publisher<UInt8MultiArray>(done_topic_, sync_qos());

        start_time_ = now_seconds();
        last_recv_time_ = start_time_;

        ready_timer_ = create_wall_timer(500ms, std::bind(&CalmSubscriber::send_ready, this));
        watchdog_timer_ = create_wall_timer(1s, std::bind(&CalmSubscriber::watchdog, this));
        send_ready();
    }

    ~CalmSubscriber() override
    {
        if (sample_csv_.is_open())
        {
            sample_csv_.close();
        }
    }

private:
    void send_ready()
    {
        UInt8MultiArray msg;
        msg.data = {1};
        ready_pub_->publish(msg);
    }

    void send_done()
    {
        UInt8MultiArray msg;
        msg.data = {1};
        done_pub_->publish(msg);
    }

    void on_data(const UInt8MultiArray& msg)
    {
        const double recv_time = now_seconds();
        double send_time = 0.0;
        uint64_t seq = 0;

        if (msg.data.size() >= 16)
        {
            std::memcpy(&send_time, msg.data.data(), sizeof(send_time));
            std::memcpy(&seq, msg.data.data() + sizeof(send_time), sizeof(seq));
        }
        else
        {
            send_time = recv_time;
            seq = recv_count_ + 1;
        }

        if (recv_count_ == 0)
        {
            first_recv_time_ = recv_time;
            first_send_time_ = send_time;
        }

        last_recv_time_ = recv_time;
        last_send_time_ = send_time;
        ++recv_count_;

        const bool first_seen = seen_sequences_.insert(seq).second;
        if (!first_seen)
        {
            ++duplicate_count_;
        }

        const double delay_ms = (recv_time - send_time) * 1000.0;
        delays_ms_.push_back(delay_ms);

        sample_csv_ << seq << ',' << std::fixed << std::setprecision(6)
                    << send_time << ',' << recv_time << ','
                    << std::setprecision(3) << delay_ms << ','
                    << payload_bytes_ << ',' << reliability_ << ','
                    << target_hz_ << '\n';

        if (recv_count_ >= expected_count_)
        {
            finalize("expected_count_reached");
        }
    }

    void watchdog()
    {
        const double now = now_seconds();
        if (recv_count_ == 0 && now - start_time_ > total_timeout_s_)
        {
            finalize("total_timeout_without_data");
            return;
        }

        if (recv_count_ > 0 && now - last_recv_time_ > idle_timeout_s_)
        {
            finalize("idle_timeout");
        }
    }

    void finalize(const std::string& reason)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;

        if (ready_timer_)
        {
            ready_timer_->cancel();
        }
        if (watchdog_timer_)
        {
            watchdog_timer_->cancel();
        }

        const double recv_duration = recv_count_ > 1 ? std::max(last_recv_time_ - first_recv_time_, 1e-9) : 0.0;
        const double send_duration = recv_count_ > 1 ? std::max(last_send_time_ - first_send_time_, 1e-9) : 0.0;
        const double measured_sub_hz = recv_count_ > 1 ? static_cast<double>(recv_count_ - 1) / recv_duration : 0.0;
        const double observed_pub_hz = recv_count_ > 1 ? static_cast<double>(recv_count_ - 1) / send_duration : 0.0;

        const double avg_delay = delays_ms_.empty()
            ? std::numeric_limits<double>::quiet_NaN()
            : std::accumulate(delays_ms_.begin(), delays_ms_.end(), 0.0) / static_cast<double>(delays_ms_.size());
        const double p50_delay = percentile(delays_ms_, 50.0);
        const double p95_delay = percentile(delays_ms_, 95.0);
        const double p99_delay = percentile(delays_ms_, 99.0);
        const double receive_ratio = expected_count_ > 0
            ? static_cast<double>(seen_sequences_.size()) / static_cast<double>(expected_count_)
            : 0.0;

        std::ofstream summary(summary_csv_path_, std::ios::out);
        summary << "run_id,reliability,payload_bytes,target_hz,expected_count,received_count,unique_count,"
                << "duplicate_count,receive_ratio,recv_duration_s,measured_sub_hz,observed_pub_hz,"
                << "avg_delay_ms,p50_delay_ms,p95_delay_ms,p99_delay_ms,reason\n";
        summary << run_id_ << ',' << reliability_ << ',' << payload_bytes_ << ','
                << target_hz_ << ',' << expected_count_ << ',' << recv_count_ << ','
                << seen_sequences_.size() << ',' << duplicate_count_ << ','
                << std::fixed << std::setprecision(6)
                << receive_ratio << ',' << recv_duration << ','
                << measured_sub_hz << ',' << observed_pub_hz << ','
                << avg_delay << ',' << p50_delay << ',' << p95_delay << ','
                << p99_delay << ',' << reason << '\n';

        RCLCPP_INFO(
            get_logger(),
            "Subscribe complete: run=%s reliability=%s payload=%d target_hz=%.3f recv=%lu unique=%lu sub_hz=%.3f reason=%s",
            run_id_.c_str(),
            reliability_.c_str(),
            payload_bytes_,
            target_hz_,
            static_cast<unsigned long>(recv_count_),
            static_cast<unsigned long>(seen_sequences_.size()),
            measured_sub_hz,
            reason.c_str());

        send_done();
        rclcpp::shutdown();
    }

    int payload_bytes_;
    double target_hz_;
    uint64_t expected_count_;
    std::string reliability_;
    std::string run_id_;
    std::string data_topic_;
    std::string ready_topic_;
    std::string done_topic_;
    double idle_timeout_s_;
    double total_timeout_s_;

    std::string sample_csv_path_;
    std::string summary_csv_path_;
    std::ofstream sample_csv_;

    bool finished_ = false;
    uint64_t recv_count_ = 0;
    uint64_t duplicate_count_ = 0;
    std::unordered_set<uint64_t> seen_sequences_;
    std::vector<double> delays_ms_;

    double start_time_ = 0.0;
    double first_recv_time_ = 0.0;
    double last_recv_time_ = 0.0;
    double first_send_time_ = 0.0;
    double last_send_time_ = 0.0;

    rclcpp::Subscription<UInt8MultiArray>::SharedPtr data_sub_;
    rclcpp::Publisher<UInt8MultiArray>::SharedPtr ready_pub_;
    rclcpp::Publisher<UInt8MultiArray>::SharedPtr done_pub_;
    rclcpp::TimerBase::SharedPtr ready_timer_;
    rclcpp::TimerBase::SharedPtr watchdog_timer_;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);

    const int payload_bytes = int_arg(argc, argv, "--payload-bytes", 32768);
    const double target_hz = double_arg(argc, argv, "--hz", 30.0);
    const int count = int_arg(argc, argv, "--count", 1000);
    const std::string reliability = arg_value(argc, argv, "--reliability", "reliable");
    const std::string run_id = arg_value(argc, argv, "--run-id", "manual_run");
    const std::string csv_dir = arg_value(argc, argv, "--csv-dir", "results/pretest_csv");
    const double idle_timeout_s = double_arg(argc, argv, "--idle-timeout-s", 20.0);
    const double total_timeout_s = double_arg(argc, argv, "--total-timeout-s", 120.0);

    if (reliability != "reliable" && reliability != "best_effort")
    {
        std::cerr << "--reliability must be reliable or best_effort\n";
        return 2;
    }

    auto node = std::make_shared<CalmSubscriber>(
        payload_bytes,
        target_hz,
        static_cast<uint64_t>(count),
        reliability,
        run_id,
        csv_dir,
        idle_timeout_s,
        total_timeout_s);
    rclcpp::spin(node);
    return 0;
}
