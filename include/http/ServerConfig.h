/**
 * @file ServerConfig.h
 * @brief 服务器全局统一配置类与基于 POSIX getopt_long 的命令行解析器
 * @author woruo03
 * @date 2026-11-19
 * @version 1.0
 */

#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>

#include "base/Logger.h"
#include "net/TcpConnection.h"

namespace lean_web_server::http {

/**
 * @struct ServerConfig
 * @brief 服务器全局统一配置类
 *
 * 集中管理网络服务参数、并发线程数、连接双轨超时与异步日志选项。
 * 消除散落在 main.cpp、HttpServer 及各模块的硬编码常数与零散配置。
 */
struct ServerConfig {
    /// 计算基于当前主机硬件并发核心数的默认 Sub-Reactor IO 线程数（预留 1 核给 Main Reactor，保底
    /// 1）
    static int default_io_threads() noexcept;

    uint16_t port{9006};  ///< 监听端口（默认 9006）
    /// Sub-Reactor 工作线程数（默认 CPU 核心数 - 1，保底 1）
    int io_threads{default_io_threads()};
    int listen_backlog{SOMAXCONN};  ///< TCP 监听队列最大排队长度

    net::TimeoutConfig timeout{};  ///< L1 空闲超时 (30s) + L2 绝对生存期 (120s) + 时间轮槽数 (1024)
    int shutdown_drain_timeout_sec{5};  ///< 优雅停机慢客户端写缓冲排空宽限期（默认 5s）

    base::LogConfig log{};  ///< 日志目录、切分大小、级别与刷盘周期

    std::string static_resource_dir{"./resources"};  ///< 静态资源根目录

    /// 校验所有参数的合法性，若校验失败返回 false 并填充错误说明
    [[nodiscard]] bool is_valid(std::string* error_msg = nullptr) const;

    /// 格式化为美观的多行描述文本（供日志启动输出）
    [[nodiscard]] std::string to_string() const;

    /// 基于 POSIX getopt_long 标准解析命令行参数（严格长短选项驱动，拒绝位置参数）
    /// 成功解析返回 true；若请求 -h/--help 或存在未知选项、位置参数、数值转换错误返回 false
    bool parse_args(int argc, char* argv[], bool* help_requested = nullptr,
                    std::string* error_msg = nullptr);

    /// 输出标准命令行使用说明与各参数默认值
    static void print_usage(const char* prog_name);

private:
    bool parse_option(int opt, const char* arg, bool* help_requested, std::string& err);
};

}  // namespace lean_web_server::http

namespace lean_web_server {
using http::ServerConfig;
}
