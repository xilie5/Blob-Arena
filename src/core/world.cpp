#include "core/world.h"

#include <algorithm>
#include <cmath>

#include "core/bot_ai.h"
#include "protocol/protocol.h"

namespace blob {
namespace {

// 玩家调色板：新玩家从中按世界随机流取色，颜色仅影响显示
const uint32_t kPlayerPalette[] = {
    0x4FC3F7, 0xFF8A65, 0x81C784, 0xBA68C8, 0xFFD54F, 0x4DB6AC,
    0xF06292, 0x9575CD, 0xA1887F, 0x7986CB, 0x4DD0E1, 0xDCE775,
};
constexpr std::size_t kPlayerPaletteSize =
    sizeof(kPlayerPalette) / sizeof(kPlayerPalette[0]);

// 食物调色板（NPC 也借用此调色板）
const uint32_t kFoodPalette[] = {
    0x7FE57F, 0x8FD9FF, 0xFFC96B, 0xFF9FB3, 0xC7A9F5,
};
constexpr std::size_t kFoodPaletteSize =
    sizeof(kFoodPalette) / sizeof(kFoodPalette[0]);

}  // namespace

World::World(uint32_t seed, float width, float height)
    : rngState_(seed == 0 ? 1u : seed),
      // NPC 随机流由世界种子派生（异或/移位混淆），做到两条流相关但不相同
      botRngState_(seed == 0 ? 0x9E3779B9u
                             : (seed ^ 0x85EBCA6Bu ^ (seed << 13))),
      width_(width),
      height_(height) {
    // 预分配容量到实体上限，避免游戏进行中 vector 扩容造成的移动/指针失效
    entities_.reserve(static_cast<std::size_t>(cfg::kMaxEntities));
    index_.reserve(static_cast<std::size_t>(cfg::kMaxEntities));
}

EntityId World::spawnEntity(EntityKind kind, const Vec2& pos, float radius,
                            uint32_t color, const std::string& name) {
    // 实体总数硬上限：防止无限生成撑爆内存与协议中的 u32/快照容量
    if (entities_.size() >= static_cast<std::size_t>(cfg::kMaxEntities)) {
        return kInvalidEntityId;
    }

    Entity entity;
    entity.id = nextId_++;                 // ID 单调递增，整局唯一
    entity.kind = kind;
    entity.name = name;
    entity.radius = radius;
    entity.mass = massFromRadius(radius);  // 质量始终与半径保持 r² 关系
    entity.color = color;
    entity.alive = true;
    entity.pos = clampToBounds(pos, radius); // 出生点夹到界内

    // 先登记 ID->下标，再入 vector；通知事件放在最后，取到的是最终 ID
    index_[entity.id] = entities_.size();
    entities_.push_back(std::move(entity));
    notifyChange(EntityChange::Type::Added, entities_.back().id);
    return entities_.back().id;
}

EntityId World::spawnPlayer(const std::string& name) {
    // 世界随机位置 + 调色板随机色 + 初始半径
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
    // NPC 位置/颜色走 NPC 独立随机流
    const Vec2 pos(nextBotRandomFloat(0.0f, width_),
                   nextBotRandomFloat(0.0f, height_));
    const uint32_t color = kFoodPalette[nextBotRandom() % kFoodPaletteSize];
    const EntityId id = spawnEntity(EntityKind::Bot, pos, cfg::kBotStartRadius,
                                    color, name);
    if (id != kInvalidEntityId) {
        Entity* bot = find(id);
        if (bot != nullptr) {
            // 初始化 AI：进入漫游、计时清零、给一个随机初始方向
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
    // 不存在或已标记删除则忽略（幂等，防止同一帧重复删）
    if (it == index_.end() || !entities_[it->second].alive) {
        return;
    }
    entities_[it->second].alive = false;          // 只打标记
    pendingRemove_.emplace_back(id, reason);      // 帧末统一物理删除
}

Entity* World::find(EntityId id) {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &entities_[it->second];
}

const Entity* World::find(EntityId id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &entities_[it->second];
}

/// 把内部累积的实体增删事件倒进 out（插入后清空，drain 语义：一次性消费）
void World::drainChanges(std::vector<EntityChange>& out) {
    out.insert(out.end(), changes_.begin(), changes_.end());
    changes_.clear();
}

/// 把内部累积的击杀事件倒进 out
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
    // 只接受存活能动单位的输入；食物/已删除实体忽略
    if (entity == nullptr || !entity->alive || !entity->isActor()) {
        return;
    }
    // 零方向表示停止；非零方向归一化（长度不影响速度，只表示方向）
    entity->moveDir = dir.isZero() ? Vec2() : dir.normalized();
}

void World::step(float dt) {
    elapsed_ += dt;
    tickBots(dt);            // 1. NPC 先决策（改写各自 moveDir）
    integrateMovement(dt);   // 2. 所有能动单位按 方向×速度×恐慌倍率×dt 位移
    tickShrink(dt);          // 3. 推进缩圈，圈外掉体积/重生/删除
    resolveWorldBounds();    // 4. 坐标夹回矩形边界
    resolveEating();         // 5. 吞噬结算（先吃食物，再互吃）
    updateFoodSpawn(dt);     // 6. 动态补食物
    updateBotSpawn(dt);      // 7. 动态补 NPC
    flushRemovals();         // 8. 统一物理删除本帧被标记的实体
    ++tick_;                 // 9. 帧号推进
}

void World::integrateMovement(float dt) {
    for (Entity& entity : entities_) {
        // 只移动“存活、能动、当前有方向”的实体
        if (!entity.alive || !entity.isActor() || entity.moveDir.isZero()) {
            continue;
        }
        // 位移 = 单位方向 × 速度 × AI状态倍率（恐慌1.6） × 时间步长
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
    resolveFoodEating();     // 先结算吃食物
    resolveActorEating();    // 再结算能动单位互吃
}

void World::resolveFoodEating() {
    // 朴素两层遍历：食物无反抗，且食物数量虽多但判定极轻
    for (Entity& eater : entities_) {
        if (!eater.alive || !eater.isActor()) {
            continue;
        }
        for (Entity& food : entities_) {
            if (!food.alive || food.kind != EntityKind::Food ||
                !canEat(eater, food)) {
                continue;
            }

            absorbMass(eater, food);           // 质量与半径增长
            eater.eaten += 1;
            eater.score += cfg::kScoreFood;    // +1 分
            ++foodEaten_;
            eater.pos = clampToBounds(eater.pos, eater.radius); // 变大后重新夹边界
            removeEntity(food.id);
        }
    }
}

void World::resolveActorEating() {
    // 收集全部存活能动单位，准备按“大优先”排序结算
    eatOrder_.clear();
    for (Entity& entity : entities_) {
        if (entity.alive && entity.isActor()) {
            eatOrder_.push_back(&entity);
        }
    }
    // 排序规则：半径大的先结算（大吃小，避免同一帧互相可吃时结果不确定）；
    // 半径相同则 ID 小的先，保证结算顺序完全确定、对局可复现
    std::sort(eatOrder_.begin(), eatOrder_.end(),
              [](const Entity* a, const Entity* b) {
                  if (a->radius != b->radius) {
                      return a->radius > b->radius;
                  }
                  return a->id < b->id;
              });

    for (Entity* eater : eatOrder_) {
        for (Entity& victim : entities_) {
            // 跳过：已死亡、非能动单位、自己、不满足吞噬条件
            if (!victim.alive || !victim.isActor() || victim.id == eater->id ||
                !canEat(*eater, victim)) {
                continue;
            }

            const EntityId victimId = victim.id;
            if (victim.kind == EntityKind::Bot) {
                // 吃 NPC：+5 分、记击杀，NPC 直接删除（之后由生成器补充）
                eater->kills += 1;
                eater->score += cfg::kScoreBot;
                notifyKill(eater->id, victimId);
                removeEntity(victimId);
                continue;
            }

            // 吃真人玩家：吸收质量 +20 分，被吃者缩小重生（不退出对局）
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

    // 安全区最大半径取世界短边一半（正方形地图即 width/2）
    const float maxRadius = std::min(width_, height_) * 0.5f;
    const uint32_t nowMs = elapsedMs();
    const uint32_t elapsed = nowMs > shrinkOriginMs_ ? nowMs - shrinkOriginMs_ : 0u;
    const uint32_t duration = std::max<uint32_t>(shrinkDurationMs_, 1u);
    float progress = static_cast<float>(elapsed) / static_cast<float>(duration);
    progress = std::clamp(progress, 0.0f, 1.0f);
    // smoothstep 平滑：t²(3-2t)，起止时刻速度为 0，圈半径变化不突兀
    const float smooth = progress * progress * (3.0f - 2.0f * progress);
    shrinkRadius_ = maxRadius *
                    (cfg::kShrinkStartRatio +
                     (cfg::kShrinkEndRatio - cfg::kShrinkStartRatio) * smooth);

    // 两阶段收集，避免在遍历 entities_ 时直接增删实体
    std::vector<EntityId> removeBots;
    std::vector<EntityId> respawnPlayers;
    const float radiusSq = shrinkRadius_ * shrinkRadius_;
    for (Entity& entity : entities_) {
        // 只处理：存活、能动、且在安全区外（距圆心距离 > 安全区半径）
        if (!entity.alive || !entity.isActor() ||
            entity.pos.distanceSq(shrinkCenter_) < radiusSq) {
            continue;
        }

        // 圈外：每秒损失当前半径的 6%
        entity.radius -= cfg::kShrinkDpsRatio * entity.radius * dt;
        if (entity.radius > cfg::kPlayerMinRadius) {
            entity.mass = massFromRadius(entity.radius); // 还没缩到下限，仅缩小
            continue;
        }
        // 已缩到下限：玩家安全重生，NPC 直接消亡
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
            break;   // 不超过场上食物上限
        }
        Vec2 pos;
        if (!pickFoodSpawnPos(pos) || spawnFood(pos) == kInvalidEntityId) {
            continue;
        }
        ++current;
    }
}

/// @brief 为新食物找一个远离能动单位的位置
/// @return true=找到并写入 out；尝试 kFoodSpawnAttempts 次失败返回 false
bool World::pickFoodSpawnPos(Vec2& out) {
    const float safeSq = cfg::kFoodSafeRadius * cfg::kFoodSafeRadius;
    for (int attempt = 0; attempt < cfg::kFoodSpawnAttempts; ++attempt) {
        const Vec2 candidate = clampToBounds(
            Vec2(nextRandomFloat(0.0f, width_), nextRandomFloat(0.0f, height_)),
            cfg::kFoodRadius);
        // 检查 60 半径内是否有能动单位，有则换点重试（食物不刷在玩家身上）
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
    // 累积器按“每秒生成数 × dt”增长，攒够 1 就生成 1 个（帧率无关的稳定速率）
    foodSpawnAccum_ += cfg::kFoodSpawnPerSec * dt;
    std::size_t current = foodCount();
    while (foodSpawnAccum_ >= 1.0f &&
           current < static_cast<std::size_t>(cfg::kFoodMaxCount)) {
        foodSpawnAccum_ -= 1.0f;
        Vec2 pos;
        if (!pickFoodSpawnPos(pos) || spawnFood(pos) == kInvalidEntityId) {
            foodSpawnAccum_ = 0.0f;  // 找不到安全点：清零避免空转狂试
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
            tickBotAI(*this, entity, dt);  // 委托给 bot_ai 状态机
        }
    }
}

void World::updateBotSpawn(float dt) {
    if (!botSpawnEnabled_ || cfg::kBotSpawnPerSec <= 0.0f || dt <= 0.0f) {
        return;
    }
    // NPC 阵亡后按每秒 1 个补充，保持场上对抗密度
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

/// @brief 为重生的能动单位寻找安全出生点（远离其他能动单位 220 半径）
bool World::pickActorSpawnPos(const Entity& actor, Vec2& out) {
    for (int attempt = 0; attempt < cfg::kPlayerRespawnAttempts; ++attempt) {
        const Vec2 candidate = clampToBounds(
            Vec2(nextRandomFloat(0.0f, width_), nextRandomFloat(0.0f, height_)),
            cfg::kPlayerMinRadius);
        bool safe = true;
        for (const Entity& other : entities_) {
            if (!other.alive || !other.isActor() || other.id == actor.id) {
                continue;   // 忽略自己
            }
            const float minDistance = other.radius + cfg::kPlayerRespawnSafeRadius;
            if (candidate.distanceSq(other.pos) < minDistance * minDistance) {
                safe = false;   // 离别的球太近，重生即被吃，换点
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

/// @brief 玩家被吃后重生：半径缩到最小值，放到安全点，死亡数 +1
void World::respawnActor(Entity& actor) {
    Vec2 pos;
    if (!pickActorSpawnPos(actor, pos)) {
        // 找不到安全点则在任意界内位置重生（极端拥挤情况下的兜底）
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
    // 圆心活动范围：[radius, 边-radius]；max 处理世界比球还小的极端情况
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
    // 统一物理删除：不能在遍历过程中直接删 vector 元素（会使迭代/下标失效）
    for (const auto& pending : pendingRemove_) {
        auto it = index_.find(pending.first);
        if (it == index_.end()) {
            continue;
        }
        const std::size_t removedIndex = it->second;
        const std::size_t lastIndex = entities_.size() - 1;
        if (removedIndex != lastIndex) {
            // swap-pop：用最后一个实体覆盖被删位置，O(1) 删除
            entities_[removedIndex] = std::move(entities_[lastIndex]);
            index_[entities_[removedIndex].id] = removedIndex; // 被搬移实体更新下标
        }
        entities_.pop_back();
        index_.erase(pending.first);
        notifyChange(EntityChange::Type::Removed, pending.first, pending.second);
    }
    pendingRemove_.clear();
}

// ---- xorshift 随机数：无标准库 RNG 的线程安全/跨平台开销，序列确定可复现 ----
uint32_t World::nextRandom() {
    uint32_t state = rngState_;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    rngState_ = state;
    return state;
}

/// 把随机 uint32 的低 24 位映射到 [lo,hi) 浮点区间
float World::nextRandomFloat(float lo, float hi) {
    const float unit =
        static_cast<float>(nextRandom() & 0x00FFFFFFu) / 16777216.0f;
    return lo + (hi - lo) * unit;
}

// NPC 专用随机流（与世界流算法相同、状态独立）
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
        // 首次开启：圆心定在世界中心，记录起始时间轴，缺省补默认持续时长
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
