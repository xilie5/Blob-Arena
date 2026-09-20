// =============================================================================
//  poller.h —— IO 多路复用抽象接口
//
//  平台实现：
//    Windows / 通用  ->  SelectPoller   （poller_select.cpp）
//    Linux           ->  EpollPoller    （poller_epoll.cpp）
//
//  统一抽象带来两个好处：
//    1. 本地 Windows 也能开发调试，不必依赖 WSL
//    2. 线上 Linux 走 epoll，横向扩展不受 fd 数量限制
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "net/platform.h"

namespace blob {
namespace net {

enum class PollEvent : uint8_t {
    None  = 0,
    Read  = 1u << 0,
    Write = 1u << 1,
    Error = 1u << 2,
};

inline PollEvent operator|(PollEvent a, PollEvent b) {
    return static_cast<PollEvent>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline PollEvent& operator|=(PollEvent& a, PollEvent b) {
    a = a | b;
    return a;
}

inline bool hasEvent(PollEvent set, PollEvent test) {
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(test)) != 0;
}

struct PollResult {
    socket_t  fd     = BLOB_INVALID_SOCKET;
    PollEvent events = PollEvent::None;
};

class Poller {
public:
    virtual ~Poller() = default;

    virtual bool add(socket_t fd, PollEvent events)    = 0;
    virtual bool modify(socket_t fd, PollEvent events) = 0;
    virtual bool remove(socket_t fd)                   = 0;

    // timeoutMs < 0 表示无限阻塞；返回就绪数量，0 表示超时，<0 表示出错
    virtual int wait(std::vector<PollResult>& out, int timeoutMs) = 0;

    virtual const char* name() const = 0;

    // 平台工厂：根据编译平台返回对应实现
    static Poller* create();
};

}  // namespace net
}  // namespace blob
