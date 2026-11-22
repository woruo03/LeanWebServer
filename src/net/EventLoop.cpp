/**
 * @file EventLoop.cpp
 * @brief 事件循环实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "net/EventLoop.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cassert>

#include "base/CurrentThread.h"
#include "base/Logger.h"
#include "net/TcpConnection.h"
#include "net/TimerQueue.h"
#include "net/TimerWheel.h"

namespace lean_web_server::net {

EventLoop::EventLoop(size_t timer_wheel_slots)
    : thread_id_(std::this_thread::get_id()),
      poller_(std::make_unique<EpollPoller>()),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      wakeup_channel_(std::make_unique<Channel>(this, wakeup_fd_)),
      timer_queue_(std::make_unique<TimerQueue>(this)),
      timer_wheel_(std::make_unique<TimerWheel>(this, timer_wheel_slots)) {
    if (wakeup_fd_ < 0) {
        LOG_FATAL << "Failed to create eventfd, errno=" << errno;
    }
    wakeup_channel_->set_read_callback([this] { handle_wakeup(); });
    wakeup_channel_->enable_read();
    timer_wheel_->start();
}

EventLoop::~EventLoop() {
    local_connections_.clear();
    wakeup_channel_->disable_all();
    wakeup_channel_->remove();
    int fd = wakeup_fd_;
    wakeup_fd_ = -1;
    if (fd >= 0) {
        ::close(fd);
    }
}

TimerId EventLoop::run_at(TimePoint when, TimerCallback cb) {
    return timer_queue_->add_timer(std::move(cb), when, std::chrono::nanoseconds(0));
}

TimerId EventLoop::run_after(std::chrono::nanoseconds delay, TimerCallback cb) {
    return run_at(std::chrono::steady_clock::now() + delay, std::move(cb));
}

TimerId EventLoop::run_every(std::chrono::nanoseconds interval, TimerCallback cb) {
    return timer_queue_->add_timer(std::move(cb), std::chrono::steady_clock::now() + interval,
                                   interval);
}

void EventLoop::cancel(TimerId id) {
    timer_queue_->cancel(id);
}

void EventLoop::loop() {
    looping_ = true;
    quit_ = false;
    LOG_INFO << "EventLoop " << static_cast<const void*>(this) << " start looping in thread "
             << base::current_thread::tid();

    while (!quit_.load(std::memory_order_acquire)) {
        active_channel_.clear();
        poller_->poll(active_channel_);
        for (auto* channel : active_channel_) {
            channel->handle_event();
        }
        do_pending_functors();
    }

    LOG_INFO << "EventLoop " << static_cast<const void*>(this) << " stop looping";
    looping_ = false;
}

void EventLoop::run_in_loop(Functor cb) {
    if (is_in_loop_thread()) {
        cb();
    } else {
        queue_in_loop(std::move(cb));
    }
}

void EventLoop::queue_in_loop(Functor cb) {
    {
        std::scoped_lock lock(mutex_);
        pending_functors_.push_back(std::move(cb));
    }
    if (!is_in_loop_thread() || calling_pending_functors_) {
        wakeup();
    }
}

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

void EventLoop::new_connection(int sockfd, uint64_t conn_id, const TimeoutConfig& config) {
    if (draining_) {
        LOG_WARN << "EventLoop is draining, rejecting new connection fd=" << sockfd;
        ::close(sockfd);
        return;
    }

    // 设置 TCP_NODELAY，禁用 Nagle 算法消除小包延迟抖动
    int optval = 1;
    ::setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, static_cast<socklen_t>(sizeof(optval)));

    auto conn = std::make_shared<TcpConnection>(this, sockfd, conn_id, config);
    local_connections_[sockfd] = conn;

    conn->set_close_callback([this](const std::shared_ptr<TcpConnection>& c) {
        queue_in_loop([this, c] {
            c->connect_destroyed();
            erase_connection(c->get_fd());
        });
    });

    conn->connect_established();
    LOG_INFO << "New connection established fd=" << sockfd << ", conn_id=" << conn_id;
}

void EventLoop::erase_connection(int fd) {
    local_connections_.erase(fd);
    if (draining_ && local_connections_.empty()) {
        if (on_drain_complete_) {
            on_drain_complete_();
        }
        quit();
    }
}

void EventLoop::initiate_graceful_shutdown(Functor on_complete) {
    draining_ = true;
    on_drain_complete_ = std::move(on_complete);

    if (local_connections_.empty()) {
        if (on_drain_complete_) {
            on_drain_complete_();
        }
        quit();
        return;
    }

    std::vector<std::shared_ptr<TcpConnection>> conns;
    conns.reserve(local_connections_.size());
    for (const auto& pair : local_connections_) {
        conns.push_back(pair.second);
    }
    for (const auto& conn : conns) {
        conn->force_close();
    }
}

void EventLoop::wakeup() const {
    if (wakeup_fd_ < 0) {
        return;
    }
    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        if (errno == EBADF) {
            return;
        }
        LOG_ERROR << "EventLoop::wakeup() write " << n << " bytes instead of 8, errno=" << errno;
    }
}

void EventLoop::handle_wakeup() const {
    uint64_t one = 1;
    ssize_t n = ::read(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR << "EventLoop::handle_wakeup() read " << n
                  << " bytes instead of 8, errno=" << errno;
    }
}

void EventLoop::do_pending_functors() {
    std::vector<Functor> functors;
    calling_pending_functors_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }

    for (const auto& functor : functors) {
        functor();
    }

    calling_pending_functors_ = false;
}

}  // namespace lean_web_server::net
