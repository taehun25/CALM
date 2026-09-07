/*=*
 * Usage: dds_sub <loss_rate> <idx> <payload_size> <max_count> <max_samples> <socket_buffer> <case_d_secs> <payload_type> [hz] [topic_prefix] [qos_mode] [qos_depth] [result_suffix]
 * 예) ros2 run calm_pretest_yw dds_sub 0 1 128000 100 400 0 0 image 30 main reliable 1 beta_0.8_gamma_0.5_alpha_1_qmin_0.05_normal
 *
 * rclcpp 포트: sub_original.py 동일 동작
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits.h>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <rclcpp/rclcpp.hpp>
#include <rmw/rmw.h>
#include <rclcpp/qos.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <std_msgs/msg/string.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

// ─── 상수 경로 / 인터페이스 (패키지 루트는 get_pkg_root()로 결정: LOOPBACK_TEST_PKG_ROOT 또는 walk-up) ───
static const char* network_interface()
{
  const char* configured = std::getenv("CALM_NET_INTERFACE");
  return configured != nullptr && configured[0] != '\0' ? configured : "wlp2s0";
}
static const int MAX_MESSAGE_SIZE = 1472;
static const int64_t LINK_THROUGHPUT_BPS = 90000000;
static const double LINK_UTILIZATION = 0.6;
static const double DEFAULT_PUBLISH_RATE_HZ = 30.0;

static double short_inactive_timeout_s()
{
  const char* raw = std::getenv("CALM_SUB_INACTIVE_TIMEOUT_S");
  if (raw) {
    char* end = nullptr;
    const double value = std::strtod(raw, &end);
    if (end != raw && *end == '\0' && value > 0.0)
      return value;
  }
  return 30.0;
}

static std::vector<int> progress_thresholds()
{
  std::vector<int> values;
  const char* raw = std::getenv("CALM_PROGRESS_THRESHOLDS");
  if (raw == nullptr || raw[0] == '\0')
    return values;

  std::stringstream stream(raw);
  std::string token;
  while (std::getline(stream, token, ',')) {
    try {
      const int value = std::stoi(token);
      if (value > 0)
        values.push_back(value);
    } catch (...) {
    }
  }
  return values;
}

static std::string get_pkg_root()
{
  const char* proot = std::getenv("LOOPBACK_TEST_PKG_ROOT");
  if (proot && proot[0] != '\0')
    return std::string(proot);
  char exe_path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len > 0) {
    exe_path[len] = '\0';
    std::string d(exe_path);
    size_t last_slash = d.find_last_of("/");
    d = (last_slash != std::string::npos) ? d.substr(0, last_slash) : ".";
    while (d != "." && !d.empty()) {
      std::string opt_dir = d + "/large-data-optimization";
      std::string res_dir = d + "/resource_sub";
      struct stat st;
      if ((stat(opt_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ||
          (stat(res_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode))) {
        return d;
      }
      size_t up = d.find_last_of("/");
      if (up == std::string::npos || up == 0)
        break;
      d = d.substr(0, up);
    }
  }
  return "";
}

/** 소켓 하나에 대한 정보 (fd, 타입, 로컬 주소:포트, SO_SNDBUF). */
struct SocketInfo {
  int fd;
  int type;
  std::string local_addr_port;
  int so_sndbuf;
};

/** 이 프로세스에 열린 소켓들을 수집. DDS 소켓 포함. */
static std::vector<SocketInfo> get_all_socket_info_in_process()
{
  std::vector<SocketInfo> out;
  const int max_fd = 1024;
  for (int fd = 0; fd < max_fd; ++fd) {
    int sndbuf = 0;
    socklen_t len = sizeof(sndbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) != 0)
      continue;
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
      if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) == 0) {
        char buf[INET_ADDRSTRLEN];
        const char* a = inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        local_str = a ? std::string(a) : "?";
        local_str += ":";
        local_str += std::to_string(ntohs(addr.sin_port));
      } else {
        local_str = "?:?";
      }
    } else if (domain == AF_INET6) {
      struct sockaddr_in6 addr = {};
      socklen_t addrlen = sizeof(addr);
      if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) == 0) {
        char buf[INET6_ADDRSTRLEN];
        const char* a = inet_ntop(AF_INET6, &addr.sin6_addr, buf, sizeof(buf));
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

// ════════════════════════════════════════════════════════════════════
// qdisc 기반 손실 설정 / 해제
// ════════════════════════════════════════════════════════════════════
// Python: subprocess.run(del_cmd, ..., stdout=DEVNULL, stderr=DEVNULL); if loss_rate<=0: return True; subprocess.run(add_cmd, check=True)
static bool set_ingress_loss(double loss_rate)
{
  std::ostringstream del_cmd;
  del_cmd << "echo 'csi123' | sudo -S tc qdisc del dev " << network_interface() << " root >/dev/null 2>&1";
  std::system(del_cmd.str().c_str());

  if (loss_rate <= 0.0)
    return true;

  std::ostringstream cmd;
  cmd << "echo 'csi123' | sudo -S tc qdisc add dev " << network_interface()
      << " root netem loss " << loss_rate << "%";
  int ret = std::system(cmd.str().c_str());
  if (ret == 0) {
    printf("[SUB] %.1f%% loss configured via qdisc on %s\n", loss_rate, network_interface());
    return true;
  }
  printf("[SUB] [ERROR] Failed to set qdisc loss (ret=%d)\n", ret);
  return false;
}

// Python: subprocess.run(..., stdout=DEVNULL, stderr=DEVNULL)
static void clear_ingress_loss()
{
  std::ostringstream cmd;
  cmd << "echo 'csi123' | sudo -S tc qdisc del dev " << network_interface() << " root >/dev/null 2>&1";
  std::system(cmd.str().c_str());
}

// ════════════════════════════════════════════════════════════════════
// DDS Optimizer 실행 (Python: cwd=dirname(script), timeout=30, stdout/stderr=DEVNULL)
// ════════════════════════════════════════════════════════════════════
static void run_dds_optimizer(double publish_rate_hz, int payload_bytes,
                              int64_t T_bps = LINK_THROUGHPUT_BPS,
                              double w = LINK_UTILIZATION)
{
  std::string script_path = get_pkg_root() + "/large-data-optimization/DDS_Optimizer.py";
  std::ifstream f(script_path);
  if (!f.good())
    return;
  f.close();

  size_t last_slash = script_path.find_last_of("/");
  std::string script_dir = (last_slash != std::string::npos) ? script_path.substr(0, last_slash) : ".";

  std::ostringstream cmd;
  cmd << "cd " << script_dir << " && python3 " << script_path
      << " r=" << publish_rate_hz
      << " u=" << payload_bytes
      << " T=" << T_bps
      << " w=" << w
      << " >/dev/null 2>&1";
  std::system(cmd.str().c_str());
}

// ════════════════════════════════════════════════════════════════════
// RMW XML 템플릿 → 플레이스홀더 치환
// ════════════════════════════════════════════════════════════════════
static std::string get_rmw_implementation()
{
  const char* v = std::getenv("RMW_IMPLEMENTATION");
  return v ? std::string(v) : std::string("rmw_fastrtps_cpp");
}

static std::string generate_rmw_xml(int max_samples, int socket_buffer, int max_message_size)
{
  std::string rmw_impl = get_rmw_implementation();
  std::string base_path = get_pkg_root().empty() ? "" : (get_pkg_root() + "/resource_sub");
  if (base_path.empty())
    return "";

  std::string template_path;
  std::string output_path;

  if (rmw_impl == "rmw_zenoh_cpp")
    return "";

  if (rmw_impl == "rmw_fastrtps_cpp" || rmw_impl == "rmw_fastrtps_dynamic_cpp") {
    template_path = base_path + "/subscriber_default.xml";
    output_path = base_path + "/subscriber_new.xml";
  } else if (rmw_impl == "rmw_cyclonedds_cpp") {
    template_path = base_path + "/subscriber_default_cyclonedds.xml";
    output_path = base_path + "/subscriber_new_cyclonedds.xml";
  } else {
    template_path = base_path + "/subscriber_default.xml";
    output_path = base_path + "/subscriber_new.xml";
  }
  std::ifstream in(template_path);
  if (!in)
    return "";

  std::stringstream buf;
  buf << in.rdbuf();
  std::string content = buf.str();
  in.close();

  std::ostringstream out_lines;
  std::istringstream line_stream(content);
  std::string line;
  bool skip_general = false;

  while (std::getline(line_stream, line)) {
    if (rmw_impl == "rmw_cyclonedds_cpp") {
      if (line.find("<General>") != std::string::npos) {
        skip_general = true;
        continue;
      }
      if (skip_general) {
        if (line.find("</General>") != std::string::npos)
          skip_general = false;
        continue;
      }
    }

    std::string stripped = line;
    size_t start = line.find_first_not_of(" \t");
    if (start != std::string::npos)
      stripped = line.substr(start);

    if (stripped.substr(0, 4) == "<!--" || (!stripped.empty() && stripped[0] == '*')) {
      out_lines << line << "\n";
      continue;
    }

    if (line.find("MAX_SAMPLES") != std::string::npos ||
        line.find("MAX_MESSAGE_SIZE") != std::string::npos) {
      out_lines << "<!-- " << stripped << " (disabled, using DDS default) -->\n";
    } else {
      out_lines << line << "\n";
    }
  }

  std::string result = out_lines.str();
  size_t pos = 0;
  std::string placeholder = "SOCKET_BUFFER";
  std::string replacement = std::to_string(socket_buffer);
  while ((pos = result.find(placeholder, pos)) != std::string::npos) {
    result.replace(pos, placeholder.size(), replacement);
    pos += replacement.size();
  }

  std::ofstream out(output_path);
  if (!out)
    return "";
  out << result;
  out.close();
  return output_path;
}



// ════════════════════════════════════════════════════════════════════
// 통계 헬퍼
// ════════════════════════════════════════════════════════════════════
static double mean(const std::vector<double>& v)
{
  if (v.empty())
    return std::nan("");
  return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

static double stdev(const std::vector<double>& v)
{
  if (v.size() <= 1)
    return std::nan("");
  double m = mean(v);
  double sq_sum = 0.0;
  for (double x : v)
    sq_sum += (x - m) * (x - m);
  return std::sqrt(sq_sum / static_cast<double>(v.size() - 1));
}

static std::string sanitize_label(const std::string& text, const std::string& fallback = "default")
{
  if (text.empty())
    return fallback;
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
      out.push_back(c);
    else
      out.push_back('_');
  }
  return out.empty() ? fallback : out;
}

static std::string topic_name(const std::string& prefix, const std::string& base)
{
  if (prefix.empty())
    return base;
  return sanitize_label(prefix, "main") + "_" + base;
}

static std::string node_name(const std::string& base, const std::string& prefix)
{
  if (prefix.empty())
    return base;
  return base + "_" + sanitize_label(prefix, "main");
}

static rclcpp::QoS make_data_qos(const std::string& qos_mode, int depth)
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

// ════════════════════════════════════════════════════════════════════
// DDSSub 노드
// ════════════════════════════════════════════════════════════════════
class DDSSub : public rclcpp::Node
{
public:
  DDSSub(std::atomic<bool>& done_flag,
         const std::string& csv_filename,
         int max_count,
         double orig_loss_pct,
         int burst_secs,
         const std::string& payload_type,
         const std::string& topic_prefix,
         const std::string& qos_mode,
         int qos_depth,
         std::set<int> socket_fds_before_node = {})
    : Node(node_name("dds_sub_lo", topic_prefix)),
      done_flag_(done_flag),
      max_count_(max_count),
      payload_type_(payload_type),
      topic_prefix_(sanitize_label(topic_prefix, "")),
      qos_mode_(qos_mode),
      qos_depth_(qos_depth > 0 ? qos_depth : 1),
      recv_count_(0),
      start_time_(std::nan("")),
      first_send_time_(std::nan("")),
      last_send_time_(std::nan("")),
      first_recv_t_(std::nan("")),
      last_recv_t_(std::nan("")),
      orig_loss_pct_(orig_loss_pct),
      progress_thresholds_(progress_thresholds()),
      socket_fds_before_node_(std::move(socket_fds_before_node))
  {
    rclcpp::QoS sync_qos = make_sync_qos();
    rclcpp::QoS data_qos = make_data_qos(qos_mode_, qos_depth_);

    // Python: print(qos_profile_new.reliability)
    RCLCPP_INFO(this->get_logger(), "%d", static_cast<int>(data_qos.reliability()));
    RCLCPP_INFO(this->get_logger(), "[SUB] topic_prefix=%s qos_mode=%s qos_depth=%d",
                topic_prefix_.empty() ? "(default)" : topic_prefix_.c_str(),
                qos_mode_.c_str(), qos_depth_);

    if (payload_type == "image") {
      sub_ = create_subscription<sensor_msgs::msg::Image>(
          topic_name(topic_prefix_, "image_topic"), data_qos,
          std::bind(&DDSSub::callback_image, this, std::placeholders::_1));
    } else if (payload_type == "point") {
      sub_ = create_subscription<sensor_msgs::msg::PointCloud2>(
          topic_name(topic_prefix_, "point_topic"), data_qos,
          std::bind(&DDSSub::callback_point, this, std::placeholders::_1));
    } else if (payload_type == "uint8") {
      sub_ = create_subscription<std_msgs::msg::UInt8MultiArray>(
          topic_name(topic_prefix_, "uint8_topic"), data_qos,
          std::bind(&DDSSub::callback_uint8, this, std::placeholders::_1));
    } else {
      sub_ = create_subscription<std_msgs::msg::String>(
          topic_name(topic_prefix_, "string_topic"), data_qos,
          std::bind(&DDSSub::callback_string, this, std::placeholders::_1));
    }

    ready_pub_ = create_publisher<std_msgs::msg::UInt8MultiArray>(
      topic_name(topic_prefix_, "sub_ready_topic"), sync_qos);
    done_pub_ = create_publisher<std_msgs::msg::UInt8MultiArray>(
      topic_name(topic_prefix_, "sub_done_topic"), sync_qos);
    progress_pub_ = create_publisher<std_msgs::msg::UInt8MultiArray>(
      topic_name(topic_prefix_, "sub_progress_topic"), sync_qos);

    // CSV 디렉토리 생성 및 파일 열기
    std::string dir = csv_filename.substr(0, csv_filename.find_last_of("/"));
    std::string mkdir_cmd = "mkdir -p " + dir;
    std::system(mkdir_cmd.c_str());

    csv_file_.open(csv_filename);
    if (csv_file_.is_open()) {
      csv_file_ << "msg_index,send_time_s,delay_ms\n";
      csv_file_.flush();
    }

    last_recv_t_ = now_sec();
    ready_timer_ = create_wall_timer(std::chrono::seconds(1), std::bind(&DDSSub::publish_ready_signal, this));
    check_timer_ = create_wall_timer(std::chrono::seconds(1), std::bind(&DDSSub::check_inactive, this));

    RCLCPP_INFO(this->get_logger(), "[SUB] Node started. Expecting %d messages.", max_count);
    RCLCPP_INFO(this->get_logger(), "[SUB] CSV => %s", csv_filename.c_str());

    publish_ready_signal();
  }

  void publish_ready_signal()
  {
    std_msgs::msg::UInt8MultiArray msg;
    msg.data.push_back(1);
    ready_pub_->publish(msg);
  }

  void publish_done_signal()
  {
    std_msgs::msg::UInt8MultiArray msg;
    msg.data.push_back(1);
    done_pub_->publish(msg);
  }

  void callback_image(const sensor_msgs::msg::Image::SharedPtr msg)
  {
    double recv_time = now_sec();
    last_recv_t_ = recv_time;
    recv_count_++;

    if (std::isnan(start_time_))
      start_time_ = last_recv_t_;

    double send_time = static_cast<double>(msg->header.stamp.sec) +
                       static_cast<double>(msg->header.stamp.nanosec) / 1e9;
    double delay_ms = (recv_time - send_time) * 1000.0;

    if (recv_count_ == 1) {
      first_send_time_ = send_time;
      first_recv_t_ = last_recv_t_;
    }
    last_send_time_ = send_time;

    write_csv_row(recv_count_, send_time, delay_ms);
    csv_file_.flush();
    delays_.push_back(delay_ms);
    publish_progress_if_needed();
    check_completion();
  }

  void callback_point(const sensor_msgs::msg::PointCloud2::SharedPtr msg)
  {
    double recv_time = now_sec();
    last_recv_t_ = recv_time;
    recv_count_++;

    if (std::isnan(start_time_))
      start_time_ = last_recv_t_;

    double send_time = static_cast<double>(msg->header.stamp.sec) +
                       static_cast<double>(msg->header.stamp.nanosec) / 1e9;
    double delay_ms = (recv_time - send_time) * 1000.0;

    if (recv_count_ == 1) {
      first_send_time_ = send_time;
      first_recv_t_ = last_recv_t_;
    }
    last_send_time_ = send_time;

    write_csv_row(recv_count_, send_time, delay_ms);
    csv_file_.flush();
    delays_.push_back(delay_ms);
    publish_progress_if_needed();
    check_completion();
  }

  void callback_string(const std_msgs::msg::String::SharedPtr msg)
  {
    double recv_time = now_sec();
    last_recv_t_ = recv_time;
    recv_count_++;

    if (std::isnan(start_time_))
      start_time_ = last_recv_t_;

    double send_time = recv_time;
    double delay_ms = 0.0;
    std::string data = msg->data;
    size_t colon_pos = data.find(':');
    if (colon_pos != std::string::npos) {
      try {
        send_time = std::stod(data.substr(0, colon_pos));
        delay_ms = (recv_time - send_time) * 1000.0;
      } catch (...) {
        send_time = recv_time;
        delay_ms = 0.0;
      }
    }

    if (recv_count_ == 1) {
      first_send_time_ = send_time;
      first_recv_t_ = last_recv_t_;
    }
    last_send_time_ = send_time;

    write_csv_row(recv_count_, send_time, delay_ms);
    csv_file_.flush();
    delays_.push_back(delay_ms);
    publish_progress_if_needed();
    check_completion();
  }

  void callback_uint8(const std_msgs::msg::UInt8MultiArray::SharedPtr msg)
  {
    double recv_time = now_sec();
    last_recv_t_ = recv_time;
    recv_count_++;

    if (std::isnan(start_time_))
      start_time_ = last_recv_t_;

    double send_time = recv_time;
    double delay_ms = 0.0;
    try {
      if (msg->data.size() >= 8) {
        double ts = 0.0;
        std::memcpy(&ts, msg->data.data(), sizeof(double));
        send_time = ts;
        delay_ms = (recv_time - send_time) * 1000.0;
      }
    } catch (...) {
      send_time = recv_time;
      delay_ms = 0.0;
    }

    if (recv_count_ == 1) {
      first_send_time_ = send_time;
      first_recv_t_ = last_recv_t_;
    }
    last_send_time_ = send_time;

    write_csv_row(recv_count_, send_time, delay_ms);
    csv_file_.flush();
    delays_.push_back(delay_ms);
    publish_progress_if_needed();
    check_completion();
  }

  void publish_progress_if_needed()
  {
    while (next_progress_threshold_ < progress_thresholds_.size() &&
           recv_count_ >= progress_thresholds_[next_progress_threshold_]) {
      const uint64_t count = static_cast<uint64_t>(recv_count_);
      std_msgs::msg::UInt8MultiArray msg;
      msg.data.resize(sizeof(count));
      std::memcpy(msg.data.data(), &count, sizeof(count));
      progress_pub_->publish(msg);
      RCLCPP_INFO(this->get_logger(), "[SUB_PROGRESS_SENT] recv_count=%d threshold=%d",
                  recv_count_, progress_thresholds_[next_progress_threshold_]);
      ++next_progress_threshold_;
    }
  }

  void check_completion()
  {
    if (recv_count_ >= max_count_) {
      RCLCPP_INFO(this->get_logger(),
                  "[SUB] Received %d messages (max_count: %d) – finalizing.", recv_count_, max_count_);
      do_finalize("max_count reached");
    }
  }

  void check_inactive()
  {
    if (recv_count_ >= max_count_)
      return;

    double now = now_sec();
    if (recv_count_ > 0 && (now - last_recv_t_) > short_inactive_timeout_s())
      do_finalize("Short inactivity timeout");
  }

  // Python: do_finalize – print stats, csv flush/close, done_future.set_result(True); no publish_done_signal
  void do_finalize(const std::string& reason)
  {
    // 수신 완료 시점에 노드 생성 후 생긴 소켓(DDS 추정) 한 번 더 출력
    std::vector<SocketInfo> socks = get_all_socket_info_in_process();
    int n_new = 0;
    for (const auto& s : socks)
      if (socket_fds_before_node_.count(s.fd) == 0) n_new++;
    if (n_new > 0) {
      printf("[SUB] Sockets after receive done (likely DDS), %d sockets:\n", n_new);
      for (const auto& s : socks) {
        if (socket_fds_before_node_.count(s.fd) != 0) continue;
        const char* typestr = (s.type == SOCK_DGRAM) ? "UDP" : "TCP";
        printf("[SUB]   fd=%d type=%s local=%s SO_SNDBUF=%d\n",
               s.fd, typestr, s.local_addr_port.c_str(), s.so_sndbuf);
      }
      fflush(stdout);
    }

    double avg_delay = mean(delays_);
    double std_delay = stdev(delays_);

    double hz_pub = std::nan("");
    if (!std::isnan(first_send_time_) && last_send_time_ > first_send_time_ && recv_count_ > 1)
      hz_pub = static_cast<double>(recv_count_ - 1) / (last_send_time_ - first_send_time_);

    double hz_sub = std::nan("");
    if (!std::isnan(first_recv_t_) && last_recv_t_ > first_recv_t_ && recv_count_ > 1)
      hz_sub = static_cast<double>(recv_count_ - 1) / (last_recv_t_ - first_recv_t_);

    printf("[SUB_STATS] hz_sub: %.3f\n", hz_sub);
    printf("[SUB_STATS] avg_delay: %.3f\n", avg_delay);
    printf("[SUB_STATS] std_delay: %.3f\n", std_delay);
    printf("[SUB_STATS] hz_pub: %.3f\n", hz_pub);
    printf("[SUB_STATS] recv_count: %d\n", recv_count_);

    if (csv_file_.is_open()) {
      csv_file_.flush();
      csv_file_.close();
    }

    done_flag_.store(true);
  }

  bool is_done() const { return done_flag_.load(); }

private:
  double now_sec()
  {
    return static_cast<double>(this->get_clock()->now().nanoseconds()) / 1e9;
  }

  void write_csv_row(int idx, double send_time, double delay_ms)
  {
    if (csv_file_.is_open()) {
      csv_file_ << idx << "," << send_time << "," << delay_ms << "\n";
      csv_file_.flush();
    }
  }

  std::atomic<bool>& done_flag_;
  int max_count_;
  std::string payload_type_;
  std::string topic_prefix_;
  std::string qos_mode_{"reliable"};
  int qos_depth_{1};

  rclcpp::SubscriptionBase::SharedPtr sub_;
  rclcpp::Publisher<std_msgs::msg::UInt8MultiArray>::SharedPtr ready_pub_;
  rclcpp::Publisher<std_msgs::msg::UInt8MultiArray>::SharedPtr done_pub_;
  rclcpp::Publisher<std_msgs::msg::UInt8MultiArray>::SharedPtr progress_pub_;

  std::ofstream csv_file_;
  std::vector<double> delays_;
  int recv_count_;
  double start_time_;
  double first_send_time_;
  double last_send_time_;
  double first_recv_t_;
  double last_recv_t_;
  double orig_loss_pct_;
  std::vector<int> progress_thresholds_;
  size_t next_progress_threshold_{0};
  std::set<int> socket_fds_before_node_;

  rclcpp::TimerBase::SharedPtr ready_timer_;
  rclcpp::TimerBase::SharedPtr check_timer_;
};

// ════════════════════════════════════════════════════════════════════
// main()
// ════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[])
{
  // Python: if len(sys.argv) < 9: sys.exit(1)  (usage 출력 없음)
  if (argc < 9) {
    return 1;
  }

  double loss_pct = std::stod(argv[1]);
  int idx = std::stoi(argv[2]);
  int payload_bytes = std::stoi(argv[3]);
  int max_count = std::stoi(argv[4]);
  int max_samples = std::stoi(argv[5]);
  int socket_buffer = std::stoi(argv[6]);
  int burst_secs = std::stoi(argv[7]);
  std::string payload_type = argv[8];
  int publish_hz = (argc > 9) ? std::atoi(argv[9]) : 0;
  std::string topic_prefix = (argc > 10) ? std::string(argv[10]) : "";
  std::string qos_mode = (argc > 11) ? std::string(argv[11]) : "reliable";
  int qos_depth = (argc > 12) ? std::atoi(argv[12]) : 1;
  std::string result_suffix = (argc > 13) ? sanitize_label(argv[13], "") : "";

  // 소켓 버퍼/sysctl 및 netem은 변경하지 않고 OS 기본값을 사용합니다.

  std::string rmw_impl = get_rmw_implementation();
  const char* use_opt_env = std::getenv("USE_DDS_OPTIMIZER");
  std::string use_opt_str = use_opt_env ? use_opt_env : "1";
  bool use_optimizer = (use_opt_str != "0" && use_opt_str != "false" && use_opt_str != "no");

  /* if (rmw_impl == "rmw_fastrtps_cpp" || rmw_impl == "rmw_fastrtps_dynamic_cpp" || rmw_impl == "rmw_cyclonedds_cpp") {
    if (use_optimizer) {
      int64_t T_bps = LINK_THROUGHPUT_BPS;
      const char* t_env = std::getenv("DDS_OPTIMIZER_T_BPS");
      if (t_env)
        T_bps = std::stoll(t_env);
      double r_hz = DEFAULT_PUBLISH_RATE_HZ;
      const char* r_env = std::getenv("DDS_OPTIMIZER_R_HZ");
      if (r_env)
        r_hz = std::stod(r_env);
      run_dds_optimizer(r_hz, payload_bytes, T_bps, LINK_UTILIZATION);
    }
  } */

  // 사용자 정의 DDS XML 프로파일도 비활성화합니다.
#if 0
  if (rmw_impl == "rmw_fastrtps_cpp" || rmw_impl == "rmw_fastrtps_dynamic_cpp") {
    std::string pkg_root = get_pkg_root();
    if (pkg_root.empty())
      pkg_root = ".";
    std::string optimized_sub = pkg_root + "/large-data-optimization/Optimized_profile_sub.xml";
    std::ifstream opt_check(optimized_sub);
    if (use_optimizer && opt_check.good()) {
      opt_check.close();
      setenv("FASTRTPS_DEFAULT_PROFILES_FILE", optimized_sub.c_str(), 1);
      printf("[SUB] Using optimized profile: %s\n", optimized_sub.c_str());
      fflush(stdout);
    } else {
      std::string xml_path = generate_rmw_xml(max_samples, socket_buffer, MAX_MESSAGE_SIZE);
      if (!xml_path.empty()) {
        char abs_path[PATH_MAX];
        if (realpath(xml_path.c_str(), abs_path)) {
          setenv("FASTRTPS_DEFAULT_PROFILES_FILE", abs_path, 1);
          printf("[SUB] Using fallback profile: %s\n", abs_path);
          fflush(stdout);
        }
      }
    }
  } else if (rmw_impl == "rmw_cyclonedds_cpp") {
    std::string pkg_root = get_pkg_root();
    if (pkg_root.empty())
      pkg_root = ".";
    std::string optimized_sub = pkg_root + "/large-data-optimization/Optimized_profile_sub_cyclonedds.xml";
    std::ifstream opt_check(optimized_sub);
    if (use_optimizer && opt_check.good()) {
      opt_check.close();
      char abs_opt[PATH_MAX];
      std::string uri = (realpath(optimized_sub.c_str(), abs_opt) != nullptr)
        ? (std::string("file://") + abs_opt)
        : (std::string("file://") + optimized_sub);
      setenv("CYCLONEDDS_URI", uri.c_str(), 1);
      printf("[SUB] Using optimized CycloneDDS profile: %s\n", optimized_sub.c_str());
      fflush(stdout);
    } else {
      std::string xml_path = generate_rmw_xml(max_samples, socket_buffer, MAX_MESSAGE_SIZE);
      if (!xml_path.empty()) {
        char abs_path[PATH_MAX];
        if (realpath(xml_path.c_str(), abs_path)) {
          std::string uri = std::string("file://") + abs_path;
          setenv("CYCLONEDDS_URI", uri.c_str(), 1);
          printf("[SUB] Using fallback CycloneDDS profile: %s\n", abs_path);
          fflush(stdout);
        }
      }
    }
  } else {
    std::string xml_path = generate_rmw_xml(max_samples, socket_buffer, MAX_MESSAGE_SIZE);
    if (!xml_path.empty()) {
      char abs_path[PATH_MAX];
      if (realpath(xml_path.c_str(), abs_path)) {
        setenv("FASTRTPS_DEFAULT_PROFILES_FILE", abs_path, 1);
        printf("[SUB] Using fallback profile: %s\n", abs_path);
        fflush(stdout);
      }
    }
  }

#endif

  // Python: f"sub_loss{loss_pct}_idx{idx}_payload{payload_bytes}_hz{publish_hz}_count{max_count}_samples{max_samples}_sock{socket_buffer}_burst{burst_secs}_{payload_type}.csv"
  std::ostringstream csv_name;
  const char* result_dir_env = std::getenv("CALM_RESULT_DIR");
  std::string csv_dir = result_dir_env ? std::string(result_dir_env) :
    std::string("/home/csilab/ros2_ws/results/test_yw");
  csv_name << csv_dir << "/sub_loss" << std::fixed << std::setprecision(1) << loss_pct
           << "_idx" << idx << "_payload" << payload_bytes << "_hz" << publish_hz
           << "_count" << max_count
           << "_samples" << max_samples << "_sock" << socket_buffer
           << "_burst" << burst_secs
           << "_topic" << sanitize_label(topic_prefix, "default")
           << "_qos" << sanitize_label(qos_mode, "reliable")
           << "_" << payload_type;
  if (!result_suffix.empty()) {
    csv_name << "_" << result_suffix;
  }
  csv_name << ".csv";
  std::string csv_filename = csv_name.str();

  rclcpp::init(argc, argv);

  const char* actual_rmw = rmw_get_implementation_identifier();
  const std::string actual_rmw_impl = actual_rmw ? actual_rmw : "(null)";
  const bool rmw_matches = actual_rmw_impl == rmw_impl;
  std::cout << "[DDS_ENDPOINT] role=subscriber expected_rmw=" << rmw_impl
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
  for (const auto& s : get_all_socket_info_in_process())
    fds_before_node.insert(s.fd);

  std::atomic<bool> done_flag{false};
  auto node = std::make_shared<DDSSub>(
    done_flag, csv_filename, max_count, loss_pct, burst_secs, payload_type,
    topic_prefix, qos_mode, qos_depth, fds_before_node);

  std::vector<SocketInfo> socks = get_all_socket_info_in_process();
  int n_new = 0;
  for (const auto& s : socks)
    if (fds_before_node.count(s.fd) == 0) n_new++;
  if (n_new > 0) {
    printf("[SUB] Sockets created after node creation (likely DDS transport), %d sockets:\n", n_new);
    for (const auto& s : socks) {
      if (fds_before_node.count(s.fd) != 0) continue;
      const char* typestr = (s.type == SOCK_DGRAM) ? "UDP" : "TCP";
      printf("[SUB]   fd=%d type=%s local=%s SO_SNDBUF=%d\n",
             s.fd, typestr, s.local_addr_port.c_str(), s.so_sndbuf);
    }
    fflush(stdout);
  } else {
    printf("[SUB] no new sockets after node creation (or none found)\n");
    fflush(stdout);
  }

  while (rclcpp::ok() && !done_flag.load()) {
    rclcpp::spin_some(node);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (rclcpp::ok()) { }
  rclcpp::shutdown();
  // clear_ingress_loss();  // 기존 qdisc도 건드리지 않음
  return 0;
}
