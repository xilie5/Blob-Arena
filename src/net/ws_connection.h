// =============================================================================
//  ws_connection.h —— 单条连接的协议状态机
//
//  职责：
//    1. 缓冲入站字节
//    2. 解析 HTTP 请求头（静态资源请求 与 WebSocket 升级请求共用）
//    3. WebSocket 升级握手
//    4. WebSocket 帧编解码（含分片、ping/pong、close）
//
//  不做真实 IO —— 真正的 read/write 由上层事件循环统一处理，
//  这样连接对象是纯粹的数据结构，易于测试。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "net/platform.h"

namespace blob {
namespace net {

struct HttpRequest {
    std::string method;
    std::string path;   // 不含 query
    std::string query;
    std::string version;
    std::string body;
    std::map<std::string, std::string> headers;  // key 统一转小写

    std::string header(const std::string& key) const;
    bool isWebSocketUpgrade() const;
};

class WsConnection {
public:
    enum class HttpStatus {
        NeedMore,
        Done,
        Error,
    };

    explicit WsConnection(socket_t fd);
    ~WsConnection() = default;

    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    socket_t fd() const { return fd_; }

    // ------------------------------ 入站 ------------------------------
    void        appendInput(const char* data, std::size_t len);
    std::size_t bufferedInputSize() const { return in_.size(); }

    HttpStatus parseHttpRequest();
    const HttpRequest& request() const { return request_; }
    bool headerParsed() const { return headerParsed_; }

    // 静态资源响应发送完成后调用，支持同一连接上的后续 HTTP 请求
    void prepareForNextRequest();

    // ------------------------------ WebSocket ------------------------------
    bool acceptWebSocketUpgrade();
    bool isWebSocket() const { return websocket_; }

    // 解码出所有完整消息；closed 置位代表收到 close 帧或协议错误
    bool decodeFrames(std::vector<std::string>& out, bool& closed);

    // ------------------------------ 出站 ------------------------------
    void sendBinary(const std::string& payload);
    void sendText(const std::string& payload);
    void sendPong(const std::string& payload);
    void sendClose(uint16_t code = 1000);

    void sendHttpResponse(int statusCode, const std::string& statusText,
                          const std::string& contentType, const std::string& body);
    void sendHttpNotFound();
    void sendHttpForbidden();

    const std::string& outBuffer() const { return out_; }
    bool               hasPendingOut() const { return !out_.empty(); }
    void               consumeOut(std::size_t n);
    std::size_t        pendingOutSize() const { return out_.size(); }

    // 连接建立时间（毫秒，用于统计连接寿命）
    uint64_t createdAtMs() const { return createdAtMs_; }

private:
    void encodeFrame(uint8_t opcode, const char* data, std::size_t len);

    socket_t    fd_;
    std::string in_;
    std::string out_;

    HttpRequest request_;
    std::size_t headerEnd_    = 0;
    bool        headerParsed_ = false;

    bool        websocket_  = false;
    uint8_t     fragOpcode_ = 0;
    std::string fragBuffer_;

    uint64_t createdAtMs_ = 0;
};

}  // namespace net
}  // namespace blob
