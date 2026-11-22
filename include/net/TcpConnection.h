/**
 * @file TcpConnection.h
 * @brief TCP 连接抽象类，管理连接四态状态机、双轨超时治理、非阻塞写缓冲与 HTTP Keep-Alive 响应
 * @author woruo03
 * @date 2026-11-04
 * @version 1.0
 */

#pragma once

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "http/HttpContext.h"
#include "net/Buffer.h"
#include "net/Channel.h"
#include "net/TimerQueue.h"

namespace lean_web_server::net {

class EventLoop;

/// 双轨连接超时配置
struct TimeoutConfig {
    int idle_timeout_sec = 30;     ///< L1 滚动空闲超时时间（秒，默认 30s，由散列时间轮治理）
    int max_lifetime_sec = 120;    ///< L2 绝对生存期上限（秒，默认 120s，由 TimerQueue 强杀）
    int timer_wheel_slots = 1024;  ///< 散列时间轮槽位数（默认 1024 槽，需为 2 的幂次）
};

/// 超时触发原因类型
enum class TimeoutKind : uint8_t { kIdle, kLifetime };

/**
 * @class TcpConnection
 * @brief TCP 客户端连接对象
 *
 * 核心设计：
 * 1. 四态生命周期状态机：严格防重入、防重复析构、防延迟拔树期复活；
 * 2. 细粒度非阻塞写缓冲：数据未一次性写完时监听 EPOLLOUT 让出重试，仅网络崩溃故障才拆除；
 * 3. 启用 TCP_NODELAY：禁用 Nagle 算法，消除小包 40ms 延迟抖动；
 * 4. 双轨超时治理：L1 时间轮散列槽位同秒短路续期 + L2 TimerQueue 绝对存活封顶；
 * 5. HTTP Keep-Alive 响应：自动注入 Keep-Alive 报头或优雅半关闭。
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using CloseCallBack = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    /// TCP 连接四态生命周期状态机
    enum class State : uint8_t {
        kConnecting,     ///< 1. 对象已构建，尚未挂载并调用 connect_established()
        kConnected,      ///< 2. 连接就绪，Channel 已注册 EPOLLIN 并挂载超时保活
        kDisconnecting,  ///< 3. 正在优雅排空输出缓冲（拒收新 send，等待已积压数据发送完毕）
        kDisconnected,   ///< 4. 彻底断开（不可逆终止状态，阻断一切重入，等待最终析构）
    };

    TcpConnection(EventLoop* event_loop, int fd, uint64_t conn_id, TimeoutConfig tc = {});
    ~TcpConnection();

    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] State get_state() const noexcept { return state(); }
    [[nodiscard]] bool connected() const noexcept { return state() == State::kConnected; }
    [[nodiscard]] bool is_connected() const noexcept { return connected(); }
    [[nodiscard]] const TimeoutConfig& timeout_config() const noexcept { return timeout_config_; }
    [[nodiscard]] const TimeoutConfig& get_timeout_config() const noexcept {
        return timeout_config();
    }

    /// 连接建立初始化：开启读监听、装载 L1/L2 定时器并设置 TCP_NODELAY
    void connect_established();

    /// 连接销毁收尾：切入 kDisconnected，卸载定时器并从 Channel 树解挂
    void connect_destroyed();

    /// 设置关闭回调
    void set_close_callback(CloseCallBack cb) { close_callback_ = std::move(cb); }

    /// 处理完成的完整 HTTP 请求并发送对应响应
    void on_http_request();

    [[nodiscard]] int get_fd() const { return fd_; }
    [[nodiscard]] uint64_t get_conn_id() const { return conn_id_; }
    [[nodiscard]] EventLoop* get_event_loop() const { return event_loop_; }

    /// 发送数据（线程安全，自动路由至所属 loop 线程）
    void send(const std::string& msg);

    /// 优雅半关闭/排空写缓冲（kConnected -> kDisconnecting）
    void shutdown();

    /// 强制关闭或停机排水关闭（跨线程投递至所属 io_loop 线程拆除）
    void force_close();

    /// 超时剔除统一入口（记录告警日志并驱动 handle_close）
    void timeout_close(TimeoutKind kind);

    /// 手工更新活动时间戳并向时间轮续命
    void update_activity();

    // ── 散列时间轮槽位代际跟踪 ──
    [[nodiscard]] int last_slot() const noexcept { return last_slot_; }
    void set_last_slot(int slot) noexcept { last_slot_ = slot; }

private:
    void set_state(State s) noexcept { state_.store(s, std::memory_order_release); }
    void send_in_loop(const std::string& msg);
    void shutdown_in_loop();
    void force_close_in_loop();
    void arm_timeouts();
    void disarm_timeouts();
    void refresh_idle_timeout();

    void handle_read();
    bool read_from_socket();
    void process_http_pipeline();
    void handle_write();
    void handle_close();
    void handle_error();

    EventLoop* event_loop_;
    int fd_;
    uint64_t conn_id_;
    std::unique_ptr<Channel> channel_;
    TimeoutConfig timeout_config_;
    std::atomic<State> state_{State::kConnecting};

    Buffer input_buffer_;
    Buffer output_buffer_;
    http::HttpContext http_context_;

    CloseCallBack close_callback_;
    TimerId lifetime_timer_id_;
    int last_slot_{-1};
};

}  // namespace lean_web_server::net
