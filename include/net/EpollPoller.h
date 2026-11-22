/**
 * @file EpollPoller.h
 * @brief Epoll I/O 多路复用轮询器封装，负责调用 epoll_wait 并填充就绪 Channel
 * @author woruo03
 * @date 2026-10-29
 * @version 1.0
 */

#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include <vector>

namespace lean_web_server::net {

class Channel;

/**
 * @class EpollPoller
 * @brief 基于 Linux epoll 的 I/O 多路复用器
 *
 * 核心机制：
 * - 构造时通过 epoll_create1(EPOLL_CLOEXEC) 创建独立 epoll 实例；
 * - 析构时安全关闭 epoll_fd_（RAII）；
 * - 动态自适应事件数组扩容机制；
 * - 内置对 EINTR 信号打断的处理与连续 1000 次致命系统调用故障自杀熔断守卫，防止异常忙轮询自旋。
 */
class EpollPoller {
public:
    EpollPoller();
    ~EpollPoller();

    /// 轮询就绪事件，将就绪通道填充至 active_channels
    void poll(std::vector<Channel*>& active_channels, int timeout = -1);

    /// 向 epoll 树添加（EPOLL_CTL_ADD）或修改（EPOLL_CTL_MOD）通道关注的事件
    void update_channel(Channel* channel) const;

    /// 从 epoll 树彻底注销（EPOLL_CTL_DEL）通道
    void remove_channel(Channel* channel) const;

private:
    int epoll_fd_{-1};
    std::vector<struct epoll_event> events_;
    /// 单 Reactor 连续 epoll_wait 失败次数计数（防自旋熔断守卫，线程局部隔离）
    int consecutive_errors_{0};
};

}  // namespace lean_web_server::net
