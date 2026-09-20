// =============================================================================
//  bot_ai.h —— NPC 行为状态机
// =============================================================================
#pragma once

#include <cstdint>

#include "core/entity.h"

namespace blob {

class World;

enum class BotState : uint8_t {
    Wander = 0,
    Panic  = 1,
};

void tickBotAI(World& world, Entity& bot, float dt);
void enterPanic(Entity& bot);
float aiSpeedMul(uint8_t aiState);

}  // namespace blob
