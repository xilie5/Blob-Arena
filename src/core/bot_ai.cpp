#include "core/bot_ai.h"

#include <cmath>

#include "core/game_config.h"
#include "core/world.h"

namespace blob {

namespace {

/// @brief 随机抽取一个移动方向（角度均匀分布）
/// @note 使用 World 的 NPC 专用随机流，保证 NPC 行为不影响世界生成的可复现性
Vec2 pickRandomDir(World& world) {
    const float angle = world.nextBotRandomFloat(0.0f, 6.2831853f);
    return Vec2(std::cos(angle), std::sin(angle));
}

/// @brief 计算逃跑方向：把所有威胁的“反方向单位向量”求和后归一化
/// @details 对每个感知半径内的威胁，取 (自己 - 威胁) 的单位向量累加，
///          这样多数威胁所在方向的反方向权重最大；最后归一化为一个方向。
/// @return 逃离方向；没有威胁时返回零向量（调用方保持原方向）
Vec2 pickFleeDir(const World& world, const Entity& bot) {
    Vec2 sum(0.0f, 0.0f);
    int threatCount = 0;
    const float radiusSq = cfg::kPanicRadius * cfg::kPanicRadius;

    for (const Entity& entity : world.entities()) {
        if (!entity.alive || entity.id == bot.id) {
            continue;   // 忽略死亡实体和自己
        }
        if (entity.pos.distanceSq(bot.pos) > radiusSq) {
            continue;   // 超出感知半径，无关
        }

        // 判定威胁：正在恐慌的同类（恐慌会“传染”），或半径达到 1.5 倍的真人
        bool threat = false;
        if (entity.kind == EntityKind::Bot &&
            entity.aiState == static_cast<uint8_t>(BotState::Panic)) {
            threat = true;
        } else if (entity.kind == EntityKind::Player &&
                   entity.radius >= bot.radius * cfg::kPanicPredatorRatio) {
            threat = true;
        }
        if (!threat) {
            continue;
        }

        const float distanceSq = entity.pos.distanceSq(bot.pos);
        if (distanceSq <= 1e-6f) {
            continue;   // 与自己位置重合时无法定义方向，跳过避免除零
        }
        const float inv = 1.0f / std::sqrt(distanceSq);
        sum.x += (bot.pos.x - entity.pos.x) * inv;  // 单位反方向分量累加
        sum.y += (bot.pos.y - entity.pos.y) * inv;
        ++threatCount;
    }

    return threatCount == 0 ? Vec2() : sum.normalized();
}

/// @brief 检测是否应进入恐慌：感知半径内是否存在威胁
/// @return true=存在大玩家或恐慌同类，应立刻逃跑
bool shouldEnterPanic(const World& world, const Entity& bot) {
    const float radiusSq = cfg::kPanicRadius * cfg::kPanicRadius;
    for (const Entity& entity : world.entities()) {
        if (!entity.alive || entity.id == bot.id) {
            continue;
        }
        if (entity.pos.distanceSq(bot.pos) > radiusSq) {
            continue;
        }
        if (entity.kind == EntityKind::Bot &&
            entity.aiState == static_cast<uint8_t>(BotState::Panic)) {
            return true;
        }
        if (entity.kind == EntityKind::Player &&
            entity.radius >= bot.radius * cfg::kPanicPredatorRatio) {
            return true;
        }
    }
    return false;
}

}  // namespace

/// @brief 进入恐慌（幂等：已在恐慌中不重复刷新计时）
void enterPanic(Entity& bot) {
    if (bot.aiState == static_cast<uint8_t>(BotState::Panic)) {
        return;
    }
    bot.aiState = static_cast<uint8_t>(BotState::Panic);
    bot.aiPanicTimer = cfg::kPanicDuration;
}

/// @brief NPC AI 每帧决策（World::step 第 1 步调用）
void tickBotAI(World& world, Entity& bot, float dt) {
    if (bot.kind != EntityKind::Bot || !bot.alive) {
        return;   // 只驱动存活的 NPC
    }

    // ---- 恐慌状态：倒计时 + 逃跑 ----
    if (bot.aiState == static_cast<uint8_t>(BotState::Panic)) {
        bot.aiPanicTimer -= dt;
        if (bot.aiPanicTimer <= 0.0f) {
            // 恐慌结束：回到漫游，清空计时器与方向，下一帧重新决策
            bot.aiState = static_cast<uint8_t>(BotState::Wander);
            bot.aiPanicTimer = 0.0f;
            bot.aiThinkTimer = 0.0f;
            bot.moveDir = Vec2();
        } else {
            const Vec2 flee = pickFleeDir(world, bot);
            if (!flee.isZero()) {
                bot.moveDir = flee;
            }
            return;   // 恐慌中不再做漫游决策
        }
    }

    // ---- 漫游状态：每 kBotThinkInterval(1.5s) 换一个随机方向 ----
    bot.aiThinkTimer -= dt;
    if (bot.aiThinkTimer <= 0.0f) {
        bot.aiThinkTimer = cfg::kBotThinkInterval;
        bot.moveDir = pickRandomDir(world);
    }

    // 漫游中也要随时感知威胁，发现即转入恐慌并立即逃跑
    if (shouldEnterPanic(world, bot)) {
        enterPanic(bot);
        const Vec2 flee = pickFleeDir(world, bot);
        if (!flee.isZero()) {
            bot.moveDir = flee;
        }
    }
}

/// @brief AI 速度倍率查询（World 移动积分时调用）
float aiSpeedMul(uint8_t aiState) {
    return aiState == static_cast<uint8_t>(BotState::Panic)
               ? cfg::kPanicSpeedMul
               : 1.0f;
}

}  // namespace blob
