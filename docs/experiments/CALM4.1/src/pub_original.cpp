#include <rclcpp/rclcpp.hpp>
#include <rmw/rmw.h>

#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/msg/point_field.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
Usage: ros2 run calm_pretest_yw dds_pub <loss_rate> <idx> <payload_size> <max_count> <max_samples> <socket_buffer> <payload_type> <publish_period> [case_d_secs] [hz] [topic_prefix] [qos_mode] [qos_depth] [case_d_enabled] [trigger_after]
예: ros2 run calm_pretest_yw dds_pub 0 1 128000 100 400 0 image 0.033 0 30 main reliable 1 0 200
*/

static const char * network_interface()
{
  const char * configured = std::getenv("CALM_NET_INTERFACE");
  return configured != nullptr && configured[0] != '\0' ? configured : "wlp2s0";
}

static double env_double(const char * name, double fallback)
{
  const char * value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char * end = nullptr;
  const double parsed = std::strtod(value, &end);
  return end == value ? fallback : parsed;
}

static bool env_bool(const char * name, bool fallback = false)
{
  const char * value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
         std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0;
}

static std::vector<int> env_int_list(const char * name)
{
  std::vector<int> values;
  const char * raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return values;
  }

  std::stringstream stream(raw);
  std::string token;
  while (std::getline(stream, token, ',')) {
    try {
      const int value = std::stoi(token);
      if (value > 0) {
        values.push_back(value);
      }
    } catch (...) {
    }
  }
  return values;
}
// static constexpr int MAX_MESSAGE_SIZE = 65500;  // Fast DDS UDP 최대 메시지 크기
static constexpr int MAX_MESSAGE_SIZE = 1472;

/** 소켓 하나에 대한 정보 (fd, 타입, 로컬 주소:포트, SO_SNDBUF). */
struct SocketInfo {
  int fd;
  int type;       // SOCK_DGRAM, SOCK_STREAM
  std::string local_addr_port;
  int so_sndbuf;
};

/** 이 프로세스에 열린 소켓들을 수집. fd, 타입(UDP/TCP), 로컬 주소:포트, SO_SNDBUF. DDS 소켓 포함. */
static std::vector<SocketInfo> get_all_socket_info_in_process() {
  std::vector<SocketInfo> out;
  const int max_fd = 1024;
  for (int fd = 0; fd < max_fd; ++fd) {
    int sndbuf = 0;
    socklen_t len = sizeof(sndbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) != 0)
      continue;  // 소켓 아님 또는 조회 실패
    int type = 0;
    len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0)
      continue;
    int domain = 0;
    len = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0)
      continue;

    std::string local_str;
    if (domain == AF_INET) {
      struct sockaddr_in addr = {};
      socklen_t addrlen = sizeof(addr);
      if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &addrlen) == 0) {
        char buf[INET_ADDRSTRLEN];
        const char * a = inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        local_str = a ? std::string(a) : "?";
        local_str += ":";
        local_str += std::to_string(ntohs(addr.sin_port));
      } else {
        local_str = "?:?";
      }
    } else if (domain == AF_INET6) {
      struct sockaddr_in6 addr = {};
      socklen_t addrlen = sizeof(addr);
      if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &addrlen) == 0) {
        char buf[INET6_ADDRSTRLEN];
        const char * a = inet_ntop(AF_INET6, &addr.sin6_addr, buf, sizeof(buf));
        local_str = a ? std::string(a) : "?";
        local_str += ":";
        local_str += std::to_string(ntohs(addr.sin6_port));
      } else {
        local_str = "?:?";
      }
    } else {
      local_str = "?:?";
    }

    out.push_back({ fd, type, local_str, sndbuf });
  }
  return out;
}

static bool set_netem_loss(double loss_rate)
{
  try {
    const double baseline_rate_mbit = env_double("CALM_NETEM_RATE_MBIT", 0.0);
    const double baseline_delay_ms = env_double("CALM_NETEM_DELAY_MS", 0.0);
    const double baseline_jitter_ms = env_double("CALM_NETEM_JITTER_MS", 0.0);
    const double baseline_loss_pct = env_double("CALM_NETEM_BASE_LOSS_PCT", 0.0);
    const double effective_loss_pct = std::max(loss_rate, baseline_loss_pct);

    if (baseline_rate_mbit <= 0.0 &&
        baseline_delay_ms <= 0.0 &&
        effective_loss_pct <= 0.0) {
      std::ostringstream clear;
      clear << "sudo -n tc qdisc del dev " << network_interface()
            << " root > /dev/null 2>&1";
      (void)std::system(clear.str().c_str());
      return true;
    }

    std::ostringstream oss;
    oss << "sudo -n tc qdisc replace dev " << network_interface()
        << " root netem limit 10000";
    if (baseline_delay_ms > 0.0) {
      oss << " delay " << baseline_delay_ms << "ms";
      if (baseline_jitter_ms > 0.0) {
        oss << " " << baseline_jitter_ms << "ms distribution normal";
      }
    }
    if (effective_loss_pct > 0.0) {
      oss << " loss " << effective_loss_pct << "%";
    }
    if (baseline_rate_mbit > 0.0) {
      oss << " rate " << baseline_rate_mbit << "mbit";
    }
    oss << " > /dev/null 2>&1";
    int rc = std::system(oss.str().c_str());
    return rc == 0;
  } catch (...) {
    return false;
  }
}

static bool clear_netem()
{
  // qdisc 규칙 해제
  try {
    std::ostringstream oss;
    oss << "sudo tc qdisc del dev " << network_interface() << " root > /dev/null 2>&1";
    (void)std::system(oss.str().c_str());
    return true;
  } catch (...) {
    return false;
  }
}

static std::string getenv_str(const char * key, const std::string & fallback = "")
{
  const char * v = std::getenv(key);
  if (!v) {
    return fallback;
  }
  return std::string(v);
}

static std::string sanitize_label(const std::string & text, const std::string & fallback = "default")
{
  if (text.empty()) {
    return fallback;
  }
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? fallback : out;
}

static std::string topic_name(const std::string & prefix, const std::string & base)
{
  if (prefix.empty()) {
    return base;
  }
  return sanitize_label(prefix, "main") + "_" + base;
}

static std::string node_name(const std::string & base, const std::string & prefix)
{
  if (prefix.empty()) {
    return base;
  }
  return base + "_" + sanitize_label(prefix, "main");
}

static rclcpp::QoS make_data_qos(const std::string & qos_mode, int depth)
{
  if (qos_mode == "best_effort") {
    rclcpp::QoS qos{rclcpp::KeepLast(depth > 0 ? depth : 1)};
    qos.best_effort();
    qos.durability_volatile();
    return qos;
  }

  rclcpp::QoS qos{rclcpp::KeepAll()};
  qos.reliable();
  qos.durability_volatile();
  return qos;
}

static rclcpp::QoS make_sync_qos()
{
  rclcpp::QoS qos{rclcpp::KeepAll()};
  qos.reliable();
  qos.transient_local();
  return qos;
}

static std::string get_pkg_root()
{
  std::string pkg_root = getenv_str("LOOPBACK_TEST_PKG_ROOT");
  if (!pkg_root.empty())
    return pkg_root;
  std::filesystem::path d = std::filesystem::current_path();
  while (d != d.parent_path()) {
    if (std::filesystem::is_directory(d / "resource_pub") ||
        std::filesystem::is_directory(d / "large-data-optimization")) {
      return d.string();
    }
    d = d.parent_path();
  }
  return "";
}

static std::optional<std::string> generate_rmw_xml(
  int max_samples,
  int socket_buffer,
  int max_message_size,
  const std::optional<std::string> & template_path_opt = std::nullopt,
  const std::optional<std::string> & output_path_opt = std::nullopt)
{
  (void)max_samples;
  (void)max_message_size;
  // RMW 구현체에 따라 XML 설정 파일 생성
  // FastDDS와 CycloneDDS 모두 지원 (템플릿 파일의 플레이스홀더를 치환)

  // RMW 구현체 확인
  const std::string rmw_impl = getenv_str("RMW_IMPLEMENTATION", "rmw_fastrtps_cpp");

  const std::string base_path = get_pkg_root().empty() ? "" : (get_pkg_root() + "/resource_pub");
  if (base_path.empty())
    return std::nullopt;

  std::string template_path;
  if (template_path_opt) {
    template_path = *template_path_opt;
  } else {
    if (rmw_impl == "rmw_fastrtps_cpp" || rmw_impl == "rmw_fastrtps_dynamic_cpp") {
      template_path = base_path + "/publisher_default.xml";
    } else if (rmw_impl == "rmw_cyclonedds_cpp") {
      template_path = base_path + "/publisher_default_cyclonedds.xml";
    } else if (rmw_impl == "rmw_zenoh_cpp") {
      // Zenoh는 XML 파일 불필요
      return std::nullopt;
    } else {
      // 기본값으로 FastDDS 사용
      template_path = base_path + "/publisher_default.xml";
    }
  }

  std::string output_path;
  if (output_path_opt) {
    output_path = *output_path_opt;
  } else {
    if (rmw_impl == "rmw_fastrtps_cpp" || rmw_impl == "rmw_fastrtps_dynamic_cpp") {
      output_path = base_path + "/publisher_new.xml";
    } else if (rmw_impl == "rmw_cyclonedds_cpp") {
      output_path = base_path + "/publisher_new_cyclonedds.xml";
    } else if (rmw_impl == "rmw_zenoh_cpp") {
      // Zenoh는 XML 파일 불필요
      return std::nullopt;
    } else {
      output_path = base_path + "/publisher_new.xml";
    }
  }

  std::ifstream ifs(template_path);
  if (!ifs.is_open()) {
    return std::nullopt;
  }
  std::ostringstream buf;
  buf << ifs.rdbuf();
  std::string template_txt = buf.str();

  // 플레이스홀더 치환 (소켓 버퍼만 설정, 나머지는 DDS 기본값 사용)
  std::vector<std::string> lines;
  {
    std::istringstream iss(template_txt);
    std::string line;
    while (std::getline(iss, line)) {
      lines.push_back(line);
    }
  }

  std::vector<std::string> filtered_lines;
  bool skip_general = false;  // CycloneDDS의 경우 General 태그 제거를 위한 플래그

  for (const auto & line : lines) {
    // CycloneDDS의 경우 General 태그 블록 제거
    if (rmw_impl == "rmw_cyclonedds_cpp") {
      if (line.find("<General>") != std::string::npos) {
        skip_general = true;
        continue;
      }
      if (skip_general && line.find("</General>") != std::string::npos) {
        skip_general = false;
        continue;
      }
      if (skip_general) {
        continue;
      }
    }

    // 이미 주석 처리된 라인은 건너뛰기
    auto trim = [](const std::string & s) -> std::string {
      size_t b = s.find_first_not_of(" \t\r\n");
      if (b == std::string::npos) {
        return "";
      }
      size_t e = s.find_last_not_of(" \t\r\n");
      return s.substr(b, e - b + 1);
    };
    const std::string stripped_line = trim(line);
    if (stripped_line.rfind("<!--", 0) == 0 || stripped_line.rfind("*", 0) == 0) {
      filtered_lines.push_back(line);
      continue;
    }

    // MAX_SAMPLES나 MAX_MESSAGE_SIZE가 포함된 라인은 주석 처리
    if (line.find("MAX_SAMPLES") != std::string::npos ||
      line.find("MAX_MESSAGE_SIZE") != std::string::npos)
    {
      filtered_lines.push_back("<!-- " + stripped_line + " (disabled, using DDS default) -->");
    } else {
      filtered_lines.push_back(line);
    }
  }

  std::ostringstream out;
  for (size_t i = 0; i < filtered_lines.size(); ++i) {
    out << filtered_lines[i];
    if (i + 1 < filtered_lines.size()) {
      out << "\n";
    }
  }
  std::string replaced = out.str();

  // 소켓 버퍼만 치환 (CycloneDDS에서는 SOCKET_BUFFER 플레이스홀더가 없으므로 무시됨)
  {
    const std::string needle = "SOCKET_BUFFER";
    const std::string repl = std::to_string(socket_buffer);
    size_t pos = 0;
    while ((pos = replaced.find(needle, pos)) != std::string::npos) {
      replaced.replace(pos, needle.size(), repl);
      pos += repl.size();
    }
  }

  std::ofstream ofs(output_path, std::ios::out | std::ios::trunc);
  if (!ofs.is_open()) {
    return std::nullopt;
  }
  ofs << replaced;
  ofs.flush();
  return output_path;
}

class DDSPub final : public rclcpp::Node
{
public:
  DDSPub(
    const std::string & csv_filename,
    int max_count,
    int payload_size,
    const std::string & payload_type = "default",
    double publish_period = 0.033,
    int burst_secs = 0,
    double original_loss_rate = 0.0,
    const std::string & topic_prefix = "",
    const std::string & qos_mode = "reliable",
    int qos_depth = 1,
    bool case_d_enabled = false,
    int trigger_after = 200,
    std::set<int> socket_fds_before_node = {})
  : rclcpp::Node(node_name("dds_pub", topic_prefix)),
    period_s_(publish_period),
    r_ms_(period_s_ * 1000.0),
    max_count_(max_count),
    count_(0),
    sub_ready_(false),
    payload_size_(payload_size),
    payload_type_(payload_type),
    topic_prefix_(sanitize_label(topic_prefix, "")),
    qos_mode_(qos_mode),
    qos_depth_(qos_depth > 0 ? qos_depth : 1),
    burst_secs_(burst_secs),
    burst_done_(false),
    original_loss_rate_(original_loss_rate),
    case_a_loss_pct_(std::clamp(env_double("CALM_CASE_A_LOSS_PCT", 0.0), 0.0, 100.0)),
    case_d_enabled_(case_d_enabled),
    trigger_after_(trigger_after > 0 ? trigger_after : 200),
    trigger_reported_(false),
    receive_progress_enabled_(env_bool("CALM_USE_RECEIVE_PROGRESS")),
    case_d_triggers_(env_int_list("CALM_CASE_D_TRIGGERS")),
    case_d_durations_s_(env_int_list("CALM_CASE_D_DURATIONS_S")),
    socket_fds_before_node_(std::move(socket_fds_before_node))
  {
    if (case_d_triggers_.empty()) {
      case_d_triggers_.push_back(trigger_after_);
    }
    if (case_d_durations_s_.empty()) {
      case_d_durations_s_.push_back(burst_secs_);
    }
    while (case_d_durations_s_.size() < case_d_triggers_.size()) {
      case_d_durations_s_.push_back(case_d_durations_s_.back());
    }

    rclcpp::QoS main_qos = make_data_qos(qos_mode_, qos_depth_);
    std::cout << static_cast<int>(main_qos.reliability()) << std::endl;
    std::cout << "[PUB] topic_prefix=" << (topic_prefix_.empty() ? "(default)" : topic_prefix_)
              << " qos_mode=" << qos_mode_ << " qos_depth=" << qos_depth_
              << " case_d=" << (case_d_enabled_ ? "on" : "off")
              << " case_a_loss_pct=" << case_a_loss_pct_
              << " trigger_after=" << trigger_after_ << std::endl;

    if (payload_type_ == "image") {
      image_pub_ = this->create_publisher<sensor_msgs::msg::Image>(
        topic_name(topic_prefix_, "image_topic"), main_qos);
    } else if (payload_type_ == "point") {
      point_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>(
        topic_name(topic_prefix_, "point_topic"), main_qos);
    } else if (payload_type_ == "uint8") {
      uint8_pub_ = this->create_publisher<std_msgs::msg::UInt8MultiArray>(
        topic_name(topic_prefix_, "uint8_topic"), main_qos);
    } else {  // default
      string_pub_ = this->create_publisher<std_msgs::msg::String>(
        topic_name(topic_prefix_, "string_topic"), main_qos);
    }

    rclcpp::QoS sync_qos = make_sync_qos();
    sub_ready_sub_ = this->create_subscription<std_msgs::msg::UInt8MultiArray>(
      topic_name(topic_prefix_, "sub_ready_topic"),
      sync_qos,
      std::bind(&DDSPub::on_sub_ready, this, std::placeholders::_1));
    sub_progress_sub_ = this->create_subscription<std_msgs::msg::UInt8MultiArray>(
      topic_name(topic_prefix_, "sub_progress_topic"),
      sync_qos,
      std::bind(&DDSPub::on_sub_progress, this, std::placeholders::_1));

    // (5) 메시지 발행 타이머 (None→준비신호 오면 시작)
    publish_timer_.reset();

    // (6) 페이로드 준비 (payload_type에 따라 다른 데이터 생성)
    prepare_payload();

    // (7) CSV 초기화
    {
      std::filesystem::path p(csv_filename);
      std::filesystem::create_directories(p.parent_path());
    }
    csv_filename_ = csv_filename;
    csv_file_.open(csv_filename_, std::ios::out | std::ios::trunc);
    csv_file_ << "msg_index,publish_time_s,publish_delay_ms\n";
    csv_file_.flush();

    publish_queue_maxsize_ = 10;
    publisher_thread_ = std::thread(&DDSPub::publisher_worker, this);

    // 스레드 우선순위 설정 (Linux에서만 작동)
    try_set_high_priority();

    interval_list_.clear();
    publish_time_list_.clear();
    stats_calculated_ = false;
    first_publish_time_.reset();
    last_publish_time_.reset();
  }

  ~DDSPub() override
  {
    if (case_d_enabled_) {
      set_netem_loss(original_loss_rate_);
    }
    if (case_a_applied_) {
      const bool restored = set_netem_loss(original_loss_rate_);
      const auto event_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
      std::cout << "[CASE_A_EVENT] time_ns=" << event_time_ns
                << " action=end loss_pct=" << case_a_loss_pct_
                << " restored_loss=" << original_loss_rate_
                << " restored=" << (restored ? 1 : 0) << std::endl;
      std::cout.flush();
      case_a_applied_ = false;
    }
    stop_worker_ = true;
    {
      std::lock_guard<std::mutex> lk(queue_mtx_);
      // no-op; just ensure ordering before notify
    }
    queue_cv_.notify_all();
    if (publisher_thread_.joinable()) {
      publisher_thread_.join();
    }
    try {
      csv_file_.flush();
      csv_file_.close();
    } catch (...) {
    }
  }

  bool stats_calculated() const { return stats_calculated_; }
  size_t interval_size() const { return interval_list_.size(); }
  size_t publish_time_size() const { return publish_time_list_.size(); }

  void report_averages()
  {
    // publish_time_s 기준 publish rate 계산
    double publish_rate = std::numeric_limits<double>::quiet_NaN();
    try {
      csv_file_.flush();
      std::ifstream ifs(csv_filename_);
      std::string line;
      std::vector<std::vector<std::string>> rows;
      while (std::getline(ifs, line)) {
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, ',')) {
          cols.push_back(col);
        }
        rows.push_back(cols);
      }
      if (rows.size() > 2) {
        double first_time = std::stod(rows[1][1]);
        double last_time = std::stod(rows.back()[1]);
        double count = static_cast<double>(rows.size() - 1);
        publish_rate = (last_time > first_time)
                         ? ((count - 1.0) / (last_time - first_time))
                         : std::numeric_limits<double>::quiet_NaN();
      }
    } catch (...) {
      publish_rate = std::numeric_limits<double>::quiet_NaN();
    }

    std::vector<double> intervals;
    {
      std::lock_guard<std::mutex> lk(stats_mtx_);
      if (interval_list_.size() > 1) {
        intervals.assign(interval_list_.begin() + 1, interval_list_.end());
      }
    }
    double avg_interval = 0.0;
    double std_interval = 0.0;
    if (!intervals.empty()) {
      double sum = 0.0;
      for (double v : intervals) {
        sum += v;
      }
      avg_interval = sum / static_cast<double>(intervals.size());
      double var = 0.0;
      for (double v : intervals) {
        double d = (v - avg_interval);
        var += d * d;
      }
      var /= static_cast<double>(intervals.size());
      std_interval = std::sqrt(var);
    }

    std::vector<double> pub_times;
    {
      std::lock_guard<std::mutex> lk(stats_mtx_);
      pub_times = publish_time_list_;
    }
    double avg_pub_time = 0.0;
    double std_pub_time = 0.0;
    if (!pub_times.empty()) {
      double sum = 0.0;
      for (double v : pub_times) {
        sum += v;
      }
      avg_pub_time = sum / static_cast<double>(pub_times.size());
      double var = 0.0;
      for (double v : pub_times) {
        double d = (v - avg_pub_time);
        var += d * d;
      }
      var /= static_cast<double>(pub_times.size());
      std_pub_time = std::sqrt(var);
    }

    // 통계 출력은 calculate_and_print_stats에서 처리
    std::cout << "[PUB_STATS] publish_rate: " << std::fixed << std::setprecision(3) << publish_rate
              << std::endl;
    std::cout << "[PUB_STATS] avg_interval: " << std::fixed << std::setprecision(3) << avg_interval
              << std::endl;
    std::cout << "[PUB_STATS] std_interval: " << std::fixed << std::setprecision(3) << std_interval
              << std::endl;
    std::cout << "[PUB_STATS] avg_pub_time: " << std::fixed << std::setprecision(3) << avg_pub_time
              << std::endl;
    std::cout << "[PUB_STATS] std_pub_time: " << std::fixed << std::setprecision(3) << std_pub_time
              << std::endl;
    std::cout.flush();
    stats_calculated_ = true;
  }

private:
  void try_set_high_priority()
  {
    // Python의 os.nice(-10) 의도(우선순위 높임)를 최대한 비슷하게 수행
    // setpriority는 값이 작을수록(음수) 우선순위 높음. 권한 없으면 실패.
    (void)setpriority(PRIO_PROCESS, 0, -10);
  }

  void on_sub_ready(const std_msgs::msg::UInt8MultiArray::SharedPtr /*msg*/)
  {
    // Subscriber로부터 처음 신호를 받으면 타이머 시작
    if (!sub_ready_) {
      sub_ready_ = true;
      std::cout << "[PUB] Received subscriber ready message. Starting publish timer." << std::endl;
      std::cout.flush();

      if (case_a_loss_pct_ > 0.0) {
        case_a_applied_ = set_netem_loss(case_a_loss_pct_);
        const auto event_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
        std::cout << "[CASE_A_EVENT] time_ns=" << event_time_ns
                  << " action=start loss_pct=" << case_a_loss_pct_
                  << " applied=" << (case_a_applied_ ? 1 : 0) << std::endl;
        std::cout.flush();
      }

      auto period = std::chrono::duration<double>(period_s_);
      publish_timer_ = this->create_wall_timer(
        std::chrono::duration_cast<std::chrono::nanoseconds>(period),
        std::bind(&DDSPub::timer_callback, this));
    }
  }

  void on_sub_progress(const std_msgs::msg::UInt8MultiArray::SharedPtr msg)
  {
    if (msg->data.size() < sizeof(uint64_t)) {
      return;
    }

    uint64_t recv_count = 0;
    std::memcpy(&recv_count, msg->data.data(), sizeof(recv_count));
    std::cout << "[SUB_PROGRESS] recv_count=" << recv_count
              << " topic_prefix=" << (topic_prefix_.empty() ? "(default)" : topic_prefix_)
              << std::endl;
    std::cout.flush();

    if (!case_d_enabled_ || !receive_progress_enabled_ || case_d_burst_active_ ||
        case_d_next_trigger_ >= case_d_triggers_.size() ||
        recv_count < static_cast<uint64_t>(case_d_triggers_[case_d_next_trigger_])) {
      return;
    }

    start_case_d_outage(recv_count);
  }

  void start_case_d_outage(uint64_t recv_count)
  {
    if (case_d_next_trigger_ >= case_d_triggers_.size()) {
      return;
    }

    const int trigger = case_d_triggers_[case_d_next_trigger_];
    const int duration_s = case_d_durations_s_[case_d_next_trigger_];
    ++case_d_next_trigger_;
    case_d_burst_active_ = true;
    const auto event_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

    std::cout << "[CASE_D_EVENT] time_ns=" << event_time_ns
              << " action=start recv_count=" << recv_count
              << " trigger=" << trigger << " duration_s=" << duration_s << std::endl;
    std::cout.flush();
    set_netem_loss(100.0);

    auto delay = std::chrono::seconds(duration_s);
    burst_timer_ = this->create_wall_timer(delay, std::bind(&DDSPub::restore_loss, this));
  }

  void timer_callback()
  {
    if (count_ >= max_count_) {
      if (count_ == max_count_) {
        std::cout << "[PUB] [INFO] All " << max_count_ << " messages have been published." << std::endl;
        std::cout.flush();
        // 발행 완료 시점에 "노드 생성 후 생긴 소켓(DDS 추정)" 한 번 더 출력
        {
          std::vector<SocketInfo> socks = get_all_socket_info_in_process();
          int n_new = 0;
          for (const auto & s : socks)
            if (socket_fds_before_node_.count(s.fd) == 0) n_new++;
          if (n_new > 0) {
            std::cout << "[PUB] Sockets after publish done (likely DDS), " << n_new << " sockets:" << std::endl;
            for (const auto & s : socks) {
              if (socket_fds_before_node_.count(s.fd) != 0) continue;
              const char * typestr = (s.type == SOCK_DGRAM) ? "UDP" : "TCP";
              std::cout << "[PUB]   fd=" << s.fd << " type=" << typestr
                        << " local=" << s.local_addr_port
                        << " SO_SNDBUF=" << s.so_sndbuf << std::endl;
            }
            fflush(stdout);
          }
        }
        report_averages();
        count_ += 1;
      }
      return;
    }

    // 카운트 증가 (링크 단절 체크를 위해 먼저 증가)
    count_ += 1;
    const int msg_index = count_;

    if (!trigger_reported_ && msg_index >= trigger_after_) {
      std::cout << "[PUB_TRIGGER] msg_index=" << msg_index
                << " trigger_after=" << trigger_after_
                << " topic_prefix=" << (topic_prefix_.empty() ? "(default)" : topic_prefix_) << std::endl;
      std::cout.flush();
      trigger_reported_ = true;
    }

    if (case_d_enabled_ && !receive_progress_enabled_ && !burst_done_ &&
        burst_secs_ > 0 && msg_index >= trigger_after_) {
      std::cout << "[PUB] CASE_D on: reached " << msg_index << "/" << max_count_
                << ". Injecting 100% loss for " << burst_secs_ << "s" << std::endl;
      std::cout.flush();
      set_netem_loss(100.0);
      burst_done_ = true;

      auto delay = std::chrono::seconds(burst_secs_);
      burst_timer_ = this->create_wall_timer(
        delay,
        std::bind(&DDSPub::restore_loss, this));
      std::cout << "[PUB] Timer created: " << burst_secs_ << "s delay" << std::endl;
      std::cout.flush();
    }

    // payload_type에 따라 다른 메시지 생성 (최적화된 방식)
    if (payload_type_ == "image") {
      auto msg = std::make_shared<sensor_msgs::msg::Image>();
      *msg = image_template_;
      msg->header.stamp = this->get_clock()->now();
      enqueue_publish(msg_index, [this, msg]() { image_pub_->publish(*msg); });

    } else if (payload_type_ == "point") {
      auto msg = std::make_shared<sensor_msgs::msg::PointCloud2>();
      *msg = point_template_;
      msg->header.stamp = this->get_clock()->now();
      enqueue_publish(msg_index, [this, msg]() { point_pub_->publish(*msg); });

    } else if (payload_type_ == "uint8") {
      auto msg = std::make_shared<std_msgs::msg::UInt8MultiArray>();
      msg->data = uint8_template_.data;

      // 타임스탬프만 업데이트 (앞 8바이트) - ROS 2 Time 사용
      const double current_time = static_cast<double>(this->get_clock()->now().nanoseconds()) / 1e9;
      uint8_t ts_bytes[8];
      static_assert(sizeof(double) == 8, "double must be 8 bytes");
      std::memcpy(ts_bytes, &current_time, 8);
      if (msg->data.size() >= 8) {
        std::copy(ts_bytes, ts_bytes + 8, msg->data.begin());
      }

      enqueue_publish(msg_index, [this, msg]() { uint8_pub_->publish(*msg); });

    } else {  // default
      auto msg = std::make_shared<std_msgs::msg::String>();
      const double current_time = static_cast<double>(this->get_clock()->now().nanoseconds()) / 1e9;
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(9) << current_time << ":";
      msg->data = oss.str() + string_data_;
      enqueue_publish(msg_index, [this, msg]() { string_pub_->publish(*msg); });
    }
  }

  void enqueue_publish(int msg_index, std::function<void()> pub_fn)
  {
    // 큐에 메시지 넣기 (큐가 가득 찰 경우 대기)
    std::unique_lock<std::mutex> lk(queue_mtx_);
    queue_cv_.wait_for(lk, std::chrono::seconds(1), [this]() {
      return stop_worker_ || publish_queue_.size() < publish_queue_maxsize_;
    });
    if (!stop_worker_ && publish_queue_.size() >= publish_queue_maxsize_) {
      queue_cv_.wait_for(lk, std::chrono::seconds(5), [this]() {
        return stop_worker_ || publish_queue_.size() < publish_queue_maxsize_;
      });
    }
    if (stop_worker_) {
      return;
    }
    publish_queue_.push(PublishItem{msg_index, std::move(pub_fn)});
    lk.unlock();
    queue_cv_.notify_one();
  }

  void publisher_worker()
  {
    // ROS 2 Time으로 초기화 (나노초 정밀도)
    double last_publish_time = static_cast<double>(this->get_clock()->now().nanoseconds()) / 1e9;

    while (rclcpp::ok() && !stop_worker_) {
      PublishItem item;
      {
        std::unique_lock<std::mutex> lk(queue_mtx_);
        if (!queue_cv_.wait_for(lk, std::chrono::seconds(1), [this]() {
              return stop_worker_ || !publish_queue_.empty();
            }))
        {
          continue;
        }
        if (stop_worker_) {
          break;
        }
        if (publish_queue_.empty()) {
          continue;
        }
        item = std::move(publish_queue_.front());
        publish_queue_.pop();
        lk.unlock();
        queue_cv_.notify_one();
      }

      // ── ⓐ 간격 계산 -----------------------------------
      const double current_time = static_cast<double>(this->get_clock()->now().nanoseconds()) / 1e9;
      const double time_since_last = (current_time - last_publish_time) * 1000.0;  // ms
      {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        interval_list_.push_back(time_since_last);
      }
      // --------------------------------------------------

      // ── ⓑ publish 시간 측정 ---------------------------
      const double start_t = static_cast<double>(this->get_clock()->now().nanoseconds()) / 1e9;
      item.publish_fn();
      const double end_t = static_cast<double>(this->get_clock()->now().nanoseconds()) / 1e9;
      const double publish_dt = (end_t - start_t) * 1000.0;  // ms
      {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        publish_time_list_.push_back(publish_dt);
      }

      if (!first_publish_time_) {
        first_publish_time_ = start_t;
      }
      last_publish_time_ = start_t;
      // --------------------------------------------------

      // ── ⓒ CSV 로깅 -----------------------------------
      const double publish_time_s = start_t;
      try {
        csv_file_ << item.msg_index << "," << std::setprecision(15) << publish_time_s << ","
                  << std::fixed << std::setprecision(6) << publish_dt << "\n";
      } catch (...) {
        // CSV 파일이 닫혔거나 I/O 오류 시 무시
      }
      // --------------------------------------------------

      last_publish_time = end_t;
    }

    // 스레드 종료 직전 CSV flush 보장
    try {
      csv_file_.flush();
    } catch (...) {
    }
  }

  void restore_loss()
  {
    // 링크 단절 복구
    std::cout << "[PUB] Restoring original loss rate: " << original_loss_rate_ << "%" << std::endl;
    std::cout.flush();
    set_netem_loss(original_loss_rate_);
    case_d_burst_active_ = false;
    const auto event_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    std::cout << "[CASE_D_EVENT] time_ns=" << event_time_ns
              << " action=end next_trigger_index=" << case_d_next_trigger_
              << " restored_loss=" << original_loss_rate_ << std::endl;
    std::cout.flush();

    // 타이머가 다시 호출되지 않도록 취소
    if (burst_timer_) {
      burst_timer_->cancel();
      burst_timer_.reset();
      std::cout << "[PUB] Timer cancelled" << std::endl;
      std::cout.flush();
    }
  }

  void prepare_payload()
  {
    if (payload_type_ == "image") {
      // Image 메시지용 데이터 준비
      int img_size = static_cast<int>(std::sqrt(static_cast<double>(payload_size_)));
      if (img_size * img_size != payload_size_) {
        img_size = static_cast<int>(std::sqrt(static_cast<double>(payload_size_))) + 1;
        payload_size_ = img_size * img_size;
      }

      std::mt19937 rng(std::random_device{}());
      std::uniform_int_distribution<int> dist(0, 255);

      image_template_ = sensor_msgs::msg::Image();
      image_template_.header.frame_id = "camera_frame";
      image_template_.height = static_cast<uint32_t>(img_size);
      image_template_.width = static_cast<uint32_t>(img_size);
      image_template_.encoding = "mono8";
      image_template_.step = static_cast<sensor_msgs::msg::Image::_step_type>(img_size);
      image_template_.data.resize(static_cast<size_t>(img_size * img_size));
      for (auto & b : image_template_.data) {
        b = static_cast<uint8_t>(dist(rng));
      }

    } else if (payload_type_ == "point") {
      // PointCloud2 메시지용 데이터 준비
      int n_points = payload_size_ / 12;
      if (n_points * 12 != payload_size_) {
        n_points = (payload_size_ / 12) + 1;
        payload_size_ = n_points * 12;
      }

      std::mt19937 rng(std::random_device{}());
      std::uniform_real_distribution<float> dist(0.0f, 1.0f);

      std::vector<float> points;
      points.resize(static_cast<size_t>(n_points * 3));
      for (auto & f : points) {
        f = dist(rng);
      }

      sensor_msgs::msg::PointField x_field;
      x_field.name = "x";
      x_field.offset = 0;
      x_field.datatype = sensor_msgs::msg::PointField::FLOAT32;
      x_field.count = 1;

      sensor_msgs::msg::PointField y_field;
      y_field.name = "y";
      y_field.offset = 4;
      y_field.datatype = sensor_msgs::msg::PointField::FLOAT32;
      y_field.count = 1;

      sensor_msgs::msg::PointField z_field;
      z_field.name = "z";
      z_field.offset = 8;
      z_field.datatype = sensor_msgs::msg::PointField::FLOAT32;
      z_field.count = 1;

      point_template_ = sensor_msgs::msg::PointCloud2();
      point_template_.header.frame_id = "map";
      point_template_.height = 1;
      point_template_.width = static_cast<uint32_t>(n_points);
      point_template_.fields = {x_field, y_field, z_field};
      point_template_.point_step = 12;  // 3 * float32 = 12 bytes
      point_template_.row_step = static_cast<uint32_t>(n_points * 12);
      point_template_.is_bigendian = false;
      point_template_.is_dense = true;
      point_template_.data.resize(static_cast<size_t>(n_points * 12));
      std::memcpy(point_template_.data.data(), points.data(), point_template_.data.size());

    } else if (payload_type_ == "uint8") {
      // UInt8MultiArray 메시지용 데이터 준비 (C++ 코드와 유사)
      int actual_payload_size = payload_size_ - 8;
      if (actual_payload_size <= 0) {
        actual_payload_size = 1;
      }

      std::mt19937 rng(std::random_device{}());
      std::uniform_int_distribution<int> dist(0, 255);

      std::vector<uint8_t> payload;
      payload.resize(static_cast<size_t>(actual_payload_size));
      for (auto & b : payload) {
        b = static_cast<uint8_t>(dist(rng));
      }

      uint8_template_ = std_msgs::msg::UInt8MultiArray();
      uint8_template_.data.resize(static_cast<size_t>(8 + actual_payload_size));
      // 더미 타임스탬프
      const double dummy_ts = 0.0;
      std::memcpy(uint8_template_.data.data(), &dummy_ts, 8);
      std::memcpy(uint8_template_.data.data() + 8, payload.data(), payload.size());

    } else {
      // String 메시지용 데이터 준비 (타임스탬프 문자열 제외한 크기)
      const std::string timestamp_prefix = "123456.123456:";
      int actual_payload_size = payload_size_ - static_cast<int>(timestamp_prefix.size());
      if (actual_payload_size <= 0) {
        actual_payload_size = 1;
      }
      string_data_.assign(static_cast<size_t>(actual_payload_size), 'A');
    }
  }

private:
  struct PublishItem
  {
    int msg_index{};
    std::function<void()> publish_fn;
  };

  // parameters/state
  double period_s_{0.033};
  double r_ms_{33.0};
  int max_count_{0};
  int count_{0};
  bool sub_ready_{false};

  int payload_size_{0};
  std::string payload_type_{"default"};
  std::string topic_prefix_;
  std::string qos_mode_{"reliable"};
  int qos_depth_{1};

  // publishers (one active depending on payload_type)
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr image_pub_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr point_pub_;
  rclcpp::Publisher<std_msgs::msg::UInt8MultiArray>::SharedPtr uint8_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr string_pub_;

  rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr sub_ready_sub_;
  rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr sub_progress_sub_;

  rclcpp::TimerBase::SharedPtr publish_timer_;

  // prebuilt payload templates
  sensor_msgs::msg::Image image_template_;
  sensor_msgs::msg::PointCloud2 point_template_;
  std_msgs::msg::UInt8MultiArray uint8_template_;
  std::string string_data_;

  // CSV
  std::string csv_filename_;
  std::ofstream csv_file_;

  // worker queue
  std::mutex queue_mtx_;
  std::condition_variable queue_cv_;
  std::queue<PublishItem> publish_queue_;
  size_t publish_queue_maxsize_{10};
  std::thread publisher_thread_;
  bool stop_worker_{false};

  // stats
  mutable std::mutex stats_mtx_;
  std::vector<double> interval_list_;
  std::vector<double> publish_time_list_;
  bool stats_calculated_{false};
  std::optional<double> first_publish_time_;
  std::optional<double> last_publish_time_;

  // link interruption
  int burst_secs_{0};
  bool burst_done_{false};
  rclcpp::TimerBase::SharedPtr burst_timer_;
  double original_loss_rate_{0.0};
  double case_a_loss_pct_{0.0};
  bool case_a_applied_{false};
  bool case_d_enabled_{false};
  int trigger_after_{200};
  bool trigger_reported_{false};
  bool receive_progress_enabled_{false};
  std::vector<int> case_d_triggers_;
  std::vector<int> case_d_durations_s_;
  size_t case_d_next_trigger_{0};
  bool case_d_burst_active_{false};

  // 노드 생성 직전 소켓 fd 집합 (이후 생긴 소켓 = DDS로 추정)
  std::set<int> socket_fds_before_node_;
};

static std::string make_timestamp_yyyymmdd_hhmmss()
{
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return std::string(buf);
}

int main(int argc, char ** argv)
{
  /*
  Usage: dds_pub <loss_rate> <idx> <payload_size> <max_count> <max_samples> <socket_buffer> <payload_type> <publish_period> [case_d_secs] [hz] [topic_prefix] [qos_mode] [qos_depth] [case_d_enabled] [trigger_after]
  예: dds_pub 0 1 128000 100 400 212992 image 0.033 20 30 main reliable 1 1 200
  */

  if (argc < 9 + 1) {
    std::cerr << "Usage: " << argv[0]
              << " <loss_rate> <idx> <payload_size> <max_count> <max_samples> <socket_buffer>"
              << " <payload_type> <publish_period> [case_d_secs] [hz] [topic_prefix] [qos_mode]"
              << " [qos_depth] [case_d_enabled] [trigger_after]" << std::endl;
    return 1;
  }

  const std::string loss_rate_str = argv[1];
  const std::string idx_str = argv[2];
  const std::string payload_size_str = argv[3];
  const std::string max_count_str = argv[4];
  const std::string max_samples_str = argv[5];
  const std::string socket_buffer_str = argv[6];
  const std::string payload_type = argv[7];
  const std::string publish_period_str = argv[8];
  const int burst_secs = (argc > 9) ? std::atoi(argv[9]) : 0;
  int publish_hz = (argc > 10) ? std::atoi(argv[10]) : 0;
  const std::string topic_prefix = (argc > 11) ? std::string(argv[11]) : "";
  const std::string qos_mode = (argc > 12) ? std::string(argv[12]) : "reliable";
  const int qos_depth = (argc > 13) ? std::atoi(argv[13]) : 1;
  const bool case_d_enabled = (argc > 14) ? (std::atoi(argv[14]) != 0) : false;
  const int trigger_after = (argc > 15) ? std::atoi(argv[15]) : 200;

  // (1) 인자 파싱
  double loss_rate = 0.0;
  try {
    loss_rate = std::stod(loss_rate_str);
  } catch (...) {
    loss_rate = 0.0;
  }

  int idx = 1;
  try {
    idx = std::stoi(idx_str);
  } catch (...) {
    idx = 1;
  }

  int payload_size = 1000;
  try {
    payload_size = std::stoi(payload_size_str);
  } catch (...) {
    payload_size = 1000;
  }

  int max_count = 100;
  try {
    max_count = std::stoi(max_count_str);
  } catch (...) {
    max_count = 100;
  }

  int max_samples = 5000;
  try {
    max_samples = std::stoi(max_samples_str);
  } catch (...) {
    max_samples = 5000;
  }

  int socket_buffer = 212992;
  try {
    socket_buffer = std::stoi(socket_buffer_str);
  } catch (...) {
    socket_buffer = 212992;
  }

  double publish_period = 0.033;
  try {
    publish_period = std::stod(publish_period_str);
  } catch (...) {
    publish_period = 0.033;
  }
  if (publish_hz <= 0 && publish_period > 0.0) {
    publish_hz = static_cast<int>(std::round(1.0 / publish_period));
  }

  // 소켓 버퍼/sysctl 및 netem은 변경하지 않고 OS 기본값을 사용합니다.

  // Fast DDS XML 프로파일/Optimizer를 적용하지 않고 기본 프로파일을 사용합니다.
  const std::string rmw_impl = getenv_str("RMW_IMPLEMENTATION", "rmw_fastrtps_cpp"); //RMW_IMPLEMENTATION 읽어보고 없으면 fastrtps로 설정.
  /* if (rmw_impl == "rmw_fastrtps_cpp" || rmw_impl == "rmw_fastrtps_dynamic_cpp") {
    std::string pkg_root = getenv_str("LOOPBACK_TEST_PKG_ROOT", "");
    if (pkg_root.empty()) {
      // Python의 __file__ 기반 탐색을 C++에서는 대체: 현재 작업 디렉토리에서 상위로 탐색
      std::filesystem::path d = std::filesystem::current_path();
      while (d != d.parent_path()) {
        if (std::filesystem::is_directory(d / "large-data-optimization")) {
          pkg_root = d.string();
          break;
        }
        d = d.parent_path();
      }
    }
    const std::string use_opt = getenv_str("USE_DDS_OPTIMIZER", "1");
    const bool use_optimizer = (use_opt != "0" && use_opt != "false" && use_opt != "no");
    const std::string optimized_pub =
      (!pkg_root.empty())
        ? (std::filesystem::path(pkg_root) / "large-data-optimization" / "Optimized_profile_pub.xml").string()
        : "";

    if (use_optimizer && !pkg_root.empty() && std::filesystem::is_regular_file(optimized_pub)) {
      setenv("FASTRTPS_DEFAULT_PROFILES_FILE", optimized_pub.c_str(), 1);
      std::cout << "[PUB] Using optimized profile: " << optimized_pub << std::endl;
      std::cout.flush();
    } else {
      auto xml_path = generate_rmw_xml(max_samples, socket_buffer, MAX_MESSAGE_SIZE);
      if (xml_path) {
        const std::string abs_xml = std::filesystem::absolute(*xml_path).string();
        setenv("FASTRTPS_DEFAULT_PROFILES_FILE", abs_xml.c_str(), 1);
        std::cout << "[PUB] Using fallback profile: " << abs_xml << std::endl;
        std::cout.flush();
      }
    }
  } else if (rmw_impl == "rmw_cyclonedds_cpp") {
    std::string pkg_root = getenv_str("LOOPBACK_TEST_PKG_ROOT", "");
    if (pkg_root.empty()) {
      std::filesystem::path d = std::filesystem::current_path();
      while (d != d.parent_path()) {
        if (std::filesystem::is_directory(d / "large-data-optimization")) {
          pkg_root = d.string();
          break;
        }
        d = d.parent_path();
      }
    }
    const std::string use_opt = getenv_str("USE_DDS_OPTIMIZER", "1");
    const bool use_optimizer = (use_opt != "0" && use_opt != "false" && use_opt != "no");
    const std::string optimized_pub =
      (!pkg_root.empty())
        ? (std::filesystem::path(pkg_root) / "large-data-optimization" / "Optimized_profile_pub_cyclonedds.xml").string()
        : "";

    if (use_optimizer && !pkg_root.empty() && std::filesystem::is_regular_file(optimized_pub)) {
      const std::string uri = std::string("file://") + optimized_pub;
      setenv("CYCLONEDDS_URI", uri.c_str(), 1);
      std::cout << "[PUB] Using optimized CycloneDDS profile: " << optimized_pub << std::endl;
      std::cout.flush();
    } else {
      auto xml_path = generate_rmw_xml(max_samples, socket_buffer, MAX_MESSAGE_SIZE);
      if (xml_path) {
        const std::string abs_xml = std::filesystem::absolute(*xml_path).string();
        const std::string uri = std::string("file://") + abs_xml;
        setenv("CYCLONEDDS_URI", uri.c_str(), 1);
        std::cout << "[PUB] Using fallback CycloneDDS profile: " << abs_xml << std::endl;
        std::cout.flush();
      }
    }
  } */
  // Zenoh는 XML 파일 불필요

  // (4) CSV 파일 이름 생성 (세션 구분을 위해 타임스탬프 추가)
  const std::string csv_dir = getenv_str(
    "CALM_RESULT_DIR", "/home/csilab/ros2_ws/results/test_yw");
  const std::string ts = make_timestamp_yyyymmdd_hhmmss();
  std::ostringstream csv_oss;
  csv_oss << csv_dir << "/"
          << "pub_loss" << loss_rate
          << "_idx" << idx
          << "_payload" << payload_size
          << "_hz" << publish_hz
          << "_count" << max_count
          << "_samples" << max_samples
          << "_sock" << socket_buffer
          << "_topic" << sanitize_label(topic_prefix, "default")
          << "_qos" << sanitize_label(qos_mode, "reliable")
          << "_" << payload_type
          << "_" << ts
          << ".csv";
  const std::string csv_filename = csv_oss.str();

  // (5) ROS 초기화
  // 적용 순서 (FastDDS): ① 위 (4)에서 setenv(FASTRTPS_DEFAULT_PROFILES_FILE) ② rclcpp::init()
  // ③ make_shared<DDSPub> → create_publisher(). 프로파일은 "엔티티 생성 전"에 읽혀야 하므로,
  // ①이 ②·③보다 반드시 먼저 실행되어야 한다. 현재 순서가 맞음.
  if (rmw_impl == "rmw_fastrtps_cpp" || rmw_impl == "rmw_fastrtps_dynamic_cpp") {
    const char * prof = std::getenv("FASTRTPS_DEFAULT_PROFILES_FILE");
    if (prof && *prof)
      std::cout << "[PUB] Before init: FASTRTPS_DEFAULT_PROFILES_FILE=" << prof << std::endl;
    else
      std::cout << "[PUB] Before init: FASTRTPS_DEFAULT_PROFILES_FILE not set" << std::endl;
    std::cout.flush();
  }
  // Python 버전(rclpy.init(args=sys.argv[6:]))처럼, 사용자 인자는 ROS로 넘기지 않음
  int ros_argc = 0;
  char ** ros_argv = nullptr;
  rclcpp::init(ros_argc, ros_argv);

  const char * actual_rmw = rmw_get_implementation_identifier();
  const std::string actual_rmw_impl = actual_rmw ? actual_rmw : "(null)";
  const bool rmw_matches = actual_rmw_impl == rmw_impl;
  std::cout << "[DDS_ENDPOINT] role=publisher expected_rmw=" << rmw_impl
            << " actual_rmw=" << actual_rmw_impl
            << " match=" << (rmw_matches ? "true" : "false") << std::endl;
  std::cout.flush();
  if (!rmw_matches) {
    std::cerr << "[DDS_ENDPOINT] Refusing mixed-RMW experiment" << std::endl;
    rclcpp::shutdown();
    return 2;
  }

  // 노드 생성 직전 소켓 fd 수집 → 이후 생긴 소켓만 "DDS로 추정"하여 출력
  std::set<int> fds_before_node;
  for (const auto & s : get_all_socket_info_in_process())
    fds_before_node.insert(s.fd);

  auto node = std::make_shared<DDSPub>(
    csv_filename, max_count, payload_size, payload_type, publish_period, burst_secs, loss_rate,
    topic_prefix, qos_mode, qos_depth, case_d_enabled, trigger_after, fds_before_node);

  std::vector<SocketInfo> socks = get_all_socket_info_in_process();
  int n_new = 0;
  for (const auto & s : socks)
    if (fds_before_node.count(s.fd) == 0) n_new++;
  if (n_new > 0) {
    std::cout << "[PUB] Sockets created after node creation (likely DDS transport), " << n_new << " sockets:" << std::endl;
    for (const auto & s : socks) {
      if (fds_before_node.count(s.fd) != 0) continue;
      const char * typestr = (s.type == SOCK_DGRAM) ? "UDP" : "TCP";
      std::cout << "[PUB]   fd=" << s.fd << " type=" << typestr
                << " local=" << s.local_addr_port
                << " SO_SNDBUF=" << s.so_sndbuf << std::endl;
    }
    fflush(stdout);
  } else {
    std::cout << "[PUB] no new sockets after node creation (or none found)" << std::endl;
    fflush(stdout);
  }

  try {
    rclcpp::spin(node);
  } catch (const std::exception &) {
  } catch (...) {
  }

  // 종료 시점에도 통계 출력 시도 (정상 완료 전 종료된 경우 대비)
  try {
    if (!node->stats_calculated() && node->interval_size() > 1 && node->publish_time_size() > 0) {
      node->report_averages();
    }
  } catch (...) {
  }

  if (rclcpp::ok()) { }
  rclcpp::shutdown();
  if (case_d_enabled || env_double("CALM_CASE_A_LOSS_PCT", 0.0) > 0.0) {
    set_netem_loss(loss_rate);
  }
  return 0;
}
