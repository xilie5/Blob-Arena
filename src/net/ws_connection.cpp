#include "net/ws_connection.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include "net/ws_util.h"

namespace blob {
namespace net {

namespace {

// HTTP 头部最大字节数，超过视为攻击/畸形请求
constexpr std::size_t kMaxHttpHeaderBytes = 16 * 1024;
// 单个 WebSocket 帧载荷上限（1MB），防止超大帧耗尽内存
constexpr std::size_t kMaxFramePayload     = 1024 * 1024;

/// 字符串转小写（HTTP 头名称大小写不敏感）
std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// 去掉首尾空格/制表符/回车（解析 HTTP 头值用）
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

/// 当前单调时钟的毫秒数（连接建立时间戳用）
uint64_t nowMillis() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

std::string HttpRequest::header(const std::string& key) const {
    // 存储时 key 已统一小写，查询时也把入参转小写
    auto it = headers.find(toLower(key));
    if (it == headers.end()) {
        return std::string();
    }
    return it->second;
}

bool HttpRequest::isWebSocketUpgrade() const {
    const std::string upgrade = toLower(header("upgrade"));
    const std::string conn    = toLower(header("connection"));
    // 两个条件同时成立才是 WebSocket 升级握手
    return upgrade == "websocket" && conn.find("upgrade") != std::string::npos;
}

// ---------------------------------------------------------------------------
// WsConnection
// ---------------------------------------------------------------------------

WsConnection::WsConnection(socket_t fd) : fd_(fd) {
    createdAtMs_ = nowMillis();
}

void WsConnection::appendInput(const char* data, std::size_t len) {
    in_.append(data, len);  // 仅累积，不解析（解析由事件循环显式驱动）
}

void WsConnection::consumeOut(std::size_t n) {
    // send 部分成功后，把已发送的前缀删掉；全部发完直接 clear（O(1) 常见路径）
    if (n >= out_.size()) {
        out_.clear();
        return;
    }
    out_.erase(0, n);
}

// ------------------------------ HTTP 解析 ------------------------------

WsConnection::HttpStatus WsConnection::parseHttpRequest() {
    if (headerParsed_) {
        return HttpStatus::Done;  // 已解析过，等待 prepareForNextRequest 复位
    }

    // HTTP 请求头以空行 \r\n\r\n 结束；找不到说明头还没收全（半包）
    const std::size_t pos = in_.find("\r\n\r\n");
    if (pos == std::string::npos) {
        if (in_.size() > kMaxHttpHeaderBytes) {
            return HttpStatus::Error;  // 头超大：防护
        }
        return HttpStatus::NeedMore;
    }

    const std::size_t end = pos + 4;  // 包含空行的 4 字节
    const std::string head = in_.substr(0, pos);

    request_ = HttpRequest();

    // 逐行解析：第一行是请求行，其余每行是 "Key: Value"
    std::size_t lineStart = 0;
    bool firstLine = true;
    while (lineStart <= head.size()) {
        const std::size_t lineEnd = head.find("\r\n", lineStart);
        const std::string line =
            (lineEnd == std::string::npos) ? head.substr(lineStart)
                                           : head.substr(lineStart, lineEnd - lineStart);

        if (firstLine) {
            firstLine = false;
            // 请求行：METHOD SP TARGET SP VERSION
            std::size_t sp1 = line.find(' ');
            if (sp1 == std::string::npos) {
                return HttpStatus::Error;
            }
            std::size_t sp2 = line.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) {
                return HttpStatus::Error;
            }
            request_.method  = line.substr(0, sp1);
            const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            request_.version = trim(line.substr(sp2 + 1));

            // target 拆成 path 与 query（以 ? 分界）
            const std::size_t qm = target.find('?');
            if (qm == std::string::npos) {
                request_.path = target;
            } else {
                request_.path  = target.substr(0, qm);
                request_.query = target.substr(qm + 1);
            }
        } else if (!line.empty()) {
            // 头部行：第一个冒号前是 key（转小写），后面是 value（去空白）
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string key = toLower(trim(line.substr(0, colon)));
                const std::string val = trim(line.substr(colon + 1));
                if (!key.empty()) {
                    request_.headers[key] = val;
                }
            }
        }

        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 2;
    }

    // 按 Content-Length 等请求体（POST /api/login 的 JSON 在这里）
    std::size_t contentLength = 0;
    const std::string contentLengthText = request_.header("content-length");
    if (!contentLengthText.empty()) {
        try {
            contentLength = static_cast<std::size_t>(
                std::stoull(contentLengthText));
        } catch (...) {
            return HttpStatus::Error;  // 长度字段不是数字
        }
    }
    if (contentLength > kMaxHttpHeaderBytes) {
        return HttpStatus::Error;
    }
    if (in_.size() < end + contentLength) {
        return HttpStatus::NeedMore;  // 请求体还没到齐
    }

    // 请求完整：记录结束位置、取请求体，并从入站缓冲删掉已消费字节
    // （剩余字节可能是 keep-alive 连接上的下一个请求）
    headerEnd_    = end;
    headerParsed_ = true;
    request_.body = in_.substr(end, contentLength);
    in_.erase(0, end + contentLength);
    return HttpStatus::Done;
}

void WsConnection::prepareForNextRequest() {
    if (websocket_) {
        return;  // WebSocket 升级后没有“下一个 HTTP 请求”，不可复位
    }
    request_      = HttpRequest();
    headerEnd_    = 0;
    headerParsed_ = false;
}

// ------------------------------ 握手 ------------------------------

bool WsConnection::acceptWebSocketUpgrade() {
    if (!headerParsed_ || !request_.isWebSocketUpgrade()) {
        return false;
    }

    const std::string key = request_.header("sec-websocket-key");
    if (key.empty()) {
        return false;  // 缺少客户端随机 key，无法完成握手
    }

    // 按 RFC 6455 计算应答值
    const std::string accept = computeWebSocketAccept(key);

    // 拼 101 Switching Protocols 握手响应
    std::string response;
    response += "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + accept + "\r\n";
    response += "\r\n";
    out_ += response;

    websocket_    = true;   // 此后入站字节按 WS 帧解析
    headerParsed_ = false;
    return true;
}

// ------------------------------ 帧解码 ------------------------------

bool WsConnection::decodeFrames(std::vector<std::string>& out, bool& closed) {
    closed = false;

    std::size_t pos = 0;
    const std::size_t size = in_.size();
    const uint8_t* data = reinterpret_cast<const uint8_t*>(in_.data());

    // 循环解析尽可能多的完整帧；不完整就跳出保留字节等下次（半包）
    while (true) {
        if (size - pos < 2) {
            break;  // 连 2 字节帧头都不够
        }
        // 帧第 1 字节：最高位 FIN（是否最后一片），低 4 位 opcode
        const uint8_t b0     = data[pos];
        const uint8_t b1     = data[pos + 1];
        const bool    fin    = (b0 & 0x80u) != 0;
        const uint8_t opcode = b0 & 0x0Fu;
        // 帧第 2 字节：最高位 MASK（客户端帧必须掩码），低 7 位是载荷长度
        const bool    masked = (b1 & 0x80u) != 0;

        uint64_t    payloadLen = b1 & 0x7Fu;
        std::size_t headerLen  = 2;

        // 长度 126：随后 2 字节才是真实长度；127：随后 8 字节
        if (payloadLen == 126u) {
            if (size - pos < 4) {
                break;
            }
            payloadLen = (static_cast<uint64_t>(data[pos + 2]) << 8) |
                         static_cast<uint64_t>(data[pos + 3]);
            headerLen = 4;
        } else if (payloadLen == 127u) {
            if (size - pos < 10) {
                break;
            }
            payloadLen = 0;
            for (int i = 0; i < 8; ++i) {
                payloadLen = (payloadLen << 8) | static_cast<uint64_t>(data[pos + 2 + i]);
            }
            headerLen = 10;
        }

        // 掩码占 4 字节（紧跟长度字段之后、载荷之前）
        std::size_t maskOffset = 0;
        if (masked) {
            if (size - pos < headerLen + 4) {
                break;
            }
            maskOffset = headerLen;
            headerLen += 4;
        }

        if (payloadLen > kMaxFramePayload) {
            closed = true;   // 超大帧：协议错误，关闭
            return false;
        }
        if (size - pos < headerLen + static_cast<std::size_t>(payloadLen)) {
            break;  // 载荷没收全：半包
        }

        // 拷出载荷；客户端帧必须按 4 字节掩码循环异或还原
        std::string payload(
            reinterpret_cast<const char*>(data + pos + headerLen),
            static_cast<std::size_t>(payloadLen));
        if (masked) {
            const uint8_t* maskKey = data + pos + maskOffset;
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(
                    static_cast<uint8_t>(payload[i]) ^ maskKey[i % 4]);
            }
        }
        pos += headerLen + static_cast<std::size_t>(payloadLen);

        // 按 opcode 分发
        switch (opcode) {
            case 0x0:  // continuation 续帧：拼到当前分片消息后面
                fragBuffer_ += payload;
                if (fin) {
                    // 最后一片到达：按首帧类型作为完整消息产出
                    if (fragOpcode_ == 0x1 || fragOpcode_ == 0x2) {
                        out.push_back(fragBuffer_);
                    }
                    fragBuffer_.clear();
                    fragOpcode_ = 0;
                }
                break;
            case 0x1:  // text 文本
            case 0x2:  // binary 二进制（游戏业务包）
                if (fin) {
                    out.push_back(std::move(payload));  // 单帧完整消息
                } else {
                    // 分片消息的第一片：记住类型并开始累积
                    fragOpcode_ = opcode;
                    fragBuffer_ = std::move(payload);
                }
                break;
            case 0x8:  // close：通知上层关闭连接
                closed = true;
                break;
            case 0x9:  // ping：协议要求必须立即回 pong（保活机制）
                sendPong(payload);
                break;
            case 0xA:  // pong：心跳应答，忽略
                break;
            default:
                closed = true;  // 未知 opcode：协议错误
                return false;
        }

        if (closed) {
            break;
        }
    }

    // 丢掉已解析的前缀（一次处理多帧，剩余的留给下次；天然解决帧的粘包）
    in_.erase(0, pos);
    return true;
}

// ------------------------------ 帧编码 ------------------------------

void WsConnection::encodeFrame(uint8_t opcode, const char* data, std::size_t len) {
    // 服务端发出的帧不加掩码（RFC 6455 规定）；最高位固定 1 表示 FIN
    out_.push_back(static_cast<char>(0x80u | opcode));

    // 长度编码三档：<126 直接写；<65536 写 126+2字节；否则写 127+8字节（大端序）
    if (len < 126) {
        out_.push_back(static_cast<char>(len));
    } else if (len < 65536) {
        out_.push_back(static_cast<char>(126));
        out_.push_back(static_cast<char>((len >> 8) & 0xFFu));
        out_.push_back(static_cast<char>(len & 0xFFu));
    } else {
        out_.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out_.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFFu));
        }
    }

    if (len > 0) {
        out_.append(data, len);
    }
}

void WsConnection::sendBinary(const std::string& payload) {
    encodeFrame(0x2, payload.data(), payload.size());
}

void WsConnection::sendText(const std::string& payload) {
    encodeFrame(0x1, payload.data(), payload.size());
}

void WsConnection::sendPong(const std::string& payload) {
    encodeFrame(0xA, payload.data(), payload.size());
}

void WsConnection::sendClose(uint16_t code) {
    // close 帧带 2 字节大端序关闭码
    char buf[2];
    buf[0] = static_cast<char>((code >> 8) & 0xFFu);
    buf[1] = static_cast<char>(code & 0xFFu);
    encodeFrame(0x8, buf, 2);
}

// ------------------------------ HTTP 响应 ------------------------------

void WsConnection::sendHttpResponse(int statusCode, const std::string& statusText,
                                    const std::string& contentType,
                                    const std::string& body) {
    // 手工拼 HTTP/1.1 响应（无第三方 HTTP 库）
    std::string response;
    response += "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n";
    response += "Server: blob-arena\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Cache-Control: no-cache\r\n";     // 接口/页面不缓存，便于联调
    response += "Connection: keep-alive\r\n";      // 支持同连接发后续请求
    response += "\r\n";                            // 头结束空行
    response += body;
    out_ += response;
}

void WsConnection::sendHttpNotFound() {
    sendHttpResponse(404, "Not Found", "text/plain; charset=utf-8", "404 Not Found");
}

void WsConnection::sendHttpForbidden() {
    sendHttpResponse(403, "Forbidden", "text/plain; charset=utf-8", "403 Forbidden");
}

}  // namespace net
}  // namespace blob
