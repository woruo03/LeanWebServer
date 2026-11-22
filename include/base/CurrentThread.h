/**
 * @file CurrentThread.h
 * @brief 线程局部缓存 Linux 内核真实 TID（Thread ID）与预格式化字符串
 * @author woruo03
 * @date 2026-11-13
 * @version 1.0
 */

#pragma once

#include <sys/syscall.h>
#include <unistd.h>

#include <array>

namespace lean_web_server::base::current_thread {

extern thread_local int t_cached_tid;
extern thread_local std::array<char, 32> t_tid_string;
extern thread_local int t_tid_string_length;

/// 首次访问时通过 SYS_gettid 系统调用获取内核真实 TID 并格式化存入 TLS
void cache_tid();

/**
 * @brief 获取当前线程的真实内核 TID
 * @return 整数型 TID（如 12345）
 */
inline int tid() {
    if (__builtin_expect(static_cast<long>(t_cached_tid == 0), 0) != 0) {
        cache_tid();
    }
    return t_cached_tid;
}

/**
 * @brief 获取已预格式化为以 null 结尾的 TID 字符串
 * @return 字符指针，如 "12345"
 */
inline const char* tid_string() {
    if (__builtin_expect(static_cast<long>(t_cached_tid == 0), 0) != 0) {
        cache_tid();
    }
    return t_tid_string.data();
}

/**
 * @brief 获取 TID 字符串的有效字符长度
 */
inline int tid_string_length() {
    if (__builtin_expect(static_cast<long>(t_cached_tid == 0), 0) != 0) {
        cache_tid();
    }
    return t_tid_string_length;
}

}  // namespace lean_web_server::base::current_thread
