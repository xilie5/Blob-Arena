#include "server/lobby.h"

#include <cstdio>

#include "core/game_config.h"

namespace blob {

namespace {

constexpr char kCodeAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr std::size_t kCodeAlphabetSize = sizeof(kCodeAlphabet) - 1;
constexpr int kCodeLength = 4;
constexpr int kCodeGenAttempts = 32;

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

uint32_t Lobby::nextRng() {
    uint32_t state = rngState_;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    rngState_ = state;
    return state;
}

std::string Lobby::generateUniqueCode() {
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
    return std::string();
}

Room* Lobby::createRoom(bool publicMatch) {
    const std::string code = generateUniqueCode();
    if (code.empty()) {
        return nullptr;
    }
    const uint32_t roomSeed = nextRng();
    auto room = std::make_unique<Room>(
        code, roomSeed, worldWidth_, worldHeight_,
        static_cast<std::size_t>(cfg::kArenaPlayers), publicMatch,
        countdownSec_, durationSec_, shrinkStartSec_);
    Room* raw = room.get();
    rooms_.emplace(code, std::move(room));
    rebuildRoomList();
    std::printf("[lobby] room created code=%s public=%d seed=%u rooms=%zu\n",
                code.c_str(), publicMatch ? 1 : 0,
                static_cast<unsigned>(roomSeed), rooms_.size());
    return raw;
}

Room* Lobby::quickMatch() {
    for (Room* room : roomList_) {
        if (room->isPublic() && room->canJoin()) {
            return room;
        }
    }
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
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        if (it->second->empty()) {
            it = rooms_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        rebuildRoomList();
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
