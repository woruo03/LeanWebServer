/**
 * @file EpollPoller.cpp
 * @brief EpollPoller 实现
 * @author woruo03
 * @date 2026-11-18
 * @version 1.0
 */

#include "net/EpollPoller.h"

#include <unistd.h>

#include <cerrno>

#include "base/Logger.h"
#include "net/Channel.h"

namespace lean_web_server::net {

EpollPoller::EpollPoller() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)), events_(16) {
    if (epoll_fd_ < 0) {
        LOG_FATAL << "Failed to create epoll_fd: errno=" << errno;
    }
}

EpollPoller::~EpollPoller() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

void EpollPoller::poll(std::vector<Channel*>& active_channels, int timeout) {
    int n = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout);

    int saved_errno = errno;
    if (n == -1) {
        if (saved_errno != EINTR) {
            LOG_ERROR << "EpollPoller::poll error: errno=" << saved_errno;
            if (++consecutive_errors_ >= 1000) {
                LOG_FATAL << "EpollPoller::poll consecutive error limit (1000) reached, aborting "
                             "to prevent spin";
            }
        }
        return;
    }
    consecutive_errors_ = 0;

    for (int i = 0; i < n; ++i) {
        auto* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        active_channels.push_back(channel);
    }
    if (n == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);
    }
}

void EpollPoller::update_channel(Channel* channel) const {
    int fd = channel->fd();

    struct epoll_event event{};
    event.data.ptr = channel;
    event.events = channel->events();
    if (channel->state() == Channel::State::kNew || channel->state() == Channel::State::kDeleted) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
        channel->set_state(Channel::State::kAdded);
    } else {
        if (channel->events() == 0) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &event);
            channel->set_state(Channel::State::kDeleted);
        } else {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
        }
    }
}

void EpollPoller::remove_channel(Channel* channel) const {
    int fd = channel->fd();
    if (channel->state() == Channel::State::kAdded) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    }
    channel->set_state(Channel::State::kNew);
}

}  // namespace lean_web_server::net
