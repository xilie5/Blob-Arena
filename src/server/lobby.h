// =============================================================================
//  lobby.h —— 公共匹配房间与私有码房注册表
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/room.h"

namespace blob {

class Lobby {
public:
    Lobby(uint32_t seed, float worldWidth, float worldHeight,
          int countdownSec = cfg::kArenaCountdownSec,
          int durationSec = cfg::kArenaDurationSec,
          int shrinkStartSec = cfg::kArenaShrinkStartSec);
    ~Lobby();

    Lobby(const Lobby&) = delete;
    Lobby& operator=(const Lobby&) = delete;

    // 快速匹配：优先加入尚未开局的公共房，否则创建新房。
    Room* quickMatch();

    // 创建私有码房。
    std::string createPrivateRoom();

    Room* joinByCode(const std::string& code);
    void pruneEmptyRooms();

    const std::vector<Room*>& rooms() const { return roomList_; }
    std::size_t codeRoomCount() const { return rooms_.size(); }

private:
    Room* createRoom(bool publicMatch);
    std::string generateUniqueCode();
    uint32_t nextRng();
    void rebuildRoomList();

    uint32_t seed_;
    float worldWidth_;
    float worldHeight_;
    int countdownSec_;
    int durationSec_;
    int shrinkStartSec_;
    uint32_t rngState_;
    std::unordered_map<std::string, std::unique_ptr<Room>> rooms_;
    std::vector<Room*> roomList_;
};

}  // namespace blob
