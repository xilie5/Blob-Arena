// =============================================================================
//  ws_util.h —— WebSocket 握手所需的 SHA-1 与 Base64
//
//  自研实现，不引入 OpenSSL 等外部依赖，保证项目开箱即可编译。
//
//  用途（RFC 6455 握手规则）：
//    客户端握手时带一个随机 Sec-WebSocket-Key，服务端必须返回
//    Sec-WebSocket-Accept = base64( sha1( key + 固定魔术串 ) )
//    以此证明服务器理解 WebSocket 协议，而不是随便回了个 HTTP。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace blob {
namespace net {

/// @brief SHA-1 摘要计算器（RFC 3174，输出 20 字节摘要）
/// @note 仅用于 WebSocket 握手。update 可分多次喂数据，最后 final 出结果。
class Sha1 {
public:
    Sha1();

    /// @brief 喂入待摘要数据（可多次调用，内部按 64 字节分块缓存处理）
    void update(const void* data, std::size_t len);

    /// @brief 收尾并输出 20 字节摘要
    /// @param out 至少 20 字节的输出缓冲
    void final(uint8_t out[20]);

    /// @brief 把 20 字节摘要编码成 40 个十六进制字符（调试用）
    static std::string hex(const uint8_t digest[20]);

private:
    /// @brief 处理一个完整的 64 字节分组（SHA-1 的 80 轮压缩函数）
    void processBlock(const uint8_t* block);

    uint32_t h_[5];            ///< 5 个 32 位哈希状态字
    uint64_t totalBits_;       ///< 已喂入的总比特数（最终 padding 用）
    uint8_t  buffer_[64];      ///< 不足 64 字节的残留缓冲
    std::size_t bufferLen_;    ///< 残留长度
};

/// @brief Base64 编码（3 字节 -> 4 个可打印字符，不足补 '='）
std::string base64Encode(const uint8_t* data, std::size_t len);

/// @brief 计算 WebSocket 握手应答值
/// @param clientKey 请求头中的 Sec-WebSocket-Key
/// @return 应写入 Sec-WebSocket-Accept 响应头的字符串
std::string computeWebSocketAccept(const std::string& clientKey);

}  // namespace net
}  // namespace blob
