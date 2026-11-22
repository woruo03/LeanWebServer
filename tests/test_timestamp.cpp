/**
 * @file test_timestamp.cpp
 * @brief Timestamp 与 base 基础设施单元测试 (TS1-TS4, B1-B2)
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include <gtest/gtest.h>

#include <cstring>
#include <thread>
#include <type_traits>

#include "base/CurrentThread.h"
#include "base/NonCopyable.h"
#include "base/Timestamp.h"

using namespace lean_web_server::base;

// ── B1: NonCopyable 约束 ──
class DummyResource : NonCopyable {};

TEST(BaseTest, B1_NonCopyableConstraint) {
    static_assert(!std::is_copy_constructible_v<NonCopyable>);
    static_assert(!std::is_copy_assignable_v<NonCopyable>);
    static_assert(!std::is_copy_constructible_v<DummyResource>);
    static_assert(!std::is_copy_assignable_v<DummyResource>);
    SUCCEED();
}

// ── B2: CurrentThread ──
TEST(BaseTest, B2_CurrentThreadConsistency) {
    int tid1 = current_thread::tid();
    int tid2 = current_thread::tid();
    EXPECT_EQ(tid1, tid2);
    EXPECT_GT(tid1, 0);

    const char* str = current_thread::tid_string();
    int len = current_thread::tid_string_length();
    EXPECT_EQ(len, static_cast<int>(std::strlen(str)));
    EXPECT_EQ(std::stoi(str), tid1);

    int other_tid = 0;
    std::thread t([&other_tid] { other_tid = current_thread::tid(); });
    t.join();
    EXPECT_NE(tid1, other_tid);
    EXPECT_GT(other_tid, 0);
}

// ── TS1: 微秒换算与秒-残余切分 ──
TEST(TimestampTest, TS1_MicroSecondsConversion) {
    int64_t total_us = 1700000000123456LL;
    Timestamp t(total_us);
    EXPECT_TRUE(t.valid());
    EXPECT_EQ(t.micro_seconds_since_epoch(), total_us);
    EXPECT_EQ(t.seconds(), 1700000000LL);
    EXPECT_EQ(t.micro_seconds(), 123456LL);

    Timestamp inv = Timestamp::invalid();
    EXPECT_FALSE(inv.valid());
}

// ── TS2: add_time 跨秒边界 ──
TEST(TimestampTest, TS2_AddTimeCrossSecond) {
    int64_t base_us = 1000000999999LL;
    Timestamp t(base_us);
    Timestamp t_next = t.add_time(0.000002);  // +2 us
    EXPECT_EQ(t_next.micro_seconds_since_epoch(), base_us + 2);
    EXPECT_EQ(t_next.seconds(), t.seconds() + 1);
    EXPECT_EQ(t_next.micro_seconds(), 1);

    Timestamp t_prev = t.add_time(-1.0);
    EXPECT_EQ(t_prev.seconds(), t.seconds() - 1);
    EXPECT_EQ(t_prev.micro_seconds(), t.micro_seconds());
}

// ── TS3: 格式串长度恒 26 与闰日 fixture ──
TEST(TimestampTest, TS3_FormattedStringLength) {
    Timestamp now = Timestamp::now();
    std::string s = now.to_formatted_string(true);
    EXPECT_EQ(s.length(), 26U);
    EXPECT_EQ(s[4], '-');
    EXPECT_EQ(s[7], '-');
    EXPECT_EQ(s[10], ' ');
    EXPECT_EQ(s[13], ':');
    EXPECT_EQ(s[16], ':');
    EXPECT_EQ(s[19], '.');

    std::string s_no_us = now.to_formatted_string(false);
    EXPECT_EQ(s_no_us.length(), 19U);

    std::string fn = now.to_filename_string();
    EXPECT_EQ(fn.length(), 15U);
    EXPECT_EQ(fn[8], '-');
}

// ── TS4: 比较全序 ──
TEST(TimestampTest, TS4_OrderOperators) {
    Timestamp t1(1000);
    Timestamp t2(2000);
    Timestamp t1_dup(1000);

    EXPECT_LT(t1, t2);
    EXPECT_LE(t1, t2);
    EXPECT_LE(t1, t1_dup);
    EXPECT_EQ(t1, t1_dup);
    EXPECT_NE(t1, t2);
    EXPECT_GT(t2, t1);
    EXPECT_GE(t2, t1);
    EXPECT_GE(t1, t1_dup);
}

// ── TS5: 微秒差值与秒换算 ──
TEST(TimestampTest, TS5_MicroSecondsDiff) {
    Timestamp t1(1000000);  // 1s
    Timestamp t2(3500000);  // 3.5s
    int64_t diff_us = t2.micro_seconds_since_epoch() - t1.micro_seconds_since_epoch();
    EXPECT_EQ(diff_us, 2500000LL);
    double diff_sec = static_cast<double>(diff_us) / Timestamp::k_micro_seconds_per_second;
    EXPECT_DOUBLE_EQ(diff_sec, 2.5);
}
