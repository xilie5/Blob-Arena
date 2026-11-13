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
      // 状态机种子与世界种子异或区分开：两条随机流互不干扰
      arena_(seed ^ 0xA5A5A5A5u, countdownSec, durationSec, shrinkStartSec) {
    events_.reserve(256);
    kills_.reserve(32);
    // 房间建好即铺场：初始食物与 NPC，玩家一进来就能看到完整世界
    world_.populateFood(static_cast<std::size_t>(cfg::kFoodInitialCount));
    world_.populateBots(static_cast<std::size_t>(cfg::kBotCount));
    // 铺场产生的 Added 事件直接丢弃：玩家入房时会通过 AOI 查询重新收到
    // 视野内实体，不需要这批“房间创建期”的事件。
    std::vector<EntityChange> discarded;
    world_.drainChanges(discarded);
}

EntityId Room::addPlayer(const std::string& name) {
    if (!canJoin()) {
        return kInvalidEntityId;  // 房间满或已开局
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
    // 双重门：对局阶段必须接受输入，且实体存在
    if (!arena_.acceptsInput() || world_.find(id) == nullptr) {
        return false;
    }
    world_.setMoveDir(id, Vec2(dirX, dirY));
    return true;
}

void Room::step(float dt) {
    // 1) 先推进对局状态机（可能触发 Waiting->Countdown->Playing 等切换）
    arena_.tick(world_, dt, playerCount());

    // 2) 只有 Playing 阶段世界才运转；倒计时阶段世界冻结，只展示不移动
    if (arena_.acceptsInput()) {
        world_.step(dt);
    }

    // 3) 收集世界产出的事件（供 Worker 本帧处理）
    world_.drainChanges(events_);
    world_.drainKills(kills_);

    // 4) 收集状态机的状态变化与最终结算
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
