// =============================================================================
//  room.h —— 4 人竞技场房间
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/arena_match.h"
#include "core/world.h"

namespace blob {

using EntityEvent = EntityChange;

class Room {
public:
    Room(const std::string& code, uint32_t seed, float width, float height,
         std::size_t maxPlayers, bool publicMatch,
         int countdownSec = cfg::kArenaCountdownSec,
         int durationSec = cfg::kArenaDurationSec,
         int shrinkStartSec = cfg::kArenaShrinkStartSec);

    EntityId addPlayer(const std::string& name);
    bool removePlayer(EntityId id, uint8_t reason);
    bool setInput(EntityId id, float dirX, float dirY);
    void step(float dt);

    void drainEvents(std::vector<EntityEvent>& out);
    void drainKills(std::vector<KillEvent>& out);
    bool consumeMatchState(MatchInfo& out);
    bool consumeMatchResult(std::vector<Standing>& out);

    const std::string& code() const { return code_; }
    std::string matchId() const {
        return code_ + "-" + std::to_string(arena_.roundSeed());
    }
    uint32_t seed() const { return seed_; }
    bool isPublic() const { return publicMatch_; }
    bool empty() const { return playerCount() == 0; }
    bool canJoin() const {
        return !isFull() && arena_.acceptsPlayers();
    }
    bool acceptsInput() const { return arena_.acceptsInput(); }
    bool isMatchOver() const { return arena_.isOver(); }

    World& world() { return world_; }
    const World& world() const { return world_; }

    std::size_t playerCount() const {
        return world_.countKind(EntityKind::Player);
    }
    std::size_t entityCount() const { return world_.size(); }
    std::size_t maxPlayers() const { return maxPlayers_; }
    bool isFull() const { return playerCount() >= maxPlayers_; }

private:
    std::string code_;
    uint32_t seed_;
    std::size_t maxPlayers_;
    bool publicMatch_;
    World world_;
    ArenaMatch arena_;
    std::vector<EntityEvent> events_;
    std::vector<KillEvent> kills_;
    MatchInfo matchInfo_{};
    bool matchStatePending_ = false;
    std::vector<Standing> matchResult_;
};

}  // namespace blob
