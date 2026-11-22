/**
 * @file Buffer.h
 * @brief 动态扩容应用层读写缓冲区，支持 readv 栈缓冲辅助读取与零拷贝就地解析
 * @author woruo03
 * @date 2026-10-30
 * @version 1.0
 */

#pragma once

#include <vector>

namespace lean_web_server::net {

/**
 * @class Buffer
 * @brief 应用层非阻塞读写缓冲区
 *
 * 核心机制：
 * - 内部基于 std::vector<char> 连续存储；
 * - 维护 read_index_ 与 write_index_ 双游标，read_available() 为可读数据，write_available()
 * 为可写空间；
 * - read_fd() 结合 64KB 栈上 extrabuf 与 readv()
 * 散射读取，既防止单次内存分配过大，又避免了反复小系统调用的开销；
 * - 空间不足时自动先搬移紧凑已读空间，彻底耗尽时按需几何扩容。
 */
class Buffer {
public:
    static constexpr size_t k_initial_size = 1024;

    Buffer(size_t initial_size = k_initial_size) : buffer_(initial_size) {}

    /// 从文件描述符非阻塞读取数据（内部结合栈上 64KB 缓冲）
    int read_fd(int fd, int& saved_errno);

    /// 追加数据至缓冲区写游标处
    void append(const char* data, size_t size);

    /// 标记已消费的数据字节数（向前推进 read_index_）
    void mark_used(size_t size);

    /// 获取当前未消费的可读数据长度
    [[nodiscard]] size_t read_available() const { return write_index_ - read_index_; }

    /// 获取缓冲区末尾当前剩余的可写字节数
    [[nodiscard]] size_t write_available() const { return buffer_.size() - write_index_; }

    /// 获取可读数据起始地址
    [[nodiscard]] const char* read_ptr() const { return buffer_.data() + read_index_; }

    /// 获取当前可写位置地址
    [[nodiscard]] const char* write_ptr() const { return buffer_.data() + write_index_; }

private:
    std::vector<char> buffer_;
    size_t read_index_ = 0;
    size_t write_index_ = 0;

    void ensure_write_available(size_t size);
};

}  // namespace lean_web_server::net
