/**
 * @file gen_load.cpp
 * @brief 简单受控并发 TCP/HTTP 负载发生器
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc >= 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        std::cout << "Usage: " << argv[0] << " [port] [threads] [duration_sec]\n"
                  << "  port: Server port to target (default: 9008)\n"
                  << "  threads: Number of client threads (default: 4)\n"
                  << "  duration_sec: Test duration in seconds (default: 5)\n";
        return 0;
    }

    uint16_t port = 9008;
    int num_threads = 4;
    int duration_sec = 5;

    if (argc >= 2)
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    if (argc >= 3)
        num_threads = std::stoi(argv[2]);
    if (argc >= 4)
        duration_sec = std::stoi(argv[3]);

    std::atomic<bool> running{true};
    std::atomic<uint64_t> total_requests{0};

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&running, &total_requests, port] {
            std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
            std::array<char, 512> buf{};

            while (running.load(std::memory_order_relaxed)) {
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0)
                    continue;

                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

                if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    for (int r = 0; r < 20 && running.load(std::memory_order_relaxed); ++r) {
                        if (::write(fd, req.data(), req.size()) <= 0)
                            break;
                        if (::read(fd, buf.data(), buf.size()) <= 0)
                            break;
                        total_requests.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                ::close(fd);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    running.store(false, std::memory_order_relaxed);

    for (auto& w : workers) {
        w.join();
    }

    std::cout << "[gen_load] Completed " << total_requests.load() << " requests across "
              << num_threads << " threads in " << duration_sec << " seconds." << std::endl;
    return 0;
}
