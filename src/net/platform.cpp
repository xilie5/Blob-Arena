#include "net/platform.h"

#include <cstdio>

#ifdef _WIN32
  // timeBeginPeriod：把 Windows 系统定时器分辨率从默认 ~15.6ms 提到 1ms。
  // 否则 select() 节拍会整段睡过头（30Hz 的 33ms 帧被拉到 ~47ms，
  // 靠 catch-up 连发补帧），帧间隔 P95 指标失真（QA 压测阶段实测发现）。
  // 注：MinGW 无 timeapi.h，声明在 mmsystem.h（需先让 winsock2.h 拉 windows.h）。
  #include <mmsystem.h>
#endif

namespace blob {
namespace net {

bool initialize() {
#ifdef _WIN32
    // Windows 使用 winsock 前必须先 WSAStartup 协商版本（2.2）--MAKEWORD(主版本号, 副版本号)
    // /WSADATA是什么：输出结构体，调用后系统会把当前 Winsock 的实际信息填进去：
    // 1. 系统实际支持的最高版本；
    // 2. 系统支持的 Winsock 描述；
    // 3. 最多支持的 socket 数量、最大数据报长度等。
    WSADATA wsaData;
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != 0) {
        return false;
    }
    // 失败不致命：只是回到默认粒度（帧率抖动，逻辑帧率仍由 catch-up 保证）
    timeBeginPeriod(1);
    return true;
#else
    // Linux 无需进程级网络初始化
    return true;
#endif
}

void shutdown() {
#ifdef _WIN32
    timeEndPeriod(1);   // 恢复系统定时器精度
    WSACleanup();       // 释放 winsock 资源
#endif
}

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool isWouldBlock(int err) {
    // 非阻塞 socket 在“无数据可读/发送缓冲暂满”时返回的不是致命错误，
    // 事件循环据此停止本轮读/写，等待下一次就绪通知。
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

bool setNonBlocking(socket_t fd) {
#ifdef _WIN32
    // Windows：ioctlsocket + FIONBIO 切换阻塞模式
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    // Linux：先取当前 flag，再位或 O_NONBLOCK 写回（保留其他 flag）
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool setNoDelay(socket_t fd) {
    // 关闭 Nagle 算法：游戏指令是高频小包，不能为“凑大包”而延迟发出
    int enable = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&enable),
                      static_cast<int>(sizeof(enable))) == 0;
}

int sendFlags() {
#ifdef _WIN32
    return 0;
#else
    // Linux 默认向已关闭的 socket 写数据会触发 SIGPIPE，其默认动作是杀死进程；
    // MSG_NOSIGNAL 让 send 只返回错误，不发信号。
    return MSG_NOSIGNAL;
#endif
}

int closeSocket(socket_t fd) {
#ifdef _WIN32
    return closesocket(fd);
#else
    return close(fd);
#endif
}

std::string peerAddress(socket_t fd) {
    sockaddr_storage addr;
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "unknown";
    }

    // 说明：部分 MinGW 运行库未声明 inet_ntop，这里用 inet_ntoa 处理 IPv4，
    // IPv6 仅记录端口（日志用，不影响功能）。POSIX 平台使用线程安全的 inet_ntop。
    if (addr.ss_family == AF_INET) {
        const sockaddr_in* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        char host[64] = {0};
#ifdef _WIN32
        const char* text = inet_ntoa(v4->sin_addr);
        if (text != nullptr) {
            std::snprintf(host, sizeof(host), "%s", text);
        }
#else
        inet_ntop(AF_INET, &v4->sin_addr, host, sizeof(host));
#endif
        // 端口是网络字节序，ntohs 转主机字节序
        return std::string(host) + ":" + std::to_string(ntohs(v4->sin_port));
    }

    if (addr.ss_family == AF_INET6) {
        const sockaddr_in6* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        return std::string("ipv6:") + std::to_string(ntohs(v6->sin6_port));
    }
    return "unknown";
}

void sleepMillis(int ms) {
    if (ms <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep(static_cast<DWORD>(ms));
#else
    usleep(static_cast<useconds_t>(ms) * 1000); // usleep 单位是微秒
#endif
}

}  // namespace net
}  // namespace blob
