/**
 * @file AsyncLogging.cpp
 * @brief 异步落盘引擎核心实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "base/AsyncLogging.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace lean_web_server::base {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
AsyncLogging::AsyncLogging(std::string log_dir, std::string base_name, size_t roll_size,
                           int flush_interval)
    : log_dir_(std::move(log_dir)),
      base_name_(std::move(base_name)),
      roll_size_bytes_(roll_size),
      flush_interval_sec_(flush_interval),
      current_buffer_(std::make_unique<Buffer>()),
      next_buffer_(std::make_unique<Buffer>()) {
    current_buffer_->bzero();
    next_buffer_->bzero();
    buffers_.reserve(16);
}

void AsyncLogging::start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        thread_ = std::thread(&AsyncLogging::thread_func, this);
    }
}

AsyncLogging::~AsyncLogging() {
    if (running_.load()) {
        stop();
    }
    if (fp_ != nullptr) {
        ::fflush(fp_);
        ::fclose(fp_);
        fp_ = nullptr;
    }
}

void AsyncLogging::stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        cond_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

void AsyncLogging::append(const char* log_line, size_t len) {
    std::scoped_lock lock(mutex_);
    if (current_buffer_->avail() >= len) {
        current_buffer_->append(log_line, len);
    } else {
        buffers_.push_back(std::move(current_buffer_));
        if (next_buffer_) {
            current_buffer_ = std::move(next_buffer_);
        } else {
            current_buffer_ = std::make_unique<Buffer>();
        }
        current_buffer_->append(log_line, len);
        cond_.notify_one();
    }
}

void AsyncLogging::emergency_flush(const char* log_line, size_t len) {
    std::scoped_lock lock(mutex_);
    if (fp_ != nullptr) {
        if ((log_line != nullptr) && len > 0) {
            ::fwrite_unlocked(log_line, 1, len, fp_);
        }
        ::fflush(fp_);
    }
}

void AsyncLogging::write_to_file(const char* data, size_t len) {
    if (fp_ == nullptr) {
        roll_file();
    }
    if (fp_ == nullptr) {
        return;
    }

    size_t written = ::fwrite_unlocked(data, 1, len, fp_);
    written_bytes_ += written;

    if (written_bytes_ >= roll_size_bytes_) {
        roll_file();
    } else {
        time_t now = ::time(nullptr);
        struct tm tm_now{};
        ::localtime_r(&now, &tm_now);
        if (tm_now.tm_mday != current_day_) {
            roll_file();
        }
    }
}

void AsyncLogging::roll_file() {
    if (fp_ != nullptr) {
        ::fflush(fp_);
        ::fclose(fp_);
        fp_ = nullptr;
    }

    ::mkdir(log_dir_.c_str(), 0755);

    Timestamp now = Timestamp::now();
    time_t sec = now.seconds();
    struct tm tm_now{};
    ::localtime_r(&sec, &tm_now);
    current_day_ = tm_now.tm_mday;
    last_roll_time_ = sec;
    written_bytes_ = 0;

    static uint32_t s_roll_counter = 0;
    std::string filename = log_dir_ + "/" + base_name_ + "." + now.to_filename_string() + "." +
                           std::to_string(::getpid()) + ".log";

    struct stat st{};
    if (::stat(filename.c_str(), &st) == 0) {
        filename = log_dir_ + "/" + base_name_ + "." + now.to_filename_string() + "." +
                   std::to_string(::getpid()) + "." + std::to_string(++s_roll_counter) + ".log";
    }

    fp_ = ::fopen(filename.c_str(), "ae");
    if (fp_ != nullptr) {
        ::setbuffer(fp_, file_buffer_.data(), file_buffer_.size());
    }

    // 清理超过 max_backup_files (10) 的旧日志
    DIR* dir = ::opendir(log_dir_.c_str());
    if (dir != nullptr) {
        std::vector<std::string> log_files;
        struct dirent* entry = nullptr;
        std::string prefix = base_name_ + ".";
        std::string suffix = ".log";

        while ((entry = ::readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name.rfind(prefix, 0) == 0 && name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                log_files.push_back(log_dir_ + "/" + name);
            }
        }
        ::closedir(dir);

        if (log_files.size() > 10) {
            std::sort(log_files.begin(), log_files.end());
            size_t to_delete = log_files.size() - 10;
            for (size_t i = 0; i < to_delete; ++i) {
                ::unlink(log_files[i].c_str());
            }
        }
    }
}

void AsyncLogging::handle_backpressure(BufferPtrVector& buffers) {
    if (buffers.size() > k_log_queue_capacity) {
        size_t dropped_buffers = buffers.size() - 2;
        dropped_lines_.fetch_add(dropped_buffers, std::memory_order_relaxed);
        std::array<char, 256> drop_msg{};
        int n = std::snprintf(drop_msg.data(), drop_msg.size(),
                              "\n*** DROPPED %zu LOG BUFFERS DUE TO DISK BACKPRESSURE ***\n",
                              dropped_buffers);
        buffers.erase(buffers.begin() + 2, buffers.end());
        write_to_file(drop_msg.data(), static_cast<size_t>(n));
    }
}

void AsyncLogging::write_buffers(const BufferPtrVector& buffers) {
    for (const auto& buffer : buffers) {
        if (buffer && buffer->length() > 0) {
            write_to_file(buffer->data(), buffer->length());
        }
    }
}

void AsyncLogging::replenish_buffers(BufferPtr& new_buffer1, BufferPtr& new_buffer2,
                                     BufferPtrVector& buffers_to_write) {
    if (buffers_to_write.size() > 2) {
        buffers_to_write.resize(2);
    }

    if (!new_buffer1 && !buffers_to_write.empty()) {
        new_buffer1 = std::move(buffers_to_write.back());
        buffers_to_write.pop_back();
        (*new_buffer1).reset();
    }
    if (!new_buffer2 && !buffers_to_write.empty()) {
        new_buffer2 = std::move(buffers_to_write.back());
        buffers_to_write.pop_back();
        (*new_buffer2).reset();
    }

    if (!new_buffer1) {
        new_buffer1 = std::make_unique<Buffer>();
        new_buffer1->bzero();
    }
    if (!new_buffer2) {
        new_buffer2 = std::make_unique<Buffer>();
        new_buffer2->bzero();
    }

    buffers_to_write.clear();
}

void AsyncLogging::thread_func() {
    auto new_buffer1 = std::make_unique<Buffer>();
    auto new_buffer2 = std::make_unique<Buffer>();
    new_buffer1->bzero();
    new_buffer2->bzero();

    BufferPtrVector buffers_to_write;
    buffers_to_write.reserve(16);

    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (buffers_.empty()) {
                cond_.wait_for(lock, std::chrono::seconds(flush_interval_sec_));
            }
            buffers_.push_back(std::move(current_buffer_));
            current_buffer_ = std::move(new_buffer1);
            buffers_to_write.swap(buffers_);
            if (!next_buffer_) {
                next_buffer_ = std::move(new_buffer2);
            }
        }

        handle_backpressure(buffers_to_write);
        write_buffers(buffers_to_write);
        replenish_buffers(new_buffer1, new_buffer2, buffers_to_write);

        if (fp_ != nullptr) {
            ::fflush(fp_);
        }
    }

    // 退出前完全排空
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (current_buffer_ && current_buffer_->length() > 0) {
            buffers_.push_back(std::move(current_buffer_));
        }
        buffers_to_write.swap(buffers_);
    }

    write_buffers(buffers_to_write);

    if (fp_ != nullptr) {
        ::fflush(fp_);
    }
}

}  // namespace lean_web_server::base
