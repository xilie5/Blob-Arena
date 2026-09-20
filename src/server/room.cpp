#include "server/room.h"

#include "core/game_config.h"

namespace blob {

Room::Room(const std::string& code, uint32_t seed, float width, float height,
           std::size_t maxPlayers, bool publicMatch,
           int countdownSec, int durationSec, int shrinkStartSec)
    : code_(code),
      seed_(seed),
      maxPlayers_(maxPlayers),
      publicMatch_(publicMatch),
      world_(seed, width, height),
      arena_(seed ^ 0xA5A5A5A5u, countdownSec, durationSec, shrinkStartSec) {
    events_.reserve(256);
    kills_.reserve(32);
    world_.populateFood(static_cast<std::size_t>(cfg::kFoodInitialCount));
    world_.populateBots(static_cast<std::size_t>(cfg::kBotCount));
    std::vector<EntityChange> discarded;
    world_.drainChanges(discarded);
}

EntityId Room::addPlayer(const std::string& name) {
    if (!canJoin()) {
        return kInvalidEntityId;
    }
    return world_.spawnPlayer(name);
}

bool Room::removePlayer(EntityId id, uint8_t reason) {
    if (world_.find(id) == nullptr) {
        return false;
    }
    world_.removeEntity(id, reason);
    return true;
}

bool Room::setInput(EntityId id, float dirX, float dirY) {
    if (!arena_.acceptsInput() || world_.find(id) == nullptr) {
        return false;
    }
    world_.setMoveDir(id, Vec2(dirX, dirY));
    return true;
}

void Room::step(float dt) {
    arena_.tick(world_, dt, playerCount());

    if (arena_.acceptsInput()) {
        world_.step(dt);
    }

    world_.drainChanges(events_);
    world_.drainKills(kills_);

    MatchInfo info;
    if (arena_.consumeStateChanged(world_, playerCount(), info)) {
        matchInfo_ = info;
        matchStatePending_ = true;
    }
    arena_.consumeResult(matchResult_);
}

void Room::drainEvents(std::vector<EntityEvent>& out) {
    out.insert(out.end(), events_.begin(), events_.end());
    events_.clear();
}

void Room::drainKills(std::vector<KillEvent>& out) {
    out.insert(out.end(), kills_.begin(), kills_.end());
    kills_.clear();
}

bool Room::consumeMatchState(MatchInfo& out) {
    if (!matchStatePending_) {
        return false;
    }
    out = matchInfo_;
    matchStatePending_ = false;
    return true;
}

bool Room::consumeMatchResult(std::vector<Standing>& out) {
    if (matchResult_.empty()) {
        return false;
    }
    out.insert(out.end(), matchResult_.begin(), matchResult_.end());
    matchResult_.clear();
    return true;
}

}  // namespace blob
