// =============================================================================
//  protocol.h —— 二进制通信协议
//
//  包结构（小端序）：
//    [u32 totalLength][u16 msgId][u16 seq][payload]
//
//  为什么用自定义二进制而不是 JSON：
//    游戏消息频率高（移动 30Hz、快照 20Hz），二进制包体积极小、解析极快。
//
//  粘包/半包问题：
//    TCP 是无边界的字节流，两条消息可能一次到达（粘包），一条也可能分两次
//    到达（半包）。本协议用“u32 总长度”作定长前缀：接收方先读够 8 字节头，
//    再按 totalLength 读整包，并在缓冲中循环切分，即可同时处理两种情况。
//
//  小端序：
//    所有多字节整数都手动逐字节拼装（低位字节在前），不依赖机器自身字节序，
//    保证换任何 CPU 架构（x86/ARM）编解码结果一致。
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace blob {
namespace proto {

constexpr uint32_t kHeaderSize = 8;          ///< 包头固定长度：4(totalLength)+2(msgId)+2(seq)
constexpr uint32_t kMaxPayloadSize = 64 * 1024;  ///< 单包最大 64KB（含头），防恶意大包

/// @brief 客户端 -> 服务端的消息号（C2S）
enum class C2S : uint16_t {
    Auth       = 0x0001,  // [str8 token] 鉴权：携带登录拿到的 Session Token
    Join       = 0x0010,  // [str8 displayName] 快速匹配（注：实际用户名取自会话）
    Resume     = 0x0011,  // [str8 roomCode] 断线重连，凭房间码恢复原实体
    Input      = 0x0020,  // [u32 seq][i8 dx][i8 dy] 移动方向，分量 -127~127
    Ping       = 0x0030,  // [u32 clientTime] 心跳/往返时延测量
    Leave      = 0x0031,  // 主动离开房间
    CreateRoom = 0x0040,  // 创建私有房间（返回 4 位房间码）
    JoinRoom   = 0x0041,  // [str8 roomCode] 凭房间码加入私房
};

/// @brief 服务端 -> 客户端的消息号（S2C）
enum class S2C : uint16_t {
    AuthOk      = 0x0080,  // 鉴权成功
    JoinOk      = 0x0081,  // [u32 entityId][f32 worldW][f32 worldH][u16 tick][u32 color][u32 seed]
    EntityAdd   = 0x0082,  // 实体进入玩家 AOI 视野（增量下发完整实体信息）
    EntityRemove = 0x0083, // [u32 id][u8 reason] 实体离开视野/被吃/离场
    Snapshot    = 0x0084,  // [u32 tick][u16 n][n * (id,x,y,radius,score)] 变化实体快照
    RoomJoined  = 0x008A,  // [str8 code][f32 w][f32 h][u8 max][u8 public] 房间信息
    ResumeOk    = 0x008B,  // [str8 roomCode][u32 entityId][f32 worldW][f32 worldH]
    Pong        = 0x0090,  // [u32 clientTime][u32 serverTime]
    Stats       = 0x0091,  // [u16 players][u16 entities][u16 tickRate] 全局统计
    KillFeed    = 0x0092,  // [u32 killer][u32 victim] 击杀播报
    MatchState  = 0x0093,  // [u8 phase][u16 remain][u16 players][f32 shrinkRadius]
    MatchResult = 0x0094,  // [u16 n][n * 排名记录] 对局最终排名
    Error       = 0x00FF,  // [u8 code][str8 message] 业务错误
};

// ---- 实体种类（与 EntityKind 数值一致，供协议层使用） ----
constexpr uint8_t kKindPlayer = 0;  ///< 真人玩家
constexpr uint8_t kKindFood   = 1;  ///< 食物（静止、可吃）
constexpr uint8_t kKindBot    = 2;  ///< NPC 机器人

// ---- 实体移除原因（EntityRemove.payload 第二字节） ----
constexpr uint8_t kRemoveReasonEaten       = 0;  // 被吞噬
constexpr uint8_t kRemoveReasonLeave       = 1;  // 主动离开/超时清理
constexpr uint8_t kRemoveReasonOutOfBounds = 2;  // 缩圈时圈外消亡
constexpr uint8_t kRemoveReasonAoi         = 3;  // 仅离开玩家视野（实体仍存在）

/// @brief 二进制包写入器（服务端拼包用）
/// @note 内部维护一个 std::string 缓冲，所有方法链式追加字节；
///       整数统一小端序，f32 用 memcpy 取其内存位模式（不可用强转，会丢精度）。
class Writer {
public:
    /// 写入 1 字节无符号整数
    void u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
    /// 写入 1 字节有符号整数（直接复用 u8 的位模式）
    void i8(int8_t v) { u8(static_cast<uint8_t>(v)); }

    /// 写入 2 字节无符号整数（小端序：低字节在前）
    void u16(uint16_t v) {
        buf_.push_back(static_cast<char>(v & 0xFFu));
        buf_.push_back(static_cast<char>((v >> 8) & 0xFFu));
    }

    /// 写入 4 字节无符号整数（小端序，循环移位 4 次）
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<char>((v >> (i * 8)) & 0xFFu));
        }
    }

    /// 写入 4 字节有符号整数（按位模式转 u32 写入）
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

    /// 写入 4 字节 IEEE-754 单精度浮点
    void f32(float v) {
        uint32_t raw = 0;
        std::memcpy(&raw, &v, sizeof(raw));  // 位模式重解释，保持字节完全一致
        u32(raw);
    }

    /// @brief 写入“1 字节长度前缀 + 字符串”
    /// @note 超过 255 字节截断（长度字段只有 1 字节）
    void str8(const std::string& value) {
        const std::string clipped =
            value.size() > 255 ? value.substr(0, 255) : value;
        u8(static_cast<uint8_t>(clipped.size()));
        buf_ += clipped;
    }

    /// 取最终拼好的字节串
    const std::string& data() const { return buf_; }
    /// 当前字节数
    std::size_t size() const { return buf_.size(); }

private:
    std::string buf_;  ///< 拼装缓冲
};

/// @brief 二进制包读取器（读客户端消息用）
/// @note 内部维护读取偏移 offset_ 与合法性标志 ok_；任何越界读取都会把
///       ok_ 置 false，调用方只需在读包结束后检查一次 ok()，即可防御
///       客户端发来的截断包/畸形包，杜绝越界内存访问。
class Reader {
public:
    /// @brief 构造读取器
    /// @param data 字节缓冲起始指针
    /// @param len  缓冲长度
    Reader(const char* data, std::size_t len)
        : data_(reinterpret_cast<const uint8_t*>(data)), len_(len) {}

    /// 包是否仍然完整合法（无越界读取记录）
    bool ok() const { return ok_; }
    /// 剩余未读字节数（不合法时返回 0）
    std::size_t remaining() const { return ok_ ? len_ - offset_ : 0; }

    /// 读取 1 字节无符号整数；越界返回 0 并置 ok_=false
    uint8_t u8() {
        if (!ensure(1)) {
            return 0;
        }
        return data_[offset_++];
    }

    /// 读取 1 字节有符号整数
    int8_t i8() { return static_cast<int8_t>(u8()); }

    /// 读取 2 字节无符号整数（小端序拼回）
    uint16_t u16() {
        if (!ensure(2)) {
            return 0;
        }
        const uint16_t value = static_cast<uint16_t>(data_[offset_]) |
                               (static_cast<uint16_t>(data_[offset_ + 1]) << 8);
        offset_ += 2;
        return value;
    }

    /// 读取 4 字节无符号整数（小端序：逐字节左移累加）
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

    /// 读取 4 字节有符号整数
    int32_t i32() { return static_cast<int32_t>(u32()); }

    /// 读取 4 字节单精度浮点（先按 u32 读位模式，再 memcpy 还原为 float）
    float f32() {
        const uint32_t raw = u32();
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    /// @brief 读取 str8（先读 1 字节长度，再读对应字节数）
    /// @note 长度不足时返回空串且 ok_ 置 false
    std::string str8() {
        const uint8_t length = u8();
        if (!ensure(length)) {
            return std::string();
        }
        std::string value(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return value;
    }

private:
    /// @brief 检查剩余字节是否足够读取 count 个
    /// @return true=足够；false=不足（置非法标志，后续读取全部短路）
    bool ensure(std::size_t count) {
        if (!ok_ || offset_ + count > len_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* data_;   ///< 缓冲起始（不持有所有权）
    std::size_t len_;       ///< 缓冲总长度
    std::size_t offset_ = 0;///< 当前读取位置
    bool ok_ = true;        ///< 合法性标志，越界一次即永久 false
};

/// @brief 打包一个完整业务包
/// @param msgId   消息号（S2C）
/// @param seq     序列号（当前业务未使用，固定传 0，保留扩展）
/// @param payload Writer 拼好的消息体
/// @return 可直接交给 WebSocket 帧发送的完整字节串（含 8 字节头）
inline std::string packPacket(uint16_t msgId, uint16_t seq, const std::string& payload) {
    std::string out;
    const uint32_t total = static_cast<uint32_t>(kHeaderSize + payload.size());
    out.reserve(total);
    // u32 totalLength（小端序）
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((total >> (i * 8)) & 0xFFu));
    }
    // u16 msgId（小端序）
    out.push_back(static_cast<char>(msgId & 0xFFu));
    out.push_back(static_cast<char>((msgId >> 8) & 0xFFu));
    // u16 seq（小端序）
    out.push_back(static_cast<char>(seq & 0xFFu));
    out.push_back(static_cast<char>((seq >> 8) & 0xFFu));
    out += payload;
    return out;
}

/// @brief 从字节缓冲中切出一个完整业务包（粘包/半包处理的接收端）
/// @param buffer 已累积的接收字节（可能含 0 条、1 条或多条消息）
/// @param[in,out] offset 本次解析起始位置；成功切包后前进一个整包长度，
///                       供调用方在 while 循环中继续切下一条
/// @param[out] msgId   解析出的消息号
/// @param[out] seq     解析出的序列号
/// @param[out] payload 解析出的消息体（不含头）
/// @return true=成功切出一条；false=数据不够（半包，等更多字节）或头损坏
/// @note total 非法时 offset 仅前进 1 字节尝试重新同步；total 超过 64KB
///       或小于头长都视为非法，避免畸形包把解析器锁死。
inline bool unpackPacket(const std::string& buffer, std::size_t& offset,
                         uint16_t& msgId, uint16_t& seq,
                         std::string& payload) {
    if (buffer.size() - offset < kHeaderSize) {
        return false;   // 连 8 字节头都不够：半包
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.data()) + offset;
    // 读 u32 totalLength（小端序）
    uint32_t total = 0;
    for (int i = 0; i < 4; ++i) {
        total |= static_cast<uint32_t>(data[i]) << (i * 8);
    }
    if (total < kHeaderSize || total > kMaxPayloadSize) {
        ++offset;       // 长度非法：跳过 1 字节，尝试重新对齐同步
        return false;
    }
    if (buffer.size() - offset < total) {
        return false;   // 头声称的整包还没到齐：半包
    }
    // u16 msgId / u16 seq（小端序）
    msgId = static_cast<uint16_t>(data[4] | (static_cast<uint16_t>(data[5]) << 8));
    seq = static_cast<uint16_t>(data[6] | (static_cast<uint16_t>(data[7]) << 8));
    payload.assign(buffer, offset + kHeaderSize, total - kHeaderSize);
    offset += total;    // 前进一整包，调用方可继续循环处理粘在一起的下一包
    return true;
}

}  // namespace proto
}  // namespace blob
