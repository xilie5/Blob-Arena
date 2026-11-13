// =============================================================================
//  entity.h —— 世界中的玩家、食物与 NPC
//
//  定义实体 ID 类型、实体种类枚举、实体数据结构，以及“半径↔质量↔速度”
//  物理换算和吞噬判定规则。被 World/Room 等游戏逻辑层广泛使用。
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/game_config.h"
#include "core/vec2.h"

namespace blob {

/// 实体唯一 ID 类型；0 保留为“无效 ID”
using EntityId = uint32_t;
constexpr EntityId kInvalidEntityId = 0;

/// @brief 实体种类（数值同时用于协议下发，勿调整顺序）
enum class EntityKind : uint8_t {
    Player = 0,  // 真人玩家
    Food   = 1,  // 食物：静止、被吃后消失补充
    Bot    = 2,  // NPC：由 bot_ai 驱动的机器人
};

/// 种类转字符串（日志/调试用）
inline const char* to_string(EntityKind kind) {
    switch (kind) {
        case EntityKind::Player: return "player";
        case EntityKind::Food:   return "food";
        case EntityKind::Bot:    return "bot";
    }
    return "unknown";
}

/// @brief 由半径算质量：质量正比于“面积”（r²），视觉上圆越大质量按平方增长
inline float massFromRadius(float radius) {
    return radius * radius;
}

/// @brief 由质量反算半径：sqrt(m)
inline float radiusFromMass(float mass) {
    return std::sqrt(mass);
}

/// @brief 由半径算移动速度
/// @details 幂函数衰减 speed = base * (r / r0)^(-0.22)：
///          半径越大速度越慢，平衡“大球强但不灵活”；小球快但弱。
///          ratio 极小时直接返回基础速度，防止 pow() 数值异常。
inline float speedFromRadius(float radius) {
    const float ratio = radius / cfg::kPlayerStartRadius;
    if (ratio <= 1e-6f) {
        return cfg::kPlayerBaseSpeed;
    }
    return cfg::kPlayerBaseSpeed * std::pow(ratio, -cfg::kSpeedFalloff);
}

/// @brief 一个世界实体（玩家/食物/NPC 的统一数据结构）
struct Entity {
    EntityId    id   = kInvalidEntityId;  ///< 全局唯一 ID（World 分配，从 1 递增）
    EntityKind  kind = EntityKind::Player;///< 种类

    std::string name;  ///< 显示名（食物为空）
    Vec2        pos;   ///< 当前圆心坐标
    Vec2        moveDir;///< 期望移动方向（单位向量或零向量）

    float    radius = cfg::kPlayerStartRadius; ///< 半径（决定碰撞与吞噬）
    float    mass   = massFromRadius(cfg::kPlayerStartRadius); ///< 质量（r²）
    uint32_t color  = 0x4FC3F7;                ///< 显示颜色（RGB，调色板随机分配）

    bool alive = true;  ///< 存活标志；被标记删除后置 false，帧末统一物理移除
    int  score = 0;     ///< 分数（食物 1 / NPC 5 / 玩家 20）
    int  eaten = 0;     ///< 吞噬计数（吃了多少个单位）
    int  kills = 0;     ///< 击杀计数
    int  deaths = 0;    ///< 死亡/被吞噬计数（玩家被吃重生时 +1）

    // ---- NPC AI 专用字段（玩家实体不使用） ----
    uint8_t  aiState      = 0;                    ///< BotState（0 漫游 / 1 恐慌）
    uint32_t aiTargetId   = kInvalidEntityId;     ///< 预留：AI 目标实体 ID
    float    aiThinkTimer = 0.0f;                 ///< 距离下次换方向的倒计时
    float    aiPanicTimer = 0.0f;                 ///< 恐慌剩余时间

    /// @brief 是否为“能动单位”（玩家或 NPC）；食物永远静止
    bool isActor() const {
        return kind == EntityKind::Player || kind == EntityKind::Bot;
    }

    /// @brief 当前移速：食物为 0，其余按半径幂函数衰减
    float speed() const {
        return kind == EntityKind::Food ? 0.0f : speedFromRadius(radius);
    }
};

/// @brief 吞噬判定：a 能否吃掉 b
/// @details 规则（全部满足才可吃）：
///          1. NPC 不能吃任何能动单位（NPC 只能吃食物，避免机器人互吃）；
///          2. 双方半径必须为正；
///          3. a 半径至少为 b 的 kEatRadiusRatio(1.15) 倍；
///          4. 距离近到“a 半径 - b 半径×0.40”以内（要求重叠约 40%，
///             擦边不算，避免高速移动时误吞）。
/// @note 距离比较全部使用平方值，省去 sqrt。
inline bool canEat(const Entity& a, const Entity& b) {
    if (a.kind == EntityKind::Bot && b.isActor()) {
        return false;
    }
    if (b.radius <= 0.0f || a.radius <= 0.0f) {
        return false;
    }
    if (a.radius < b.radius * cfg::kEatRadiusRatio) {
        return false;
    }
    const float reach = a.radius - b.radius * cfg::kEatOverlap;
    if (reach <= 0.0f) {
        return false;
    }
    return a.pos.distanceSq(b.pos) < reach * reach;
}

/// @brief 结算吞噬：把 victim 的质量加到 eater 身上并重算半径
/// @details 质量相加后由质量反推半径，再用 kPlayerMaxRadius(400) 封顶；
///          封顶后回写 mass 保持 mass 与 radius 的 r² 一致关系。
inline void absorbMass(Entity& eater, const Entity& victim) {
    eater.mass += victim.mass * cfg::kEatMassGain;
    eater.radius = std::min(radiusFromMass(eater.mass), cfg::kPlayerMaxRadius);
    eater.mass = massFromRadius(eater.radius);
}

}  // namespace blob
