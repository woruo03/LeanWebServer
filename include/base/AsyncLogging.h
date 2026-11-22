/**
 * @file AsyncLogging.h
 * @brief 异步落盘引擎核心（双缓冲交换、写盘线程、自动大小/日期切分与背压丢弃）
 * @author woruo03
 * @date 2026-11-13
 * @version 1.0
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/Logger.h"
#include "base/NonCopyable.h"

namespace lean_web_server::base {

constexpr size_t k_large_buffer_size =
    static_cast<const size_t>(4 * 1024 * 1024);  ///< 单个落盘缓冲块大小（4MB）
constexpr size_t k_log_queue_capacity = 25;      ///< 背压保护队列最大容量（25 块，共计 100MB 封顶）

/**
 * @class AsyncLogging
 * @brief 高性能异步日志落盘引擎
 *
 * 采用经典的 Double Buffering（双缓冲）技术：
 * - 前端通过 current_buffer_ 快速吸收日志行；满时推入 buffers_ 并与 next_buffer_ 快速置换；
 * - 后端写线程周期性（默认 3 秒）或在缓冲区满时被唤醒，通过原子 vector::swap 互换落盘队列；
 * - 提供 25 块背压丢弃保护（防止极端瞬时突发洪峰耗尽进程内存）；
 * - 内置 64KB 用户态缓冲的 ::fwrite_unlocked 高性能顺序写盘与基于大小/跨日的自动轮转清理。
 */
class AsyncLogging : NonCopyable {
public:
    AsyncLogging(std::string log_dir, std::string base_name, size_t roll_size,
                 int flush_interval = 3);
    ~AsyncLogging();

    /// 前端线程追加日志行（带锁轻量操作，极快返回）
    void append(const char* log_line, size_t len);

    /// 启动后端落盘工作线程
    void start();

    /// 停止后端线程并确保所有驻留在缓冲区中的日志完全落盘
    void stop();

    /// 致命异常/崩溃时旁路直刷（无锁同步写入当前打开的日志文件或 stderr）
    void emergency_flush(const char* log_line, size_t len);

private:
    using Buffer = FixedBuffer<k_large_buffer_size>;
    using BufferPtr = std::unique_ptr<Buffer>;
    using BufferPtrVector = std::vector<BufferPtr>;

    void thread_func();
    void roll_file();
    void write_to_file(const char* data, size_t len);
    void handle_backpressure(BufferPtrVector& buffers);
    void write_buffers(const BufferPtrVector& buffers);
    static void replenish_buffers(BufferPtr& new_buffer1, BufferPtr& new_buffer2,
                           BufferPtrVector& buffers_to_write);

    const std::string log_dir_;
    const std::string base_name_;
    const size_t roll_size_bytes_;
    const int flush_interval_sec_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;

    BufferPtr current_buffer_;
    BufferPtr next_buffer_;
    BufferPtrVector buffers_;

    // 文件落盘与轮转内聚状态
    FILE* fp_{nullptr};
    std::array<char, static_cast<std::size_t>(64 * 1024)>
        file_buffer_{};  // 64KB 用户态缓冲，加速 fwrite_unlocked
    size_t written_bytes_{0};
    time_t last_roll_time_{0};
    int current_day_{0};
    std::atomic<uint64_t> dropped_lines_{0};
};

}  // namespace lean_web_server::base
