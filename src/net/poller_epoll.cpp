// =============================================================================
//  poller_epoll.cpp —— 基于 epoll 的 Linux 实现（线上部署使用）
//
//  相比 select：
//    - 无 fd 数量上限（只受进程 fd limit 约束）
//    - O(1) 就绪事件通知，连接数增长时不会线性退化（「事件回调 + 就绪队列」的机制）
//    这是 epoll 性能高的灵魂：**不是内核轮询检查谁就绪，是 socket 状态变化时，主动通过回调把自己塞进就绪队列。**

//  #### 完整流程（以读事件为例）
// 1. 你调用 `epoll_ctl(ADD, fd, EPOLLIN)`，内核把 fd 加入红黑树，同时给这个 socket 绑定一个回调函数；
// 2. 对端发数据过来，网卡收到数据，内核把数据拷进 socket 的接收缓冲区；
// 3. 这个 socket 状态从「无数据」变成「有数据」，触发绑定的回调函数；
// 4. 回调函数把这个 fd 的就绪事件，挂到 epoll 的就绪队列里；
// 5. 如果有线程正在 `epoll_wait` 阻塞，内核就唤醒它。
//
//  - 支持 ET（边沿触发）模式，配合非阻塞 IO（必须） 减少系统调用次数
//
//  该文件在 Windows 构建中被 Makefile / CMake 排除。
//
//  三个核心系统调用：
//    epoll_create1  创建 epoll 实例（其本身也是一个 fd）
//    epoll_ctl      向内核兴趣集合中 增/改/删 被监视的 fd
//    epoll_wait     取出已就绪的事件列表（只含有事件的 fd）
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

/// @brief 把抽象 PollEvent 翻译成 epoll 的事件标志位
uint32_t toEpollEvents(PollEvent events) {
    uint32_t out = 0;
    if (hasEvent(events, PollEvent::Read)) {
        // EPOLLIN 有数据可读；EPOLLRDHUP 对端半关闭/挂断（及时发现断开）
        out |= EPOLLIN | EPOLLRDHUP;
    }
    if (hasEvent(events, PollEvent::Write)) {
        out |= EPOLLOUT;  // 发送缓冲有空间，可写
    }
    // 边沿触发：只在状态变化的一瞬间通知一次。
    // 高并发下显著减少 epoll_wait 返回次数；代价是必须配合非阻塞 socket，
    // 并在收到可读事件时循环 recv 直到 EAGAIN，否则会漏掉剩余数据。
    out |= EPOLLET;
    return out;
}

class EpollPoller : public Poller {
public:
    EpollPoller() {
        // EPOLL_CLOEXEC：进程 fork+exec 时自动关闭该 fd，防止 fd 泄漏给子进程
        epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    }

    ~EpollPoller() override {
        if (epollFd_ >= 0) {
            ::close(epollFd_);
        }
    }

    bool add(socket_t fd, PollEvent events) override {
        // epoll_event 结构体定义：
        // struct epoll_event {
        //  uint32_t events;---32位无符号整数：关注的事件标志位
        //  epoll_data_t data;---数据：用户自定义数据，或 socket fd 等
        // };
        epoll_event ev;
        ::memset(&ev, 0, sizeof(ev));
        ev.events  = toEpollEvents(events);
        ev.data.fd  = fd;   // 就绪时原样带回，用它在连接表中定位连接
        if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            return false;
        }
        registered_[fd] = events;  // 本地也存一份（epoll 不支持方便地查回关注集合）
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
        // DEL 即使失败也清理本地记录（fd 可能已经关闭）
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        return registered_.erase(fd) > 0;
    }

    int wait(std::vector<PollResult>& out, int timeoutMs) override {
        out.clear();
        constexpr int kMaxEvents = 256;   // 单次最多取 256 个事件（栈数组，零分配）
        epoll_event events[kMaxEvents];

        // 阻塞等待，返回就绪事件数 n；n=0 超时，n<0 出错
        const int n = ::epoll_wait(epollFd_, events, kMaxEvents, timeoutMs);
        if (n <= 0) {
            return n;
        }
        // 把内核事件位翻译成抽象 Read/Write/Error
        for (int i = 0; i < n; ++i) {
            const uint32_t raw = events[i].events;
            PollEvent ev = PollEvent::None;
            // 收到挂断/错误也当“可读”处理：后续 recv 会返回 0 或错误以完成关闭
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
    int  epollFd_ = -1;   ///< epoll 实例 fd
    std::map<socket_t, PollEvent> registered_;     ///< 已注册表（关注集合备份）
};

}  // namespace

// Linux 构建的工厂实现
Poller* Poller::create() {
    return new EpollPoller();
}

}  // namespace net
}  // namespace blob

#endif  // !_WIN32
