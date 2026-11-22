/**
 * @file ServerConfig.cpp
 * @brief ServerConfig 全局统一配置类实现与 getopt_long 参数解析
 * @author woruo03
 * @date 2026-11-19
 * @version 1.0
 */

#include "http/ServerConfig.h"

#include <getopt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

namespace lean_web_server::http {

namespace {

std::string to_lower_copy(std::string_view sv) {
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_log_level_string(std::string_view sv, base::LogLevel* level) {
    std::string lower = to_lower_copy(sv);
    if (lower == "trace") {
        *level = base::LogLevel::TRACE;
        return true;
    }
    if (lower == "debug") {
        *level = base::LogLevel::DEBUG;
        return true;
    }
    if (lower == "info") {
        *level = base::LogLevel::INFO;
        return true;
    }
    if (lower == "warn") {
        *level = base::LogLevel::WARN;
        return true;
    }
    if (lower == "error") {
        *level = base::LogLevel::ERROR;
        return true;
    }
    if (lower == "fatal") {
        *level = base::LogLevel::FATAL;
        return true;
    }
    if (lower == "off") {
        *level = base::LogLevel::OFF;
        return true;
    }
    return false;
}

const char* log_level_to_string(base::LogLevel level) {
    switch (level) {
        case base::LogLevel::TRACE:
            return "TRACE";
        case base::LogLevel::DEBUG:
            return "DEBUG";
        case base::LogLevel::INFO:
            return "INFO";
        case base::LogLevel::WARN:
            return "WARN";
        case base::LogLevel::ERROR:
            return "ERROR";
        case base::LogLevel::FATAL:
            return "FATAL";
        case base::LogLevel::OFF:
            return "OFF";
    }
    return "UNKNOWN";
}

bool parse_int(const char* str, int* out) {
    if (str == nullptr || str[0] == '\0') {
        return false;
    }
    size_t len = std::strlen(str);
    int val = 0;
    auto [ptr, ec] = std::from_chars(str, str + len, val);
    if (ec != std::errc{} || ptr != str + len) {
        return false;
    }
    *out = val;
    return true;
}

bool parse_ulong(const char* str, unsigned long* out) {
    if (str == nullptr || str[0] == '\0') {
        return false;
    }
    size_t len = std::strlen(str);
    unsigned long val = 0;
    auto [ptr, ec] = std::from_chars(str, str + len, val);
    if (ec != std::errc{} || ptr != str + len) {
        return false;
    }
    *out = val;
    return true;
}

}  // namespace

int ServerConfig::default_io_threads() noexcept {
    unsigned int hc = std::thread::hardware_concurrency();
    return (hc > 1) ? static_cast<int>(hc - 1) : 1;
}

bool ServerConfig::is_valid(std::string* error_msg) const {
    auto fail = [&](const std::string& msg) {
        if (error_msg) {
            *error_msg = msg;
        }
        return false;
    };

    if (port == 0) {
        return fail("Port cannot be 0 (must be 1-65535)");
    }
    if (io_threads < 0 || io_threads > 1024) {
        return fail("io_threads must be between 0 and 1024");
    }
    if (timeout.timer_wheel_slots < 64 || timeout.timer_wheel_slots > 65536) {
        return fail("timer_wheel_slots must be between 64 and 65536");
    }
    if ((timeout.timer_wheel_slots & (timeout.timer_wheel_slots - 1)) != 0) {
        return fail(
            "timer_wheel_slots must be a power of 2 (e.g. 128, 256, 512, 1024, 2048, 4096)");
    }
    if (timeout.idle_timeout_sec <= 0 || timeout.idle_timeout_sec >= timeout.timer_wheel_slots) {
        return fail("idle_timeout_sec must be positive and strictly less than timer_wheel_slots (" +
                    std::to_string(timeout.timer_wheel_slots) + ")");
    }
    if (timeout.max_lifetime_sec < timeout.idle_timeout_sec) {
        return fail("max_lifetime_sec cannot be less than idle_timeout_sec");
    }
    if (shutdown_drain_timeout_sec < 0) {
        return fail("shutdown_drain_timeout_sec cannot be negative");
    }
    if (log.roll_size_bytes < static_cast<size_t>(1024 * 1024)) {
        return fail("roll_size_bytes must be at least 1MB (1048576 bytes)");
    }
    if (log.flush_interval_sec <= 0) {
        return fail("flush_interval_sec must be positive");
    }
    if (log.max_backup_files < 1) {
        return fail("max_backup_files must be at least 1");
    }
    return true;
}

std::string ServerConfig::to_string() const {
    std::ostringstream oss;
    oss << "ServerConfig {\n"
        << "  port: " << port << "\n"
        << "  io_threads: " << io_threads << "\n"
        << "  listen_backlog: " << listen_backlog << "\n"
        << "  timeout: { idle=" << timeout.idle_timeout_sec
        << "s, max_lifetime=" << timeout.max_lifetime_sec
        << "s, wheel_slots=" << timeout.timer_wheel_slots
        << ", drain=" << shutdown_drain_timeout_sec << "s }\n"
        << "  log: { dir=" << log.log_dir << ", base=" << log.base_name
        << ", level=" << log_level_to_string(log.default_level)
        << ", roll_size=" << (log.roll_size_bytes / (static_cast<size_t>(1024 * 1024))) << "MB"
        << ", flush=" << log.flush_interval_sec << "s, backups=" << log.max_backup_files << " }\n"
        << "  static_resource_dir: " << static_resource_dir << "\n"
        << "}";
    return oss.str();
}

namespace {

bool parse_port_opt(const char* arg, uint16_t& port, std::string& err) {
    int val = 0;
    const char* display = (arg != nullptr) ? arg : "null";
    if (!parse_int(arg, &val) || val <= 0 || val > 65535) {
        LOG_ERROR << "ServerConfig: invalid port value: " << display;
        err = "Invalid port range (must be 1-65535): " + std::string((arg != nullptr) ? arg : "");
        return false;
    }
    port = static_cast<uint16_t>(val);
    return true;
}

bool parse_threads_opt(const char* arg, int& threads, std::string& err) {
    int val = 0;
    const char* display = (arg != nullptr) ? arg : "null";
    if (!parse_int(arg, &val) || val < 0 || val > 1024) {
        LOG_ERROR << "ServerConfig: invalid threads count: " << display;
        err = "Invalid threads count (0-1024): " + std::string((arg != nullptr) ? arg : "");
        return false;
    }
    threads = val;
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool parse_log_dir_opt(const char* arg, std::string& log_dir, std::string& err) {
    if (arg == nullptr || arg[0] == '\0') {
        LOG_ERROR << "ServerConfig: log directory cannot be empty";
        err = "Log directory cannot be empty";
        return false;
    }
    log_dir = arg;
    return true;
}

bool parse_log_level_opt(const char* arg, base::LogLevel& level, std::string& err) {
    const char* display = (arg != nullptr) ? arg : "null";
    if (!parse_log_level_string(arg, &level)) {
        LOG_ERROR << "ServerConfig: invalid log level: " << display;
        err = "Invalid log level: " + std::string((arg != nullptr) ? arg : "") +
              " (allowed: trace, debug, info, warn, error, fatal, off)";
        return false;
    }
    return true;
}

bool parse_idle_timeout_opt(const char* arg, int& timeout_sec, std::string& err) {
    int val = 0;
    const char* display = (arg != nullptr) ? arg : "null";
    if (!parse_int(arg, &val) || val <= 0) {
        LOG_ERROR << "ServerConfig: invalid idle-timeout: " << display;
        err = "idle-timeout must be positive: " + std::string((arg != nullptr) ? arg : "");
        return false;
    }
    timeout_sec = val;
    return true;
}

bool parse_max_lifetime_opt(const char* arg, int& lifetime_sec, std::string& err) {
    int val = 0;
    const char* display = (arg != nullptr) ? arg : "null";
    if (!parse_int(arg, &val) || val <= 0) {
        LOG_ERROR << "ServerConfig: invalid max-lifetime: " << display;
        err = "max-lifetime must be positive: " + std::string((arg != nullptr) ? arg : "");
        return false;
    }
    lifetime_sec = val;
    return true;
}

bool parse_wheel_slots_opt(const char* arg, int& slots, std::string& err) {
    int val = 0;
    const char* display = (arg != nullptr) ? arg : "null";
    if (!parse_int(arg, &val) || val < 64 || val > 65536 || (val & (val - 1)) != 0) {
        LOG_ERROR << "ServerConfig: invalid wheel-slots: " << display;
        err = "Invalid wheel-slots (must be power of 2 between 64 and 65536): " +
              std::string((arg != nullptr) ? arg : "");
        return false;
    }
    slots = val;
    return true;
}

bool parse_roll_size_opt(const char* arg, size_t& roll_size_bytes, std::string& err) {
    unsigned long val = 0;
    const char* display = (arg != nullptr) ? arg : "null";
    if (!parse_ulong(arg, &val) || val < 1) {
        LOG_ERROR << "ServerConfig: invalid roll-size: " << display;
        err = "roll-size must be at least 1 (MB): " + std::string((arg != nullptr) ? arg : "");
        return false;
    }
    roll_size_bytes = static_cast<size_t>(val) * 1024 * 1024;
    return true;
}

}  // namespace

bool ServerConfig::parse_option(int opt, const char* arg, bool* help_requested, std::string& err) {
    switch (opt) {
        case 'p':
            return parse_port_opt(arg, port, err);
        case 't':
            return parse_threads_opt(arg, io_threads, err);
        case 'd':
            return parse_log_dir_opt(arg, log.log_dir, err);
        case 'l':
            return parse_log_level_opt(arg, log.default_level, err);
        case 'i':
            return parse_idle_timeout_opt(arg, timeout.idle_timeout_sec, err);
        case 'm':
            return parse_max_lifetime_opt(arg, timeout.max_lifetime_sec, err);
        case 'w':
            return parse_wheel_slots_opt(arg, timeout.timer_wheel_slots, err);
        case 'r':
            return parse_roll_size_opt(arg, log.roll_size_bytes, err);
        case 'h':
            if (help_requested != nullptr) {
                *help_requested = true;
            }
            return false;
        case '?':
            err = "Unknown or missing argument for option";
            if (optopt != 0) {
                err += std::string(" -") + static_cast<char>(optopt);
            }
            LOG_ERROR << "ServerConfig: " << err;
            return false;
        default:
            LOG_ERROR << "ServerConfig: unexpected option parsed by getopt: " << opt;
            err = "Unexpected option parsed by getopt";
            return false;
    }
}

bool ServerConfig::parse_args(int argc, char* argv[], bool* help_requested,
                              std::string* error_msg) {
    if (help_requested != nullptr) {
        *help_requested = false;
    }

    // 重置 getopt 全局状态，确保多轮次解析（如单元测试）时行为正确
    optind = 1;
    opterr = 0;

    static const std::array<struct option, 10> long_options = {
        {
            {"port", required_argument, nullptr, 'p'},
            {"threads", required_argument, nullptr, 't'},
            {"log-dir", required_argument, nullptr, 'd'},
            {"log-level", required_argument, nullptr, 'l'},
            {"idle-timeout", required_argument, nullptr, 'i'},
            {"max-lifetime", required_argument, nullptr, 'm'},
            {"wheel-slots", required_argument, nullptr, 'w'},
            {"roll-size", required_argument, nullptr, 'r'},
            {"help", no_argument, nullptr, 'h'},
            {nullptr, 0, nullptr, 0},
        },
    };

    int opt = 0;
    int option_index = 0;
    std::string err;
    while ((opt = ::getopt_long(argc, argv, "p:t:d:l:i:m:w:r:h", long_options.data(),
                                &option_index)) != -1) {
        if (!parse_option(opt, optarg, help_requested, err)) {
            if (error_msg != nullptr) {
                *error_msg = err;
            }
            return false;
        }
    }

    // 严格拒绝任何非选项捕获的位置参数
    if (optind < argc) {
        std::string err_pos =
            std::string("Unexpected positional argument: ") + argv[optind] +
            " (positional arguments are not supported, please use options like -p <port>)";
        LOG_ERROR << "ServerConfig: " << err_pos;
        if (error_msg != nullptr) {
            *error_msg = err_pos;
        }
        return false;
    }

    return true;
}

void ServerConfig::print_usage(const char* prog_name) {
    const char* name =
        (prog_name != nullptr && prog_name[0] != '\0') ? prog_name : "lean_web_server";
    std::cout << "Usage: " << name << " [options]\n\n"
              << "Options:\n"
              << "  -p, --port <port>          Server listen port (1-65535, default: 9006)\n"
              << "  -t, --threads <num>        Number of Sub-Reactor IO threads (default: "
              << default_io_threads() << " [CPU cores - 1])\n"
              << "  -d, --log-dir <dir>        Directory to store log files (default: ./logs)\n"
              << "  -l, --log-level <level>    Minimum log level (trace, debug, info, warn, error, "
                 "fatal, off; default: info)\n"
              << "  -i, --idle-timeout <sec>   L1 rolling idle connection timeout in seconds "
                 "(default: 30)\n"
              << "  -m, --max-lifetime <sec>   L2 absolute maximum connection lifetime in seconds "
                 "(default: 120)\n"
              << "  -w, --wheel-slots <num>    Number of TimerWheel hash slots (power of 2, "
                 "default: 1024)\n"
              << "  -r, --roll-size <MB>       Log file roll size in megabytes (default: 64)\n"
              << "  -h, --help                 Display this help message and exit\n"
              << '\n';
}

}  // namespace lean_web_server::http
