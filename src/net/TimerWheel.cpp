/**
 * @file TimerWheel.cpp
 * @brief 基于散列时间轮的长连接滚动空闲检测剔除器实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "net/TimerWheel.h"

#include <cassert>

#include "net/EventLoop.h"
#include "net/TcpConnection.h"

namespace lean_web_server::net {

TimerWheel::TimerWheel(EventLoop* loop, size_t slot_count)
    : loop_(loop),
      num_slots_(slot_count > 0 ? slot_count : k_default_slot_count),
      cursor_(0),
      slots_(num_slots_) {}

TimerWheel::~TimerWheel() = default;

void TimerWheel::start() {
    wheel_timer_id_ = loop_->run_every(std::chrono::seconds{1}, [this] { tick(); });
}

void TimerWheel::refresh(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        return;
    }
    int timeout = conn->timeout_config().idle_timeout_sec;
    assert(timeout > 0 && static_cast<size_t>(timeout) < num_slots_);
    int target_slot = static_cast<int>((cursor_ + static_cast<size_t>(timeout)) & (num_slots_ - 1));

    // 1. 同秒内短路去重
    if (conn->last_slot() == target_slot) {
        return;
    }
    // 2. 更新标记
    conn->set_last_slot(target_slot);
    // 3. 常数级追加弱引用
    slots_[target_slot].emplace_back(conn);
}

void TimerWheel::tick() {
    cursor_ = (cursor_ + 1) & (num_slots_ - 1);
    auto& slot = slots_[cursor_];

    for (const auto& weak_conn : slot) {
        auto conn = weak_conn.lock();
        if (!conn) {
            continue;
        }
        // 代际验真防误杀：仅当连接的最近槽位与当前游标一致时才说明超时
        if (conn->last_slot() == static_cast<int>(cursor_)) {
            conn->timeout_close(TimeoutKind::kIdle);
        }
    }
    slot.clear();
}

}  // namespace lean_web_server::net
