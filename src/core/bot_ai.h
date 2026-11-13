// =============================================================================
//  bot_ai.h —— NPC 行为状态机
//
//  NPC 只有两个状态：
//    Wander（漫游）：每 1.5 秒随机换一个方向匀速游动；
//    Panic（恐慌）：发现附近大玩家或恐慌同类时，朝远离威胁的方向逃跑 2.5 秒，
//                   逃跑期间速度 ×1.6。
//  本模块只计算“移动方向”，真正的位置积分由 World::integrateMovement 完成。
//  注意：tickBotAI 只在 RoomWorker 线程被调用（World 线程封闭）。
// =============================================================================
#pragma once

#include <cstdint>

#include "core/entity.h"

namespace blob {

class World;  // 前置声明，减少头文件耦合；实现在 .cpp 中包含 world.h

/// @brief NPC AI 状态枚举（数值写入 Entity::aiState）
enum class BotState : uint8_t {
    Wander = 0,  // 漫游
    Panic  = 1,  // 恐慌逃跑
};

/// @brief 推进单个 NPC 的 AI 一帧
/// @param world 世界引用（用于查实体、取 NPC 专用随机数）
/// @param bot   待决策的 NPC 实体
/// @param dt    距上一帧的秒数（固定 1/30）
void tickBotAI(World& world, Entity& bot, float dt);

/// @brief 让 NPC 进入恐慌状态（已在恐慌中则不重置计时）
/// @param bot 目标 NPC
void enterPanic(Entity& bot);

/// @brief 按 AI 状态返回速度倍率
/// @return Panic 返回 kPanicSpeedMul(1.6)；Wander 返回 1.0
float aiSpeedMul(uint8_t aiState);

}  // namespace blob
