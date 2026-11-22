/**
 * @file main.cpp
 * @brief LeanWebServer 主程序入口与基于 ServerConfig 的 14 步标准生命周期启动/优雅停机编排
 * @author woruo03
 * @date 2026-11-19
 * @version 1.0
 */

#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <iostream>

#include "base/Logger.h"
#include "http/HttpServer.h"
#include "http/ServerConfig.h"
#include "net/EventLoop.h"
#include "version.hpp"

namespace {
// 进程退出码规范
constexpr int k_exit_bind = 10;       ///< 端口绑定/监听失败
constexpr int k_exit_log_init = 11;   ///< 日志目录创建失败
constexpr int k_exit_loop_init = 12;  ///< 信号屏蔽失败
}  // namespace

int main(int argc, char* argv[]) {
    // 步骤 1: 解析统一全局配置 ServerConfig（基于 POSIX getopt_long
    // 标准，严格选项驱动并拒绝位置参数）
    lean_web_server::ServerConfig config;
    bool help_requested = false;
    std::string err_msg;
    if (!config.parse_args(argc, argv, &help_requested, &err_msg)) {
        if (help_requested) {
            lean_web_server::ServerConfig::print_usage(argv[0]);
            return 0;
        }
        std::cerr << "Command line error: " << err_msg << "\n\n";
        lean_web_server::ServerConfig::print_usage(argv[0]);
        return 1;
    }

    // 步骤 2: 严格校验配置项合法性（端口、线程数、超时矩阵等边界）
    if (!config.is_valid(&err_msg)) {
        std::cerr << "Configuration error: " << err_msg << "\n\n";
        lean_web_server::ServerConfig::print_usage(argv[0]);
        return 1;
    }

    // 步骤 3: 忽略 SIGPIPE，防止客户端异常断开时写套接字触发内核信号崩溃退出
    std::signal(SIGPIPE, SIG_IGN);

    // 步骤 4: 尽早调用 pthread_sigmask 屏蔽 SIGINT/SIGTERM，交给 Linux 原生 signalfd 事件化接管；
    // 必须在一切派生线程（包括 Logger 后端写线程与
    // EventLoopThreadPool）创建前完成，以保证所有子线程继承屏蔽掩码
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        std::cerr << "Failed to set pthread_sigmask: errno=" << errno << '\n';
        return k_exit_loop_init;
    }

    // 步骤 5: 检查并创建日志输出目录（失败输出至 stderr 并以 k_exit_log_init 退出）
    if (::mkdir(config.log.log_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "Failed to create log directory " << config.log.log_dir << ", errno=" << errno
                  << '\n';
        return k_exit_log_init;
    }

    // 步骤 6: 初始化异步日志单例（自此所有组件均可通过 LOG_* 与 LOG_FMT_* 进行安全异步落盘）
    lean_web_server::base::Logger::get_instance().init(config.log);
    LOG_INFO << "LeanWebServer version " << LEAN_SERVER_VERSION << " starting up";

    // 步骤 7: 创建 Main Reactor 主事件循环（专职监听并接管 signalfd）
    lean_web_server::net::EventLoop main_loop(config.timeout.timer_wheel_slots);

    // 步骤 8: 注入统一配置构造 HttpServer 主体
    lean_web_server::http::HttpServer server(&main_loop, config);

    // 步骤 9: 严谨校验 Accepter 监听套接字有效性，防止端口冲突时异常静默运行
    if (!server.valid()) {
        LOG_FATAL << "Server listen socket is invalid, port=" << config.port;
        lean_web_server::base::Logger::get_instance().shutdown();
        return k_exit_bind;
    }

    // 步骤 10: 启动服务器（开启监听套接字、启动 Sub-Reactor 线程池与挂接 signalfd 通道）
    server.start();

    // 步骤 11: 主事件循环阻塞运行，等待所有 Sub-Reactor 排空完成后被信号唤醒退出
    main_loop.loop();

    // 步骤 12: 停机编排收尾：安全注销监听器、等待所有 Sub-Reactor 线程池完全 join() 回收
    server.stop();

    // 步骤 13: 记录最终停机完成日志，并通知 Logger 排空落盘最后剩余的缓冲区
    LOG_INFO << "Shutdown complete, flushing logs";
    lean_web_server::base::Logger::get_instance().shutdown();

    // 步骤 14: 确定性以 EXIT_SUCCESS (0) 退出进程
    return 0;
}
