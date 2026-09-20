// =============================================================================
//  ws_util.h —— WebSocket 握手所需的 SHA-1 与 Base64
//
//  自研实现，不引入 OpenSSL 等外部依赖，保证项目开箱即可编译。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace blob {
namespace net {

// SHA-1（RFC 3174），仅用于 WebSocket 握手计算 Sec-WebSocket-Accept
class Sha1 {
public:
    Sha1();

    void update(const void* data, std::size_t len);
    void final(uint8_t out[20]);

    static std::string hex(const uint8_t digest[20]);

private:
    void processBlock(const uint8_t* block);

    uint32_t h_[5];
    uint64_t totalBits_;
    uint8_t  buffer_[64];
    std::size_t bufferLen_;
};

std::string base64Encode(const uint8_t* data, std::size_t len);

// WebSocket 握手应答值：base64(sha1(key + 魔术字符串))
std::string computeWebSocketAccept(const std::string& clientKey);

}  // namespace net
}  // namespace blob
