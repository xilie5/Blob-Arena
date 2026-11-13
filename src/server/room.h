// =============================================================================
//  room.h —— 4 人竞技场房间
//
//  Room 是一个“对局容器”，把三样东西绑在一起：
//    World（游戏世界）+ ArenaMatch（对局状态机）+ 房间元数据（房间码、公开/私有）
//  并负责在每个 tick 协调它们：先推进状态机，阶段允许时再推进世界，
//  最后收集世界事件/击杀/对局状态/结算结果交给 RoomWorker。
//
//  线程归属：仅在 RoomWorker 线程被访问，内部无锁。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/arena_match.h"
#include "core/world.h"

namespace blob {

// 实体事件类型别名（Room 层沿用 World 的增删事件）
using EntityEvent = EntityChange;

/// @brief 竞技场房间
class Room {
public:
    /// @brief 构造房间
    /// @param code 房间码  @param seed 世界种子  @param width/height 世界尺寸
    /// @param maxPlayers 人数上限（4）  @param publicMatch 公共房(true)/私有房(false)
    /// @param countdownSec/durationSec/shrinkStartSec 对局时间参数
    Room(const std::string& code, uint32_t seed, float width, float height,
         std::size_t maxPlayers, bool publicMatch,
         int countdownSec = cfg::kArenaCountdownSec,
         int durationSec = cfg::kArenaDurationSec,
         int shrinkStartSec = cfg::kArenaShrinkStartSec);

    EntityId addPlayer(const std::string& name);  ///< 玩家加入（在世界生成实体）
    bool removePlayer(EntityId id, uint8_t reason); ///< 玩家移除（离开/超时）
    bool setInput(EntityId id, float dirX, float dirY); ///< 写入玩家移动方向
    void step(float dt);                           ///< 推进房间一帧（状态机+世界）

    void drainEvents(std::vector<EntityEvent>& out);  ///< 抽走实体事件
    void drainKills(std::vector<KillEvent>& out);     ///< 抽走击杀事件
    bool consumeMatchState(MatchInfo& out);           ///< 取对局状态变化（有则 true）
    bool consumeMatchResult(std::vector<Standing>& out); ///< 取对局结算（一次）

    const std::string& code() const { return code_; }  ///< 房间码
    /// @brief 对局唯一 ID = 房间码-对局种子（战绩幂等键的一部分）
    std::string matchId() const {
        return code_ + "-" + std::to_string(arena_.roundSeed());
    }
    uint32_t seed() const { return seed_; }            ///< 房间/世界种子
    bool isPublic() const { return publicMatch_; }     ///< 是否公共房（可被快速匹配）
    bool empty() const { return playerCount() == 0; }  ///< 是否无真人玩家（空房回收用）
    /// 当前是否允许加入：未满且处于 Waiting/Countdown
    bool canJoin() const {
        return !isFull() && arena_.acceptsPlayers();
    }
    bool acceptsInput() const { return arena_.acceptsInput(); } ///< 是否接受移动输入
    bool isMatchOver() const { return arena_.isOver(); }        ///< 对局是否已结束

    World& world() { return world_; }                  ///< 世界（可变）
    const World& world() const { return world_; }      ///< 世界（只读）

    std::size_t playerCount() const {
        return world_.countKind(EntityKind::Player);   ///< 存活真人玩家数
    }
    std::size_t entityCount() const { return world_.size(); } ///< 实体总数（统计用）
    std::size_t maxPlayers() const { return maxPlayers_; }
    bool isFull() const { return playerCount() >= maxPlayers_; }

private:
    std::string code_;        ///< 4 位房间码
    uint32_t seed_;           ///< 世界种子
    std::size_t maxPlayers_;  ///< 人数上限
    bool publicMatch_;        ///< 公共房 / 私有房
    World world_;             ///< 本房间的权威游戏世界
    ArenaMatch arena_;        ///< 本房间的对局状态机
    std::vector<EntityEvent> events_;     ///< 暂存世界增删事件
    std::vector<KillEvent> kills_;        ///< 暂存击杀事件
    MatchInfo matchInfo_{};               ///< 暂存最新对局状态
    bool matchStatePending_ = false;      ///< 对局状态是否有未消费变化
    std::vector<Standing> matchResult_;   ///< 暂存对局结算排名
};

}  // namespace blob
