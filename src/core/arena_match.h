// =============================================================================
//  arena_match.h —— 短局竞技场对局状态机
//
//  规则与网络层解耦：
//    Waiting -> Countdown -> Playing -> Ended
//
//  单局最多 4 人，第一人进入后开始倒计时；满员立即开局；180 秒后按分数
//  排名。分数来自 World 中的吞噬结算，Match 只负责阶段与结算快照。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/entity.h"

namespace blob {

class World;

enum class MatchPhase : uint8_t {
    Waiting   = 0,
    Countdown = 1,
    Playing   = 2,
    Ended     = 3,
};

inline const char* to_string(MatchPhase phase) {
    switch (phase) {
        case MatchPhase::Waiting:   return "waiting";
        case MatchPhase::Countdown: return "countdown";
        case MatchPhase::Playing:   return "playing";
        case MatchPhase::Ended:     return "ended";
    }
    return "unknown";
}

struct Standing {
    EntityId    id      = kInvalidEntityId;
    std::string name;
    int         score   = 0;
    int         kills   = 0;
    int         deaths  = 0;
    float       radius  = 0.0f;
};

struct MatchInfo {
    MatchPhase phase       = MatchPhase::Waiting;
    uint16_t   remainSec   = 0;
    uint16_t   playerCount = 0;
    float      shrinkRadius = 0.0f;
};

class ArenaMatch {
public:
    explicit ArenaMatch(
        uint32_t seed,
        int countdownSec = cfg::kArenaCountdownSec,
        int durationSec = cfg::kArenaDurationSec,
        int shrinkStartSec = cfg::kArenaShrinkStartSec);

    // 每帧由 Room 调用。playerCount 是当前房间中的真人玩家数。
    void tick(World& world, float dt, std::size_t playerCount);

    bool acceptsPlayers() const {
        return phase_ == MatchPhase::Waiting || phase_ == MatchPhase::Countdown;
    }
    bool acceptsInput() const { return phase_ == MatchPhase::Playing; }
    bool isOver() const { return phase_ == MatchPhase::Ended; }

    MatchPhase phase() const { return phase_; }
    uint32_t roundSeed() const { return roundSeed_; }

    MatchInfo info(const World& world, std::size_t playerCount) const;

    // 自上次调用后阶段、倒计时、人数或缩圈半径发生变化时返回 true。
    bool consumeStateChanged(const World& world, std::size_t playerCount,
                             MatchInfo& out);

    // 对局结束后只交付一次完整排名。
    bool consumeResult(std::vector<Standing>& out);

    // 测试用：跳过倒计时，直接进入 Playing。
    void debugStartPlaying() {
        phase_ = MatchPhase::Playing;
        phaseElapsedSec_ = 0.0;
    }

    // 测试用：直接推进大段时间，不依赖真实帧数。
    void debugAdvance(float seconds) {
        phaseElapsedSec_ += static_cast<double>(seconds);
        totalElapsedSec_ += static_cast<double>(seconds);
    }

private:
    void transitionTo(MatchPhase next, World& world);
    void finish(World& world);
    static std::vector<Standing> buildStandings(const World& world);
    static bool sameInfo(const MatchInfo& a, const MatchInfo& b);

    uint32_t roundSeed_ = 1;
    int countdownSec_ = cfg::kArenaCountdownSec;
    int durationSec_ = cfg::kArenaDurationSec;
    int shrinkStartSec_ = cfg::kArenaShrinkStartSec;

    MatchPhase phase_ = MatchPhase::Waiting;
    double phaseElapsedSec_ = 0.0;
    double totalElapsedSec_ = 0.0;
    bool   shrinkStarted_ = false;

    MatchInfo lastInfo_{};
    bool      stateChanged_ = false;

    std::vector<Standing> result_;
    bool resultPending_ = false;
};

}  // namespace blob
