// =============================================================================
//  poller_select.cpp —— 基于 select() 的通用实现（Windows 开发环境使用）
//
//  注意：select() 有 fd 数量上限（见 FD_SETSIZE，本项目放宽到 1024），
//  且每次调用都要遍历全部连接检查就绪，复杂度 O(连接数)，
//  仅用于本地开发与小规模联调；线上 Linux 部署会自动切换到 epoll 实现。
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
        maxFd_ = 0; //每次检测0-maxFd_之间的fd
       }

    bool add(socket_t fd, PollEvent events) override {
        // select 的位图容量由 FD_SETSIZE 决定，超出直接失败
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
        it->second = events;  // 仅更新关注集合，下次 wait 重新填表时生效
        return true;
    }

    bool remove(socket_t fd) override {
        const bool erased = entries_.erase(fd) > 0;
        // 若删的恰好是最大 fd，需要重新扫描出新的最大值
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

        // 没有任何连接时 select 无意义，按超时睡眠避免 busy-loop 空转烧 CPU
        if (entries_.empty()) {
            sleepMillis(timeoutMs < 0 ? 1 : timeoutMs);
            return 0;
        }

        // 1) 清空三张位图（fd_set 是“值-结果”参数，select 返回时会被改写）
        FD_ZERO(&readSet_);
        FD_ZERO(&writeSet_);
        FD_ZERO(&exceptSet_);

        // 2) 遍历注册表，把关注的 fd 重新填进位图（每轮必须重建）
        for (const auto& kv : entries_) {
            if (hasEvent(kv.second, PollEvent::Read)) {
                FD_SET(kv.first, &readSet_);
            }
            if (hasEvent(kv.second, PollEvent::Write)) {
                FD_SET(kv.first, &writeSet_);
            }
            FD_SET(kv.first, &exceptSet_);  // 异常始终关注
        }

        // 3) 组装超时：timeoutMs<0 传 nullptr 表示无限等待
        timeval tv;
        timeval* tvPtr = nullptr;
        if (timeoutMs >= 0) {
            tv.tv_sec  = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            tvPtr      = &tv;
        }

        // 4) 调用 select：最多阻塞到超时，返回就绪 fd 总数---关键步骤
        //核心的**系统调用**：从用户态陷入内核态，把三张填好的 fd_set 位图交给内核，让操作系统内核帮我们批量监控所有注册的 socket；
        //当前线程会阻塞休眠，直到有连接就绪、超时、或出错才被唤醒。
        //调用返回时，内核会**直接改写这三张 fd_set 位图**：把「关注列表」覆盖成「就绪列表」，
        //同时返回就绪的 socket 总数。

        const int rc = select(static_cast<int>(maxFd_ + 1), &readSet_, &writeSet_, &exceptSet_, tvPtr);
        if (rc <= 0) {
            // select 在 Windows 被信号中断时也可能报 would-block，按超时处理
            if (rc < 0 && isWouldBlock(lastError())) {
                return 0;
            }
            return rc;
        }

        // 5) 再遍历注册表，用 FD_ISSET 逐个检查谁就绪（线性扫描，连接越多越慢）
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
    std::map<socket_t, PollEvent> entries_;  // 注册表：map 保证遍历顺序稳定，便于调试复现
    fd_set readSet_{};    // 读事件位图
    fd_set writeSet_{};   // 写事件位图
    fd_set exceptSet_{};  // 异常事件位图
    socket_t maxFd_ = 0;  // 最大 fd（select 第一个参数需要 maxfd+1）
};

}  // namespace

// Windows/通用构建的工厂实现（epoll 版本在另一个文件，编译期二选一）
Poller* Poller::create() {
    return new SelectPoller();
}

}  // namespace net
}  // namespace blob
