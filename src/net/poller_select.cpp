// =============================================================================
//  poller_select.cpp —— 基于 select() 的通用实现（Windows 开发环境使用）
//
//  注意：select() 有 fd 数量上限（见 FD_SETSIZE），仅用于本地开发与小规模
//  联调；线上 Linux 部署会自动切换到 epoll 实现。
// =============================================================================
#include "net/poller.h"

#include <algorithm>
#include <map>

namespace blob {
namespace net {

namespace {

class SelectPoller : public Poller {
public:
    SelectPoller() {
        maxFd_ = 0;
    }

    bool add(socket_t fd, PollEvent events) override {
        if (entries_.size() >= static_cast<std::size_t>(FD_SETSIZE)) {
            return false;
        }
        // Windows 下 socket 值不可用 < 0 判断，统一用 kInvalidSocket
        if (fd == BLOB_INVALID_SOCKET) {
            return false;
        }
        entries_[fd] = events;
        // select() 的第一个参数必须是"最大 fd + 1"（Windows 忽略该参数，
        // 但 Linux/POSIX 上必须正确，否则监听不到任何就绪事件）
        if (fd > maxFd_) {
            maxFd_ = fd;
        }
        return true;
    }

    bool modify(socket_t fd, PollEvent events) override {
        auto it = entries_.find(fd);
        if (it == entries_.end()) {
            return false;
        }
        it->second = events;
        return true;
    }

    bool remove(socket_t fd) override {
        const bool erased = entries_.erase(fd) > 0;
        if (erased && fd == maxFd_) {
            maxFd_ = 0;
            for (const auto& kv : entries_) {
                if (kv.first > maxFd_) {
                    maxFd_ = kv.first;
                }
            }
        }
        return erased;
    }

    int wait(std::vector<PollResult>& out, int timeoutMs) override {
        out.clear();

        if (entries_.empty()) {
            sleepMillis(timeoutMs < 0 ? 1 : timeoutMs);
            return 0;
        }

        FD_ZERO(&readSet_);
        FD_ZERO(&writeSet_);
        FD_ZERO(&exceptSet_);

        for (const auto& kv : entries_) {
            if (hasEvent(kv.second, PollEvent::Read)) {
                FD_SET(kv.first, &readSet_);
            }
            if (hasEvent(kv.second, PollEvent::Write)) {
                FD_SET(kv.first, &writeSet_);
            }
            FD_SET(kv.first, &exceptSet_);
        }

        timeval tv;
        timeval* tvPtr = nullptr;
        if (timeoutMs >= 0) {
            tv.tv_sec  = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            tvPtr      = &tv;
        }

        const int rc = select(static_cast<int>(maxFd_ + 1), &readSet_, &writeSet_,
                              &exceptSet_, tvPtr);
        if (rc <= 0) {
            if (rc < 0 && isWouldBlock(lastError())) {
                return 0;
            }
            return rc;
        }

        for (const auto& kv : entries_) {
            PollEvent ev = PollEvent::None;
            if (FD_ISSET(kv.first, &readSet_)) {
                ev |= PollEvent::Read;
            }
            if (FD_ISSET(kv.first, &writeSet_)) {
                ev |= PollEvent::Write;
            }
            if (FD_ISSET(kv.first, &exceptSet_)) {
                ev |= PollEvent::Error;
            }
            if (ev != PollEvent::None) {
                out.push_back(PollResult{kv.first, ev});
            }
        }
        return static_cast<int>(out.size());
    }

    const char* name() const override { return "select"; }

private:
    std::map<socket_t, PollEvent> entries_;  // map 保证遍历顺序稳定，便于调试复现
    fd_set readSet_{};
    fd_set writeSet_{};
    fd_set exceptSet_{};
    socket_t maxFd_ = 0;
};

}  // namespace

Poller* Poller::create() {
    return new SelectPoller();
}

}  // namespace net
}  // namespace blob
