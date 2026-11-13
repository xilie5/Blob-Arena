// =============================================================================
//  game_config.h —— 全局可调参数
//
//  所有规则数值集中在这里，核心逻辑不散落魔法数字。
//
//  两层配置的关系：
//    这里的 cfg::kXxx 是“游戏规则单一数据源”（编译期常量，核心逻辑直接引用）；
//    ServerOptions 中的运行参数默认值也引用这里，运维可用命令行临时覆盖
//    （如 --arena-seconds 60），无需改代码重编译。
// =============================================================================
#pragma once

#include <cstdint>

namespace blob {
namespace cfg {

// --------------------------- 世界与网络 ---------------------------------------
constexpr float kWorldWidth  = 1800.0f;   // 默认世界宽度（世界单位）
constexpr float kWorldHeight = 1800.0f;   // 默认世界高度（正方形地图）

constexpr int   kTickRate          = 30;  // 逻辑帧率：每秒 30 个 tick
constexpr float kTickSeconds       = 1.0f / static_cast<float>(kTickRate); // 单 tick 时长 ≈33.3ms
constexpr float kSnapshotInterval  = 1.0f / 20.0f;  // 快照下发间隔：20Hz（50ms）
constexpr int   kMaxPlayersPerRoom = 4;   // 每个房间真人玩家上限
constexpr int   kMaxEntities       = 4096;// 单世界实体总数上限（含食物/NPC/玩家）

// --------------------------- 玩家 --------------------------------------------
constexpr float kPlayerStartRadius = 20.0f;  // 出生半径
constexpr float kPlayerMinRadius   = 14.0f;  // 被吃重生/缩圈后的最小半径
constexpr float kPlayerBaseSpeed   = 260.0f; // 初始半径下的基础移速（世界单位/秒）
constexpr float kSpeedFalloff      = 0.22f;  // 速度衰减指数：半径越大速度越慢
constexpr float kPlayerMaxRadius   = 400.0f; // 半径硬上限，防止无限成长

constexpr float kPlayerRespawnSafeRadius = 220.0f; // 重生点与其他能动单位的最小安全距离
constexpr int   kPlayerRespawnAttempts   = 12;     // 寻找安全重生点最多尝试次数

// --------------------------- 食物 --------------------------------------------
constexpr float kFoodRadius        = 7.0f;  // 单个食物半径
constexpr float kFoodMaxCount      = 600.0f;// 场上食物数量上限
constexpr float kFoodSpawnPerSec   = 40.0f; // 被吃掉后每秒补充 40 个
constexpr float kFoodSafeRadius    = 60.0f; // 食物不刷在能动单位 60 半径内
constexpr int   kFoodInitialCount  = 400;   // 房间创建时预铺的初始食物数
constexpr int   kFoodSpawnAttempts = 10;    // 单个食物找安全点的最大尝试次数

// --------------------------- 吞噬与成长 --------------------------------------
constexpr float kEatRadiusRatio = 1.15f; // 吞噬者半径至少为对方 1.15 倍才能吃
constexpr float kEatOverlap     = 0.40f; // 重叠判定：距离 < a.r - b.r*0.40
constexpr float kEatMassGain    = 1.0f;  // 质量吸收率 100%

// --------------------------- 排行榜 ------------------------------------------
constexpr int kLeaderboardSize = 10; // 排行榜条目数（预留）

// --------------------------- NPC 群众 ----------------------------------------
constexpr int   kBotCount            = 60;     // 场上 NPC 上限
constexpr float kBotStartRadius      = 16.0f;  // NPC 出生半径
constexpr float kBotBaseSpeed        = 200.0f; // NPC 基础移速
constexpr float kBotSpawnPerSec      = 1.0f;   // NPC 被吃后每秒补 1 个
constexpr float kBotThinkInterval    = 1.5f;   // 漫游状态每 1.5 秒换一个随机方向

// --------------------------- 恐慌 --------------------------------------------
constexpr float kPanicRadius         = 260.0f; // NPC 威胁感知半径
constexpr float kPanicPredatorRatio  = 1.5f;   // 玩家半径达 NPC 的 1.5 倍即视为威胁
constexpr float kPanicDuration       = 2.5f;   // 恐慌状态持续 2.5 秒
constexpr float kPanicSpeedMul       = 1.6f;   // 恐慌时移速倍率

// --------------------------- 竞技场对局 --------------------------------------
constexpr float    kArenaWorldSize        = 1800.0f;  // 竞技场默认边长
constexpr int      kArenaPlayers          = 4;        // 满员人数（满员立即开局）
constexpr int      kArenaCountdownSec     = 8;        // 开局准备倒计时（秒）
constexpr int      kArenaDurationSec      = 180;      // 单局时长（秒）
constexpr int      kArenaShrinkStartSec   = 90;       // 对局进行到 90 秒开始缩圈
constexpr uint32_t kArenaShrinkDurationMs = 20000u;   // 缩圈动画持续 20 秒（演示调快）

// AOI：玩家只接收视野附近的实体；网格用于降低查询复杂度。
constexpr float kAoiRadius   = 450.0f;  // 兴趣区域（视野）半径
constexpr float kAoiCellSize = 300.0f;  // 均匀空间网格的单元格边长

// 断线后保留实体和 AI 托管的宽限时间。
constexpr int kReconnectGraceMs = 10000; // 断线 10 秒内可 Resume 拿回原实体

// 分数规则：食物 1 分、NPC 5 分、真人玩家 20 分。
constexpr int kScoreFood   = 1;
constexpr int kScoreBot    = 5;
constexpr int kScorePlayer = 20;

// 边界收缩从全图开始，最后保留 35% 半径。
constexpr float kShrinkStartRatio = 1.0f;  // 缩圈起始半径比例（全图）
constexpr float kShrinkEndRatio   = 0.35f; // 缩圈终局半径比例
constexpr float kShrinkDpsRatio   = 0.06f; // 圈外单位每秒损失 6% 当前半径

}  // namespace cfg
}  // namespace blob
