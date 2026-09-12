#include <chrono>
#include <array>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct Config
{
    std::string experiment_name = "reliability_vs_besteffort_capacity";
    std::string remote_host;
    std::string remote_user = "csilab";
    std::string local_ws = "/home/csi/ros2_ws";
    std::string remote_ws = "/home/csilab/ros2_ws";
    std::string csv_root = "results/pretest_csv";
    int ros_domain_id = 117;
    int count = 1000;
    int repeat = 5;
    int max_runs = 0;
    int payload_bytes_override = 0;
    double hz_override = 0.0;
    std::string reliability_override;
    double idle_timeout_s = 30.0;
    double total_timeout_s = 240.0;
    double ready_timeout_s = 120.0;
    double sub_start_delay_s = 10.0;
    double inter_run_delay_s = 5.0;
    std::string rmw_implementation = "rmw_fastrtps_cpp";
    std::string ros_localhost_only = "0";
};

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

std::string shell_quote(const std::string& value)
{
    std::string out = "'";
    for (char c : value)
    {
        if (c == '\'')
        {
            out += "'\\''";
        }
        else
        {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

int run_command(const std::string& cmd)
{
    std::cout << "[CMD] " << cmd << std::endl;
    return std::system(cmd.c_str());
}

std::string read_command(const std::string& cmd)
{
    std::array<char, 256> buffer{};
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
    {
        return result;
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    {
        result += buffer.data();
    }
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
    {
        result.pop_back();
    }
    return result;
}

bool is_unsigned_integer(const std::string& text)
{
    if (text.empty())
    {
        return false;
    }
    for (char c : text)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    return true;
}

std::string abs_under_ws(const std::string& ws, const std::string& path)
{
    if (!path.empty() && path.front() == '/')
    {
        return path;
    }
    return ws + "/" + path;
}

void print_usage()
{
    std::cout
        << "Usage:\n"
        << "  ros2 run calm_pretest calm_automation --remote-host <SUB_IP>\n\n"
        << "Options:\n"
        << "  --remote-user <user>       default: csilab\n"
        << "  --local-ws <path>          default: /home/csi/ros2_ws\n"
        << "  --remote-ws <path>         default: /home/csilab/ros2_ws\n"
        << "  --csv-root <path>          default: results/pretest_csv\n"
        << "  --domain-id <id>           default: 117\n"
        << "  --count <N>                default: 1000\n"
        << "  --repeat <N>               default: 5\n"
        << "  --max-runs <N>             default: 0, unlimited\n"
        << "  --payload-bytes <N>        default: matrix payloads\n"
        << "  --hz <Hz>                  default: matrix rates\n"
        << "  --reliability <kind>       default: reliable,best_effort\n"
        << "  --idle-timeout-s <sec>     default: 30\n"
        << "  --total-timeout-s <sec>    default: 240\n"
        << "  --ready-timeout-s <sec>    default: 120\n"
        << "  --sub-start-delay-s <sec>  default: 10\n"
        << "  --inter-run-delay-s <sec>  default: 5\n"
        << "  --rmw <name>               default: rmw_fastrtps_cpp\n"
        << "  --localhost-only <0|1>     default: 0\n";
}

}  // namespace

int main(int argc, char** argv)
{
    Config cfg;
    cfg.remote_host = arg_value(argc, argv, "--remote-host", "");
    cfg.remote_user = arg_value(argc, argv, "--remote-user", cfg.remote_user);
    cfg.local_ws = arg_value(argc, argv, "--local-ws", cfg.local_ws);
    cfg.remote_ws = arg_value(argc, argv, "--remote-ws", cfg.remote_ws);
    cfg.csv_root = arg_value(argc, argv, "--csv-root", cfg.csv_root);
    cfg.ros_domain_id = int_arg(argc, argv, "--domain-id", cfg.ros_domain_id);
    cfg.count = int_arg(argc, argv, "--count", cfg.count);
    cfg.repeat = int_arg(argc, argv, "--repeat", cfg.repeat);
    cfg.max_runs = int_arg(argc, argv, "--max-runs", cfg.max_runs);
    cfg.payload_bytes_override = int_arg(argc, argv, "--payload-bytes", cfg.payload_bytes_override);
    cfg.hz_override = double_arg(argc, argv, "--hz", cfg.hz_override);
    cfg.reliability_override = arg_value(argc, argv, "--reliability", cfg.reliability_override);
    cfg.idle_timeout_s = double_arg(argc, argv, "--idle-timeout-s", cfg.idle_timeout_s);
    cfg.total_timeout_s = double_arg(argc, argv, "--total-timeout-s", cfg.total_timeout_s);
    cfg.ready_timeout_s = double_arg(argc, argv, "--ready-timeout-s", cfg.ready_timeout_s);
    cfg.sub_start_delay_s = double_arg(argc, argv, "--sub-start-delay-s", cfg.sub_start_delay_s);
    cfg.inter_run_delay_s = double_arg(argc, argv, "--inter-run-delay-s", cfg.inter_run_delay_s);
    cfg.rmw_implementation = arg_value(argc, argv, "--rmw", cfg.rmw_implementation);
    cfg.ros_localhost_only = arg_value(argc, argv, "--localhost-only", cfg.ros_localhost_only);

    if (cfg.remote_host.empty())
    {
        print_usage();
        std::cerr << "\nERROR: --remote-host is required.\n";
        return 2;
    }

    std::vector<int> payloads = {
        32 * 1024,
        64 * 1024,
        128 * 1024,
        256 * 1024,
        512 * 1024
    };
    std::vector<double> hzs = {5.0, 10.0, 20.0, 30.0, 50.0};
    std::vector<std::string> reliabilities = {"reliable", "best_effort"};

    if (cfg.payload_bytes_override > 0)
    {
        payloads = {cfg.payload_bytes_override};
    }
    if (cfg.hz_override > 0.0)
    {
        hzs = {cfg.hz_override};
    }
    if (!cfg.reliability_override.empty())
    {
        if (cfg.reliability_override != "reliable" && cfg.reliability_override != "best_effort")
        {
            std::cerr << "--reliability must be reliable or best_effort\n";
            return 2;
        }
        reliabilities = {cfg.reliability_override};
    }

    const std::string date = timestamp();
    const std::string batch_name = cfg.experiment_name + "_" + date;
    const std::string local_batch_dir = abs_under_ws(cfg.local_ws, cfg.csv_root + "/" + batch_name);
    const std::string remote_batch_dir = abs_under_ws(cfg.remote_ws, cfg.csv_root + "/" + batch_name);
    const std::string master_csv = local_batch_dir + "/runs_manifest.csv";

    std::filesystem::create_directories(local_batch_dir);

    std::ofstream manifest(master_csv, std::ios::out);
    manifest << "run_id,experiment_date,reliability,payload_bytes,payload_kb,target_hz,count,repeat_index,"
             << "local_run_dir,remote_run_dir,local_pub_exit,sub_wait_exit,scp_exit\n";
    manifest.flush();

    const std::string ssh_target = cfg.remote_user + "@" + cfg.remote_host;
    const std::string ssh_base = "ssh -n -o BatchMode=yes -o ConnectTimeout=10 -T ";
    const std::string scp_base = "scp -o BatchMode=yes -o ConnectTimeout=10 -q -r ";

    const int ssh_preflight = run_command(
        ssh_base + shell_quote(ssh_target) + " " +
        shell_quote("bash -lc " + shell_quote("hostname && test -d " + shell_quote(cfg.remote_ws))));
    if (ssh_preflight != 0)
    {
        std::cerr
            << "[ERROR] Cannot run non-interactive SSH command on subscriber laptop.\n"
            << "        Check Wi-Fi network, sub IP, ssh server, and passwordless SSH.\n"
            << "        Target: " << ssh_target << "\n";
        return 3;
    }

    run_command(
        ssh_base + shell_quote(ssh_target) + " " +
        shell_quote("bash -lc " + shell_quote("mkdir -p " + shell_quote(remote_batch_dir))));

    int idx = 1;
    int completed_runs = 0;
    bool stop = false;
    for (const std::string& reliability : reliabilities)
    {
        for (int payload : payloads)
        {
            for (double hz : hzs)
            {
                for (int rep = 1; rep <= cfg.repeat; ++rep)
                {
                    if (stop)
                    {
                        break;
                    }

                    std::ostringstream run_id_oss;
                    run_id_oss << "run" << std::setw(4) << std::setfill('0') << idx
                               << "_" << reliability
                               << "_payload" << (payload / 1024) << "KB"
                               << "_hz" << static_cast<int>(hz)
                               << "_rep" << rep;
                    const std::string run_id = run_id_oss.str();
                    const std::string local_run_dir = local_batch_dir + "/" + run_id;
                    const std::string remote_run_dir = remote_batch_dir + "/" + run_id;

                    std::filesystem::create_directories(local_run_dir);

                    std::cout << "\n============================================================\n";
                    std::cout << "[RUN] " << run_id
                              << " reliability=" << reliability
                              << " payload=" << payload
                              << " hz=" << hz
                              << " repeat=" << rep << "/" << cfg.repeat << std::endl;

                    const std::string remote_sub_inner =
                        "mkdir -p " + shell_quote(remote_run_dir) + " && "
                        "source /opt/ros/humble/setup.bash && "
                        "cd " + shell_quote(cfg.remote_ws) + " && "
                        "source install/setup.bash && "
                        "printf 'ROS_DOMAIN_ID=" + std::to_string(cfg.ros_domain_id) +
                        "\\nROS_LOCALHOST_ONLY=" + cfg.ros_localhost_only +
                        "\\nRMW_IMPLEMENTATION=" + cfg.rmw_implementation +
                        "\\n' > " + shell_quote(remote_run_dir + "/sub_stdout.log") + " && "
                        "nohup env "
                        "ROS_DOMAIN_ID=" + std::to_string(cfg.ros_domain_id) + " "
                        "ROS_LOCALHOST_ONLY=" + cfg.ros_localhost_only + " "
                        "RMW_IMPLEMENTATION=" + cfg.rmw_implementation + " "
                        "ros2 run calm_pretest calm_sub "
                        "--payload-bytes " + std::to_string(payload) + " "
                        "--hz " + std::to_string(hz) + " "
                        "--count " + std::to_string(cfg.count) + " "
                        "--reliability " + reliability + " "
                        "--run-id " + run_id + " "
                        "--csv-dir " + shell_quote(remote_run_dir) + " "
                        "--idle-timeout-s " + std::to_string(cfg.idle_timeout_s) + " "
                        "--total-timeout-s " + std::to_string(cfg.total_timeout_s) + " "
                        ">> " + shell_quote(remote_run_dir + "/sub_stdout.log") + " 2>&1";

                    const std::string remote_sub_cmd =
                        ssh_base + shell_quote(ssh_target) + " " +
                        shell_quote("bash -lc " + shell_quote(remote_sub_inner));
                    const std::string local_sub_launch_cmd =
                        "bash -lc " + shell_quote(
                            remote_sub_cmd + " > " + shell_quote(local_run_dir + "/sub_ssh_stdout.log") +
                            " 2>&1 & echo $!");

                    const std::string local_sub_pid = read_command(local_sub_launch_cmd);
                    std::cout << "[SYS] local ssh-sub pid: " << local_sub_pid << std::endl;
                    if (!is_unsigned_integer(local_sub_pid))
                    {
                        std::cerr << "[ERROR] Failed to launch subscriber SSH process. Output: "
                                  << local_sub_pid << std::endl;
                        return 4;
                    }

                    std::cout << "[SYS] waiting " << cfg.sub_start_delay_s
                              << " seconds for remote subscriber startup..." << std::endl;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(static_cast<int>(cfg.sub_start_delay_s * 1000.0)));

                    const std::string local_pub_inner =
                        "source /opt/ros/humble/setup.bash && "
                        "cd " + shell_quote(cfg.local_ws) + " && "
                        "source install/setup.bash && "
                        "printf 'ROS_DOMAIN_ID=" + std::to_string(cfg.ros_domain_id) +
                        "\\nROS_LOCALHOST_ONLY=" + cfg.ros_localhost_only +
                        "\\nRMW_IMPLEMENTATION=" + cfg.rmw_implementation +
                        "\\n' > " + shell_quote(local_run_dir + "/pub_stdout.log") + " && "
                        "env "
                        "ROS_DOMAIN_ID=" + std::to_string(cfg.ros_domain_id) + " "
                        "ROS_LOCALHOST_ONLY=" + cfg.ros_localhost_only + " "
                        "RMW_IMPLEMENTATION=" + cfg.rmw_implementation + " "
                        "ros2 run calm_pretest calm_pub "
                        "--payload-bytes " + std::to_string(payload) + " "
                        "--hz " + std::to_string(hz) + " "
                        "--count " + std::to_string(cfg.count) + " "
                        "--reliability " + reliability + " "
                        "--run-id " + run_id + " "
                        "--csv-dir " + shell_quote(local_run_dir) + " "
                        "--ready-timeout-s " + std::to_string(cfg.ready_timeout_s) + " "
                        ">> " + shell_quote(local_run_dir + "/pub_stdout.log") + " 2>&1";

                    const int pub_exit = run_command("bash -lc " + shell_quote(local_pub_inner));

                    const int wait_seconds = static_cast<int>(
                        cfg.total_timeout_s + cfg.idle_timeout_s + cfg.ready_timeout_s +
                        cfg.sub_start_delay_s + 20.0);
                    const std::string sub_wait_inner =
                        "for i in $(seq 1 " + std::to_string(wait_seconds) + "); do "
                        "kill -0 " + local_sub_pid + " >/dev/null 2>&1 || exit 0; "
                        "sleep 1; "
                        "done; "
                        "kill " + local_sub_pid + " >/dev/null 2>&1 || true";
                    const int sub_wait_exit = run_command("bash -lc " + shell_quote(sub_wait_inner));

                    const int scp_exit = run_command(
                        scp_base + shell_quote(ssh_target + ":" + remote_run_dir + "/") + " " +
                        shell_quote(local_batch_dir + "/"));

                    manifest << run_id << ',' << date << ',' << reliability << ','
                             << payload << ',' << (payload / 1024) << ','
                             << hz << ',' << cfg.count << ',' << rep << ','
                             << local_run_dir << ',' << remote_run_dir << ','
                             << pub_exit << ',' << sub_wait_exit << ',' << scp_exit << '\n';
                    manifest.flush();

                    ++completed_runs;
                    ++idx;
                    if (cfg.max_runs > 0 && completed_runs >= cfg.max_runs)
                    {
                        stop = true;
                    }

                    std::cout << "[SYS] waiting " << cfg.inter_run_delay_s
                              << " seconds before next run..." << std::endl;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(static_cast<int>(cfg.inter_run_delay_s * 1000.0)));
                }
                if (stop)
                {
                    break;
                }
            }
            if (stop)
            {
                break;
            }
        }
        if (stop)
        {
            break;
        }
    }

    std::cout << "\n[SYS] All experiments finished.\n";
    std::cout << "[SYS] Results: " << local_batch_dir << std::endl;
    std::cout << "[SYS] Manifest: " << master_csv << std::endl;
    return 0;
}
