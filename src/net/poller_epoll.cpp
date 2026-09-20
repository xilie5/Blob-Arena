// =============================================================================
//  poller_epoll.cpp —— 基于 epoll 的 Linux 实现（线上部署使用）
//
//  相比 select：
//    - 无 fd 数量上限（只受进程 fd limit 约束）
//    - O(1) 就绪事件通知，连接数增长时不会线性退化
//    - 支持 ET（边沿触发）模式，配合非阻塞 IO 减少系统调用次数
//
//  该文件在 Windows 构建中被 Makefile / CMake 排除。
// =============================================================================
#ifndef _WIN32

#include "net/poller.h"

#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <map>

namespace blob {
namespace net {

namespace {

uint32_t toEpollEvents(PollEvent events) {
    uint32_t out = 0;
    if (hasEvent(events, PollEvent::Read)) {
        out |= EPOLLIN | EPOLLRDHUP;
    }
    if (hasEvent(events, PollEvent::Write)) {
        out |= EPOLLOUT;
    }
    // 边沿触发：高并发下显著减少 epoll_wait 返回次数
    out |= EPOLLET;
    return out;
}

class EpollPoller : public Poller {
public:
    EpollPoller() {
        epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    }

    ~EpollPoller() override {
        if (epollFd_ >= 0) {
            ::close(epollFd_);
        }
    }

    bool add(socket_t fd, PollEvent events) override {
        epoll_event ev;
        ::memset(&ev, 0, sizeof(ev));
        ev.events   = toEpollEvents(events);
        ev.data.fd  = fd;
        if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            return false;
        }
        registered_[fd] = events;
        return true;
    }

    bool modify(socket_t fd, PollEvent events) override {
        epoll_event ev;
        ::memset(&ev, 0, sizeof(ev));
        ev.events  = toEpollEvents(events);
        ev.data.fd = fd;
        if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
            return false;
        }
        registered_[fd] = events;
        return true;
    }

    bool remove(socket_t fd) override {
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        return registered_.erase(fd) > 0;
    }

    int wait(std::vector<PollResult>& out, int timeoutMs) override {
        out.clear();
        constexpr int kMaxEvents = 256;
        epoll_event events[kMaxEvents];

        const int n = ::epoll_wait(epollFd_, events, kMaxEvents, timeoutMs);
        if (n <= 0) {
            return n;
        }
        for (int i = 0; i < n; ++i) {
            const uint32_t raw = events[i].events;
            PollEvent ev = PollEvent::None;
            if (raw & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                ev |= PollEvent::Read;
            }
            if (raw & EPOLLOUT) {
                ev |= PollEvent::Write;
            }
            if (raw & (EPOLLERR | EPOLLHUP)) {
                ev |= PollEvent::Error;
            }
            if (ev != PollEvent::None) {
                out.push_back(PollResult{events[i].data.fd, ev});
            }
        }
        return static_cast<int>(out.size());
    }

    const char* name() const override { return "epoll"; }

private:
    int                                epollFd_ = -1;
    std::map<socket_t, PollEvent>      registered_;
};

}  // namespace

Poller* Poller::create() {
    return new EpollPoller();
}

}  // namespace net
}  // namespace blob

#endif  // !_WIN32
