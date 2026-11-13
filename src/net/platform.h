// =============================================================================
//  platform.h —— 跨平台 socket 兼容层
//
//  让同一套网络代码同时跑在 Windows（本地开发）与 Linux（线上部署）。
//  事件多路复用由 poller_select / poller_epoll 两个实现分别提供。
//
//  本层只做差异收口（类型、头文件、错误码、系统调用名字），
//  上层（IoReactor/WsConnection）只写一份平台无关的代码。
// =============================================================================
#pragma once

#include <string>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN   // 裁剪 windows.h 中不用的部分，加快编译
  #endif
  // 声明 inet_ntop / inet_pton 等 Vista+ 接口（MinGW 需要显式指定）
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601   // 目标 Windows 7+ 目前工业界的主流最低兼容版本：
  #endif                         //既保证了所有现代 Socket API 可用，又覆盖了绝大多数本地开发环境。
  // select() 默认只支持 64 个 fd，本地开发放宽到 1024；
  // Linux 部署走 epoll，不受此限制。
  #ifndef FD_SETSIZE
    #define FD_SETSIZE 1024
  #endif
  #include <winsock2.h>   // 必须在 windows.h 之前包含，防止 winsock1 冲突 ---Winsock 核心 API，socket、bind、listen、send、recv 等基础函数；
  #include <ws2tcpip.h>   // TCP/IP 扩展头，提供 `TCP_NODELAY` 等选项常量、`sockaddr_storage` 通用地址结构、`getaddrinfo` 等扩展函数。

  // Windows 下的 socket 不是普通整数，而是 **`SOCKET` 类型**—— 本质是 `UINT_PTR`（无符号指针大小的整数），是内核对象的句柄，和文件描述符不是一回事。
  using socket_t = SOCKET;
  #define BLOB_SOCKET_ERROR SOCKET_ERROR // Windows 下的 socket 错误码是 `SOCKET_ERROR`，不是 -1
  #define BLOB_INVALID_SOCKET INVALID_SOCKET // Windows 下的无效 socket 句柄是 `INVALID_SOCKET`，不是 -1
#else
  // POSIX（Linux/macOS）相关系统头文件
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>

  // Linux 上 socket 就是普通文件描述符（一个 int）
  using socket_t = int;
  #define BLOB_SOCKET_ERROR (-1)
  #define BLOB_INVALID_SOCKET (-1)
#endif

namespace blob {
namespace net {

// 进程级初始化：Windows 需要 WSAStartup，Linux 为空实现
bool initialize();
void shutdown();

int  lastError();               // 取最近一次网络错误码
bool isWouldBlock(int err);     // 错误码是否表示“现在无法立即完成”（非阻塞重试依据）

bool setNonBlocking(socket_t fd); // 把 socket 设为非阻塞（epoll ET 的必要前提）
bool setNoDelay(socket_t fd);     // 开启 TCP_NODELAY，关闭 Nagle，小包即时发出

int  sendFlags();                 // Linux 下带 MSG_NOSIGNAL，避免写已关闭连接导致进程退出
int  closeSocket(socket_t fd);    // 关闭连接；返回 0 成功

std::string peerAddress(socket_t fd); // 取对端 IP:端口（仅日志用）
void        sleepMillis(int ms);      // 毫秒级睡眠（select 空转时让出 CPU）

}  // namespace net
}  // namespace blob
