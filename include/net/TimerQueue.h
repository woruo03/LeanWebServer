/**
 * @file TimerQueue.h
 * @brief 单一内核 timerfd 统一驱动的高精度单调定时调度队列
 * @author woruo03
 * @date 2026-11-14
 * @version 1.0
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <vector>

#include "base/NonCopyable.h"
#include "net/Channel.h"

namespace lean_web_server::net {

class EventLoop;

/**
 * @class Timer
 * @brief 定时器实体类
 *
 * 记录单次/周期定时任务的回调函数、下一次到期时间点、周期间隔以及全局单调递增的代际序列号。
 */
class Timer : base::NonCopyable {
public:
    using Callback = std::function<void()>;
    using TimePoint = std::chrono::steady_clock::time_point;

    Timer(Callback cb, TimePoint when, std::chrono::nanoseconds interval)
        : callback_(std::move(cb)),
          expiration_(when),
          interval_(interval),
          repeat_(interval.count() > 0),
          sequence_(s_num_created.fetch_add(1, std::memory_order_relaxed)) {}

    /// 执行定时器绑定的回调业务
    void run() const { callback_(); }

    /// 获取下一次到期时间戳
    [[nodiscard]] TimePoint expiration() const { return expiration_; }

    /// 是否为周期重复任务
    [[nodiscard]] bool repeat() const { return repeat_; }

    /// 获取定时器唯一全局序列号（防 ABA 撮合）
    [[nodiscard]] int64_t sequence() const { return sequence_; }

    /// 计算并更新下一次到期时间点
    void restart(TimePoint now) {
        if (repeat_) {
            expiration_ = now + interval_;
        } else {
            expiration_ = TimePoint{};
        }
    }

private:
    const Callback callback_;
    TimePoint expiration_;
    const std::chrono::nanoseconds interval_;
    const bool repeat_;
    const int64_t sequence_;

    static std::atomic<int64_t> s_num_created;
};

/**
 * @class TimerId
 * @brief 外部值类型定时器句柄（用于 O(log N) 安全注销）
 *
 * 携带 Timer* 指针与唯一的 64 位代际序列号，严格防止因原指针被复用导致的 ABA 误取消。
 */
class TimerId {
public:
    TimerId() = default;
    TimerId(Timer* timer, int64_t seq) : timer_(timer), sequence_(seq) {}

    /// 句柄是否有效绑定
    [[nodiscard]] bool valid() const { return timer_ != nullptr; }

    friend class TimerQueue;

private:
    Timer* timer_{nullptr};
    int64_t sequence_{0};
};

/**
 * @class TimerQueue
 * @brief 通用高精度定时器调度队列（单内核 timerfd 驱动）
 *
 * 核心机制：
 * - 采用 Linux 原生 timerfd_create(CLOCK_MONOTONIC)，彻底时钟免疫，不因墙钟跳变导致雪崩；
 * - 内部以 std::set 红黑树按 TimePoint 升序组织，始终以最早到期的任务动态校准内核 timerfd；
 * - 到期时通过 EpollPoller 驱动唤醒所属 EventLoop，零额外线程开销；
 * - 支持在定时器回调内部自撤销（通过 calling_cancel_ 记录并在重置阶段安全规避）。
 */
class TimerQueue : base::NonCopyable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    /// 向队列添加定时任务（跨线程自动转发至所属 loop 线程无锁执行）
    TimerId add_timer(Timer::Callback cb, TimePoint when, std::chrono::nanoseconds interval);

    /// 注销指定定时器（若未到期则从红黑树抹除并重校准 timerfd）
    void cancel(TimerId id);

#ifdef LEAN_WEB_SERVER_TESTING
    [[nodiscard]] size_t size_for_debug() const noexcept { return timers_.size(); }
#endif

private:
    using Entry = std::pair<TimePoint, Timer*>;
    using ActiveTimer = std::pair<Timer*, int64_t>;

    void handle_read();
    std::vector<Entry> get_expired(TimePoint now);
    void reset(const std::vector<Entry>& expired, TimePoint now);
    bool insert(Timer* timer);

    EventLoop* loop_;
    const int timer_fd_;
    Channel timerfd_channel_;

    std::set<Entry> timers_;
    std::set<ActiveTimer> active_timers_;
    bool calling_expired_{false};
    std::set<ActiveTimer> calling_cancel_;
};

}  // namespace lean_web_server::net
