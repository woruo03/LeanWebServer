/**
 * @file test_server_config.cpp
 * @brief ServerConfig 全局配置类与 getopt_long 命令行解析单元测试
 * @author woruo03
 * @date 2026-11-19
 * @version 1.0
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "http/ServerConfig.h"

using namespace lean_web_server;
using namespace lean_web_server::http;
using namespace lean_web_server::base;

TEST(ServerConfigTest, DefaultValues) {
    ServerConfig cfg;

    EXPECT_EQ(cfg.port, 9006);
    EXPECT_EQ(cfg.io_threads, ServerConfig::default_io_threads());
    EXPECT_GT(cfg.io_threads, 0);
    EXPECT_EQ(cfg.timeout.idle_timeout_sec, 30);
    EXPECT_EQ(cfg.timeout.max_lifetime_sec, 120);
    EXPECT_EQ(cfg.timeout.timer_wheel_slots, 1024);
    EXPECT_EQ(cfg.shutdown_drain_timeout_sec, 5);
    EXPECT_EQ(cfg.log.log_dir, "./logs");
    EXPECT_EQ(cfg.log.base_name, "lean_web_server");
    EXPECT_EQ(cfg.log.roll_size_bytes, 64 * 1024 * 1024);
    EXPECT_EQ(cfg.log.flush_interval_sec, 3);
    EXPECT_EQ(cfg.log.default_level, LogLevel::INFO);
    EXPECT_EQ(cfg.log.max_backup_files, 10);
    EXPECT_EQ(cfg.static_resource_dir, "./resources");

    std::string err;
    EXPECT_TRUE(cfg.is_valid(&err)) << "Default config should be valid: " << err;
}

TEST(ServerConfigTest, DefaultIoThreadsCpuAdaptive) {
    int threads = ServerConfig::default_io_threads();
    unsigned int hc = std::thread::hardware_concurrency();
    if (hc > 1) {
        EXPECT_EQ(threads, static_cast<int>(hc - 1));
    } else {
        EXPECT_EQ(threads, 1);
    }
}

TEST(ServerConfigTest, ValidationBoundary) {
    ServerConfig cfg;
    std::string err;

    // 端口为 0
    cfg.port = 0;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.port = 9006;

    // 线程数为负数
    cfg.io_threads = -1;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.io_threads = 1025;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.io_threads = 15;

    // 空闲超时非正数
    cfg.timeout.idle_timeout_sec = 0;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.timeout.idle_timeout_sec = -10;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.timeout.idle_timeout_sec = 30;

    // 时间轮槽数非 2 的幂次或超出界限 [64, 65536]
    cfg.timeout.timer_wheel_slots = 1000;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.timeout.timer_wheel_slots = 32;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.timeout.timer_wheel_slots = 131072;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.timeout.timer_wheel_slots = 1024;

    // 空闲超时 >= 时间轮槽数
    cfg.timeout.idle_timeout_sec = 1024;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.timeout.idle_timeout_sec = 30;

    // 存活上限小于空闲超时
    cfg.timeout.max_lifetime_sec = 20;
    cfg.timeout.idle_timeout_sec = 30;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.timeout.max_lifetime_sec = 120;

    // 宽限期负数
    cfg.shutdown_drain_timeout_sec = -1;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.shutdown_drain_timeout_sec = 5;

    // 日志切分大小小于 1MB
    cfg.log.roll_size_bytes = 1024;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.log.roll_size_bytes = 64 * 1024 * 1024;

    // 刷盘周期非正数
    cfg.log.flush_interval_sec = 0;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.log.flush_interval_sec = 3;

    // 备份数量小于 1
    cfg.log.max_backup_files = 0;
    EXPECT_FALSE(cfg.is_valid(&err));
    cfg.log.max_backup_files = 10;

    EXPECT_TRUE(cfg.is_valid(&err));
}

TEST(ServerConfigTest, ShortOptionsParsing) {
    ServerConfig cfg;
    bool help = false;
    std::string err;

    std::vector<std::string> args = {"lean_web_server",
                                     "-p",
                                     "8088",
                                     "-t",
                                     "4",
                                     "-d",
                                     "/tmp/l",
                                     "-l",
                                     "warn",
                                     "-i",
                                     "10",
                                     "-m",
                                     "60",
                                     "-w",
                                     "2048",
                                     "-r",
                                     "128"};
    std::vector<char*> argv;
    for (auto& s : args) {
        argv.push_back(s.data());
    }

    EXPECT_TRUE(cfg.parse_args(static_cast<int>(argv.size()), argv.data(), &help, &err))
        << "Failed to parse short options: " << err;
    EXPECT_FALSE(help);

    EXPECT_EQ(cfg.port, 8088);
    EXPECT_EQ(cfg.io_threads, 4);
    EXPECT_EQ(cfg.log.log_dir, "/tmp/l");
    EXPECT_EQ(cfg.log.default_level, LogLevel::WARN);
    EXPECT_EQ(cfg.timeout.idle_timeout_sec, 10);
    EXPECT_EQ(cfg.timeout.max_lifetime_sec, 60);
    EXPECT_EQ(cfg.timeout.timer_wheel_slots, 2048);
    EXPECT_EQ(cfg.log.roll_size_bytes, 128UL * 1024 * 1024);
}

TEST(ServerConfigTest, LongOptionsParsing) {
    ServerConfig cfg;
    bool help = false;
    std::string err;

    std::vector<std::string> args = {"lean_web_server",
                                     "--port",
                                     "7070",
                                     "--threads",
                                     "8",
                                     "--log-dir",
                                     "./custom_logs",
                                     "--log-level",
                                     "debug",
                                     "--idle-timeout",
                                     "15",
                                     "--max-lifetime",
                                     "90",
                                     "--wheel-slots",
                                     "512",
                                     "--roll-size",
                                     "32"};
    std::vector<char*> argv;
    for (auto& s : args) {
        argv.push_back(s.data());
    }

    EXPECT_TRUE(cfg.parse_args(static_cast<int>(argv.size()), argv.data(), &help, &err))
        << "Failed to parse long options: " << err;
    EXPECT_FALSE(help);

    EXPECT_EQ(cfg.port, 7070);
    EXPECT_EQ(cfg.io_threads, 8);
    EXPECT_EQ(cfg.log.log_dir, "./custom_logs");
    EXPECT_EQ(cfg.log.default_level, LogLevel::DEBUG);
    EXPECT_EQ(cfg.timeout.idle_timeout_sec, 15);
    EXPECT_EQ(cfg.timeout.max_lifetime_sec, 90);
    EXPECT_EQ(cfg.timeout.timer_wheel_slots, 512);
    EXPECT_EQ(cfg.log.roll_size_bytes, 32UL * 1024 * 1024);
}

TEST(ServerConfigTest, HelpOption) {
    ServerConfig cfg;
    bool help = false;
    std::string err;

    std::vector<std::string> args1 = {"lean_web_server", "-h"};
    std::vector<char*> argv1 = {args1[0].data(), args1[1].data()};
    EXPECT_FALSE(cfg.parse_args(static_cast<int>(argv1.size()), argv1.data(), &help, &err));
    EXPECT_TRUE(help);

    help = false;
    std::vector<std::string> args2 = {"lean_web_server", "--help"};
    std::vector<char*> argv2 = {args2[0].data(), args2[1].data()};
    EXPECT_FALSE(cfg.parse_args(static_cast<int>(argv2.size()), argv2.data(), &help, &err));
    EXPECT_TRUE(help);
}

TEST(ServerConfigTest, RejectPositionalArguments) {
    ServerConfig cfg;
    bool help = false;
    std::string err;

    // 纯裸传数字端口（旧版风格）
    std::vector<std::string> args1 = {"lean_web_server", "9008"};
    std::vector<char*> argv1 = {args1[0].data(), args1[1].data()};
    EXPECT_FALSE(cfg.parse_args(static_cast<int>(argv1.size()), argv1.data(), &help, &err));
    EXPECT_FALSE(help);
    EXPECT_NE(err.find("Unexpected positional argument: 9008"), std::string::npos);

    // 带选项后附带未知位置参数
    std::vector<std::string> args2 = {"lean_web_server", "-p", "9008", "extra_pos_arg"};
    std::vector<char*> argv2;
    for (auto& s : args2) {
        argv2.push_back(s.data());
    }
    EXPECT_FALSE(cfg.parse_args(static_cast<int>(argv2.size()), argv2.data(), &help, &err));
    EXPECT_NE(err.find("Unexpected positional argument: extra_pos_arg"), std::string::npos);
}

TEST(ServerConfigTest, UnknownAndInvalidArguments) {
    ServerConfig cfg;
    bool help = false;
    std::string err;

    // 未知选项
    std::vector<std::string> args1 = {"lean_web_server", "-x"};
    std::vector<char*> argv1 = {args1[0].data(), args1[1].data()};
    EXPECT_FALSE(cfg.parse_args(static_cast<int>(argv1.size()), argv1.data(), &help, &err));
    EXPECT_NE(err.find("Unknown or missing argument"), std::string::npos);

    // 端口非法格式
    std::vector<std::string> args2 = {"lean_web_server", "-p", "abc"};
    std::vector<char*> argv2;
    for (auto& s : args2) {
        argv2.push_back(s.data());
    }
    EXPECT_FALSE(cfg.parse_args(static_cast<int>(argv2.size()), argv2.data(), &help, &err));

    // 日志级别非法
    std::vector<std::string> args3 = {"lean_web_server", "-l", "invalid_level"};
    std::vector<char*> argv3;
    for (auto& s : args3) {
        argv3.push_back(s.data());
    }
    EXPECT_FALSE(cfg.parse_args(static_cast<int>(argv3.size()), argv3.data(), &help, &err));
}

TEST(ServerConfigTest, ToStringFormat) {
    ServerConfig cfg;
    std::string s = cfg.to_string();

    EXPECT_NE(s.find("port: 9006"), std::string::npos);
    EXPECT_NE(s.find("io_threads: " + std::to_string(ServerConfig::default_io_threads())),
              std::string::npos);
    EXPECT_NE(s.find("idle=30s"), std::string::npos);
    EXPECT_NE(s.find("max_lifetime=120s"), std::string::npos);
    EXPECT_NE(s.find("wheel_slots=1024"), std::string::npos);
    EXPECT_NE(s.find("level=INFO"), std::string::npos);
    EXPECT_NE(s.find("roll_size=64MB"), std::string::npos);
}
