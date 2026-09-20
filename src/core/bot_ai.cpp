#include "core/bot_ai.h"

#include <cmath>

#include "core/game_config.h"
#include "core/world.h"

namespace blob {

namespace {

Vec2 pickRandomDir(World& world) {
    const float angle = world.nextBotRandomFloat(0.0f, 6.2831853f);
    return Vec2(std::cos(angle), std::sin(angle));
}

Vec2 pickFleeDir(const World& world, const Entity& bot) {
    Vec2 sum(0.0f, 0.0f);
    int threatCount = 0;
    const float radiusSq = cfg::kPanicRadius * cfg::kPanicRadius;

    for (const Entity& entity : world.entities()) {
        if (!entity.alive || entity.id == bot.id) {
            continue;
        }
        if (entity.pos.distanceSq(bot.pos) > radiusSq) {
            continue;
        }

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
            continue;
        }
        const float inv = 1.0f / std::sqrt(distanceSq);
        sum.x += (bot.pos.x - entity.pos.x) * inv;
        sum.y += (bot.pos.y - entity.pos.y) * inv;
        ++threatCount;
    }

    return threatCount == 0 ? Vec2() : sum.normalized();
}

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

void enterPanic(Entity& bot) {
    if (bot.aiState == static_cast<uint8_t>(BotState::Panic)) {
        return;
    }
    bot.aiState = static_cast<uint8_t>(BotState::Panic);
    bot.aiPanicTimer = cfg::kPanicDuration;
}

void tickBotAI(World& world, Entity& bot, float dt) {
    if (bot.kind != EntityKind::Bot || !bot.alive) {
        return;
    }

    if (bot.aiState == static_cast<uint8_t>(BotState::Panic)) {
        bot.aiPanicTimer -= dt;
        if (bot.aiPanicTimer <= 0.0f) {
            bot.aiState = static_cast<uint8_t>(BotState::Wander);
            bot.aiPanicTimer = 0.0f;
            bot.aiThinkTimer = 0.0f;
            bot.moveDir = Vec2();
        } else {
            const Vec2 flee = pickFleeDir(world, bot);
            if (!flee.isZero()) {
                bot.moveDir = flee;
            }
            return;
        }
    }

    bot.aiThinkTimer -= dt;
    if (bot.aiThinkTimer <= 0.0f) {
        bot.aiThinkTimer = cfg::kBotThinkInterval;
        bot.moveDir = pickRandomDir(world);
    }

    if (shouldEnterPanic(world, bot)) {
        enterPanic(bot);
        const Vec2 flee = pickFleeDir(world, bot);
        if (!flee.isZero()) {
            bot.moveDir = flee;
        }
    }
}

float aiSpeedMul(uint8_t aiState) {
    return aiState == static_cast<uint8_t>(BotState::Panic)
               ? cfg::kPanicSpeedMul
               : 1.0f;
}

}  // namespace blob
