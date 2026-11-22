/**
 * @file Logger.h
 * @brief 前端单例日志门面、4KB 栈缓冲流式格式化与双门面日志宏定义
 * @author woruo03
 * @date 2026-11-14
 * @version 1.0
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "base/NonCopyable.h"
#include "base/Timestamp.h"

namespace lean_web_server::base {

class AsyncLogging;

/// 日志严重级别枚举
enum class LogLevel : uint8_t { TRACE, DEBUG, INFO, WARN, ERROR, FATAL, OFF };

/// 日志初始化配置选项
struct LogConfig {
    std::string log_dir = "./logs";             ///< 日志存储目标目录
    std::string base_name = "lean_web_server";  ///< 日志文件名前缀
    size_t roll_size_bytes =
        static_cast<size_t>(64 * 1024 * 1024);  ///< 触发切分的单文件大小（默认 64MB）
    int flush_interval_sec = 3;                 ///< 后端定时刷盘周期（默认 3 秒）
    LogLevel default_level = LogLevel::INFO;    ///< 默认最低输出级别
    int max_backup_files = 10;                  ///< 最多保留的历史日志备份数量
};

constexpr int k_small_buffer_size = 4096;

/**
 * @class FixedBuffer
 * @brief 栈上高性能定长内存缓冲区模板
 * @tparam SIZE 缓冲区字节数
 *
 * 专为无动态内存分配的快速行拼接设计，提供常数级 append、avail 与 reset 操作。
 */
template <int SIZE>
class FixedBuffer : NonCopyable {
public:
    FixedBuffer() : cur_(data_.data()) {}

    /// 追加数据至缓冲区（带边界检查，防溢出）
    void append(const char* buf, size_t len) {
        if (avail() >= len) {
            std::memcpy(cur_, buf, len);
            cur_ += len;
        }
    }

    [[nodiscard]] const char* data() const { return data_.data(); }
    [[nodiscard]] int length() const { return static_cast<int>(cur_ - data_.data()); }
    char* current() { return cur_; }
    [[nodiscard]] size_t avail() const { return static_cast<size_t>(end() - cur_); }
    void add(size_t len) { cur_ += len; }
    void reset() { cur_ = data_.data(); }
    void bzero() { data_.fill(0); }

private:
    [[nodiscard]] const char* end() const;
    std::array<char, SIZE> data_{};
    char* cur_;
};
template <int SIZE>
inline const char* FixedBuffer<SIZE>::end() const {
    return data_.data() + data_.size();
}

/**
 * @class LogStream
 * @brief 轻量级高性能流式格式化器
 *
 * 内部内嵌 4KB 的 FixedBuffer，基于整数快速查表法实现基础数据类型的无锁化极速组装。
 */
class LogStream : NonCopyable {
public:
    using Buffer = FixedBuffer<k_small_buffer_size>;

    LogStream& operator<<(bool v);
    LogStream& operator<<(short);
    LogStream& operator<<(unsigned short);
    LogStream& operator<<(int);
    LogStream& operator<<(unsigned int);
    LogStream& operator<<(long);
    LogStream& operator<<(unsigned long);
    LogStream& operator<<(long long);
    LogStream& operator<<(unsigned long long);
    LogStream& operator<<(float v);
    LogStream& operator<<(double);
    LogStream& operator<<(char v);
    LogStream& operator<<(const char* str);
    LogStream& operator<<(std::string_view sv);
    LogStream& operator<<(const std::string& str);
    LogStream& operator<<(const void* p);

    [[nodiscard]] const Buffer& buffer() const { return buffer_; }
    void reset_buffer() { buffer_.reset(); }

private:
    template <typename T>
    void format_integer(T);

    Buffer buffer_;
};

/**
 * @class Logger
 * @brief 日志系统前端 Meyers 单例门面类
 *
 * 采用 Meyers 单例保证多线程并发首触无竞争初始化；对外提供流式日志与 C 风格格式化双重接口。
 * 支持原子动态调整日志级别、未初始化前 stderr 兜底输出以及 FATAL 致命崩溃时旁路落盘后 abort()。
 */
class Logger : NonCopyable {
public:
    /// 获取全局单例实例（线程安全）
    [[nodiscard]] static Logger& get_instance();

    /// 使用指定配置初始化后端异步落盘引擎（幂等）
    void init(const LogConfig& cfg);

    /// 关闭日志系统并彻底排空未落盘缓冲区
    void shutdown();

    /// 动态修改过滤级别
    void set_level(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
    [[nodiscard]] LogLevel level() const noexcept { return level_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool should_log(LogLevel level) const noexcept { return level >= this->level(); }

    /// printf 风格格式化日志底层入口（利用 __attribute__((format)) 实现编译期实参校验）
    void log(LogLevel level, const char* file, int line, const char* func, const char* fmt, ...) const
        __attribute__((format(printf, 6, 7)));

    /// 前端提交待落盘日志行至后端
    void append(const char* log_line, size_t len);

    /// 致命异常/崩溃时直刷落盘旁路通道
    void emergency_flush(const char* log_line, size_t len);

private:
    Logger();
    ~Logger();

    std::atomic<LogLevel> level_{LogLevel::INFO};
    std::unique_ptr<AsyncLogging> async_logging_;
};

/**
 * @class LogMessage
 * @brief RAII 单行日志包装器
 *
 * 在构造期格式化填充 "时间戳 [级别] [tid] [文件名:行号 函数名]" 头部，并在析构时追加换行符并提交至
 * Logger。 当级别为 FATAL 时，析构自动触发 emergency_flush 并调用 ::abort() 产生 core dump。
 */
class LogMessage {
public:
    LogMessage(const char* file, int line, const char* func, LogLevel level);
    ~LogMessage();

    LogStream& stream() { return stream_; }

private:
    Timestamp time_;
    LogStream stream_;
    LogLevel level_;
    [[maybe_unused]] const char* file_;
    [[maybe_unused]] int line_;
    [[maybe_unused]] const char* func_;
};

// 6. 双门面宏定义（现代 C++ 流式规范 + printf 格式化兼容规范）
#define LOG_STREAM_IMPL(level)                                                    \
    if (::lean_web_server::base::Logger::get_instance().should_log(               \
            ::lean_web_server::base::LogLevel::level))                            \
    ::lean_web_server::base::LogMessage(__FILE__, __LINE__, __func__,             \
                                        ::lean_web_server::base::LogLevel::level) \
        .stream()

#define LOG_TRACE LOG_STREAM_IMPL(TRACE)
#define LOG_DEBUG LOG_STREAM_IMPL(DEBUG)
#define LOG_INFO LOG_STREAM_IMPL(INFO)
#define LOG_WARN LOG_STREAM_IMPL(WARN)
#define LOG_ERROR LOG_STREAM_IMPL(ERROR)
#define LOG_FATAL                                                                 \
    ::lean_web_server::base::LogMessage(__FILE__, __LINE__, __func__,             \
                                        ::lean_web_server::base::LogLevel::FATAL) \
        .stream()

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#define LEAN_LOG_FMT_IMPL(level, ...)                                         \
    do {                                                                      \
        if (::lean_web_server::base::Logger::get_instance().should_log(       \
                ::lean_web_server::base::LogLevel::level)) {                  \
            ::lean_web_server::base::Logger::get_instance().log(              \
                ::lean_web_server::base::LogLevel::level, __FILE__, __LINE__, \
                __func__, ##__VA_ARGS__);                                     \
        }                                                                     \
    } while (0)

#define LOG_FMT_TRACE(...) LEAN_LOG_FMT_IMPL(TRACE, __VA_ARGS__)
#define LOG_FMT_DEBUG(...) LEAN_LOG_FMT_IMPL(DEBUG, __VA_ARGS__)
#define LOG_FMT_INFO(...) LEAN_LOG_FMT_IMPL(INFO, __VA_ARGS__)
#define LOG_FMT_WARN(...) LEAN_LOG_FMT_IMPL(WARN, __VA_ARGS__)
#define LOG_FMT_ERROR(...) LEAN_LOG_FMT_IMPL(ERROR, __VA_ARGS__)
#define LOG_FMT_FATAL(...) LEAN_LOG_FMT_IMPL(FATAL, __VA_ARGS__)

}  // namespace lean_web_server::base
