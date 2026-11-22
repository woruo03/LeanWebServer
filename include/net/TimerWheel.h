/**
 * @file TimerWheel.h
 * @brief 基于动态可配置散列时间轮的长连接滚动空闲检测与剔除器
 * @author woruo03
 * @date 2026-11-16
 * @version 1.0
 */

#pragma once

#include <memory>
#include <vector>

#include "base/NonCopyable.h"
#include "net/TimerQueue.h"

namespace lean_web_server::net {

class EventLoop;
class TcpConnection;

/**
 * @class TimerWheel
 * @brief 基于散列时间轮的长连接滚动空闲检测剔除器
 *
 * 核心机制：
 * - 槽位数默认 1024 槽（可基于 ServerConfig 灵活配置为 2 的幂次），取模优化为位与操作 `&
 * (num_slots_ - 1)`；
 * - 统一向所属 EventLoop 的 TimerQueue 注册 1s 周期任务进行步进驱动（tick）；
 * - 槽位仅保存 `std::weak_ptr<TcpConnection>`，彻底消除定时容器对连接的生命周期侵入；
 * - **纳秒级同秒短路去重**：连接在同 1 秒内的多次读写交互直接命中短路，不发生槽位迁移与弱引用追加；
 * - **代际验真防误杀**：仅当当前轮转游标与连接记录的 `last_slot`
 * 一致时才确认超时并触发剔除，杜绝因续命导致假死误杀。
 */
class TimerWheel : base::NonCopyable {
public:
    static constexpr size_t k_default_slot_count = 1024;  ///< 默认环形槽位总数（1024 槽）

    explicit TimerWheel(EventLoop* loop, size_t slot_count = k_default_slot_count);
    ~TimerWheel();

    /// 获取时间轮当前总槽位数
    [[nodiscard]] size_t slot_count() const noexcept { return num_slots_; }

    /// 启动时间轮（向所属 loop 注册 1 秒步进任务）
    void start();

    /// 活跃续命：按连接配置计算目标槽位，同秒短路去重并常数级追加弱引用
    void refresh(const std::shared_ptr<TcpConnection>& conn);

private:
    /// 每秒步进驱动与槽位代际验真
    void tick();

    EventLoop* loop_;
    size_t num_slots_{k_default_slot_count};
    size_t cursor_;
    TimerId wheel_timer_id_;

    // 每个槽位保存 weak_ptr，实现惰性淘汰与零锁开销
    std::vector<std::vector<std::weak_ptr<TcpConnection>>> slots_;
};

}  // namespace lean_web_server::net
