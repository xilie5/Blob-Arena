#include "server/lobby.h"

#include <cstdio>

#include "core/game_config.h"

namespace blob {

namespace {

// 房间码字母表：去掉了易混淆的 I、L、O、0、1（玩家口头报码时不容易看错念错）
constexpr char kCodeAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr std::size_t kCodeAlphabetSize = sizeof(kCodeAlphabet) - 1;
constexpr int kCodeLength = 4;          // 4 位码
constexpr int kCodeGenAttempts = 32;    // 生成不碰撞房间码的最大尝试次数

}  // namespace

Lobby::Lobby(uint32_t seed, float worldWidth, float worldHeight,
             int countdownSec, int durationSec, int shrinkStartSec)
    : seed_(seed),
      worldWidth_(worldWidth),
      worldHeight_(worldHeight),
      countdownSec_(countdownSec),
      durationSec_(durationSec),
      shrinkStartSec_(shrinkStartSec),
      rngState_(seed == 0 ? 1u : seed) {}

Lobby::~Lobby() = default;

// 大厅自己的 xorshift 随机数（与 World 中的实现相同，状态独立）
uint32_t Lobby::nextRng() {
    uint32_t state = rngState_;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    rngState_ = state;
    return state;
}

std::string Lobby::generateUniqueCode() {
    // 多次尝试生成 4 位码，直到不与现有房间码冲突
    for (int attempt = 0; attempt < kCodeGenAttempts; ++attempt) {
        std::string code;
        code.reserve(kCodeLength);
        for (int i = 0; i < kCodeLength; ++i) {
            code.push_back(kCodeAlphabet[nextRng() % kCodeAlphabetSize]);
        }
        if (rooms_.find(code) == rooms_.end()) {
            return code;
        }
    }
    return std::string();  // 极端情况下（房间极多）32 次都碰撞则失败
}

Room* Lobby::createRoom(bool publicMatch) {
    const std::string code = generateUniqueCode();
    if (code.empty()) {
        return nullptr;
    }
    const uint32_t roomSeed = nextRng();  // 每个房间独立种子，世界布局各不相同
    auto room = std::make_unique<Room>(
        code, roomSeed, worldWidth_, worldHeight_,
        static_cast<std::size_t>(cfg::kArenaPlayers), publicMatch,
        countdownSec_, durationSec_, shrinkStartSec_);
    Room* raw = room.get();  // 先留裸指针，move 进 map 后返回给调用方
    rooms_.emplace(code, std::move(room));
    rebuildRoomList();
    std::printf("[lobby] room created code=%s public=%d seed=%u rooms=%zu\n",
                code.c_str(), publicMatch ? 1 : 0,
                static_cast<unsigned>(roomSeed), rooms_.size());
    return raw;
}

Room* Lobby::quickMatch() {
    // 找到第一个“公共且当前可加入”的房间就进
    for (Room* room : roomList_) {
        if (room->isPublic() && room->canJoin()) {
            return room;
        }
    }
    // 没有合适的公共房：新建一个公共房
    return createRoom(true);
}

std::string Lobby::createPrivateRoom() {
    Room* room = createRoom(false);
    return room == nullptr ? std::string() : room->code();
}

Room* Lobby::joinByCode(const std::string& code) {
    auto it = rooms_.find(code);
    return it == rooms_.end() ? nullptr : it->second.get();
}

void Lobby::pruneEmptyRooms() {
    bool changed = false;
    // 逐个销毁空房间；erase 返回下一个迭代器，安全地边遍历边删
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        if (it->second->empty()) {
            it = rooms_.erase(it);  // unique_ptr 析构 -> 整个 World 内存释放
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        rebuildRoomList();  // 有销毁才重建，避免无意义拷贝
    }
}

void Lobby::rebuildRoomList() {
    roomList_.clear();
    roomList_.reserve(rooms_.size());
    for (const auto& entry : rooms_) {
        roomList_.push_back(entry.second.get());
    }
}

}  // namespace blob
