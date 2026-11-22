/**
 * @file EventLoopThreadPool.h
 * @brief Sub-Reactor 事件循环工作线程池，负责 IO 线程生命周期管理与 Round-Robin 均衡分发
 * @author woruo03
 * @date 2026-10-31
 * @version 1.0
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "base/NonCopyable.h"

namespace lean_web_server::net {

class EventLoop;

/**
 * @class EventLoopThreadPool
 * @brief Sub-Reactor IO 线程池管理器
 *
 * 核心机制：
 * - 每个工作线程独立运行一个专属于该线程的 EventLoop 实例（One Loop Per Thread）；
 * - start() 使用条件变量同步等待所有 Sub-Reactor 完成初始化启动；
 * - get_next_loop() 通过无锁递增取模实现严格的 Round-Robin 连接均衡轮询调度；
 * - stop() 保证全部 worker 线程通过 join() 安全回收，彻底杜绝线程泄漏与脱垂。
 */
class EventLoopThreadPool : base::NonCopyable {
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    EventLoopThreadPool(EventLoop* baseLoop, int num_threads, size_t timer_wheel_slots = 1024)
        : num_threads_(num_threads), baseLoop_(baseLoop), timer_wheel_slots_(timer_wheel_slots) {}
    ~EventLoopThreadPool();

    /// 启动线程池并同步等待所有 Sub-Reactor 启动就绪
    void start();

    /// 轮询获取下一个承接连接的 Sub-Reactor 事件循环
    EventLoop* get_next_loop();

    /// 获取所有 Sub-Reactor 事件循环指针集合（用于广播优雅停机排空）
    [[nodiscard]] const std::vector<EventLoop*>& loops() const noexcept { return eventLoops_; }

    /// 广播退出并依次 join() 回收所有工作线程
    void stop();

private:
    size_t next_{0};                      // 下一个事件循环轮询索引
    int num_threads_;                     // 线程池设定的线程数量
    EventLoop* baseLoop_;                 // 主 Reactor 指针（在 num_threads_ == 0 时兜底）
    size_t timer_wheel_slots_{1024};      // 时间轮散列槽位数
    std::mutex mutex_;                    // 保护初始化同步
    std::condition_variable cv_;          // 启动就绪条件变量
    std::vector<EventLoop*> eventLoops_;  // Sub-Reactor 事件循环集合
    std::vector<std::thread> threads_;    // 托管的 OS 线程句柄列表
    bool stopped_{false};                 // 停机防重入标志
};

}  // namespace lean_web_server::net
