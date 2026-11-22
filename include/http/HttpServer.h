/**
 * @file HttpServer.h
 * @brief HTTP 服务器主体类，实现主从 Reactor 调度、Share-Nothing 纯整数连接分发与 signalfd 优雅停机
 * @author woruo03
 * @date 2026-11-06
 * @version 1.0
 */

#pragma once

#include <cstdint>
#include <memory>

#include "base/NonCopyable.h"
#include "http/ServerConfig.h"
#include "net/Accepter.h"
#include "net/EventLoopThreadPool.h"
#include "net/TcpConnection.h"

namespace lean_web_server::http {

/**
 * @class HttpServer
 * @brief HTTP 服务器顶层协调器
 *
 * 核心机制：
 * 1. Main Reactor 架构：主事件循环仅专职 Accepter 的新连接监听；
 * 2. Share-Nothing 无锁去中心化分发：接受新连接时，主线程生成全局自增 ID，
 *    仅将 (sockfd, conn_id, config) 纯整型值跨线程投递至目标 IO
 * 线程，主线程不维护任何全局连接哈希表；
 * 3. 原生 signalfd 优雅停机：主事件循环挂接 signalfd 捕获 SIGINT/SIGTERM，避免异步信号处理器死锁；
 * 4. 5 阶段分布式排水：协调所有 Sub-Reactor 并行排空写缓冲并安全注销退出；
 * 5. 二次信号守卫：排空期间收到第 2 个信号立即转入系统默认处理强杀退出。
 */
class HttpServer : base::NonCopyable {
public:
    /// 基于统一配置 ServerConfig 构造 HttpServer
    explicit HttpServer(net::EventLoop* event_loop, const ServerConfig& config = {});

    /// 兼容老接口构造函数（转调主构造函数）
    HttpServer(net::EventLoop* event_loop, uint16_t port, int num_threads,
               net::TimeoutConfig timeout_config = {});
    ~HttpServer();

    /// 启动服务器（开启监听端口与工作线程池）
    void start();

    /// 优雅停止服务器（关闭监听并向所有 Sub-Reactor 广播排空流程）
    void stop();

    /// 检查底层监听套接字是否有效创建并绑定成功
    [[nodiscard]] bool valid() const { return accepter_->valid(); }

    /// 获取服务器当前生效的统一配置对象
    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }

private:
    net::EventLoop* main_loop_;
    uint64_t conn_id_{0};
    std::unique_ptr<net::Accepter> accepter_;
    std::unique_ptr<net::EventLoopThreadPool> event_loop_thread_pool_;
    ServerConfig config_;
    std::atomic<bool> stopping_{false};

    int signal_fd_{-1};
    std::unique_ptr<net::Channel> signal_channel_;

    /// 新连接就绪回调（整型派发至 IO 线程池）
    void on_new_connection(int sockfd);

    /// signalfd 可读事件回调（处理优雅停机与二次强杀）
    void handle_signal();
};

}  // namespace lean_web_server::http
