#include "core/world.h"

#include <algorithm>
#include <cmath>

#include "core/bot_ai.h"
#include "protocol/protocol.h"

namespace blob {
namespace {

const uint32_t kPlayerPalette[] = {
    0x4FC3F7, 0xFF8A65, 0x81C784, 0xBA68C8, 0xFFD54F, 0x4DB6AC,
    0xF06292, 0x9575CD, 0xA1887F, 0x7986CB, 0x4DD0E1, 0xDCE775,
};
constexpr std::size_t kPlayerPaletteSize =
    sizeof(kPlayerPalette) / sizeof(kPlayerPalette[0]);

const uint32_t kFoodPalette[] = {
    0x7FE57F, 0x8FD9FF, 0xFFC96B, 0xFF9FB3, 0xC7A9F5,
};
constexpr std::size_t kFoodPaletteSize =
    sizeof(kFoodPalette) / sizeof(kFoodPalette[0]);

}  // namespace

World::World(uint32_t seed, float width, float height)
    : rngState_(seed == 0 ? 1u : seed),
      botRngState_(seed == 0 ? 0x9E3779B9u
                             : (seed ^ 0x85EBCA6Bu ^ (seed << 13))),
      width_(width),
      height_(height) {
    entities_.reserve(static_cast<std::size_t>(cfg::kMaxEntities));
    index_.reserve(static_cast<std::size_t>(cfg::kMaxEntities));
}

EntityId World::spawnEntity(EntityKind kind, const Vec2& pos, float radius,
                            uint32_t color, const std::string& name) {
    if (entities_.size() >= static_cast<std::size_t>(cfg::kMaxEntities)) {
        return kInvalidEntityId;
    }

    Entity entity;
    entity.id = nextId_++;
    entity.kind = kind;
    entity.name = name;
    entity.radius = radius;
    entity.mass = massFromRadius(radius);
    entity.color = color;
    entity.alive = true;
    entity.pos = clampToBounds(pos, radius);

    index_[entity.id] = entities_.size();
    entities_.push_back(std::move(entity));
    notifyChange(EntityChange::Type::Added, entities_.back().id);
    return entities_.back().id;
}

EntityId World::spawnPlayer(const std::string& name) {
    const Vec2 pos(nextRandomFloat(0.0f, width_),
                   nextRandomFloat(0.0f, height_));
    const uint32_t color = kPlayerPalette[nextRandom() % kPlayerPaletteSize];
    return spawnEntity(EntityKind::Player, pos, cfg::kPlayerStartRadius,
                       color, name);
}

EntityId World::spawnFood(const Vec2& pos) {
    const uint32_t color = kFoodPalette[nextRandom() % kFoodPaletteSize];
    const EntityId id = spawnEntity(EntityKind::Food, pos, cfg::kFoodRadius,
                                    color, std::string());
    if (id != kInvalidEntityId) {
        ++foodSpawned_;
    }
    return id;
}

EntityId World::spawnBot(const std::string& name) {
    const Vec2 pos(nextBotRandomFloat(0.0f, width_),
                   nextBotRandomFloat(0.0f, height_));
    const uint32_t color = kFoodPalette[nextBotRandom() % kFoodPaletteSize];
    const EntityId id = spawnEntity(EntityKind::Bot, pos, cfg::kBotStartRadius,
                                    color, name);
    if (id != kInvalidEntityId) {
        Entity* bot = find(id);
        if (bot != nullptr) {
            bot->aiState = static_cast<uint8_t>(BotState::Wander);
            bot->aiThinkTimer = 0.0f;
            const float angle = nextBotRandomFloat(0.0f, 6.2831853f);
            bot->moveDir = Vec2(std::cos(angle), std::sin(angle));
        }
    }
    return id;
}

void World::removeEntity(EntityId id, uint8_t reason) {
    if (id == kInvalidEntityId) {
        return;
    }
    auto it = index_.find(id);
    if (it == index_.end() || !entities_[it->second].alive) {
        return;
    }
    entities_[it->second].alive = false;
    pendingRemove_.emplace_back(id, reason);
}

Entity* World::find(EntityId id) {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &entities_[it->second];
}

const Entity* World::find(EntityId id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &entities_[it->second];
}

void World::drainChanges(std::vector<EntityChange>& out) {
    out.insert(out.end(), changes_.begin(), changes_.end());
    changes_.clear();
}

void World::drainKills(std::vector<KillEvent>& out) {
    out.insert(out.end(), kills_.begin(), kills_.end());
    kills_.clear();
}

std::size_t World::countKind(EntityKind kind) const {
    std::size_t count = 0;
    for (const Entity& entity : entities_) {
        if (entity.alive && entity.kind == kind) {
            ++count;
        }
    }
    return count;
}

void World::setMoveDir(EntityId id, Vec2 dir) {
    Entity* entity = find(id);
    if (entity == nullptr || !entity->alive || !entity->isActor()) {
        return;
    }
    entity->moveDir = dir.isZero() ? Vec2() : dir.normalized();
}

void World::step(float dt) {
    elapsed_ += dt;
    tickBots(dt);
    integrateMovement(dt);
    tickShrink(dt);
    resolveWorldBounds();
    resolveEating();
    updateFoodSpawn(dt);
    updateBotSpawn(dt);
    flushRemovals();
    ++tick_;
}

void World::integrateMovement(float dt) {
    for (Entity& entity : entities_) {
        if (!entity.alive || !entity.isActor() || entity.moveDir.isZero()) {
            continue;
        }
        entity.pos += entity.moveDir * (entity.speed() *
                                        aiSpeedMul(entity.aiState) * dt);
    }
}

void World::resolveWorldBounds() {
    for (Entity& entity : entities_) {
        if (entity.alive) {
            entity.pos = clampToBounds(entity.pos, entity.radius);
        }
    }
}

void World::resolveEating() {
    resolveFoodEating();
    resolveActorEating();
}

void World::resolveFoodEating() {
    for (Entity& eater : entities_) {
        if (!eater.alive || !eater.isActor()) {
            continue;
        }
        for (Entity& food : entities_) {
            if (!food.alive || food.kind != EntityKind::Food ||
                !canEat(eater, food)) {
                continue;
            }

            absorbMass(eater, food);
            eater.eaten += 1;
            eater.score += cfg::kScoreFood;
            ++foodEaten_;
            eater.pos = clampToBounds(eater.pos, eater.radius);
            removeEntity(food.id);
        }
    }
}

void World::resolveActorEating() {
    eatOrder_.clear();
    for (Entity& entity : entities_) {
        if (entity.alive && entity.isActor()) {
            eatOrder_.push_back(&entity);
        }
    }
    std::sort(eatOrder_.begin(), eatOrder_.end(),
              [](const Entity* a, const Entity* b) {
                  if (a->radius != b->radius) {
                      return a->radius > b->radius;
                  }
                  return a->id < b->id;
              });

    for (Entity* eater : eatOrder_) {
        for (Entity& victim : entities_) {
            if (!victim.alive || !victim.isActor() || victim.id == eater->id ||
                !canEat(*eater, victim)) {
                continue;
            }

            const EntityId victimId = victim.id;
            if (victim.kind == EntityKind::Bot) {
                eater->kills += 1;
                eater->score += cfg::kScoreBot;
                notifyKill(eater->id, victimId);
                removeEntity(victimId);
                continue;
            }

            absorbMass(*eater, victim);
            eater->eaten += 1;
            eater->kills += 1;
            eater->score += cfg::kScorePlayer;
            eater->pos = clampToBounds(eater->pos, eater->radius);
            ++totalKills_;
            notifyKill(eater->id, victimId);
            respawnActor(victim);
        }
    }
}

void World::tickShrink(float dt) {
    if (!shrinkEnabled_) {
        return;
    }

    const float maxRadius = std::min(width_, height_) * 0.5f;
    const uint32_t nowMs = elapsedMs();
    const uint32_t elapsed = nowMs > shrinkOriginMs_ ? nowMs - shrinkOriginMs_ : 0u;
    const uint32_t duration = std::max<uint32_t>(shrinkDurationMs_, 1u);
    float progress = static_cast<float>(elapsed) / static_cast<float>(duration);
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float smooth = progress * progress * (3.0f - 2.0f * progress);
    shrinkRadius_ = maxRadius *
                    (cfg::kShrinkStartRatio +
                     (cfg::kShrinkEndRatio - cfg::kShrinkStartRatio) * smooth);

    std::vector<EntityId> removeBots;
    std::vector<EntityId> respawnPlayers;
    const float radiusSq = shrinkRadius_ * shrinkRadius_;
    for (Entity& entity : entities_) {
        if (!entity.alive || !entity.isActor() ||
            entity.pos.distanceSq(shrinkCenter_) < radiusSq) {
            continue;
        }

        entity.radius -= cfg::kShrinkDpsRatio * entity.radius * dt;
        if (entity.radius > cfg::kPlayerMinRadius) {
            entity.mass = massFromRadius(entity.radius);
            continue;
        }
        if (entity.kind == EntityKind::Player) {
            respawnPlayers.push_back(entity.id);
        } else {
            removeBots.push_back(entity.id);
        }
    }

    for (EntityId id : removeBots) {
        removeEntity(id, proto::kRemoveReasonOutOfBounds);
    }
    for (EntityId id : respawnPlayers) {
        Entity* player = find(id);
        if (player != nullptr && player->alive) {
            respawnActor(*player);
        }
    }
}

void World::populateFood(std::size_t count) {
    std::size_t current = foodCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (current >= static_cast<std::size_t>(cfg::kFoodMaxCount)) {
            break;
        }
        Vec2 pos;
        if (!pickFoodSpawnPos(pos) || spawnFood(pos) == kInvalidEntityId) {
            continue;
        }
        ++current;
    }
}

bool World::pickFoodSpawnPos(Vec2& out) {
    const float safeSq = cfg::kFoodSafeRadius * cfg::kFoodSafeRadius;
    for (int attempt = 0; attempt < cfg::kFoodSpawnAttempts; ++attempt) {
        const Vec2 candidate = clampToBounds(
            Vec2(nextRandomFloat(0.0f, width_), nextRandomFloat(0.0f, height_)),
            cfg::kFoodRadius);
        bool crowded = false;
        for (const Entity& entity : entities_) {
            if (entity.alive && entity.isActor() &&
                entity.pos.distanceSq(candidate) < safeSq) {
                crowded = true;
                break;
            }
        }
        if (!crowded) {
            out = candidate;
            return true;
        }
    }
    return false;
}

void World::updateFoodSpawn(float dt) {
    if (!foodSpawnEnabled_ || cfg::kFoodSpawnPerSec <= 0.0f || dt <= 0.0f) {
        return;
    }
    foodSpawnAccum_ += cfg::kFoodSpawnPerSec * dt;
    std::size_t current = foodCount();
    while (foodSpawnAccum_ >= 1.0f &&
           current < static_cast<std::size_t>(cfg::kFoodMaxCount)) {
        foodSpawnAccum_ -= 1.0f;
        Vec2 pos;
        if (!pickFoodSpawnPos(pos) || spawnFood(pos) == kInvalidEntityId) {
            foodSpawnAccum_ = 0.0f;
            break;
        }
        ++current;
    }
}

void World::populateBots(std::size_t count) {
    std::size_t current = botCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (current >= static_cast<std::size_t>(cfg::kBotCount) ||
            entities_.size() >= static_cast<std::size_t>(cfg::kMaxEntities)) {
            break;
        }
        if (spawnBot(std::string()) == kInvalidEntityId) {
            break;
        }
        ++current;
    }
}

void World::tickBots(float dt) {
    for (Entity& entity : entities_) {
        if (entity.alive && entity.kind == EntityKind::Bot) {
            tickBotAI(*this, entity, dt);
        }
    }
}

void World::updateBotSpawn(float dt) {
    if (!botSpawnEnabled_ || cfg::kBotSpawnPerSec <= 0.0f || dt <= 0.0f) {
        return;
    }
    botSpawnAccum_ += cfg::kBotSpawnPerSec * dt;
    std::size_t current = botCount();
    while (botSpawnAccum_ >= 1.0f &&
           current < static_cast<std::size_t>(cfg::kBotCount)) {
        botSpawnAccum_ -= 1.0f;
        if (entities_.size() >= static_cast<std::size_t>(cfg::kMaxEntities)) {
            break;
        }
        Vec2 pos;
        if (!pickFoodSpawnPos(pos) || spawnBot(std::string()) == kInvalidEntityId) {
            botSpawnAccum_ = 0.0f;
            break;
        }
        ++current;
    }
}

bool World::pickActorSpawnPos(const Entity& actor, Vec2& out) {
    for (int attempt = 0; attempt < cfg::kPlayerRespawnAttempts; ++attempt) {
        const Vec2 candidate = clampToBounds(
            Vec2(nextRandomFloat(0.0f, width_), nextRandomFloat(0.0f, height_)),
            cfg::kPlayerMinRadius);
        bool safe = true;
        for (const Entity& other : entities_) {
            if (!other.alive || !other.isActor() || other.id == actor.id) {
                continue;
            }
            const float minDistance = other.radius + cfg::kPlayerRespawnSafeRadius;
            if (candidate.distanceSq(other.pos) < minDistance * minDistance) {
                safe = false;
                break;
            }
        }
        if (safe) {
            out = candidate;
            return true;
        }
    }
    return false;
}

void World::respawnActor(Entity& actor) {
    Vec2 pos;
    if (!pickActorSpawnPos(actor, pos)) {
        pos = clampToBounds(
            Vec2(nextRandomFloat(0.0f, width_), nextRandomFloat(0.0f, height_)),
            cfg::kPlayerMinRadius);
    }
    actor.radius = cfg::kPlayerMinRadius;
    actor.mass = massFromRadius(actor.radius);
    actor.pos = pos;
    actor.moveDir = Vec2();
    actor.deaths += 1;
}

Vec2 World::clampToBounds(const Vec2& pos, float radius) const {
    const float minX = radius;
    const float maxX = std::max(radius, width_ - radius);
    const float minY = radius;
    const float maxY = std::max(radius, height_ - radius);
    return Vec2(clampf(pos.x, minX, maxX), clampf(pos.y, minY, maxY));
}

void World::notifyChange(EntityChange::Type type, EntityId id, uint8_t reason) {
    changes_.push_back(EntityChange{type, id, reason});
}

void World::notifyKill(EntityId killer, EntityId victim) {
    kills_.push_back(KillEvent{killer, victim});
}

void World::flushRemovals() {
    for (const auto& pending : pendingRemove_) {
        auto it = index_.find(pending.first);
        if (it == index_.end()) {
            continue;
        }
        const std::size_t removedIndex = it->second;
        const std::size_t lastIndex = entities_.size() - 1;
        if (removedIndex != lastIndex) {
            entities_[removedIndex] = std::move(entities_[lastIndex]);
            index_[entities_[removedIndex].id] = removedIndex;
        }
        entities_.pop_back();
        index_.erase(pending.first);
        notifyChange(EntityChange::Type::Removed, pending.first, pending.second);
    }
    pendingRemove_.clear();
}

uint32_t World::nextRandom() {
    uint32_t state = rngState_;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    rngState_ = state;
    return state;
}

float World::nextRandomFloat(float lo, float hi) {
    const float unit =
        static_cast<float>(nextRandom() & 0x00FFFFFFu) / 16777216.0f;
    return lo + (hi - lo) * unit;
}

uint32_t World::nextBotRandom() {
    uint32_t state = botRngState_;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    botRngState_ = state;
    return state;
}

float World::nextBotRandomFloat(float lo, float hi) {
    const float unit =
        static_cast<float>(nextBotRandom() & 0x00FFFFFFu) / 16777216.0f;
    return lo + (hi - lo) * unit;
}

void World::debugSetRadius(EntityId id, float radius) {
    Entity* entity = find(id);
    if (entity != nullptr) {
        entity->radius = radius;
        entity->mass = massFromRadius(radius);
    }
}

void World::setShrinkEnabled(bool enabled) {
    if (enabled && !shrinkEnabled_) {
        shrinkCenter_ = Vec2(width_ * 0.5f, height_ * 0.5f);
        shrinkOriginMs_ = elapsedMs();
        if (shrinkDurationMs_ == 0) {
            shrinkDurationMs_ = cfg::kArenaShrinkDurationMs;
        }
    }
    if (!enabled) {
        shrinkRadius_ = 0.0f;
    }
    shrinkEnabled_ = enabled;
}

}  // namespace blob
