/**
 * @file EventLoop.h
 * @brief Reactor 核心事件循环类，管理 I/O 多路复用、无锁任务队列、高精度定时与自治连接生命周期
 * @author woruo03
 * @date 2026-10-29
 * @version 1.0
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/NonCopyable.h"
#include "net/Channel.h"
#include "net/EpollPoller.h"

namespace lean_web_server::net {

class Timer;
class TimerQueue;
class TimerId;
struct TimeoutConfig;
class TimerWheel;
class TcpConnection;

/**
 * @class EventLoop
 * @brief 单线程 Reactor 核心事件循环（One Loop Per Thread）
 *
 * 核心职责：
 * 1. I/O 多路复用：封装 EpollPoller，分发就绪的 Channel 读写事件；
 * 2. 跨线程任务队列：提供 run_in_loop / queue_in_loop，带 calling_pending_functors_ 唤醒剪枝；
 * 3. 统一高精度定时：内聚基于 timerfd 的 TimerQueue 与可配置槽位长连接散列时间轮 TimerWheel；
 * 4. Sub-Reactor Share-Nothing 连接自治：完全在当前 loop 线程内维护 local_connections_
 * 表，零全局锁；
 * 5. 优雅停机协作：支持 initiate_graceful_shutdown() 排空与回调驱动的主循环 quit() 退出。
 */
class EventLoop : base::NonCopyable {
public:
    using Functor = std::function<void()>;
    using TimePoint = std::chrono::steady_clock::time_point;
    using TimerCallback = std::function<void()>;

    explicit EventLoop(size_t timer_wheel_slots = 1024);
    ~EventLoop();

    // ── 高精度定时器对外接口 ──
    [[nodiscard]] TimerId run_at(TimePoint when, TimerCallback cb);
    [[nodiscard]] TimerId run_after(std::chrono::nanoseconds delay, TimerCallback cb);
    [[nodiscard]] TimerId run_every(std::chrono::nanoseconds interval, TimerCallback cb);
    void cancel(TimerId id);

    /// 获取内置的散列时间轮指针
    TimerWheel* timer_wheel() { return timer_wheel_.get(); }

    /// 进入事件循环（阻塞运行，直到 quit() 被调用）
    void loop();

    /// 在循环所在线程中执行指定回调（若当前在本线程则立即同步执行，否则投递至队列并唤醒）
    void run_in_loop(Functor cb);

    /// 将回调函数放入待执行队列并唤醒循环线程
    void queue_in_loop(Functor cb);

    /// 向底层 poller 注册或更新通道关注的事件
    void update_channel(Channel* channel) { poller_->update_channel(channel); }

    /// 从底层 poller 移除通道
    void remove_channel(Channel* channel) { poller_->remove_channel(channel); }

    /// 判断当前调用线程是否即为该 EventLoop 的专属所属线程
    [[nodiscard]] bool is_in_loop_thread() const {
        return thread_id_ == std::this_thread::get_id();
    }

    // ── Sub-Reactor 连接自治管理接口（严格在当前 loop 线程无锁执行） ──

    /// 创建并就地托管新连接
    void new_connection(int sockfd, uint64_t conn_id, const TimeoutConfig& config);

    /// 从本地连接表中擦除连接
    void erase_connection(int fd);

    /// 触发优雅停机排空（依次 force_close 本地连接并在排空完毕后回调通知）
    void initiate_graceful_shutdown(Functor on_complete = {});

    /// 唤醒并终止事件循环
    void quit();

#ifdef LEAN_WEB_SERVER_TESTING
    [[nodiscard]] size_t connection_count_for_debug() const noexcept {
        return local_connections_.size();
    }
#endif

private:
    const std::thread::id thread_id_;          // 线程 ID
    std::atomic<bool> looping_{false};         // 循环运行状态标志
    std::unique_ptr<EpollPoller> poller_;      // I/O 轮询器
    std::vector<Channel*> active_channel_;     // 本轮活跃通道列表
    int wakeup_fd_;                            // eventfd 唤醒描述符
    std::unique_ptr<Channel> wakeup_channel_;  // 唤醒通道

    std::mutex mutex_;                       // 保护 pending_functors_
    std::vector<Functor> pending_functors_;  // 待跨线程执行的函数队列

    std::atomic<bool> quit_{false};         // 退出标志
    bool calling_pending_functors_{false};  // 是否正在执行待处理任务（用于剪枝 wakeup()）
    bool draining_{false};                  // 是否处于停机排水中
    Functor on_drain_complete_;             // 排水完成回调

    std::unique_ptr<TimerQueue> timer_queue_;
    std::unique_ptr<TimerWheel> timer_wheel_;

    // 声明序防线：必须声明在 poller_ / timer_queue_ 之后，保证反向析构先释放连接与 Channel
    std::unordered_map<int, std::shared_ptr<TcpConnection>> local_connections_;

    void wakeup() const;
    void handle_wakeup() const;
    void do_pending_functors();
};

}  // namespace lean_web_server::net
