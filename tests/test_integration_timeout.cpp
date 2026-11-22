/**
 * @file test_integration_timeout.cpp
 * @brief 超时集成、Keep-Alive、Sub-Reactor 自治与状态机测试 (K1-K8, S2-S3)
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "http/HttpServer.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"

using namespace lean_web_server::net;
using namespace lean_web_server::http;
using namespace std::chrono_literals;

namespace {

struct SignalIgnorer {
    SignalIgnorer() { std::signal(SIGPIPE, SIG_IGN); }
} s_signal_ignorer;

int connect_to_server(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

// ── K1: 空闲超时检测与关闭 ──
TEST(IntegrationTimeoutTest, K1_IdleTimeoutClose) {
    EventLoop main_loop;
    TimeoutConfig tc;
    tc.idle_timeout_sec = 1;  // 1 秒超时以加速单测
    tc.max_lifetime_sec = 10;

    HttpServer server(&main_loop, 19001, 1, tc);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    int client_fd = connect_to_server(19001);
    ASSERT_GT(client_fd, 0);

    auto start = std::chrono::steady_clock::now();
    std::array<char, 128> buf{};
    ssize_t n = ::read(client_fd, buf.data(), buf.size());
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    EXPECT_EQ(n, 0);
    EXPECT_GE(elapsed, 900);
    EXPECT_LE(elapsed, 2500);

    ::close(client_fd);
    main_loop.run_in_loop([&server, &main_loop] {
        server.stop();
        main_loop.quit();
    });
    server_thread.join();
}

// ── K2: 活动刷新存活与生命周期上限 (L2) ──
TEST(IntegrationTimeoutTest, K2_ActivityRefreshAndLifetimeCap) {
    EventLoop main_loop;
    TimeoutConfig tc;
    tc.idle_timeout_sec = 2;
    tc.max_lifetime_sec = 3;  // 3s 绝对生命周期上限

    HttpServer server(&main_loop, 19002, 1, tc);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    int client_fd = connect_to_server(19002);
    ASSERT_GT(client_fd, 0);

    auto start = std::chrono::steady_clock::now();
    std::thread client_worker([client_fd] {
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(500ms);
            std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
            ssize_t wn = ::write(client_fd, req.data(), req.size());
            if (wn <= 0)
                break;
            std::array<char, 256> buf{};
            ssize_t rn = ::read(client_fd, buf.data(), buf.size());
            if (rn <= 0)
                break;
        }
    });

    std::array<char, 128> buf{};
    ssize_t n = 0;
    while ((n = ::read(client_fd, buf.data(), buf.size())) > 0) {
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    client_worker.join();
    EXPECT_EQ(n, 0);
    EXPECT_GE(elapsed, 2500);
    EXPECT_LE(elapsed, 4500);

    ::close(client_fd);
    main_loop.run_in_loop([&server, &main_loop] {
        server.stop();
        main_loop.quit();
    });
    server_thread.join();
}

// ── K3: fd 复用防误杀 ──
TEST(IntegrationTimeoutTest, K3_FdReuseNoFalseKill) {
    EventLoop main_loop;
    TimeoutConfig tc;
    tc.idle_timeout_sec = 5;  // 5s 超时，测试 B 不被 1s 内误杀
    tc.max_lifetime_sec = 20;

    HttpServer server(&main_loop, 19003, 1, tc);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    // 1. 客户端 A 建连后立刻主动关闭，制造被释放的 fd
    int client_a = connect_to_server(19003);
    ASSERT_GT(client_a, 0);
    ::close(client_a);

    std::this_thread::sleep_for(50ms);

    // 2. 客户端 B 建连，内核大概率复用相同的 fd
    int client_b = connect_to_server(19003);
    ASSERT_GT(client_b, 0);

    // 3. 持续 1.5s 周期发送数据刷新 B，断言 B 存活且通信正常
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(400ms);
        std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        ssize_t wn = ::write(client_b, req.data(), req.size());
        EXPECT_GT(wn, 0);
        std::array<char, 256> buf{};
        ssize_t rn = ::read(client_b, buf.data(), buf.size());
        EXPECT_GT(rn, 0);
    }

    ::close(client_b);
    main_loop.run_in_loop([&server, &main_loop] {
        server.stop();
        main_loop.quit();
    });
    server_thread.join();
}

// ── K4: 关闭连接后无残留定时器 ──
TEST(IntegrationTimeoutTest, K4_NoResidualTimersAfterClose) {
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, -1, 101);
    conn->connect_established();

    // 刚建立时已装载 L1 与 L2
    conn->force_close();
    conn->connect_destroyed();

    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
    EXPECT_EQ(conn->last_slot(), -1);
}

// ── K5: Keep-Alive 响应头断言 ──
TEST(IntegrationTimeoutTest, K5_KeepAliveHeaderMatch) {
    EventLoop main_loop;
    TimeoutConfig tc;
    tc.idle_timeout_sec = 25;
    tc.max_lifetime_sec = 120;

    HttpServer server(&main_loop, 19005, 1, tc);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    int client_fd = connect_to_server(19005);
    ASSERT_GT(client_fd, 0);

    // 1. HTTP/1.1 请求断言包含 Keep-Alive: timeout=25
    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    ssize_t wn = ::write(client_fd, req.data(), req.size());
    ASSERT_GT(wn, 0);

    std::array<char, 1024> buf{};
    ssize_t n = ::read(client_fd, buf.data(), buf.size() - 1);
    ASSERT_GT(n, 0);
    std::string resp(buf.data(), static_cast<size_t>(n));

    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("Connection: keep-alive"), std::string::npos);
    EXPECT_NE(resp.find("Keep-Alive: timeout=25"), std::string::npos);

    // 2. HTTP/1.0 显式 close 请求断言响应 Connection: close
    std::string req10 = "GET / HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    wn = ::write(client_fd, req10.data(), req10.size());
    ASSERT_GT(wn, 0);

    buf.fill(0);
    n = ::read(client_fd, buf.data(), buf.size() - 1);
    ASSERT_GT(n, 0);
    std::string resp10(buf.data(), static_cast<size_t>(n));

    EXPECT_NE(resp10.find("Connection: close"), std::string::npos);

    ::close(client_fd);
    main_loop.run_in_loop([&server, &main_loop] {
        server.stop();
        main_loop.quit();
    });
    server_thread.join();
}

// ── K6: 慢客户端细粒度写缓冲不误杀 ──
TEST(IntegrationTimeoutTest, K6_SlowClientNonFatalWrite) {
    EventLoop main_loop;
    HttpServer server(&main_loop, 19006, 1);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    int client_fd = connect_to_server(19006);
    ASSERT_GT(client_fd, 0);

    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t wn = ::write(client_fd, req.data(), req.size());
    ASSERT_GT(wn, 0);

    std::this_thread::sleep_for(50ms);
    std::string full_resp;
    std::array<char, 32> buf{};
    ssize_t n = 0;
    while ((n = ::read(client_fd, buf.data(), buf.size())) > 0) {
        full_resp.append(buf.data(), static_cast<size_t>(n));
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_NE(full_resp.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(full_resp.find("<h1>Hello, World!</h1>"), std::string::npos);

    ::close(client_fd);
    main_loop.run_in_loop([&server, &main_loop] {
        server.stop();
        main_loop.quit();
    });
    server_thread.join();
}

// ── K7: Sub-Reactor 自治连接表断言 ──
TEST(IntegrationTimeoutTest, K7_SubReactorConnectionAutonomy) {
    EventLoop main_loop;
    HttpServer server(&main_loop, 19007, 1);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    int client_fd = connect_to_server(19007);
    ASSERT_GT(client_fd, 0);

    std::this_thread::sleep_for(50ms);

    // 断言连接已建立且能正常通讯
    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssize_t wn = ::write(client_fd, req.data(), req.size());
    EXPECT_GT(wn, 0);

    std::array<char, 256> buf{};
    ssize_t rn = ::read(client_fd, buf.data(), buf.size());
    EXPECT_GT(rn, 0);

    ::close(client_fd);
    main_loop.run_in_loop([&server, &main_loop] {
        server.stop();
        main_loop.quit();
    });
    server_thread.join();
}

// ── K8: TcpConnection 四态状态机防重入 ──
TEST(IntegrationTimeoutTest, K8_StateMachineGuard) {
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, -1, 100);

    EXPECT_EQ(conn->state(), TcpConnection::State::kConnecting);
    EXPECT_FALSE(conn->connected());

    conn->connect_established();
    EXPECT_EQ(conn->state(), TcpConnection::State::kConnected);
    EXPECT_TRUE(conn->connected());

    conn->shutdown();
    (void)loop.run_after(10ms, [&loop] { loop.quit(); });
    loop.loop();

    conn->connect_destroyed();
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
    EXPECT_FALSE(conn->connected());
}

// ── S3: 空载优雅停机耗时 <= 2s ──
TEST(IntegrationTimeoutTest, S3_EmptyShutdownWithinLimit) {
    EventLoop main_loop;
    HttpServer server(&main_loop, 19008, 2);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });
    std::this_thread::sleep_for(30ms);

    auto start = std::chrono::steady_clock::now();
    main_loop.run_in_loop([&server] { server.stop(); });

    server_thread.join();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();

    EXPECT_LE(elapsed_ms, 2000);
}

// ── S1: 端到端五项优雅停机定义 ──
TEST(IntegrationTimeoutTest, S1_GracefulShutdownEndToEnd) {
    EventLoop main_loop;
    HttpServer server(&main_loop, 19009, 2);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    int client1 = connect_to_server(19009);
    ASSERT_GT(client1, 0);
    int client2 = connect_to_server(19009);
    ASSERT_GT(client2, 0);

    // client1 发送正常请求
    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    ssize_t wn = ::write(client1, req.data(), req.size());
    ASSERT_GT(wn, 0);

    std::array<char, 1024> buf{};
    ssize_t rn = ::read(client1, buf.data(), buf.size() - 1);
    ASSERT_GT(rn, 0);
    EXPECT_NE(std::string(buf.data()).find("200 OK"), std::string::npos);

    // 触发停机
    main_loop.run_in_loop([&server] { server.stop(); });

    // client2 应当收到 EOF (0)，因为服务端开始排空主动关闭
    std::array<char, 128> buf2{};
    ssize_t n2 = ::read(client2, buf2.data(), buf2.size());
    EXPECT_EQ(n2, 0);

    ::close(client1);
    ::close(client2);

    server_thread.join();
}

// ── S2: 信号转接单元（signalfd 在 100ms 内唤醒并处理） ──
TEST(IntegrationTimeoutTest, S2_SignalFdDelivery) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    int sfd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    ASSERT_GT(sfd, 0);

    EventLoop loop;
    Channel sig_channel(&loop, sfd);

    std::atomic<bool> handled{false};
    sig_channel.set_read_callback([&] {
        struct signalfd_siginfo sinfo{};
        ssize_t n = ::read(sfd, &sinfo, sizeof(sinfo));
        if (n == sizeof(sinfo) && sinfo.ssi_signo == SIGUSR1) {
            handled = true;
        }
        loop.quit();
    });
    sig_channel.enable_read();

    auto start = std::chrono::steady_clock::now();
    pthread_t target_thread = ::pthread_self();
    std::thread sender([target_thread] {
        std::this_thread::sleep_for(10ms);
        ::pthread_kill(target_thread, SIGUSR1);
    });

    (void)loop.run_after(1s, [&loop] { loop.quit(); });
    loop.loop();
    sender.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    EXPECT_TRUE(handled);
    EXPECT_LE(elapsed, 200);

    sig_channel.disable_all();
    sig_channel.remove();
    ::close(sfd);
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
}

// ── S4: 排水宽限：慢客户端收齐已发响应 ──
TEST(IntegrationTimeoutTest, S4_DrainGracePeriodForSlowClient) {
    EventLoop main_loop;
    HttpServer server(&main_loop, 19010, 1);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });
    std::this_thread::sleep_for(20ms);

    int client = connect_to_server(19010);
    ASSERT_GT(client, 0);

    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssize_t wn = ::write(client, req.data(), req.size());
    ASSERT_GT(wn, 0);
    std::this_thread::sleep_for(10ms);

    // 立即发起停止
    main_loop.run_in_loop([&server] { server.stop(); });

    // 客户端依然能收齐刚才已响应的完整数据
    std::array<char, 2048> buf{};
    size_t total = 0;
    while (true) {
        ssize_t n = ::read(client, buf.data() + total, buf.size() - total - 1);
        if (n <= 0) {
            break;
        }
        total += static_cast<size_t>(n);
    }
    ::close(client);

    std::string resp(buf.data(), total);
    EXPECT_NE(resp.find("200 OK"), std::string::npos);
    EXPECT_NE(resp.find("Hello, World!"), std::string::npos);

    server_thread.join();
}

// ── K5 (HTTP/1.0): 缺省 Close 响应头与短连接关闭 ──
TEST(IntegrationTimeoutTest, K5_Http10CloseHeaderAndDisconnect) {
    EventLoop main_loop;
    HttpServer server(&main_loop, 19011, 1);
    server.start();

    std::thread server_thread([&main_loop] { main_loop.loop(); });

    int client = connect_to_server(19011);
    ASSERT_GT(client, 0);

    std::string req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    ssize_t wn = ::write(client, req.data(), req.size());
    ASSERT_GT(wn, 0);

    std::array<char, 2048> buf{};
    ssize_t n = ::read(client, buf.data(), buf.size() - 1);
    ASSERT_GT(n, 0);
    std::string resp(buf.data(), static_cast<size_t>(n));

    EXPECT_NE(resp.find("Connection: close"), std::string::npos);
    EXPECT_EQ(resp.find("Connection: keep-alive"), std::string::npos);

    // HTTP/1.0 缺省无 keep-alive，服务端发完即主动关闭，客户端随后读到 0 (EOF)
    std::array<char, 64> dump{};
    ssize_t eof_n = ::read(client, dump.data(), dump.size());
    EXPECT_EQ(eof_n, 0);

    ::close(client);
    main_loop.run_in_loop([&server, &main_loop] {
        server.stop();
        main_loop.quit();
    });
    server_thread.join();
}

// ── K8: 四态多次调用防重入与防析构异常 ──
TEST(IntegrationTimeoutTest, K8_DoubleCloseSafe) {
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, -1, 202);

    conn->connect_established();
    EXPECT_EQ(conn->state(), TcpConnection::State::kConnected);

    // 多次 force_close 幂等不崩溃
    conn->force_close();
    conn->force_close();

    // 多次 connect_destroyed 幂等不崩溃
    conn->connect_destroyed();
    conn->connect_destroyed();

    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
}
