// =============================================================================
//  game_config.h —— 全局可调参数
//
//  所有规则数值集中在这里，核心逻辑不散落魔法数字。
// =============================================================================
#pragma once

#include <cstdint>

namespace blob {
namespace cfg {

// --------------------------- 世界与网络 ---------------------------------------
constexpr float kWorldWidth  = 1800.0f;
constexpr float kWorldHeight = 1800.0f;

constexpr int   kTickRate          = 30;
constexpr float kTickSeconds       = 1.0f / static_cast<float>(kTickRate);
constexpr float kSnapshotInterval  = 1.0f / 20.0f;
constexpr int   kMaxPlayersPerRoom = 4;
constexpr int   kMaxEntities       = 4096;

// --------------------------- 玩家 --------------------------------------------
constexpr float kPlayerStartRadius = 20.0f;
constexpr float kPlayerMinRadius   = 14.0f;
constexpr float kPlayerBaseSpeed   = 260.0f;
constexpr float kSpeedFalloff      = 0.22f;
constexpr float kPlayerMaxRadius   = 400.0f;

constexpr float kPlayerRespawnSafeRadius = 220.0f;
constexpr int   kPlayerRespawnAttempts   = 12;

// --------------------------- 食物 --------------------------------------------
constexpr float kFoodRadius        = 7.0f;
constexpr float kFoodMaxCount      = 600.0f;
constexpr float kFoodSpawnPerSec   = 40.0f;
constexpr float kFoodSafeRadius    = 60.0f;
constexpr int   kFoodInitialCount  = 400;
constexpr int   kFoodSpawnAttempts = 10;

// --------------------------- 吞噬与成长 --------------------------------------
constexpr float kEatRadiusRatio = 1.15f;
constexpr float kEatOverlap     = 0.40f;
constexpr float kEatMassGain    = 1.0f;

// --------------------------- 排行榜 ------------------------------------------
constexpr int kLeaderboardSize = 10;

// --------------------------- NPC 群众 ----------------------------------------
constexpr int   kBotCount            = 60;
constexpr float kBotStartRadius      = 16.0f;
constexpr float kBotBaseSpeed        = 200.0f;
constexpr float kBotSpawnPerSec      = 1.0f;
constexpr float kBotThinkInterval    = 1.5f;

// --------------------------- 恐慌 --------------------------------------------
constexpr float kPanicRadius         = 260.0f;
constexpr float kPanicPredatorRatio  = 1.5f;
constexpr float kPanicDuration       = 2.5f;
constexpr float kPanicSpeedMul       = 1.6f;

// --------------------------- 竞技场对局 --------------------------------------
constexpr float    kArenaWorldSize        = 1800.0f;
constexpr int      kArenaPlayers          = 4;
constexpr int      kArenaCountdownSec     = 8;
constexpr int      kArenaDurationSec      = 180;
constexpr int      kArenaShrinkStartSec   = 90;
constexpr uint32_t kArenaShrinkDurationMs = 90000u;

// AOI：玩家只接收视野附近的实体；网格用于降低查询复杂度。
constexpr float kAoiRadius   = 450.0f;
constexpr float kAoiCellSize = 300.0f;

// 断线后保留实体和 AI 托管的宽限时间。
constexpr int kReconnectGraceMs = 10000;

// 分数规则：食物 1 分、NPC 5 分、真人玩家 20 分。
constexpr int kScoreFood   = 1;
constexpr int kScoreBot    = 5;
constexpr int kScorePlayer = 20;

// 边界收缩从全图开始，最后保留 35% 半径。
constexpr float kShrinkStartRatio = 1.0f;
constexpr float kShrinkEndRatio   = 0.35f;
constexpr float kShrinkDpsRatio   = 0.06f;

}  // namespace cfg
}  // namespace blob
