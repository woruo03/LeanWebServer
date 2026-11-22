/**
 * @file Logger.cpp
 * @brief 前端单例日志门面与流式格式化实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "base/Logger.h"

#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "base/AsyncLogging.h"
#include "base/CurrentThread.h"

namespace lean_web_server::base {

namespace current_thread {
thread_local int t_cached_tid = 0;
thread_local std::array<char, 32> t_tid_string{};
thread_local int t_tid_string_length = 0;

void cache_tid() {
    if (t_cached_tid == 0) {
        t_cached_tid = static_cast<int>(::syscall(SYS_gettid));
        t_tid_string_length =
            std::snprintf(t_tid_string.data(), t_tid_string.size(), "%d", t_cached_tid);
    }
}

}  // namespace current_thread

namespace {
constexpr std::string_view digits =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:
            return "T";
        case LogLevel::DEBUG:
            return "D";
        case LogLevel::INFO:
            return "I";
        case LogLevel::WARN:
            return "W";
        case LogLevel::ERROR:
            return "E";
        case LogLevel::FATAL:
            return "F";
        case LogLevel::OFF:
            return "O";
    }
    return "U";
}
}  // namespace

template <typename T>
void LogStream::format_integer(T v) {
    if (buffer_.avail() >= 32) {
        std::array<char, 32> buf{};
        char* p = buf.data() + buf.size();
        bool negative = false;
        using UnsignedT = std::make_unsigned_t<T>;
        UnsignedT u;

        if constexpr (std::is_signed_v<T>) {
            if (v < 0) {
                negative = true;
                u = static_cast<UnsignedT>(0) - static_cast<UnsignedT>(v);
            } else {
                u = static_cast<UnsignedT>(v);
            }
        } else {
            u = v;
        }

        while (u >= 100) {
            auto idx = static_cast<unsigned>((u % 100) * 2);
            u /= 100;
            *--p = digits[idx + 1];
            *--p = digits[idx];
        }

        if (u < 10) {
            *--p = static_cast<char>('0' + u);
        } else {
            auto idx = static_cast<unsigned>(u * 2);
            *--p = digits[idx + 1];
            *--p = digits[idx];
        }

        if (negative) {
            *--p = '-';
        }

        buffer_.append(p, static_cast<size_t>(buf.data() + buf.size() - p));
    }
}

LogStream& LogStream::operator<<(bool v) {
    buffer_.append(v ? "1" : "0", 1);
    return *this;
}

LogStream& LogStream::operator<<(short v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned short v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(int v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned int v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(long v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned long v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(long long v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(unsigned long long v) {
    format_integer(v);
    return *this;
}

LogStream& LogStream::operator<<(float v) {
    *this << static_cast<double>(v);
    return *this;
}

LogStream& LogStream::operator<<(double v) {
    if (buffer_.avail() >= 32) {
        std::array<char, 32> buf{};
        int len = std::snprintf(buf.data(), buf.size(), "%.12g", v);
        if (len > 0) {
            buffer_.append(buf.data(), static_cast<size_t>(len));
        }
    }
    return *this;
}

LogStream& LogStream::operator<<(char v) {
    buffer_.append(&v, 1);
    return *this;
}

LogStream& LogStream::operator<<(const char* str) {
    if (str != nullptr) {
        buffer_.append(str, std::strlen(str));
    } else {
        buffer_.append("(null)", 6);
    }
    return *this;
}

LogStream& LogStream::operator<<(std::string_view sv) {
    buffer_.append(sv.data(), sv.size());
    return *this;
}

LogStream& LogStream::operator<<(const std::string& str) {
    buffer_.append(str.data(), str.size());
    return *this;
}

LogStream& LogStream::operator<<(const void* p) {
    auto u = reinterpret_cast<uintptr_t>(p);
    if (buffer_.avail() >= 32) {
        std::array<char, 32> buf{};
        int len = std::snprintf(buf.data(), buf.size(), "0x%lx", u);
        if (len > 0) {
            buffer_.append(buf.data(), static_cast<size_t>(len));
        }
    }
    return *this;
}

// ──────────────────────────────────────────
// Logger
// ──────────────────────────────────────────

Logger::Logger() = default;

Logger::~Logger() {
    shutdown();
}

Logger& Logger::get_instance() {
    static Logger instance;
    return instance;
}

void Logger::init(const LogConfig& cfg) {
    if (!async_logging_) {
        set_level(cfg.default_level);
        async_logging_ = std::make_unique<AsyncLogging>(
            cfg.log_dir, cfg.base_name, cfg.roll_size_bytes, cfg.flush_interval_sec);
        async_logging_->start();
    }
}

void Logger::shutdown() {
    if (async_logging_) {
        async_logging_->stop();
        async_logging_.reset();
    }
}

void Logger::append(const char* log_line, size_t len) {
    if (async_logging_) {
        async_logging_->append(log_line, len);
    } else {
        ::fwrite(log_line, 1, len, stderr);
    }
}

void Logger::emergency_flush(const char* log_line, size_t len) {
    if (async_logging_) {
        async_logging_->emergency_flush(log_line, len);
    } else {
        ::fwrite(log_line, 1, len, stderr);
        ::fflush(stderr);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Logger::log(LogLevel level, const char* file, int line, const char* func, const char* fmt,
                 ...) const {
    if (!should_log(level)) {
        return;
    }

    LogMessage msg(file, line, func, level);
    std::array<char, k_small_buffer_size> buf{};
    va_list ap;
    va_start(ap, fmt);
    int len = std::vsnprintf(buf.data(), buf.size(), fmt, ap);
    va_end(ap);

    if (len > 0) {
        msg.stream() << std::string_view(buf.data(), static_cast<size_t>(len));
    }
}

// ──────────────────────────────────────────
// LogMessage
// ──────────────────────────────────────────

LogMessage::LogMessage(const char* file, int line, const char* func, LogLevel level)
    : time_(Timestamp::now()), level_(level), file_(file), line_(line), func_(func) {
    stream_ << time_.to_formatted_string(true) << " [" << level_name(level) << "] [tid "
            << current_thread::tid_string() << "] [" << file << ":" << line << " " << func << "] ";
}

LogMessage::~LogMessage() {
    stream_ << '\n';
    const auto& buf = stream_.buffer();

    if (level_ == LogLevel::FATAL) {
        ::write(STDERR_FILENO, buf.data(), buf.length());
        Logger::get_instance().emergency_flush(buf.data(), buf.length());

        std::array<void*, 64> callstack;
        int frames = ::backtrace(callstack.data(), 64);
        ::backtrace_symbols_fd(callstack.data(), frames, STDERR_FILENO);

        ::abort();
    }
    Logger::get_instance().append(buf.data(), buf.length());
}

}  // namespace lean_web_server::base
