/**
 * @file TcpConnection.cpp
 * @brief TCP 连接实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "net/TcpConnection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "base/Logger.h"
#include "net/EventLoop.h"
#include "net/TimerWheel.h"

namespace lean_web_server::net {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
TcpConnection::TcpConnection(EventLoop* event_loop, int fd, uint64_t conn_id, TimeoutConfig tc)
    : event_loop_(event_loop),
      fd_(fd),
      conn_id_(conn_id),
      channel_(std::make_unique<Channel>(event_loop, fd)),
      timeout_config_(tc) {
    channel_->set_read_callback([this]() { handle_read(); });
    channel_->set_write_callback([this]() { handle_write(); });
    channel_->set_error_callback([this]() { handle_error(); });
    channel_->set_close_callback([this]() { handle_close(); });
}

TcpConnection::~TcpConnection() {
    ::close(fd_);
}

void TcpConnection::connect_established() {
    if (state() != State::kConnecting) {
        return;
    }
    set_state(State::kConnected);
    channel_->enable_read();
    arm_timeouts();
}

void TcpConnection::connect_destroyed() {
    if (state() == State::kConnected || state() == State::kDisconnecting) {
        set_state(State::kDisconnected);
        if (channel_->state() == Channel::State::kAdded) {
            channel_->disable_all();
        }
        disarm_timeouts();
    } else if (state() == State::kConnecting) {
        set_state(State::kDisconnected);
    }
    channel_->remove();
}

void TcpConnection::arm_timeouts() {
    refresh_idle_timeout();
    auto guard = std::weak_ptr<TcpConnection>{shared_from_this()};
    uint64_t id = conn_id_;
    lifetime_timer_id_ =
        event_loop_->run_after(std::chrono::seconds(timeout_config_.max_lifetime_sec), [guard, id] {
            if (auto c = guard.lock()) {
                if (c->get_conn_id() == id) {
                    c->timeout_close(TimeoutKind::kLifetime);
                }
            }
        });
}

void TcpConnection::disarm_timeouts() {
    if (lifetime_timer_id_.valid()) {
        event_loop_->cancel(lifetime_timer_id_);
        lifetime_timer_id_ = TimerId{};
    }
    set_last_slot(-1);
}

void TcpConnection::refresh_idle_timeout() {
    if (state() != State::kConnected) {
        return;
    }
    if (auto* wheel = event_loop_->timer_wheel()) {
        wheel->refresh(shared_from_this());
    }
}

void TcpConnection::update_activity() {
    refresh_idle_timeout();
}

bool TcpConnection::read_from_socket() {
    int saved_errno = 0;
    while (true) {
        int n = input_buffer_.read_fd(fd_, saved_errno);
        if (n == 0) {
            LOG_DEBUG << "TcpConnection::handle_read EOF fd=" << fd_ << " conn_id=" << conn_id_;
            handle_close();
            return false;
        }
        if (n < 0) {
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                break;
            }
            if (saved_errno == EINTR) {
                continue;
            }
            LOG_ERROR << "TcpConnection::handle_read error fd=" << fd_ << " conn_id=" << conn_id_
                      << " errno=" << saved_errno;
            handle_error();
            return false;
        }
        refresh_idle_timeout();
    }
    return true;
}

void TcpConnection::process_http_pipeline() {
    while (input_buffer_.read_available() > 0) {
        if (state() != State::kConnected) {
            break;
        }
        if (!http_context_.parse_request(input_buffer_)) {
            send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
            shutdown();
            return;
        }

        if (http_context_.parse_state() == http::HttpContext::HttpRequestParseState::kDone) {
            on_http_request();
            input_buffer_.mark_used(http_context_.checked_bytes());
            http_context_.reset();
        } else {
            break;
        }
    }
}

void TcpConnection::handle_read() {
    if (state() != State::kConnected && state() != State::kDisconnecting) {
        return;
    }

    if (!read_from_socket()) {
        return;
    }

    process_http_pipeline();
}

void TcpConnection::on_http_request() {
    bool keep_alive = http_context_.keep_alive(input_buffer_.read_ptr());

    std::string body =
        "<html><head><title>LeanWebServer</title></head>"
        "<body><h1>Hello, World!</h1></body></html>";
    std::string response;
    response += "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/html\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (keep_alive) {
        response += "Connection: keep-alive\r\n";
        response +=
            "Keep-Alive: timeout=" + std::to_string(timeout_config_.idle_timeout_sec) + "\r\n";
    } else {
        response += "Connection: close\r\n";
    }
    response += "\r\n";
    response += body;

    send(response);
    if (!keep_alive) {
        shutdown();
    }
}

void TcpConnection::handle_write() {
    if (state() != State::kConnected && state() != State::kDisconnecting) {
        return;
    }
    if (!channel_->is_writing()) {
        return;
    }

    while (output_buffer_.read_available() > 0) {
        ssize_t write_byte =
            ::write(fd_, output_buffer_.read_ptr(), output_buffer_.read_available());
        if (write_byte < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            LOG_ERROR << "TcpConnection::handle_write error fd=" << fd_ << " conn_id=" << conn_id_
                      << " errno=" << errno;
            handle_error();
            return;
        }
        output_buffer_.mark_used(static_cast<size_t>(write_byte));
        refresh_idle_timeout();
    }

    if (output_buffer_.read_available() == 0) {
        channel_->disable_write();
        if (state() == State::kDisconnecting) {
            shutdown_in_loop();
        }
    }
}

void TcpConnection::send(const std::string& msg) {
    if (state() != State::kConnected) {
        return;
    }
    if (event_loop_->is_in_loop_thread()) {
        send_in_loop(msg);
    } else {
        auto self = shared_from_this();
        event_loop_->run_in_loop([self, msg]() { self->send_in_loop(msg); });
    }
}

void TcpConnection::send_in_loop(const std::string& msg) {
    if (state() != State::kConnected) {
        return;
    }

    ssize_t nwrote = 0;
    size_t remaining = msg.size();
    bool fault_error = false;

    if (!channel_->is_writing() && output_buffer_.read_available() == 0) {
        nwrote = ::write(fd_, msg.data(), msg.size());
        if (nwrote >= 0) {
            remaining = msg.size() - static_cast<size_t>(nwrote);
            refresh_idle_timeout();
        } else {
            nwrote = 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (errno == EPIPE || errno == ECONNRESET) {
                    fault_error = true;
                }
                LOG_ERROR << "TcpConnection::send_in_loop error fd=" << fd_
                          << " conn_id=" << conn_id_ << " errno=" << errno;
            }
        }
    }

    if (fault_error) {
        handle_error();
        return;
    }

    if (remaining > 0) {
        output_buffer_.append(msg.data() + nwrote, remaining);
        if (!channel_->is_writing()) {
            channel_->enable_write();
        }
    }
}

void TcpConnection::shutdown() {
    if (event_loop_->is_in_loop_thread()) {
        shutdown_in_loop();
    } else {
        auto self = shared_from_this();
        event_loop_->run_in_loop([self] { self->shutdown_in_loop(); });
    }
}

void TcpConnection::shutdown_in_loop() {
    if (state() == State::kDisconnected) {
        return;
    }
    if (state() == State::kConnected) {
        set_state(State::kDisconnecting);
    }
    if (output_buffer_.read_available() == 0) {
        ::shutdown(fd_, SHUT_WR);
    }
}

void TcpConnection::force_close() {
    if (event_loop_->is_in_loop_thread()) {
        force_close_in_loop();
    } else {
        auto self = shared_from_this();
        event_loop_->run_in_loop([self] { self->force_close_in_loop(); });
    }
}

void TcpConnection::force_close_in_loop() {
    if (state() == State::kDisconnected) {
        return;
    }
    if (output_buffer_.read_available() == 0) {
        handle_close();
    } else {
        set_state(State::kDisconnecting);
        auto guard = std::weak_ptr<TcpConnection>{shared_from_this()};
        uint64_t id = conn_id_;
        (void)event_loop_->run_after(std::chrono::seconds(5), [guard, id] {
            if (auto c = guard.lock()) {
                // 预留池化复用防护
                if (c->get_conn_id() == id) {
                    c->handle_close();
                }
            }
        });
    }
}

void TcpConnection::timeout_close(TimeoutKind kind) {
    LOG_WARN << "Timeout closed conn_id=" << conn_id_ << " fd=" << fd_
             << " kind=" << (kind == TimeoutKind::kIdle ? "idle" : "lifetime");
    handle_close();
}

void TcpConnection::handle_close() {
    if (state() == State::kDisconnected) {
        return;
    }
    set_state(State::kDisconnected);

    if (channel_->state() == Channel::State::kAdded) {
        channel_->disable_all();
    }
    disarm_timeouts();

    LOG_INFO << "TcpConnection::handle_close fd=" << fd_ << " conn_id=" << conn_id_;
    auto guard_this = shared_from_this();
    if (close_callback_) {
        close_callback_(guard_this);
    }
}

void TcpConnection::handle_error() {
    handle_close();
}

}  // namespace lean_web_server::net
