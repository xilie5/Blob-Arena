// =============================================================================
//  world.h —— 服务端权威世界模拟
//
//  纯逻辑、无网络依赖。固定步长推进，所有吞噬与计分都在这里完成。
//
//  线程约定（极重要）：
//    World 只允许在 RoomWorker 线程被访问（线程封闭），内部一行锁都没有。
//    IO 线程绝不能直接读写 World——否则会与遍历中的游戏循环产生数据竞争、
//    迭代器失效、帧状态不一致。跨线程只通过 BoundedQueue 传小值。
//
//  事件输出模型：
//    World 不直接发包，只在内部累积两类事件供上层 drain（一次性抽走）：
//      changes_ 实体 Added/Removed；kills_ 击杀双方。
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

/// @brief 实体增删事件（上层据此做 AOI 增量同步）
struct EntityChange {
    /// @brief 事件类型
    enum class Type : uint8_t {
        Added = 0,    // 实体生成
        Removed = 1,  // 实体移除
    };

    Type    type = Type::Added;          ///< 增/删
    EntityId id = kInvalidEntityId;      ///< 受影响的实体 ID
    uint8_t reason = 0;                  ///< 移除原因（Removed 时有意义，见 proto::kRemoveReason*）
};

/// @brief 击杀事件：killer 吃掉了 victim
struct KillEvent {
    EntityId killer = kInvalidEntityId;  ///< 击杀者实体 ID
    EntityId victim = kInvalidEntityId;  ///< 被击杀者实体 ID
};

/// @brief 服务端权威游戏世界
/// @details 持有全部实体与随机数状态，以固定步长 step(dt) 推进规则，
///          包括移动积分、NPC AI、缩圈、边界、吞噬结算、食物/NPC 补充。
class World {
public:
    /// @brief 构造世界
    /// @param seed   随机种子（决定食物/玩家/NPC 初始分布，固定种子可复现整局）
    /// @param width  世界宽
    /// @param height 世界高
    World(uint32_t seed, float width, float height);

    // ---- 实体生成（返回新实体 ID；失败/达上限返回 kInvalidEntityId） ----
    EntityId spawnPlayer(const std::string& name);  ///< 随机位置+随机配色生成玩家
    EntityId spawnFood(const Vec2& pos);            ///< 在指定位置生成食物
    EntityId spawnBot(const std::string& name);     ///< 生成 NPC 并初始化漫游 AI
    /// @brief 实体生成总入口
    /// @param kind 种类  @param pos 位置  @param radius 半径
    /// @param color RGB 颜色  @param name 名字
    EntityId spawnEntity(EntityKind kind, const Vec2& pos, float radius,
                         uint32_t color, const std::string& name);

    /// @brief 标记删除实体（延迟到本帧 flushRemovals 才真正移除）
    /// @param id 目标实体  @param reason 移除原因
    void removeEntity(EntityId id, uint8_t reason = 0);
    Entity* find(EntityId id);               ///< 按 ID 查实体（可变），不存在返回 nullptr
    const Entity* find(EntityId id) const;   ///< 按 ID 查实体（只读），不存在返回 nullptr

    /// @brief 抽走累积的实体增删事件（抽取后内部清空）
    void drainChanges(std::vector<EntityChange>& out);
    /// @brief 抽走累积的击杀事件（抽取后内部清空）
    void drainKills(std::vector<KillEvent>& out);
    uint32_t killCount() const { return totalKills_; }  ///< 真人玩家间击杀总数

    /// @brief 补铺食物直到指定数量（受 kFoodMaxCount 上限约束）
    void populateFood(std::size_t count);
    std::size_t foodCount() const { return countKind(EntityKind::Food); } ///< 当前食物数
    uint32_t foodSpawnedCount() const { return foodSpawned_; }  ///< 累计生成食物数
    uint32_t foodEatenCount() const { return foodEaten_; }      ///< 累计被吃食物数

    /// @brief 补铺 NPC 直到指定数量（受 kBotCount 上限约束）
    void populateBots(std::size_t count);
    std::size_t botCount() const { return countKind(EntityKind::Bot); }  ///< 当前 NPC 数
    void setBotSpawnEnabled(bool enabled) { botSpawnEnabled_ = enabled; }///< 开关 NPC 补充
    bool botSpawnEnabled() const { return botSpawnEnabled_; }

    void setFoodSpawnEnabled(bool enabled) { foodSpawnEnabled_ = enabled; }///< 开关食物补充
    bool foodSpawnEnabled() const { return foodSpawnEnabled_; }

    /// @brief 设置某能动单位的移动方向（零向量表示停止；否则归一化）
    void setMoveDir(EntityId id, Vec2 dir);

    /// @brief 推进世界一个固定步长（核心流水线，见 .cpp 分步注释）
    /// @param dt 步长秒数（固定 1/30）
    void step(float dt);

    /// @brief 推进缩圈一帧（由 step 内部调用，也可独立测试）
    void tickShrink(float dt);

    const std::vector<Entity>& entities() const { return entities_; }  ///< 实体列表（只读）
    std::vector<Entity>& entities() { return entities_; }              ///< 实体列表（可变，AI 模块使用）
    std::size_t size() const { return entities_.size(); }             ///< 当前实体总数

    uint32_t tick() const { return tick_; }      ///< 已推进的帧号
    float width() const { return width_; }       ///< 世界宽
    float height() const { return height_; }     ///< 世界高
    float elapsed() const { return elapsed_; }   ///< 世界累计运行秒数
    uint32_t elapsedMs() const {                 ///< 累计运行毫秒（缩圈时间轴用）
        return static_cast<uint32_t>(elapsed_ * 1000.0f);
    }

    std::size_t countKind(EntityKind kind) const;  ///< 统计某类存活实体数
    std::size_t playerCount() const { return countKind(EntityKind::Player); } ///< 存活玩家数

    /// @brief 把坐标夹取到世界边界内（圆心至少距边一个半径，防止球出界）
    Vec2 clampToBounds(const Vec2& pos, float radius) const;

    // ---- 随机数（自实现 xorshift，跨平台序列一致；世界/NPC 两条独立流） ----
    uint32_t nextRandom();                        ///< 世界随机流下一个 uint32
    float nextRandomFloat(float lo, float hi);    ///< 世界随机流映射到 [lo,hi)
    uint32_t nextBotRandom();                     ///< NPC 随机流下一个 uint32
    float nextBotRandomFloat(float lo, float hi); ///< NPC 随机流映射到 [lo,hi)

    /// @brief 调试用：直接设置实体半径/质量（生产逻辑不调用）
    void debugSetRadius(EntityId id, float radius);

    // ---- 缩圈（安全区）状态 ----
    bool shrinkEnabled() const { return shrinkEnabled_; }        ///< 缩圈是否进行中
    float shrinkRadius() const { return shrinkRadius_; }         ///< 当前安全区半径
    Vec2 shrinkCenter() const { return shrinkCenter_; }          ///< 安全区圆心（世界中心）

    /// @brief 开启/关闭缩圈；首次开启时把圆心设为世界中心并记录起始时刻
    void setShrinkEnabled(bool enabled);
    void setShrinkOriginMs(uint32_t ms) { shrinkOriginMs_ = ms; }///< 设置缩圈起始时间轴
    uint32_t shrinkOriginMs() const { return shrinkOriginMs_; }
    uint32_t shrinkDurationMs() const { return shrinkDurationMs_; }
    void setShrinkDurationMs(uint32_t ms) { shrinkDurationMs_ = ms; }

private:
    void integrateMovement(float dt);       // 位置积分：按方向×速度×dt 移动
    void resolveWorldBounds();              // 所有实体坐标夹回边界
    void resolveEating();                   // 吞噬结算总入口（食物+能动单位）
    void resolveFoodEating();               // 吃食物结算
    void resolveActorEating();              // 玩家/NPC 互吃结算
    void updateFoodSpawn(float dt);         // 按每秒 40 个补充食物
    bool pickFoodSpawnPos(Vec2& out);       // 为食物找远离能动单位的位置
    bool pickActorSpawnPos(const Entity& actor, Vec2& out); // 为重生找安全点
    void respawnActor(Entity& actor);       // 被吃玩家缩小并安全重生
    void tickBots(float dt);                // 推进全部 NPC 的 AI
    void updateBotSpawn(float dt);          // 按每秒 1 个补充 NPC
    void notifyChange(EntityChange::Type type, EntityId id, uint8_t reason = 0); // 记增删事件
    void notifyKill(EntityId killer, EntityId victim); // 记击杀事件
    void flushRemovals();                   // 帧末统一物理删除待删实体

    // ---- 实体存储 ----
    std::vector<Entity> entities_;          ///< 实体连续存储（vector，缓存友好）
    std::unordered_map<EntityId, std::size_t> index_; ///< ID -> entities_ 下标，O(1) 查找
    std::vector<std::pair<EntityId, uint8_t>> pendingRemove_; ///< 本帧待删 (ID,原因)
    std::vector<EntityChange> changes_;     ///< 待抽走的增删事件
    std::vector<KillEvent> kills_;          ///< 待抽走的击杀事件
    std::vector<Entity*> eatOrder_;         ///< 互吃结算排序复用缓冲（避免每帧分配）

    // ---- 时间与随机 ----
    uint32_t tick_ = 0;                     ///< 帧号（每 step +1）
    uint32_t rngState_ = 1;                 ///< 世界随机流状态（xorshift）
    uint32_t botRngState_ = 1;              ///< NPC 专用随机流状态（与世界流隔离）
    float width_ = 0.0f;                    ///< 世界宽
    float height_ = 0.0f;                   ///< 世界高
    float elapsed_ = 0.0f;                  ///< 累计秒数
    EntityId nextId_ = 1;                   ///< 下一个实体 ID（从 1 开始，0 为无效）

    // ---- 食物生成 ----
    float foodSpawnAccum_ = 0.0f;           ///< 生成速率累积器（攒够 1 生 1 个）
    uint32_t foodSpawned_ = 0;              ///< 累计生成数
    uint32_t foodEaten_ = 0;                ///< 累计被吃数
    uint32_t totalKills_ = 0;               ///< 真人玩家间击杀总数
    bool foodSpawnEnabled_ = true;          ///< 是否允许动态补食物

    // ---- NPC 生成 ----
    float botSpawnAccum_ = 0.0f;            ///< NPC 生成累积器
    bool botSpawnEnabled_ = true;           ///< 是否允许动态补 NPC

    // ---- 缩圈 ----
    bool shrinkEnabled_ = false;            ///< 缩圈开关
    Vec2 shrinkCenter_{0.0f, 0.0f};         ///< 安全区圆心
    float shrinkRadius_ = 0.0f;             ///< 当前安全区半径
    uint32_t shrinkOriginMs_ = 0;           ///< 缩圈开始的世界时间(ms)
    uint32_t shrinkDurationMs_ = 0;         ///< 缩圈持续时长(ms)
};

}  // namespace blob
