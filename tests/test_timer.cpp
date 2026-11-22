/**
 * @file test_timer.cpp
 * @brief 定时器系统单元测试 (T0-T9)
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "net/EventLoop.h"
#include "net/TcpConnection.h"
#include "net/TimerQueue.h"
#include "net/TimerWheel.h"

using namespace lean_web_server::net;
using namespace std::chrono_literals;

// ── T0: 时钟单调性（基于 steady_clock） ──
TEST(TimerTest, T0_SteadyClockMonotonic) {
    auto t1 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(10ms);
    auto t2 = std::chrono::steady_clock::now();
    EXPECT_GT(t2, t1);
}

// ── T1: 单发定时器到期与执行 ──
TEST(TimerTest, T1_SingleShotTimer) {
    EventLoop loop;
    bool fired = false;
    auto start = std::chrono::steady_clock::now();

    (void)loop.run_after(50ms, [&] {
        fired = true;
        loop.quit();
    });

    loop.loop();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    EXPECT_TRUE(fired);
    EXPECT_GE(elapsed, 45);  // 允许微小抖动
    EXPECT_LE(elapsed, 150);
}

// ── T2: loop 阻塞在 epoll_wait 时由 timerfd 驱动唤醒 ──
TEST(TimerTest, T2_TimerfdWakesUpLoop) {
    EventLoop loop;
    std::thread loop_thread([&loop] { loop.loop(); });

    std::this_thread::sleep_for(20ms);  // 确保 loop 已阻塞于 epoll_wait

    std::atomic<bool> fired{false};
    auto start = std::chrono::steady_clock::now();

    (void)loop.run_after(60ms, [&] {
        fired = true;
        loop.quit();
    });

    loop_thread.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    EXPECT_TRUE(fired);
    EXPECT_GE(elapsed, 50);
}

// ── T3: cancel 晚于到期（合法竞态） ──
TEST(TimerTest, T3_CancelAfterExpiration) {
    EventLoop loop;
    int count = 0;
    TimerId tid = loop.run_after(20ms, [&] { ++count; });

    (void)loop.run_after(50ms, [&] {
        loop.cancel(tid);  // 已到期，应当静默安全忽略
        loop.quit();
    });

    loop.loop();
    EXPECT_EQ(count, 1);
}

// ── T4: weak_ptr token 作废 ──
TEST(TimerTest, T4_WeakTokenInvalidation) {
    EventLoop loop;
    bool called = false;

    struct Context {
        int val = 42;
    };
    auto ctx = std::make_shared<Context>();
    std::weak_ptr<Context> weak_ctx = ctx;

    (void)loop.run_after(30ms, [&called, weak_ctx] {
        if (auto c = weak_ctx.lock()) {
            called = true;
        }
    });

    ctx.reset();  // 提前销毁上下文

    (void)loop.run_after(60ms, [&loop] { loop.quit(); });
    loop.loop();

    EXPECT_FALSE(called);
}

// ── T5: 周期任务 repeat 与不追帧 ──
TEST(TimerTest, T5_RepeatTimer) {
    EventLoop loop;
    int count = 0;

    TimerId tid = loop.run_every(25ms, [&] {
        if (++count == 4) {
            loop.quit();
        }
    });

    loop.loop();
    loop.cancel(tid);
    EXPECT_EQ(count, 4);
}

// ── T6: 回调内自撤销 (calling_cancel_) ──
TEST(TimerTest, T6_SelfCancelInCallback) {
    EventLoop loop;
    int count = 0;
    TimerId tid;

    tid = loop.run_every(20ms, [&] {
        ++count;
        if (count == 2) {
            loop.cancel(tid);  // 回调内部撤销自身
            (void)loop.run_after(50ms, [&loop] { loop.quit(); });
        }
    });

    loop.loop();
    EXPECT_EQ(count, 2);
}

// ── T7: 精度验收（100ms 档位误差分布） ──
TEST(TimerTest, T7_PrecisionAccuracy) {
    EventLoop loop;
    constexpr int k_trials = 10;
    int completed = 0;
    std::vector<int64_t> errors_ms;

    for (int i = 0; i < k_trials; ++i) {
        auto start = std::chrono::steady_clock::now();
        (void)loop.run_after(50ms, [&loop, start, &errors_ms, &completed] {
            auto now = std::chrono::steady_clock::now();
            auto diff = std::abs(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() - 50);
            errors_ms.push_back(diff);
            if (++completed == k_trials) {
                loop.quit();
            }
        });
    }

    loop.loop();
    EXPECT_EQ(completed, k_trials);

    int64_t sum = 0;
    for (auto err : errors_ms) {
        sum += err;
        EXPECT_LE(err, 10);  // 误差不超过 10ms
    }
    double avg = static_cast<double>(sum) / k_trials;
    EXPECT_LE(avg, 5.0);
}

// ── T8: TimerWheel 槽位计算与同秒短路去重 ──
TEST(TimerTest, T8_TimerWheelSlotGeneration) {
    EventLoop loop;
    TimerWheel wheel(&loop);

    TimeoutConfig tc;
    tc.idle_timeout_sec = 15;
    auto conn = std::make_shared<TcpConnection>(&loop, -1, 555, tc);

    EXPECT_EQ(conn->last_slot(), -1);
    wheel.refresh(conn);
    int slot1 = conn->last_slot();
    EXPECT_GE(slot1, 0);
    EXPECT_LT(slot1, 1024);

    // 同一秒内再次 refresh：应当短路去重，槽位不重复插入且保持原值
    wheel.refresh(conn);
    EXPECT_EQ(conn->last_slot(), slot1);

    // 自定义槽位时间轮验证
    TimerWheel custom_wheel(&loop, 256);
    EXPECT_EQ(custom_wheel.slot_count(), 256);
    conn->set_last_slot(-1);
    custom_wheel.refresh(conn);
    int slot2 = conn->last_slot();
    EXPECT_GE(slot2, 0);
    EXPECT_LT(slot2, 256);
}

// ── T9: add / cancel 吞吐基准 ──
TEST(TimerTest, T9_AddCancelThroughput) {
    EventLoop loop;
    constexpr int k_ops = 50000;
    std::vector<TimerId> ids;
    ids.reserve(k_ops);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < k_ops; ++i) {
        ids.push_back(loop.run_after(100s, [] {}));
    }
    for (auto id : ids) {
        loop.cancel(id);
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    double sec = static_cast<double>(elapsed) / 1000.0;
    if (sec > 0.0) {
        double ops_per_sec = (k_ops * 2) / sec;
        std::cout << "[BENCHMARK] Timer add/cancel: " << static_cast<int>(ops_per_sec) << " ops/s"
                  << std::endl;
        EXPECT_GE(ops_per_sec, 100000.0);
    }
}

// ── T10: TimerQueue 计数与结构生命周期 ──
TEST(TimerTest, T10_TimerQueueSizeAndLifecycle) {
    EventLoop loop;
    TimerQueue queue(&loop);

    EXPECT_EQ(queue.size_for_debug(), 0);

    auto id1 = queue.add_timer([] {}, std::chrono::steady_clock::now() + 10s, 0s);
    auto id2 = queue.add_timer([] {}, std::chrono::steady_clock::now() + 20s, 0s);
    EXPECT_EQ(queue.size_for_debug(), 2);

    queue.cancel(id1);
    EXPECT_EQ(queue.size_for_debug(), 1);

    queue.cancel(id2);
    EXPECT_EQ(queue.size_for_debug(), 0);
}
