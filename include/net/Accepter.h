/**
 * @file Accepter.h
 * @brief 监听套接字独立封装组件，负责服务端口绑定、监听与新连接 accept 接收
 * @author woruo03
 * @date 2026-11-05
 * @version 1.0
 */

#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <functional>

#include "base/NonCopyable.h"
#include "net/Channel.h"

namespace lean_web_server::net {

class EventLoop;

/**
 * @class Accepter
 * @brief 服务端监听套接字 RAII 封装组件
 *
 * 核心机制：
 * - 构造时自动创建非阻塞 CLOEXEC 监听套接字，设置 SO_REUSEADDR/SO_REUSEPORT 并 bind/listen；
 * - 暴露 valid() 接口，支持调用方前置探测端口绑定状态；
 * - 析构与 stop() 严格遵循 RAII：注销 Channel 并关闭底层 listen_fd_；
 * - 新连接到达时，通过 accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC) 快速提取并通过回调分发。
 */
class Accepter : base::NonCopyable {
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Accepter(EventLoop* event_loop, uint16_t port);
    ~Accepter();

    /// 停止监听（解挂 Channel 并关闭 listen_fd_，进入拒绝新连接状态）
    void stop();

    /// 判断监听套接字是否已主动停止
    [[nodiscard]] bool is_stopped() const { return stopped_; }

    /// 判断底层套接字是否有效创建且绑定成功
    [[nodiscard]] bool valid() const { return listen_fd_ >= 0; }

    /// 开启套接字读事件监听
    void listen() {
        if (valid()) {
            accept_channel_.enable_read();
        }
    }

    /// 注册新连接就绪回调
    void set_new_connection_callback(NewConnectionCallback cb) {
        new_connection_callback_ = std::move(cb);
    }

private:
    int listen_fd_;
    Channel accept_channel_;
    NewConnectionCallback new_connection_callback_;
    bool stopped_{false};

    void handle_read();
};

}  // namespace lean_web_server::net
