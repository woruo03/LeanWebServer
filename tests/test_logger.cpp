/**
 * @file test_logger.cpp
 * @brief Logger 与 AsyncLogging 单元测试 (L1-L7, L-BENCH)
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "base/AsyncLogging.h"
#include "base/Logger.h"

using namespace lean_web_server::base;

namespace {

void clear_test_logs(const std::string& dir) {
    DIR* dp = ::opendir(dir.c_str());
    if (!dp)
        return;
    struct dirent* ep = nullptr;
    while ((ep = ::readdir(dp)) != nullptr) {
        std::string name(ep->d_name);
        if (name.find(".log") != std::string::npos) {
            ::unlink((dir + "/" + name).c_str());
        }
    }
    ::closedir(dp);
}

std::vector<std::string> get_test_log_files(const std::string& dir) {
    std::vector<std::string> files;
    DIR* dp = ::opendir(dir.c_str());
    if (!dp)
        return files;
    struct dirent* ep = nullptr;
    while ((ep = ::readdir(dp)) != nullptr) {
        std::string name(ep->d_name);
        if (name.find(".log") != std::string::npos) {
            files.push_back(dir + "/" + name);
        }
    }
    ::closedir(dp);
    return files;
}

}  // namespace

// ── L1: 行格式正则匹配 ──
TEST(LoggerTest, L1_LogLineFormatRegex) {
    std::string test_dir = "./logs_test_l1";
    ::mkdir(test_dir.c_str(), 0755);
    clear_test_logs(test_dir);

    AsyncLogging async(test_dir, "test_l1", 64 * 1024 * 1024, 1);
    async.start();

    LogConfig cfg;
    cfg.log_dir = test_dir;
    cfg.base_name = "test_l1";
    cfg.default_level = LogLevel::TRACE;
    Logger::get_instance().init(cfg);

    LOG_INFO << "Test regex line: " << 12345;
    LOG_FMT_WARN("Formatted warning: code=%d", 42);

    Logger::get_instance().shutdown();

    auto files = get_test_log_files(test_dir);
    ASSERT_FALSE(files.empty());

    std::ifstream ifs(files[0]);
    std::string line;
    std::regex log_regex(
        R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6} \[[TDIWEF]\] \[tid \d+\] \[[^ ]+:\d+ [^ ]+\] .*)");

    int line_count = 0;
    while (std::getline(ifs, line)) {
        if (!line.empty()) {
            EXPECT_TRUE(std::regex_match(line, log_regex)) << "Failed line: " << line;
            ++line_count;
        }
    }
    EXPECT_GE(line_count, 2);
    clear_test_logs(test_dir);
    ::rmdir(test_dir.c_str());
}

// ── L2: 单例并发获取（TSan 无竞争） ──
TEST(LoggerTest, L2_ConcurrentSingletonAccess) {
    constexpr int k_threads = 8;
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};

    for (int i = 0; i < k_threads; ++i) {
        threads.emplace_back([&start] {
            while (!start.load(std::memory_order_acquire)) {
            }
            Logger& inst = Logger::get_instance();
            EXPECT_TRUE(inst.should_log(LogLevel::INFO));
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }
}

// ── L3: 级别过滤动态生效 ──
TEST(LoggerTest, L3_LogLevelDynamicFiltering) {
    Logger& inst = Logger::get_instance();
    inst.set_level(LogLevel::WARN);

    EXPECT_FALSE(inst.should_log(LogLevel::TRACE));
    EXPECT_FALSE(inst.should_log(LogLevel::DEBUG));
    EXPECT_FALSE(inst.should_log(LogLevel::INFO));
    EXPECT_TRUE(inst.should_log(LogLevel::WARN));
    EXPECT_TRUE(inst.should_log(LogLevel::ERROR));
    EXPECT_TRUE(inst.should_log(LogLevel::FATAL));

    inst.set_level(LogLevel::DEBUG);
    EXPECT_FALSE(inst.should_log(LogLevel::TRACE));
    EXPECT_TRUE(inst.should_log(LogLevel::DEBUG));
    EXPECT_TRUE(inst.should_log(LogLevel::INFO));

    inst.set_level(LogLevel::INFO);
}

// ── L4: 轮转触发 ──
TEST(LoggerTest, L4_RollFileBySize) {
    std::string test_dir = "./logs_test_l4";
    ::mkdir(test_dir.c_str(), 0755);
    clear_test_logs(test_dir);

    // 设置很小的 roll size (64KB) 以测试快速轮转
    size_t roll_size = 64 * 1024;
    {
        AsyncLogging async(test_dir, "test_roll", roll_size, 1);
        async.start();

        std::string payload(1024, 'X');
        payload += "\n";
        for (int i = 0; i < 150; ++i) {
            async.append(payload.data(), payload.size());
        }
        async.stop();
    }

    auto files = get_test_log_files(test_dir);
    EXPECT_GE(files.size(), 2U);

    clear_test_logs(test_dir);
    ::rmdir(test_dir.c_str());
}

// ── L5: stderr 兜底与初始化切换 ──
TEST(LoggerTest, L5_StderrFallbackBeforeInit) {
    // 捕获 stderr 或直接调用 append 验证不崩溃
    Logger& inst = Logger::get_instance();
    const char* test_msg = "test fallback\n";
    inst.append(test_msg, std::strlen(test_msg));
    SUCCEED();
}

// ── L6: stop() 后无损落盘 ──
TEST(LoggerTest, L6_LosslessFlushOnStop) {
    std::string test_dir = "./logs_test_l6";
    ::mkdir(test_dir.c_str(), 0755);
    clear_test_logs(test_dir);

    constexpr int k_lines = 10000;
    {
        AsyncLogging async(test_dir, "test_l6", 64 * 1024 * 1024, 1);
        async.start();

        std::string line =
            "2026-11-18 12:00:00.000000 [I] [tid 1] [test:1 test] Hello, lossless logging!\n";
        for (int i = 0; i < k_lines; ++i) {
            async.append(line.data(), line.size());
        }
        async.stop();
    }

    auto files = get_test_log_files(test_dir);
    ASSERT_FALSE(files.empty());

    int counted_lines = 0;
    for (const auto& file : files) {
        std::ifstream ifs(file);
        std::string str;
        while (std::getline(ifs, str)) {
            if (!str.empty()) {
                ++counted_lines;
            }
        }
    }
    EXPECT_EQ(counted_lines, k_lines);

    clear_test_logs(test_dir);
    ::rmdir(test_dir.c_str());
}

// ── L7: FATAL 崩溃路径死亡测试 ──
TEST(LoggerTest, L7_FatalAbort) {
    EXPECT_DEATH({ LOG_FATAL << "Fatal test abort message"; }, "");
}

// ── L-BENCH: 多线程性能基准 ──
TEST(LoggerTest, L_BENCH_Throughput) {
    std::string test_dir = "./logs_bench";
    ::mkdir(test_dir.c_str(), 0755);
    clear_test_logs(test_dir);

    constexpr int k_threads = 4;
    constexpr int k_lines_per_thread = 250000;  // 合计 100 万行
    std::string sample_line =
        "2026-11-18 12:00:00.000000 [I] [tid 1234] [bench.cpp:99 benchmark] 96B row line padding "
        "data\n";

    {
        AsyncLogging async(test_dir, "bench", 64 * 1024 * 1024, 3);
        async.start();

        auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> workers;
        for (int t = 0; t < k_threads; ++t) {
            workers.emplace_back([&async, &sample_line] {
                for (int i = 0; i < k_lines_per_thread; ++i) {
                    async.append(sample_line.data(), sample_line.size());
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        async.stop();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        double seconds = static_cast<double>(elapsed) / 1000.0;
        double throughput = (k_threads * k_lines_per_thread) / seconds;
        std::cout << "[BENCHMARK] AsyncLogging: " << (k_threads * k_lines_per_thread)
                  << " lines in " << seconds << "s (" << static_cast<int>(throughput) << " lines/s)"
                  << std::endl;
    }

    clear_test_logs(test_dir);
    ::rmdir(test_dir.c_str());
}

// ── L8: LogStream 基础格式化流断言 ──
TEST(LoggerTest, L8_LogStreamFormatting) {
    LogStream stream;
    stream << true << false << " " << -12345 << " " << 67890ULL << " " << 3.14159;
    EXPECT_STREQ(stream.buffer().data(), "10 -12345 67890 3.14159");

    stream.reset_buffer();
    int val = 42;
    stream << &val;
    EXPECT_TRUE(std::string(stream.buffer().data()).rfind("0x", 0) == 0);
}
