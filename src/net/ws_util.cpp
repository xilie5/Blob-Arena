#include "net/ws_util.h"

#include <cstring>

namespace blob {
namespace net {

namespace {

inline uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

}  // namespace

Sha1::Sha1() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    totalBits_ = 0;
    bufferLen_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha1::update(const void* data, std::size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    totalBits_ += static_cast<uint64_t>(len) * 8u;

    while (len > 0) {
        const std::size_t space = 64 - bufferLen_;
        const std::size_t take  = (len < space) ? len : space;
        std::memcpy(buffer_ + bufferLen_, p, take);
        bufferLen_ += take;
        p          += take;
        len        -= take;

        if (bufferLen_ == 64) {
            processBlock(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Sha1::processBlock(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h_[0];
    uint32_t b = h_[1];
    uint32_t c = h_[2];
    uint32_t d = h_[3];
    uint32_t e = h_[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::final(uint8_t out[20]) {
    const uint64_t bitLen = totalBits_;

    const uint8_t pad0 = 0x80;
    update(&pad0, 1);
    const uint8_t zero = 0x00;
    while (bufferLen_ != 56) {
        update(&zero, 1);
    }

    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<uint8_t>((bitLen >> (56 - i * 8)) & 0xFFu);
    }
    // 直接写入最后一格，避免再触发 processBlock 干扰长度
    std::memcpy(buffer_ + 56, lenBytes, 8);
    processBlock(buffer_);
    bufferLen_ = 0;

    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((h_[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFFu);
        out[i * 4 + 3] = static_cast<uint8_t>((h_[i]) & 0xFFu);
    }
}

std::string Sha1::hex(const uint8_t digest[20]) {
    static const char* kTable = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 20; ++i) {
        out.push_back(kTable[(digest[i] >> 4) & 0x0Fu]);
        out.push_back(kTable[digest[i] & 0x0Fu]);
    }
    return out;
}

std::string base64Encode(const uint8_t* data, std::size_t len) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 2 < len) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           (static_cast<uint32_t>(data[i + 2]));
        out.push_back(kTable[(n >> 18) & 0x3Fu]);
        out.push_back(kTable[(n >> 12) & 0x3Fu]);
        out.push_back(kTable[(n >> 6) & 0x3Fu]);
        out.push_back(kTable[n & 0x3Fu]);
        i += 3;
    }

    if (i + 1 == len) {
        const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 0x3Fu]);
        out.push_back(kTable[(n >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == len) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 0x3Fu]);
        out.push_back(kTable[(n >> 12) & 0x3Fu]);
        out.push_back(kTable[(n >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

std::string computeWebSocketAccept(const std::string& clientKey) {
    static const char* kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string joined = clientKey + kMagic;

    Sha1 sha;
    sha.update(joined.data(), joined.size());
    uint8_t digest[20];
    sha.final(digest);

    return base64Encode(digest, 20);
}

}  // namespace net
}  // namespace blob
