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
//
//  I/O 多路复用是什么：
//    一个线程同时“盯”大量 socket，阻塞等待直到其中某些可读/可写，
//    避免为每个连接开一个线程。这是 Reactor 事件循环的底层核心。
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "net/platform.h"

namespace blob {
namespace net {

/// @brief 关注的事件类型（位标志，可用 | 组合）
enum class PollEvent : uint8_t {
    None  = 0,
    Read  = 1u << 0,  // 可读：有数据到达 / 新连接 / 对端关闭
    Write = 1u << 1,  // 可写：内核发送缓冲腾出空间（只在有待发数据时关注）
    Error = 1u << 2,  // 错误/挂断
};

/// 按位或：组合多个事件（如 Read | Write）
inline PollEvent operator|(PollEvent a, PollEvent b) {
    return static_cast<PollEvent>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline PollEvent& operator|=(PollEvent& a, PollEvent b) {
    a = a | b;
    return a;
}

/// @brief 测试事件集合 set 中是否包含 test（位与判定）
inline bool hasEvent(PollEvent set, PollEvent test) {
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(test)) != 0;
}

/// @brief 一次 wait 的就绪结果：哪个 fd、发生了什么事件
struct PollResult {
    socket_t  fd     = BLOB_INVALID_SOCKET;  ///< 就绪的连接描述符
    PollEvent events = PollEvent::None;      ///< 就绪事件集合
};

/// @brief I/O 多路复用器抽象基类
/// @details 子类 SelectPoller / EpollPoller 实现同样的接口，
///          IoReactor 面向抽象编程，编译期由工厂 create() 选择平台实现。
class Poller {
public:
    virtual ~Poller() = default;

    /// @brief 新增监视一个 fd
    /// @param fd 连接  @param events 初始关注事件
    /// @return true=成功
    virtual bool add(socket_t fd, PollEvent events)    = 0;

    /// @brief 修改 fd 的关注事件（典型：发送积压时加 Write，发完去掉）
    virtual bool modify(socket_t fd, PollEvent events) = 0;

    /// @brief 移除对 fd 的监视（关闭连接前调用）
    virtual bool remove(socket_t fd)                   = 0;

    /// @brief 等待就绪事件
    /// @param[out] out 就绪结果数组（进入时会被 clear）
    /// @param timeoutMs 超时毫秒；<0 表示无限阻塞
    /// @return 就绪数量；0 表示超时；<0 表示出错
    virtual int wait(std::vector<PollResult>& out, int timeoutMs) = 0;

    /// @return 实现名 "select" / "epoll"（日志诊断用）
    virtual const char* name() const = 0;

    /// @brief 平台工厂：Windows 编译返回 SelectPoller，Linux 返回 EpollPoller
    /// @note 返回裸指针，调用方用 unique_ptr 持有（IoReactor 中）
    static Poller* create();
};

}  // namespace net
}  // namespace blob
