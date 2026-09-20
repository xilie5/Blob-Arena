// =============================================================================
//  world.h —— 服务端权威世界模拟
//
//  纯逻辑、无网络依赖。固定步长推进，所有吞噬与计分都在这里完成。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/entity.h"
#include "core/game_config.h"
#include "core/vec2.h"

namespace blob {

struct EntityChange {
    enum class Type : uint8_t {
        Added = 0,
        Removed = 1,
    };

    Type    type = Type::Added;
    EntityId id = kInvalidEntityId;
    uint8_t reason = 0;
};

struct KillEvent {
    EntityId killer = kInvalidEntityId;
    EntityId victim = kInvalidEntityId;
};

class World {
public:
    World(uint32_t seed, float width, float height);

    EntityId spawnPlayer(const std::string& name);
    EntityId spawnFood(const Vec2& pos);
    EntityId spawnBot(const std::string& name);
    EntityId spawnEntity(EntityKind kind, const Vec2& pos, float radius,
                         uint32_t color, const std::string& name);

    void removeEntity(EntityId id, uint8_t reason = 0);
    Entity* find(EntityId id);
    const Entity* find(EntityId id) const;

    void drainChanges(std::vector<EntityChange>& out);
    void drainKills(std::vector<KillEvent>& out);
    uint32_t killCount() const { return totalKills_; }

    void populateFood(std::size_t count);
    std::size_t foodCount() const { return countKind(EntityKind::Food); }
    uint32_t foodSpawnedCount() const { return foodSpawned_; }
    uint32_t foodEatenCount() const { return foodEaten_; }

    void populateBots(std::size_t count);
    std::size_t botCount() const { return countKind(EntityKind::Bot); }
    void setBotSpawnEnabled(bool enabled) { botSpawnEnabled_ = enabled; }
    bool botSpawnEnabled() const { return botSpawnEnabled_; }

    void setFoodSpawnEnabled(bool enabled) { foodSpawnEnabled_ = enabled; }
    bool foodSpawnEnabled() const { return foodSpawnEnabled_; }

    void setMoveDir(EntityId id, Vec2 dir);
    void step(float dt);
    void tickShrink(float dt);

    const std::vector<Entity>& entities() const { return entities_; }
    std::vector<Entity>& entities() { return entities_; }
    std::size_t size() const { return entities_.size(); }

    uint32_t tick() const { return tick_; }
    float width() const { return width_; }
    float height() const { return height_; }
    float elapsed() const { return elapsed_; }
    uint32_t elapsedMs() const {
        return static_cast<uint32_t>(elapsed_ * 1000.0f);
    }

    std::size_t countKind(EntityKind kind) const;
    std::size_t playerCount() const { return countKind(EntityKind::Player); }

    Vec2 clampToBounds(const Vec2& pos, float radius) const;

    uint32_t nextRandom();
    float nextRandomFloat(float lo, float hi);
    uint32_t nextBotRandom();
    float nextBotRandomFloat(float lo, float hi);

    void debugSetRadius(EntityId id, float radius);

    bool shrinkEnabled() const { return shrinkEnabled_; }
    float shrinkRadius() const { return shrinkRadius_; }
    Vec2 shrinkCenter() const { return shrinkCenter_; }

    void setShrinkEnabled(bool enabled);
    void setShrinkOriginMs(uint32_t ms) { shrinkOriginMs_ = ms; }
    uint32_t shrinkOriginMs() const { return shrinkOriginMs_; }
    uint32_t shrinkDurationMs() const { return shrinkDurationMs_; }
    void setShrinkDurationMs(uint32_t ms) { shrinkDurationMs_ = ms; }

private:
    void integrateMovement(float dt);
    void resolveWorldBounds();
    void resolveEating();
    void resolveFoodEating();
    void resolveActorEating();
    void updateFoodSpawn(float dt);
    bool pickFoodSpawnPos(Vec2& out);
    bool pickActorSpawnPos(const Entity& actor, Vec2& out);
    void respawnActor(Entity& actor);
    void tickBots(float dt);
    void updateBotSpawn(float dt);
    void notifyChange(EntityChange::Type type, EntityId id, uint8_t reason = 0);
    void notifyKill(EntityId killer, EntityId victim);
    void flushRemovals();

    std::vector<Entity> entities_;
    std::unordered_map<EntityId, std::size_t> index_;
    std::vector<std::pair<EntityId, uint8_t>> pendingRemove_;
    std::vector<EntityChange> changes_;
    std::vector<KillEvent> kills_;
    std::vector<Entity*> eatOrder_;

    uint32_t tick_ = 0;
    uint32_t rngState_ = 1;
    uint32_t botRngState_ = 1;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float elapsed_ = 0.0f;
    EntityId nextId_ = 1;

    float foodSpawnAccum_ = 0.0f;
    uint32_t foodSpawned_ = 0;
    uint32_t foodEaten_ = 0;
    uint32_t totalKills_ = 0;
    bool foodSpawnEnabled_ = true;

    float botSpawnAccum_ = 0.0f;
    bool botSpawnEnabled_ = true;

    bool shrinkEnabled_ = false;
    Vec2 shrinkCenter_{0.0f, 0.0f};
    float shrinkRadius_ = 0.0f;
    uint32_t shrinkOriginMs_ = 0;
    uint32_t shrinkDurationMs_ = 0;
};

}  // namespace blob
