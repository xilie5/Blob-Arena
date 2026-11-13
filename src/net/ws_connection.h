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
//
//  线程归属：每个 WsConnection 只被其永久绑定的那一个 IoReactor 线程访问，
//            因此内部不需要任何锁。
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

/// @brief 解析后的 HTTP 请求
struct HttpRequest {
    std::string method;   ///< 请求方法（GET/POST）
    std::string path;     ///< 请求路径（不含 query，如 /api/login）
    std::string query;    ///< ? 后的查询串（如 limit=20）
    std::string version;  ///< HTTP 版本（如 HTTP/1.1）
    std::string body;     ///< 请求体（POST 的 JSON 等）
    std::map<std::string, std::string> headers;  ///< 请求头（key 统一转小写）

    /// @brief 按名称取请求头（大小写不敏感）；不存在返回空串
    std::string header(const std::string& key) const;

    /// @brief 是否为 WebSocket 升级请求
    /// @details 需同时满足 Upgrade: websocket 且 Connection 头含 upgrade
    bool isWebSocketUpgrade() const;
};

/// @brief 单连接协议状态机：HTTP 解析 + WS 握手 + WS 帧编解码 + 出站缓冲
class WsConnection {
public:
    /// @brief HTTP 解析状态枚举
    enum class HttpStatus {
        NeedMore,  // 数据不完整，等待更多字节（半包）
        Done,      // 一个完整请求已解析完成
        Error,     // 协议错误（应关闭连接）
    };

    /// @brief 构造：绑定 fd 并记录连接建立时刻
    explicit WsConnection(socket_t fd);
    ~WsConnection() = default;

    // 持有连接缓冲且语义上不可复制（与 socket 生命周期一对一）
    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    socket_t fd() const { return fd_; }  ///< 绑定的连接描述符

    // ------------------------------ 入站 ------------------------------
    /// @brief 追加 recv 到的原始字节到入站缓冲
    void        appendInput(const char* data, std::size_t len);
    /// @brief 入站缓冲中尚未解析的字节数
    std::size_t bufferedInputSize() const { return in_.size(); }

    /// @brief 尝试解析一个 HTTP 请求（含按 Content-Length 等请求体）
    /// @return NeedMore（半包）/ Done（完成）/ Error（非法）
    HttpStatus parseHttpRequest();
    const HttpRequest& request() const { return request_; }  ///< 当前解析出的请求
    bool headerParsed() const { return headerParsed_; }      ///< 是否已解析完请求头

    // 静态资源响应发送完成后调用，支持同一连接上的后续 HTTP 请求
    void prepareForNextRequest();

    // ------------------------------ WebSocket ------------------------------
    /// @brief 校验升级请求并把 101 握手响应拼进出站缓冲
    /// @return true=握手响应已生成；false=不是合法升级请求
    bool acceptWebSocketUpgrade();
    bool isWebSocket() const { return websocket_; }  ///< 是否已升级为 WebSocket

    /// @brief 从入站缓冲解码出所有完整 WebSocket 消息
    /// @param[out] out 解出的消息载荷列表
    /// @param[out] closed 是否收到 close 帧/协议错误（调用方应关闭连接）
    /// @return true=正常（可能 closed=true）；false=协议错误
    bool decodeFrames(std::vector<std::string>& out, bool& closed);

    // ------------------------------ 出站 ------------------------------
    void sendBinary(const std::string& payload);  ///< 拼一个二进制帧（opcode 0x2，游戏消息用）
    void sendText(const std::string& payload);    ///< 拼一个文本帧（opcode 0x1）
    void sendPong(const std::string& payload);    ///< 拼一个 pong 帧（回应 ping 保活）
    void sendClose(uint16_t code = 1000);         ///< 拼一个 close 帧（默认正常关闭码）

    /// @brief 拼完整 HTTP 响应（带 Content-Length/keep-alive/no-cache 头）
    void sendHttpResponse(int statusCode, const std::string& statusText,
                          const std::string& contentType, const std::string& body);
    void sendHttpNotFound();   ///< 拼 404 响应
    void sendHttpForbidden();  ///< 拼 403 响应

    const std::string& outBuffer() const { return out_; }  ///< 待发送字节
    bool               hasPendingOut() const { return !out_.empty(); } ///< 是否有积压
    /// @brief 通知已成功 send 掉 n 字节，从缓冲头部删除
    void               consumeOut(std::size_t n);
    std::size_t        pendingOutSize() const { return out_.size(); }    ///< 积压字节数

    // 连接建立时间（毫秒，用于统计连接寿命）
    uint64_t createdAtMs() const { return createdAtMs_; }

private:
    /// @brief 帧编码内部实现（服务端帧不加掩码）
    /// @param opcode 帧类型  @param data 载荷  @param len 载荷长度
    void encodeFrame(uint8_t opcode, const char* data, std::size_t len);

    socket_t    fd_;          ///< 绑定的连接 fd
    std::string in_;          ///< 入站累积缓冲（原始字节，解析后前缀被 erase）
    std::string out_;         ///< 出站缓冲（拼好待 send 的字节）

    HttpRequest request_;     ///< 当前 HTTP 请求
    std::size_t headerEnd_    = 0;  ///< 头部（含请求体）结束位置
    bool        headerParsed_ = false; ///< HTTP 头是否已解析

    bool        websocket_  = false;   ///< 是否已完成 WS 升级
    uint8_t     fragOpcode_ = 0;       ///< 分片消息首帧的 opcode（0x1 文本/0x2 二进制）
    std::string fragBuffer_;           ///< 分片消息累积缓冲

    uint64_t createdAtMs_ = 0;  ///< 连接建立时间戳（steady_clock 毫秒）
};

}  // namespace net
}  // namespace blob
