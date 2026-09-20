#include "net/ws_connection.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include "net/ws_util.h"

namespace blob {
namespace net {

namespace {

constexpr std::size_t kMaxHttpHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxFramePayload     = 1024 * 1024;

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

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
    auto it = headers.find(toLower(key));
    if (it == headers.end()) {
        return std::string();
    }
    return it->second;
}

bool HttpRequest::isWebSocketUpgrade() const {
    const std::string upgrade = toLower(header("upgrade"));
    const std::string conn    = toLower(header("connection"));
    return upgrade == "websocket" && conn.find("upgrade") != std::string::npos;
}

// ---------------------------------------------------------------------------
// WsConnection
// ---------------------------------------------------------------------------

WsConnection::WsConnection(socket_t fd) : fd_(fd) {
    createdAtMs_ = nowMillis();
}

void WsConnection::appendInput(const char* data, std::size_t len) {
    in_.append(data, len);
}

void WsConnection::consumeOut(std::size_t n) {
    if (n >= out_.size()) {
        out_.clear();
        return;
    }
    out_.erase(0, n);
}

// ------------------------------ HTTP 解析 ------------------------------

WsConnection::HttpStatus WsConnection::parseHttpRequest() {
    if (headerParsed_) {
        return HttpStatus::Done;
    }

    const std::size_t pos = in_.find("\r\n\r\n");
    if (pos == std::string::npos) {
        if (in_.size() > kMaxHttpHeaderBytes) {
            return HttpStatus::Error;
        }
        return HttpStatus::NeedMore;
    }

    const std::size_t end = pos + 4;
    const std::string head = in_.substr(0, pos);

    request_ = HttpRequest();

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

            const std::size_t qm = target.find('?');
            if (qm == std::string::npos) {
                request_.path = target;
            } else {
                request_.path  = target.substr(0, qm);
                request_.query = target.substr(qm + 1);
            }
        } else if (!line.empty()) {
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

    std::size_t contentLength = 0;
    const std::string contentLengthText = request_.header("content-length");
    if (!contentLengthText.empty()) {
        try {
            contentLength = static_cast<std::size_t>(
                std::stoull(contentLengthText));
        } catch (...) {
            return HttpStatus::Error;
        }
    }
    if (contentLength > kMaxHttpHeaderBytes) {
        return HttpStatus::Error;
    }
    if (in_.size() < end + contentLength) {
        return HttpStatus::NeedMore;
    }

    headerEnd_    = end;
    headerParsed_ = true;
    request_.body = in_.substr(end, contentLength);
    in_.erase(0, end + contentLength);
    return HttpStatus::Done;
}

void WsConnection::prepareForNextRequest() {
    if (websocket_) {
        return;
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
        return false;
    }

    const std::string accept = computeWebSocketAccept(key);

    std::string response;
    response += "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + accept + "\r\n";
    response += "\r\n";
    out_ += response;

    websocket_    = true;
    headerParsed_ = false;
    return true;
}

// ------------------------------ 帧解码 ------------------------------

bool WsConnection::decodeFrames(std::vector<std::string>& out, bool& closed) {
    closed = false;

    std::size_t pos = 0;
    const std::size_t size = in_.size();
    const uint8_t* data = reinterpret_cast<const uint8_t*>(in_.data());

    while (true) {
        if (size - pos < 2) {
            break;
        }
        const uint8_t b0     = data[pos];
        const uint8_t b1     = data[pos + 1];
        const bool    fin    = (b0 & 0x80u) != 0;
        const uint8_t opcode = b0 & 0x0Fu;
        const bool    masked = (b1 & 0x80u) != 0;

        uint64_t    payloadLen = b1 & 0x7Fu;
        std::size_t headerLen  = 2;

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

        std::size_t maskOffset = 0;
        if (masked) {
            if (size - pos < headerLen + 4) {
                break;
            }
            maskOffset = headerLen;
            headerLen += 4;
        }

        if (payloadLen > kMaxFramePayload) {
            closed = true;
            return false;
        }
        if (size - pos < headerLen + static_cast<std::size_t>(payloadLen)) {
            break;
        }

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

        switch (opcode) {
            case 0x0:  // continuation
                fragBuffer_ += payload;
                if (fin) {
                    if (fragOpcode_ == 0x1 || fragOpcode_ == 0x2) {
                        out.push_back(fragBuffer_);
                    }
                    fragBuffer_.clear();
                    fragOpcode_ = 0;
                }
                break;
            case 0x1:  // text
            case 0x2:  // binary
                if (fin) {
                    out.push_back(std::move(payload));
                } else {
                    fragOpcode_ = opcode;
                    fragBuffer_ = std::move(payload);
                }
                break;
            case 0x8:  // close
                closed = true;
                break;
            case 0x9:  // ping -> pong
                sendPong(payload);
                break;
            case 0xA:  // pong，忽略
                break;
            default:
                closed = true;
                return false;
        }

        if (closed) {
            break;
        }
    }

    in_.erase(0, pos);
    return true;
}

// ------------------------------ 帧编码 ------------------------------

void WsConnection::encodeFrame(uint8_t opcode, const char* data, std::size_t len) {
    // 服务端发出的帧不加掩码（RFC 6455 规定）
    out_.push_back(static_cast<char>(0x80u | opcode));

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
    char buf[2];
    buf[0] = static_cast<char>((code >> 8) & 0xFFu);
    buf[1] = static_cast<char>(code & 0xFFu);
    encodeFrame(0x8, buf, 2);
}

// ------------------------------ HTTP 响应 ------------------------------

void WsConnection::sendHttpResponse(int statusCode, const std::string& statusText,
                                    const std::string& contentType,
                                    const std::string& body) {
    std::string response;
    response += "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n";
    response += "Server: blob-arena\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Cache-Control: no-cache\r\n";
    response += "Connection: keep-alive\r\n";
    response += "\r\n";
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
