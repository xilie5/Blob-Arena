#include "core/arena_match.h"

#include <algorithm>
#include <cmath>

#include "core/game_config.h"
#include "core/world.h"

namespace blob {

namespace {

/// 剩余秒数向上取整：剩 0.1 秒也显示 1，避免玩家看到“0”却还没开局/结束
uint16_t secondsCeil(double seconds) {
    if (seconds <= 0.0) {
        return 0;
    }
    return static_cast<uint16_t>(std::ceil(seconds));
}

}  // namespace

ArenaMatch::ArenaMatch(uint32_t seed, int countdownSec, int durationSec,
                       int shrinkStartSec)
    : roundSeed_(seed == 0 ? 1u : seed),
      // 防御非法配置：时长必须为正，避免除零/永不结束
      countdownSec_(countdownSec > 0 ? countdownSec : 1),
      durationSec_(durationSec > 0 ? durationSec : 1),
      shrinkStartSec_(shrinkStartSec >= 0 ? shrinkStartSec : 0) {}

MatchInfo ArenaMatch::info(const World& world, std::size_t playerCount) const {
    MatchInfo out;
    out.phase = phase_;
    out.playerCount = static_cast<uint16_t>(
        std::min<std::size_t>(playerCount, 0xFFFFu)); // 协议字段为 u16，截断保护
    out.shrinkRadius = world.shrinkEnabled() ? world.shrinkRadius() : 0.0f;

    // 不同阶段“剩余秒数”的含义不同
    double remain = 0.0;
    switch (phase_) {
        case MatchPhase::Waiting:
            remain = 0.0;
            break;
        case MatchPhase::Countdown:
            remain = countdownSec_ - phaseElapsedSec_;   // 距开局
            break;
        case MatchPhase::Playing:
            remain = durationSec_ - totalElapsedSec_;    // 距结束
            break;
        case MatchPhase::Ended:
            remain = 0.0;
            break;
    }
    out.remainSec = secondsCeil(remain);
    return out;
}

void ArenaMatch::tick(World& world, float dt, std::size_t playerCount) {
    if (phase_ == MatchPhase::Ended) {
        return;   // 终态：不再推进
    }

    // ---- 玩家走光：任意阶段立即回到 Waiting 并重置一切（房间可复用） ----
    if (playerCount == 0) {
        if (phase_ != MatchPhase::Waiting) {
            phase_ = MatchPhase::Waiting;
            phaseElapsedSec_ = 0.0;
            totalElapsedSec_ = 0.0;
            shrinkStarted_ = false;
            world.setShrinkEnabled(false);
        }
    } else if (phase_ == MatchPhase::Waiting) {
        // 第一个玩家进入：Waiting -> Countdown
        transitionTo(MatchPhase::Countdown, world);
    }

    if (phase_ == MatchPhase::Countdown) {
        phaseElapsedSec_ += static_cast<double>(dt);
        // 满员（4人）立即开局，否则等倒计时走完
        if (playerCount >= static_cast<std::size_t>(cfg::kArenaPlayers) ||
            phaseElapsedSec_ >= static_cast<double>(countdownSec_)) {
            transitionTo(MatchPhase::Playing, world);
        }
    } else if (phase_ == MatchPhase::Playing) {
        phaseElapsedSec_ += static_cast<double>(dt);
        totalElapsedSec_ += static_cast<double>(dt); // 正局总计时

        // 到缩圈开始时刻：只开启一次，告诉 World 缩圈时长
        if (!shrinkStarted_ &&
            totalElapsedSec_ >= static_cast<double>(shrinkStartSec_)) {
            shrinkStarted_ = true;
            world.setShrinkDurationMs(cfg::kArenaShrinkDurationMs);
            world.setShrinkEnabled(true);
        }

        // 正局时间到：结算结束
        if (totalElapsedSec_ >= static_cast<double>(durationSec_)) {
            finish(world);
            return;
        }
    }

    // 变化检测：与上次对外快照比较，变了才置脏，避免每帧重复广播相同状态
    const MatchInfo current = info(world, playerCount);
    if (!sameInfo(lastInfo_, current)) {
        lastInfo_ = current;
        stateChanged_ = true;
    }
}

void ArenaMatch::transitionTo(MatchPhase next, World& world) {
    if (phase_ == next) {
        return;
    }
    phase_ = next;
    phaseElapsedSec_ = 0.0;  // 新阶段计时归零

    if (next == MatchPhase::Playing) {
        // 进入正局：正局总计时清零、缩圈复位为未开始、确保世界缩圈关闭
        totalElapsedSec_ = 0.0;
        shrinkStarted_ = false;
        world.setShrinkEnabled(false);
    }
}

void ArenaMatch::finish(World& world) {
    phase_ = MatchPhase::Ended;
    phaseElapsedSec_ = 0.0;
    world.setShrinkEnabled(false);     // 结束缩圈
    result_ = buildStandings(world);   // 立即生成排名快照
    resultPending_ = true;
    stateChanged_ = true;              // 阶段变化也要通知
}

bool ArenaMatch::consumeStateChanged(const World& world, std::size_t playerCount,
                                     MatchInfo& out) {
    const MatchInfo current = info(world, playerCount);
    if (!stateChanged_ && sameInfo(lastInfo_, current)) {
        return false;
    }
    out = current;
    lastInfo_ = current;
    stateChanged_ = false;   // 消费后清脏
    return true;
}

bool ArenaMatch::consumeResult(std::vector<Standing>& out) {
    if (!resultPending_) {
        return false;
    }
    out.insert(out.end(), result_.begin(), result_.end());
    result_.clear();
    resultPending_ = false;  // 排名只交付一次，防止重复落盘
    return true;
}

/// @brief 从世界抽取存活玩家并排序，生成最终排名
std::vector<Standing> ArenaMatch::buildStandings(const World& world) {
    std::vector<Standing> out;
    for (const Entity& entity : world.entities()) {
        // 只结算存活的真人玩家（NPC、食物、已删除实体不算）
        if (!entity.alive || entity.kind != EntityKind::Player) {
            continue;
        }
        Standing standing;
        standing.id = entity.id;
        standing.name = entity.name;
        standing.score = entity.score;
        standing.kills = entity.kills;
        standing.deaths = entity.deaths;
        standing.radius = entity.radius;
        out.push_back(std::move(standing));
    }

    // 排序优先级：分数 -> 击杀 -> 半径 -> ID（最后的 ID 保证完全确定的顺序）
    std::sort(out.begin(), out.end(), [](const Standing& a, const Standing& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.kills != b.kills) {
            return a.kills > b.kills;
        }
        if (a.radius != b.radius) {
            return a.radius > b.radius;
        }
        return a.id < b.id;
    });
    return out;
}

/// @brief 判断两份对外状态是否等价（缩圈半径容差 0.5，避免微小浮点变化刷屏）
bool ArenaMatch::sameInfo(const MatchInfo& a, const MatchInfo& b) {
    return a.phase == b.phase &&
           a.remainSec == b.remainSec &&
           a.playerCount == b.playerCount &&
           std::fabs(a.shrinkRadius - b.shrinkRadius) < 0.5f;
}

}  // namespace blob
