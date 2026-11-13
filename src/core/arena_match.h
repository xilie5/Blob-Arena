// =============================================================================
//  arena_match.h —— 短局竞技场对局状态机
//
//  规则与网络层解耦：
//    Waiting -> Countdown -> Playing -> Ended
//
//  单局最多 4 人，第一人进入后开始倒计时；满员立即开局；180 秒后按分数
//  排名。分数来自 World 中的吞噬结算，Match 只负责阶段与结算快照，
//  不参与任何移动/吞噬逻辑——这让对局规则可以脱离网络单独单测。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/entity.h"

namespace blob {

class World;

/// @brief 对局阶段（状态机的四个状态）
enum class MatchPhase : uint8_t {
    Waiting   = 0,  // 无人/等待：世界冻结，可加入
    Countdown = 1,  // 开局倒计时：世界冻结，可加入，满人或超时即开局
    Playing   = 2,  // 比赛进行：世界运转，接受移动输入
    Ended     = 3,  // 已结束：产出一次最终排名
};

/// 阶段转字符串（日志/调试用）
inline const char* to_string(MatchPhase phase) {
    switch (phase) {
        case MatchPhase::Waiting:   return "waiting";
        case MatchPhase::Countdown: return "countdown";
        case MatchPhase::Playing:   return "playing";
        case MatchPhase::Ended:     return "ended";
    }
    return "unknown";
}

/// @brief 结算排名中的一条（一名玩家的最终成绩）
struct Standing {
    EntityId    id      = kInvalidEntityId;  ///< 实体 ID
    std::string name;                        ///< 玩家名
    int         score   = 0;                 ///< 最终分数
    int         kills   = 0;                 ///< 击杀数
    int         deaths  = 0;                 ///< 死亡数
    float       radius  = 0.0f;              ///< 最终半径（排序兜底依据）
};

/// @brief 对外广播的对局状态快照（MatchState 消息载荷）
struct MatchInfo {
    MatchPhase phase       = MatchPhase::Waiting; ///< 当前阶段
    uint16_t   remainSec   = 0;    ///< 当前阶段剩余秒数（向上取整）
    uint16_t   playerCount = 0;    ///< 房间真人玩家数
    float      shrinkRadius = 0.0f;///< 当前缩圈半径（未开启为 0）
};

/// @brief 竞技场对局状态机
class ArenaMatch {
public:
    /// @brief 构造状态机
    /// @param seed 对局种子（仅作记录，World 另有自己的种子）
    /// @param countdownSec 开局倒计时秒数（默认 8）
    /// @param durationSec  单局时长秒数（默认 180）
    /// @param shrinkStartSec 缩圈开始秒数（默认 90）
    explicit ArenaMatch(
        uint32_t seed,
        int countdownSec = cfg::kArenaCountdownSec,
        int durationSec = cfg::kArenaDurationSec,
        int shrinkStartSec = cfg::kArenaShrinkStartSec);

    /// @brief 每帧由 Room 调用，推进状态机
    /// @param world 世界引用（用于开启/关闭缩圈）
    /// @param dt 步长秒数
    /// @param playerCount 当前房间真人玩家数（为 0 时回退到 Waiting）
    void tick(World& world, float dt, std::size_t playerCount);

    /// 当前是否允许玩家加入（Waiting/Countdown 可以，开局后不可）
    bool acceptsPlayers() const {
        return phase_ == MatchPhase::Waiting || phase_ == MatchPhase::Countdown;
    }
    /// 当前是否接受移动输入（仅 Playing）
    bool acceptsInput() const { return phase_ == MatchPhase::Playing; }
    /// 对局是否已结束
    bool isOver() const { return phase_ == MatchPhase::Ended; }

    MatchPhase phase() const { return phase_; }  ///< 当前阶段
    uint32_t roundSeed() const { return roundSeed_; }  ///< 对局种子

    /// @brief 计算当前对外状态快照（剩余秒数随阶段不同含义不同）
    MatchInfo info(const World& world, std::size_t playerCount) const;

    /// @brief 状态变化时输出最新 MatchInfo（带去重：没变化不重复广播）
    /// @return true=本次有变化，out 已填充；false=无变化
    bool consumeStateChanged(const World& world, std::size_t playerCount,
                             MatchInfo& out);

    /// @brief 对局结束后只交付一次完整排名
    /// @return true=有未交付的结算，out 已追加；false=无
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
    void transitionTo(MatchPhase next, World& world);  // 阶段切换（含进入 Playing 的重置）
    void finish(World& world);                         // 进入 Ended 并生成排名
    static std::vector<Standing> buildStandings(const World& world); // 从世界抽取排名
    static bool sameInfo(const MatchInfo& a, const MatchInfo& b);    // 快照去重比较

    uint32_t roundSeed_ = 1;             ///< 对局种子（记录用）
    int countdownSec_ = cfg::kArenaCountdownSec; ///< 倒计时时长
    int durationSec_ = cfg::kArenaDurationSec;   ///< 正局时长
    int shrinkStartSec_ = cfg::kArenaShrinkStartSec; ///< 缩圈开始时刻

    MatchPhase phase_ = MatchPhase::Waiting; ///< 当前阶段
    double phaseElapsedSec_ = 0.0;   ///< 当前阶段内已过秒数
    double totalElapsedSec_ = 0.0;   ///< Playing 阶段总计时（驱动缩圈/结束）
    bool   shrinkStarted_ = false;   ///< 本局是否已开启缩圈（只开一次）

    MatchInfo lastInfo_{};           ///< 上次对外的状态（变化检测基准）
    bool      stateChanged_ = false; ///< 是否有未交付的状态变化

    std::vector<Standing> result_;   ///< 待交付的最终排名
    bool resultPending_ = false;     ///< 排名是否尚未交付（保证只结算一次）
};

}  // namespace blob
