// =============================================================================
//  entity.h —— 世界中的玩家、食物与 NPC
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/game_config.h"
#include "core/vec2.h"

namespace blob {

using EntityId = uint32_t;
constexpr EntityId kInvalidEntityId = 0;

enum class EntityKind : uint8_t {
    Player = 0,
    Food   = 1,
    Bot    = 2,
};

inline const char* to_string(EntityKind kind) {
    switch (kind) {
        case EntityKind::Player: return "player";
        case EntityKind::Food:   return "food";
        case EntityKind::Bot:    return "bot";
    }
    return "unknown";
}

inline float massFromRadius(float radius) {
    return radius * radius;
}

inline float radiusFromMass(float mass) {
    return std::sqrt(mass);
}

inline float speedFromRadius(float radius) {
    const float ratio = radius / cfg::kPlayerStartRadius;
    if (ratio <= 1e-6f) {
        return cfg::kPlayerBaseSpeed;
    }
    return cfg::kPlayerBaseSpeed * std::pow(ratio, -cfg::kSpeedFalloff);
}

struct Entity {
    EntityId    id   = kInvalidEntityId;
    EntityKind  kind = EntityKind::Player;

    std::string name;
    Vec2        pos;
    Vec2        moveDir;

    float    radius = cfg::kPlayerStartRadius;
    float    mass   = massFromRadius(cfg::kPlayerStartRadius);
    uint32_t color  = 0x4FC3F7;

    bool alive = true;
    int  score = 0;
    int  eaten = 0;
    int  kills = 0;
    int  deaths = 0;

    uint8_t  aiState      = 0;
    uint32_t aiTargetId   = kInvalidEntityId;
    float    aiThinkTimer = 0.0f;
    float    aiPanicTimer = 0.0f;

    bool isActor() const {
        return kind == EntityKind::Player || kind == EntityKind::Bot;
    }

    float speed() const {
        return kind == EntityKind::Food ? 0.0f : speedFromRadius(radius);
    }
};

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

inline void absorbMass(Entity& eater, const Entity& victim) {
    eater.mass += victim.mass * cfg::kEatMassGain;
    eater.radius = std::min(radiusFromMass(eater.mass), cfg::kPlayerMaxRadius);
    eater.mass = massFromRadius(eater.radius);
}

}  // namespace blob
