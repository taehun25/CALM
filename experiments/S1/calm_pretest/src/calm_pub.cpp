#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/time.h>
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

}  // namespace

class CalmPublisher : public rclcpp::Node
{
public:
    CalmPublisher(
            int payload_bytes,
            double publish_hz,
            uint64_t max_count,
            const std::string& reliability,
            const std::string& run_id,
            const std::string& csv_dir,
            double ready_timeout_s)
        : Node("calm_pretest_pub")
        , payload_bytes_(std::max(payload_bytes, 16))
        , publish_hz_(publish_hz)
        , period_(std::chrono::duration<double>(1.0 / publish_hz))
        , max_count_(max_count)
        , reliability_(reliability)
        , run_id_(run_id)
        , ready_timeout_s_(ready_timeout_s)
    {
        std::filesystem::create_directories(csv_dir);
        pub_csv_path_ = csv_dir + "/" + run_id_ + "_pub_samples.csv";
        summary_csv_path_ = csv_dir + "/" + run_id_ + "_pub_summary.csv";

        pub_csv_.open(pub_csv_path_, std::ios::out);
        pub_csv_ << "seq,send_time_s,payload_bytes,reliability,target_hz\n";

        data_topic_ = run_topic(run_id_, "data");
        ready_topic_ = run_topic(run_id_, "sub_ready");
        done_topic_ = run_topic(run_id_, "sub_done");

        RCLCPP_INFO(
            get_logger(),
            "Using topics: data=%s ready=%s done=%s",
            data_topic_.c_str(),
            ready_topic_.c_str(),
            done_topic_.c_str());

        data_pub_ = create_publisher<UInt8MultiArray>(data_topic_, data_qos(reliability_));
        ready_sub_ = create_subscription<UInt8MultiArray>(
            ready_topic_,
            sync_qos(),
            [this](const UInt8MultiArray&)
            {
                if (!ready_seen_)
                {
                    ready_seen_ = true;
                    RCLCPP_INFO(get_logger(), "Subscriber ready signal received.");
                    try_start_publishing();
                }
            });

        done_sub_ = create_subscription<UInt8MultiArray>(
            done_topic_,
            sync_qos(),
            [this](const UInt8MultiArray&)
            {
                done_seen_ = true;
            });

        wait_timer_ = create_wall_timer(
            1s,
            [this]()
            {
                if (finished_)
                {
                    return;
                }

                try_start_publishing();

                if (!publish_timer_)
                {
                    RCLCPP_INFO(
                        get_logger(),
                        "Waiting for subscriber handshake... ready=%d data_subscribers=%zu",
                        ready_seen_ ? 1 : 0,
                        data_pub_->get_subscription_count());
                    if (now_seconds() - node_start_time_ > ready_timeout_s_)
                    {
                        finalize("subscriber_ready_timeout");
                    }
                }
            });
    }

    ~CalmPublisher() override
    {
        if (pub_csv_.is_open())
        {
            pub_csv_.close();
        }
    }

private:
    void try_start_publishing()
    {
        if (publish_timer_ || finished_)
        {
            return;
        }

        const size_t data_subscribers = data_pub_->get_subscription_count();
        if (ready_seen_ && data_subscribers > 0)
        {
            RCLCPP_INFO(
                get_logger(),
                "Subscriber ready and data topic matched. Starting publish loop.");
            start_publishing();
        }
    }

    void start_publishing()
    {
        if (publish_timer_)
        {
            return;
        }

        start_time_ = now_seconds();
        last_send_time_ = start_time_;
        publish_timer_ = create_wall_timer(
            std::chrono::duration_cast<std::chrono::nanoseconds>(period_),
            std::bind(&CalmPublisher::publish_one, this));
    }

    void publish_one()
    {
        if (sent_count_ >= max_count_)
        {
            finalize("publish_count_reached");
            return;
        }

        UInt8MultiArray msg;
        msg.data.resize(static_cast<size_t>(payload_bytes_), 0);

        const double send_time = now_seconds();
        const uint64_t seq = sent_count_ + 1;
        std::memcpy(msg.data.data(), &send_time, sizeof(send_time));
        std::memcpy(msg.data.data() + sizeof(send_time), &seq, sizeof(seq));

        data_pub_->publish(msg);
        last_send_time_ = send_time;
        ++sent_count_;

        pub_csv_ << seq << ',' << std::fixed << std::setprecision(6)
                 << send_time << ',' << payload_bytes_ << ','
                 << reliability_ << ',' << publish_hz_ << '\n';

        if (sent_count_ >= max_count_)
        {
            finalize("publish_count_reached");
        }
    }

    void finalize(const std::string& reason)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;

        if (publish_timer_)
        {
            publish_timer_->cancel();
        }

        const double duration = std::max(last_send_time_ - start_time_, 1e-9);
        const double measured_hz = sent_count_ > 1 ? static_cast<double>(sent_count_ - 1) / duration : 0.0;

        std::ofstream summary(summary_csv_path_, std::ios::out);
        summary << "run_id,reliability,payload_bytes,target_hz,sent_count,duration_s,measured_pub_hz,done_seen,reason\n";
        summary << run_id_ << ',' << reliability_ << ',' << payload_bytes_ << ','
                << publish_hz_ << ',' << sent_count_ << ','
                << std::fixed << std::setprecision(6) << duration << ','
                << measured_hz << ',' << (done_seen_ ? 1 : 0) << ','
                << reason << '\n';

        RCLCPP_INFO(
            get_logger(),
            "Publish complete: run=%s reliability=%s payload=%d target_hz=%.3f sent=%lu measured_hz=%.3f reason=%s",
            run_id_.c_str(),
            reliability_.c_str(),
            payload_bytes_,
            publish_hz_,
            static_cast<unsigned long>(sent_count_),
            measured_hz,
            reason.c_str());

        rclcpp::shutdown();
    }

    int payload_bytes_;
    double publish_hz_;
    std::chrono::duration<double> period_;
    uint64_t max_count_;
    std::string reliability_;
    std::string run_id_;
    std::string data_topic_;
    std::string ready_topic_;
    std::string done_topic_;
    std::string pub_csv_path_;
    std::string summary_csv_path_;
    std::ofstream pub_csv_;
    double ready_timeout_s_;

    bool ready_seen_ = false;
    bool done_seen_ = false;
    bool finished_ = false;
    uint64_t sent_count_ = 0;
    double node_start_time_ = now_seconds();
    double start_time_ = 0.0;
    double last_send_time_ = 0.0;

    rclcpp::Publisher<UInt8MultiArray>::SharedPtr data_pub_;
    rclcpp::Subscription<UInt8MultiArray>::SharedPtr ready_sub_;
    rclcpp::Subscription<UInt8MultiArray>::SharedPtr done_sub_;
    rclcpp::TimerBase::SharedPtr wait_timer_;
    rclcpp::TimerBase::SharedPtr publish_timer_;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);

    const int payload_bytes = int_arg(argc, argv, "--payload-bytes", 32768);
    const double publish_hz = double_arg(argc, argv, "--hz", 30.0);
    const int count = int_arg(argc, argv, "--count", 1000);
    const std::string reliability = arg_value(argc, argv, "--reliability", "reliable");
    const std::string run_id = arg_value(argc, argv, "--run-id", "manual_run");
    const std::string csv_dir = arg_value(argc, argv, "--csv-dir", "results/pretest_csv");
    const double ready_timeout_s = double_arg(argc, argv, "--ready-timeout-s", 60.0);

    if (reliability != "reliable" && reliability != "best_effort")
    {
        std::cerr << "--reliability must be reliable or best_effort\n";
        return 2;
    }

    auto node = std::make_shared<CalmPublisher>(
        payload_bytes,
        publish_hz,
        static_cast<uint64_t>(count),
        reliability,
        run_id,
        csv_dir,
        ready_timeout_s);
    rclcpp::spin(node);
    return 0;
}
