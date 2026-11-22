/**
 * @file Accepter.cpp
 * @brief 监听套接字组件实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "net/Accepter.h"

#include <arpa/inet.h>

#include <cerrno>

#include "base/Logger.h"

namespace lean_web_server::net {

Accepter::Accepter(EventLoop* event_loop, uint16_t port)
    : listen_fd_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
      accept_channel_(event_loop, listen_fd_) {
    if (listen_fd_ < 0) {
        LOG_FATAL << "Accepter::socket failed: errno=" << errno;
        return;
    }

    int opt_val = 1;
    int ret = ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));
    if (ret == -1) {
        LOG_FATAL << "Accepter::setsockopt SO_REUSEADDR failed: errno=" << errno
                  << " port=" << port;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    ret = ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt_val, sizeof(opt_val));
    if (ret == -1) {
        LOG_WARN << "Accepter::setsockopt SO_REUSEPORT failed: errno=" << errno << " port=" << port;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret == -1) {
        LOG_FATAL << "Accepter::bind failed: errno=" << errno << " port=" << port;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    ret = ::listen(listen_fd_, SOMAXCONN);
    if (ret == -1) {
        LOG_FATAL << "Accepter::listen failed: errno=" << errno << " port=" << port;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    accept_channel_.set_read_callback([this] { handle_read(); });
}

Accepter::~Accepter() {
    stop();
}

void Accepter::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    accept_channel_.disable_all();
    accept_channel_.remove();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void Accepter::handle_read() {
    if (stopped_) {
        return;
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr),
                                  &client_addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd <= -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECONNABORTED) {
                continue;
            }
            LOG_WARN << "Accepter::handle_read accept4 error errno=" << errno;
            break;
        }

        if (new_connection_callback_) {
            new_connection_callback_(client_fd);
        } else {
            ::close(client_fd);
        }
    }
}

}  // namespace lean_web_server::net
