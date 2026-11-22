/**
 * @file TimerQueue.cpp
 * @brief 内核 timerfd 统一驱动的高精度定时调度队列实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "net/TimerQueue.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cassert>
#include <cstring>

#include "base/Logger.h"
#include "net/EventLoop.h"

namespace lean_web_server::net {

std::atomic<int64_t> Timer::s_num_created{0};

namespace {

int create_timerfd() {
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        LOG_FATAL << "Failed to create timerfd, errno=" << errno;
    }
    return timerfd;
}

struct timespec how_much_time_from_now(TimerQueue::TimePoint when) {
    using namespace std::chrono;
    auto now = steady_clock::now();
    nanoseconds ns = when > now ? duration_cast<nanoseconds>(when - now) : nanoseconds(0);
    struct timespec ts{};
    ts.tv_sec = static_cast<time_t>(ns.count() / 1000000000);
    ts.tv_nsec = static_cast<long>(ns.count() % 1000000000);
    if (ts.tv_sec == 0 && ts.tv_nsec == 0) {
        ts.tv_nsec = 1000;  // 至少 1 微秒相对时间
    }
    return ts;
}

void reset_timerfd(int timerfd, TimerQueue::TimePoint expiration) {
    struct itimerspec new_value{};
    new_value.it_value = how_much_time_from_now(expiration);
    int ret = ::timerfd_settime(timerfd, 0, &new_value, nullptr);
    if (ret < 0) {
        LOG_ERROR << "timerfd_settime failed: errno=" << errno;
    }
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop), timer_fd_(create_timerfd()), timerfd_channel_(loop, timer_fd_) {
    timerfd_channel_.set_read_callback([this] { handle_read(); });
    timerfd_channel_.enable_read();
}

TimerQueue::~TimerQueue() {
    timerfd_channel_.disable_all();
    timerfd_channel_.remove();
    ::close(timer_fd_);
    for (const auto& entry : timers_) {
        delete entry.second;
    }
}

TimerId TimerQueue::add_timer(Timer::Callback cb, TimePoint when,
                              std::chrono::nanoseconds interval) {
    auto* timer = new Timer(std::move(cb), when, interval);
    TimerId id(timer, timer->sequence());
    if (loop_->is_in_loop_thread()) {
        bool earliest_changed = insert(timer);
        if (earliest_changed) {
            reset_timerfd(timer_fd_, timer->expiration());
        }
    } else {
        loop_->run_in_loop([this, timer] {
            bool earliest_changed = insert(timer);
            if (earliest_changed) {
                reset_timerfd(timer_fd_, timer->expiration());
            }
        });
    }

    return id;
}

void TimerQueue::cancel(TimerId id) {
    if (id.timer_ == nullptr) {
        return;
    }

    if (loop_->is_in_loop_thread()) {
        ActiveTimer active(id.timer_, id.sequence_);
        auto it = active_timers_.find(active);
        if (it != active_timers_.end()) {
            TimePoint exp = it->first->expiration();
            size_t n = timers_.erase(Entry(exp, it->first));
            assert(n == 1);
            (void)n;
            delete it->first;
            active_timers_.erase(it);

            if (!timers_.empty()) {
                reset_timerfd(timer_fd_, timers_.begin()->first);
            } else {
                struct itimerspec new_value{};
                ::timerfd_settime(timer_fd_, 0, &new_value, nullptr);
            }
        } else if (calling_expired_) {
            calling_cancel_.insert(active);
        }
    } else {
        loop_->run_in_loop([this, id] { cancel(id); });
    }
}

void TimerQueue::handle_read() {
    uint64_t how_many = 0;
    ssize_t n = ::read(timer_fd_, &how_many, sizeof(how_many));
    if (n != sizeof(how_many) && errno != EAGAIN) {
        LOG_ERROR << "TimerQueue::handle_read read " << n << " bytes instead of 8";
    }

    auto now = std::chrono::steady_clock::now();
    std::vector<Entry> expired = get_expired(now);

    calling_expired_ = true;
    calling_cancel_.clear();
    for (const auto& it : expired) {
        it.second->run();
    }
    calling_expired_ = false;

    reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::get_expired(TimePoint now) {
    std::vector<Entry> expired;
    Entry sentry(now + TimePoint::duration(1), nullptr);
    auto end = timers_.lower_bound(sentry);
    std::copy(timers_.begin(), end, std::back_inserter(expired));
    timers_.erase(timers_.begin(), end);

    for (const auto& it : expired) {
        ActiveTimer timer(it.second, it.second->sequence());
        size_t n = active_timers_.erase(timer);
        assert(n == 1);
        (void)n;
    }
    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, TimePoint now) {
    for (const auto& it : expired) {
        ActiveTimer timer(it.second, it.second->sequence());
        if (it.second->repeat() && calling_cancel_.find(timer) == calling_cancel_.end()) {
            it.second->restart(now);
            insert(it.second);
        } else {
            delete it.second;
        }
    }

    if (!timers_.empty()) {
        reset_timerfd(timer_fd_, timers_.begin()->first);
    }
}

bool TimerQueue::insert(Timer* timer) {
    bool earliest_changed = false;
    TimePoint when = timer->expiration();
    if (timers_.empty() || when < timers_.begin()->first) {
        earliest_changed = true;
    }
    timers_.insert(Entry(when, timer));
    active_timers_.insert(ActiveTimer(timer, timer->sequence()));
    return earliest_changed;
}

}  // namespace lean_web_server::net
