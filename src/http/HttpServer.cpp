/**
 * @file HttpServer.cpp
 * @brief HttpServer 实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "http/HttpServer.h"

#include <sys/signalfd.h>
#include <unistd.h>

#include <csignal>

#include "base/Logger.h"
#include "net/EventLoop.h"

namespace lean_web_server::http {

HttpServer::HttpServer(net::EventLoop* event_loop, const ServerConfig& config)
    : main_loop_(event_loop),
      accepter_(std::make_unique<net::Accepter>(event_loop, config.port)),
      event_loop_thread_pool_(std::make_unique<net::EventLoopThreadPool>(
          event_loop, config.io_threads, config.timeout.timer_wheel_slots)),
      config_(config) {
    accepter_->set_new_connection_callback([this](int sockfd) { on_new_connection(sockfd); });

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    signal_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ >= 0) {
        signal_channel_ = std::make_unique<net::Channel>(main_loop_, signal_fd_);
        signal_channel_->set_read_callback([this] { handle_signal(); });
        signal_channel_->enable_read();
    } else {
        LOG_FATAL << "Failed to create signalfd: errno=" << errno;
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
HttpServer::HttpServer(net::EventLoop* event_loop, uint16_t port, int num_threads,
                       net::TimeoutConfig timeout_config)
    : HttpServer(event_loop, [&] {
          ServerConfig cfg;
          cfg.port = port;
          cfg.io_threads = num_threads;
          cfg.timeout = timeout_config;
          return cfg;
      }()) {}

HttpServer::~HttpServer() {
    if (signal_channel_) {
        signal_channel_->disable_all();
        signal_channel_->remove();
    }
    if (signal_fd_ >= 0) {
        ::close(signal_fd_);
        signal_fd_ = -1;
    }
}

void HttpServer::start() {
    event_loop_thread_pool_->start();
    main_loop_->run_in_loop([this] { accepter_->listen(); });
    LOG_INFO << "HttpServer started with configuration:\n" << config_.to_string();
}

void HttpServer::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    LOG_INFO << "HttpServer::stop() initiated";
    accepter_->stop();

    auto targets = event_loop_thread_pool_->loops();
    if (targets.empty()) {
        targets.push_back(main_loop_);
    }

    auto remaining = std::make_shared<std::atomic<size_t>>(targets.size());
    for (auto* loop : targets) {
        loop->run_in_loop([loop, remaining, main = main_loop_] {
            loop->initiate_graceful_shutdown([remaining, main] {
                if (remaining->fetch_sub(1) == 1) {
                    LOG_INFO << "All Sub-Reactors drained, notifying main_loop to quit";
                    main->quit();
                }
            });
        });
    }
}

void HttpServer::on_new_connection(int sockfd) {
    if (stopping_.load()) {
        LOG_WARN << "HttpServer is stopping, rejecting connection fd=" << sockfd;
        ::close(sockfd);
        return;
    }

    net::EventLoop* io_loop = event_loop_thread_pool_->get_next_loop();
    uint64_t id = ++conn_id_;
    io_loop->run_in_loop(
        [io_loop, sockfd, id, tc = config_.timeout] { io_loop->new_connection(sockfd, id, tc); });
}

void HttpServer::handle_signal() {
    struct signalfd_siginfo siginfo{};
    ssize_t n = ::read(signal_fd_, &siginfo, sizeof(siginfo));
    if (n != sizeof(siginfo)) {
        LOG_ERROR << "HttpServer::handle_signal read failed: errno=" << errno;
        return;
    }

    int sig = static_cast<int>(siginfo.ssi_signo);
    LOG_INFO << "HttpServer received signal " << sig;

    if (stopping_.load()) {
        LOG_WARN << "Second signal received, force exiting immediately!";
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, sig);
        ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }

    stop();
}

}  // namespace lean_web_server::http
