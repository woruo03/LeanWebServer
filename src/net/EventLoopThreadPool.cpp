/**
 * @file EventLoopThreadPool.cpp
 * @brief 事件循环线程池实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "net/EventLoopThreadPool.h"

#include <cassert>

#include "net/EventLoop.h"

namespace lean_web_server::net {

EventLoopThreadPool::~EventLoopThreadPool() {
    stop();
}

void EventLoopThreadPool::start() {
    for (int i = 0; i < num_threads_; ++i) {
        threads_.emplace_back([this, wheel_slots = timer_wheel_slots_] {
            EventLoop loop(wheel_slots);
            {
                std::scoped_lock<std::mutex> lock(mutex_);
                eventLoops_.emplace_back(&loop);
            }
            cv_.notify_one();
            loop.loop();
            {
                std::scoped_lock<std::mutex> lock(mutex_);
                for (auto& l : eventLoops_) {
                    if (l == &loop) {
                        l = nullptr;
                    }
                }
            }
        });
    }
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return eventLoops_.size() == static_cast<size_t>(num_threads_); });
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    if (eventLoops_.empty()) {
        return baseLoop_;
    }
    EventLoop* loop = eventLoops_[next_];
    next_ = (next_ + 1) % eventLoops_.size();
    return loop;
}

void EventLoopThreadPool::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;

    {
        std::scoped_lock<std::mutex> lock(mutex_);
        for (auto* loop : eventLoops_) {
            if (loop != nullptr) {
                loop->quit();
            }
        }
    }
    for (auto& th : threads_) {
        if (th.joinable()) {
            th.join();
        }
    }
    threads_.clear();
    eventLoops_.clear();
}

}  // namespace lean_web_server::net
