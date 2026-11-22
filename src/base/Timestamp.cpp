/**
 * @file Timestamp.cpp
 * @brief 高精度时间戳类实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "base/Timestamp.h"

#include <array>
#include <cstdio>
#include <ctime>

namespace lean_web_server::base {

Timestamp Timestamp::now() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    int64_t micro_seconds = (static_cast<int64_t>(ts.tv_sec) * k_micro_seconds_per_second) +
                            static_cast<int64_t>(ts.tv_nsec / 1000);
    return Timestamp(micro_seconds);
}

std::string Timestamp::to_string() const {
    return to_formatted_string(true);
}

std::string Timestamp::to_formatted_string(bool show_us) const {
    std::array<char, 64> buf{};

    time_t sec = seconds();
    int64_t us = micro_seconds();

    thread_local time_t t_last_second = 0;
    thread_local std::array<char, 32> t_time_str{};

    if (sec != t_last_second) {
        struct tm tm_time{};
        ::localtime_r(&sec, &tm_time);
        std::snprintf(t_time_str.data(), t_time_str.size(), "%04d-%02d-%02d %02d:%02d:%02d",
                      tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday, tm_time.tm_hour,
                      tm_time.tm_min, tm_time.tm_sec);
        t_last_second = sec;
    }

    int len = show_us ? std::snprintf(buf.data(), buf.size(), "%s.%06ld", t_time_str.data(),
                                      static_cast<long>(us))
                      : std::snprintf(buf.data(), buf.size(), "%s", t_time_str.data());
    if (len > 0) {
        return {buf.data(), static_cast<size_t>(len)};
    }
    return "";
}

std::string Timestamp::to_filename_string() const {
    std::array<char, 32> buf{};
    time_t sec = seconds();
    struct tm tm_time{};
    ::localtime_r(&sec, &tm_time);
    int len = std::snprintf(buf.data(), buf.size(), "%04d%02d%02d-%02d%02d%02d",
                            tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
                            tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);
    if (len > 0) {
        return {buf.data(), static_cast<size_t>(len)};
    }
    return "";
}

}  // namespace lean_web_server::base
