// =============================================================================
//  protocol.h —— 二进制通信协议
//
//  包结构（小端序）：
//    [u32 totalLength][u16 msgId][u16 seq][payload]
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace blob {
namespace proto {

constexpr uint32_t kHeaderSize = 8;
constexpr uint32_t kMaxPayloadSize = 64 * 1024;

enum class C2S : uint16_t {
    Auth       = 0x0001,  // [str8 token]
    Join       = 0x0010,  // [str8 displayName]
    Resume     = 0x0011,  // [str8 roomCode]
    Input      = 0x0020,
    Ping       = 0x0030,
    Leave      = 0x0031,
    CreateRoom = 0x0040,
    JoinRoom   = 0x0041,
};

enum class S2C : uint16_t {
    AuthOk      = 0x0080,
    JoinOk      = 0x0081,
    EntityAdd   = 0x0082,
    EntityRemove = 0x0083,
    Snapshot    = 0x0084,
    RoomJoined  = 0x008A,
    ResumeOk    = 0x008B,  // [str8 roomCode][u32 entityId][f32 worldW][f32 worldH]
    Pong        = 0x0090,
    Stats       = 0x0091,
    KillFeed    = 0x0092,
    MatchState  = 0x0093,
    MatchResult = 0x0094,
    Error       = 0x00FF,
};

constexpr uint8_t kKindPlayer = 0;
constexpr uint8_t kKindFood   = 1;
constexpr uint8_t kKindBot    = 2;

constexpr uint8_t kRemoveReasonEaten       = 0;
constexpr uint8_t kRemoveReasonLeave       = 1;
constexpr uint8_t kRemoveReasonOutOfBounds = 2;
constexpr uint8_t kRemoveReasonAoi         = 3;

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
    void i8(int8_t v) { u8(static_cast<uint8_t>(v)); }

    void u16(uint16_t v) {
        buf_.push_back(static_cast<char>(v & 0xFFu));
        buf_.push_back(static_cast<char>((v >> 8) & 0xFFu));
    }

    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<char>((v >> (i * 8)) & 0xFFu));
        }
    }

    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

    void f32(float v) {
        uint32_t raw = 0;
        std::memcpy(&raw, &v, sizeof(raw));
        u32(raw);
    }

    void str8(const std::string& value) {
        const std::string clipped =
            value.size() > 255 ? value.substr(0, 255) : value;
        u8(static_cast<uint8_t>(clipped.size()));
        buf_ += clipped;
    }

    const std::string& data() const { return buf_; }
    std::size_t size() const { return buf_.size(); }

private:
    std::string buf_;
};

class Reader {
public:
    Reader(const char* data, std::size_t len)
        : data_(reinterpret_cast<const uint8_t*>(data)), len_(len) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? len_ - offset_ : 0; }

    uint8_t u8() {
        if (!ensure(1)) {
            return 0;
        }
        return data_[offset_++];
    }

    int8_t i8() { return static_cast<int8_t>(u8()); }

    uint16_t u16() {
        if (!ensure(2)) {
            return 0;
        }
        const uint16_t value = static_cast<uint16_t>(data_[offset_]) |
                               (static_cast<uint16_t>(data_[offset_ + 1]) << 8);
        offset_ += 2;
        return value;
    }

    uint32_t u32() {
        if (!ensure(4)) {
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(data_[offset_ + i]) << (i * 8);
        }
        offset_ += 4;
        return value;
    }

    int32_t i32() { return static_cast<int32_t>(u32()); }

    float f32() {
        const uint32_t raw = u32();
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    std::string str8() {
        const uint8_t length = u8();
        if (!ensure(length)) {
            return std::string();
        }
        std::string value(
            reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return value;
    }

private:
    bool ensure(std::size_t count) {
        if (!ok_ || offset_ + count > len_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    std::size_t len_;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

inline std::string packPacket(uint16_t msgId, uint16_t seq,
                              const std::string& payload) {
    std::string out;
    const uint32_t total = static_cast<uint32_t>(kHeaderSize + payload.size());
    out.reserve(total);
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((total >> (i * 8)) & 0xFFu));
    }
    out.push_back(static_cast<char>(msgId & 0xFFu));
    out.push_back(static_cast<char>((msgId >> 8) & 0xFFu));
    out.push_back(static_cast<char>(seq & 0xFFu));
    out.push_back(static_cast<char>((seq >> 8) & 0xFFu));
    out += payload;
    return out;
}

inline bool unpackPacket(const std::string& buffer, std::size_t& offset,
                         uint16_t& msgId, uint16_t& seq,
                         std::string& payload) {
    if (buffer.size() - offset < kHeaderSize) {
        return false;
    }
    const uint8_t* data =
        reinterpret_cast<const uint8_t*>(buffer.data()) + offset;
    uint32_t total = 0;
    for (int i = 0; i < 4; ++i) {
        total |= static_cast<uint32_t>(data[i]) << (i * 8);
    }
    if (total < kHeaderSize || total > kMaxPayloadSize) {
        ++offset;
        return false;
    }
    if (buffer.size() - offset < total) {
        return false;
    }
    msgId = static_cast<uint16_t>(data[4] | (static_cast<uint16_t>(data[5]) << 8));
    seq = static_cast<uint16_t>(data[6] | (static_cast<uint16_t>(data[7]) << 8));
    payload.assign(buffer, offset + kHeaderSize, total - kHeaderSize);
    offset += total;
    return true;
}

}  // namespace proto
}  // namespace blob
