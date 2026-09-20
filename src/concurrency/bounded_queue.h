// =============================================================================
//  bounded_queue.h —— 线程安全有界队列
// =============================================================================
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace blob {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool tryPush(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    bool waitPop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!notEmpty_.wait_for(lock, timeout, [&] {
                return closed_ || !queue_.empty();
            })) {
            return false;
        }
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }

    bool waitPush(T value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!notFull_.wait_for(lock, timeout, [&] {
                return closed_ || queue_.size() < capacity_;
            })) {
            return false;
        }
        if (closed_) {
            return false;
        }
        queue_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> queue_;
    bool closed_ = false;
};

}  // namespace blob
