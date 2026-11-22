/**
 * @file Channel.h
 * @brief 事件通道类，封装单个文件描述符（fd）关注的 I/O 事件、就绪事件及其对应回调
 * @author woruo03
 * @date 2026-10-29
 * @version 1.0
 */

#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <functional>

namespace lean_web_server::net {

class EventLoop;

/**
 * @class Channel
 * @brief 单描述符事件通道
 *
 * 每个 Channel 严格属于唯一一个 EventLoop，只负责一个文件描述符（fd）的事件分发。
 * 解耦感兴趣的事件（events_）与底层 poller 返回的实际就绪事件（revents_）。
 */
class Channel {
public:
    using EventCallBack = std::function<void()>;

    /// 通道在 EpollPoller 内部的注册状态
    enum class State : uint8_t {
        kNew,      ///< 新创建，尚未注册到 epoll
        kAdded,    ///< 已添加至 epoll 监听树
        kDeleted,  ///< 已从 epoll 监听树注销
    };

    Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}
    ~Channel() = default;

    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] uint32_t events() const { return events_; }
    [[nodiscard]] uint32_t revents() const { return revents_; }
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool is_writing() const { return (events_ & EPOLLOUT) != 0; }
    [[nodiscard]] bool is_reading() const { return (events_ & (EPOLLIN | EPOLLPRI)) != 0; }

    void set_revents(uint32_t revents) { revents_ = revents; }
    void set_state(State state) { state_ = state; }

    /// 启用读事件监听（支持边缘触发 EPOLLET + 非阻塞）
    void enable_read() {
        if (!is_reading()) {
            events_ |= (EPOLLIN | EPOLLPRI | EPOLLET);
            update();
        }
    }

    /// 停用读事件监听
    void disable_read() {
        if (is_reading()) {
            events_ &= ~(EPOLLIN | EPOLLPRI | EPOLLET);
            update();
        }
    }

    /// 启用写事件监听（EPOLLOUT）
    void enable_write() {
        if (!is_writing()) {
            events_ |= EPOLLOUT;
            update();
        }
    }

    /// 停用写事件监听
    void disable_write() {
        if (is_writing()) {
            events_ &= ~EPOLLOUT;
            update();
        }
    }

    /// 停用所有关注的事件监听
    void disable_all() {
        if (events_ != 0) {
            events_ = 0;
            update();
        }
    }

    /// 从所属 EventLoop 中彻底移除此 Channel
    void remove();

    /// 处理就绪事件并根据 revents_ 派发对应回调
    void handle_event();

    void set_read_callback(EventCallBack cb) { read_callback_ = std::move(cb); }
    void set_write_callback(EventCallBack cb) { write_callback_ = std::move(cb); }
    void set_close_callback(EventCallBack cb) { close_callback_ = std::move(cb); }
    void set_error_callback(EventCallBack cb) { error_callback_ = std::move(cb); }

private:
    void update();

    EventLoop* loop_;
    int fd_;
    uint32_t events_{0};
    uint32_t revents_{0};
    State state_{State::kNew};

    EventCallBack read_callback_;
    EventCallBack write_callback_;
    EventCallBack close_callback_;
    EventCallBack error_callback_;
};

}  // namespace lean_web_server::net
