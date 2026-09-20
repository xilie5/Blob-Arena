// =============================================================================
//  platform.h —— 跨平台 socket 兼容层
//
//  让同一套网络代码同时跑在 Windows（本地开发）与 Linux（线上部署）。
//  事件多路复用由 poller_select / poller_epoll 两个实现分别提供。
// =============================================================================
#pragma once

#include <string>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  // 声明 inet_ntop / inet_pton 等 Vista+ 接口（MinGW 需要显式指定）
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
  // select() 默认只支持 64 个 fd，本地开发放宽到 1024；
  // Linux 部署走 epoll，不受此限制。
  #ifndef FD_SETSIZE
    #define FD_SETSIZE 1024
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>

  using socket_t = SOCKET;
  #define BLOB_SOCKET_ERROR SOCKET_ERROR
  #define BLOB_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>

  using socket_t = int;
  #define BLOB_SOCKET_ERROR (-1)
  #define BLOB_INVALID_SOCKET (-1)
#endif

namespace blob {
namespace net {

// 进程级初始化：Windows 需要 WSAStartup，Linux 为空实现
bool initialize();
void shutdown();

int  lastError();
bool isWouldBlock(int err);

bool setNonBlocking(socket_t fd);
bool setNoDelay(socket_t fd);

int  sendFlags();                 // Linux 下带 MSG_NOSIGNAL，避免写已关闭连接导致进程退出
int  closeSocket(socket_t fd);    // 返回 0 成功

std::string peerAddress(socket_t fd);
void        sleepMillis(int ms);

}  // namespace net
}  // namespace blob
