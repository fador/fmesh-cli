#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace meshcli {

// A flag file descriptor. Writing to it wakes anyone poll()ing on it; reading
// drains the counter. Used to wake the ncurses main loop from other threads.
class EventFd {
public:
    EventFd() : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
    ~EventFd() { if (fd_ >= 0) ::close(fd_); }
    EventFd(const EventFd&) = delete;
    EventFd& operator=(const EventFd&) = delete;
    EventFd(EventFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    EventFd& operator=(EventFd&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) ::close(fd_); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    void notify() {
        uint64_t one = 1;
        ssize_t rc = ::write(fd_, &one, sizeof(one));
        // If the write fails (e.g. eventfd counter overflow or bad fd),
        // the wake is lost, but logging here would risk recursion.
        (void)rc;
    }
    void drain() {
        uint64_t v = 0;
        while (::read(fd_, &v, sizeof(v)) > 0) { /* drain all */ }
    }
    [[nodiscard]] int fd() const { return fd_; }

private:
    int fd_;
};

// Thread-safe FIFO used to hand work from background threads to a consumer.
template <typename T>
class ConcurrentQueue {
public:
    void push(T v) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            q_.push(std::move(v));
        }
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
    std::vector<T> drain_all() {
        std::vector<T> out;
        std::lock_guard<std::mutex> lock(mu_);
        out.reserve(q_.size());
        while (!q_.empty()) {
            out.push_back(std::move(q_.front()));
            q_.pop();
        }
        return out;
    }
    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return q_.empty();
    }

private:
    mutable std::mutex mu_;
    std::queue<T> q_;
};

} // namespace meshcli
