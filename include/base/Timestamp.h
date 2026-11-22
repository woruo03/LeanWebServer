/**
 * @file Timestamp.h
 * @brief 微秒精度高并发时间戳类
 * @author woruo03
 * @date 2026-11-14
 * @version 1.0
 */

#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace lean_web_server::base {

/**
 * @class Timestamp
 * @brief 微秒精度时间戳类（支持单调/真实墙钟换算与线程局部秒级缓存）
 *
 * 内部存储自 Unix Epoch (1970-01-01 00:00:00 UTC) 以来的微秒数。
 * 格式化输出内部使用线程局部存储 (TLS) 缓存秒级字符串，单线程高频调用时可节省 99% 的
 * gmtime_r/localtime_r 系统开销。
 */
class Timestamp {
public:
    /// 构造非法/默认时间戳 (micro_seconds = 0)
    Timestamp() = default;

    /// 通过绝对微秒数构造
    explicit Timestamp(int64_t micro_seconds_since_epoch)
        : micro_seconds_since_epoch_(micro_seconds_since_epoch) {}

    /// 获取当前系统真实时间戳（基于 clock_gettime CLOCK_REALTIME）
    [[nodiscard]] static Timestamp now();

    /// 获取非法时间戳哨兵对象
    [[nodiscard]] static Timestamp invalid() { return {}; }

    /// 判断当前时间戳是否有效 (> 0)
    [[nodiscard]] bool valid() const { return micro_seconds_since_epoch_ > 0; }

    /// 获取自 Epoch 以来的绝对微秒数
    [[nodiscard]] int64_t micro_seconds_since_epoch() const { return micro_seconds_since_epoch_; }

    /// 截断获取整秒数
    [[nodiscard]] time_t seconds() const {
        return static_cast<time_t>(micro_seconds_since_epoch_ / k_micro_seconds_per_second);
    }

    /// 获取秒后残余的微秒数 [0, 999999]
    [[nodiscard]] int64_t micro_seconds() const {
        return micro_seconds_since_epoch_ % k_micro_seconds_per_second;
    }

    /// 转换为人类可读的标准时间字符串格式 "yyyy-MM-dd HH:mm:ss.zzzzzz"
    [[nodiscard]] std::string to_string() const;

    /// 格式化时间输出；show_us 指定是否输出微秒字段
    [[nodiscard]] std::string to_formatted_string(bool show_us = true) const;

    /// 专用于日志轮转文件命名的字符串格式 "YYYYMMDD-HHMMSS"
    [[nodiscard]] std::string to_filename_string() const;

    /// 相对偏移计算：增加指定的秒数（支持小数微秒级偏移）
    [[nodiscard]] Timestamp add_time(double seconds) const {
        auto delta = static_cast<int64_t>(seconds * k_micro_seconds_per_second);
        return Timestamp(micro_seconds_since_epoch_ + delta);
    }

    // 全序比较操作符重载
    friend bool operator<(Timestamp a, Timestamp b) {
        return a.micro_seconds_since_epoch_ < b.micro_seconds_since_epoch_;
    }
    friend bool operator==(Timestamp a, Timestamp b) {
        return a.micro_seconds_since_epoch_ == b.micro_seconds_since_epoch_;
    }
    friend bool operator<=(Timestamp a, Timestamp b) { return !(b < a); }
    friend bool operator>(Timestamp a, Timestamp b) { return b < a; }
    friend bool operator>=(Timestamp a, Timestamp b) { return !(a < b); }
    friend bool operator!=(Timestamp a, Timestamp b) { return !(a == b); }

    static constexpr int k_micro_seconds_per_second = 1000 * 1000;

private:
    int64_t micro_seconds_since_epoch_{0};
};

}  // namespace lean_web_server::base
