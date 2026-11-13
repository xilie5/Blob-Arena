// =============================================================================
//  room_worker.h —— 独立房间逻辑线程
//
//  全服务器唯一一个 RoomWorker 线程，是 Lobby/Room/World 的唯一访问者：
//    - 从 inbound_ 队列接收网络侧（Gateway）投递来的命令并串行处理；
//    - 以 30Hz 固定步长推进所有房间（含追赶补帧）；
//    - 做 AOI 增量同步、断线 AI 托管、超时清理、对局结算落盘；
//    - 所有要发给客户端的包只写入 outbound_ 队列，由主线程取走转发，
//      本线程绝不直接做 socket I/O。
//
//  并发要点：游戏数据线程封闭、零锁；跨线程只有两条 BoundedQueue，
//  以及 std::atomic 的监控计数器（供主线程读取统计）。
// =============================================================================
#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "concurrency/bounded_queue.h"
#include "core/spatial_grid.h"
#include "net/platform.h"
#include "protocol/protocol.h"
#include "server/lobby.h"
#include "storage/data_store.h"

namespace blob {

/// @brief 进房方式
enum class JoinMode : uint8_t {
    Quick = 0,  // 快速匹配公共房
    Create = 1, // 创建私有房
    Code = 2,   // 凭房间码加入
};

// ============================== 入站命令（Gateway -> Worker） ==============================

/// 玩家加入/进房命令（鉴权完成后投递）
struct AddClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
    PlayerId playerId = kInvalidPlayerId;
    std::string username;
    JoinMode mode = JoinMode::Quick;
    std::string roomCode;  // mode=Code 时使用
};

/// 玩家主动离开命令
struct RemoveClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
};

/// 网络断开命令（玩家可能还要重连，实体先保留并 AI 托管）
struct DisconnectClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
};

/// 断线重连命令（绑定新 fd，拿回原实体）
struct ResumeClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
    PlayerId playerId = kInvalidPlayerId;
    std::string roomCode;
};

/// 移动输入命令（dx/dy 已归一化到约 -1~1）
struct InputCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
    float dirX = 0.0f;
    float dirY = 0.0f;
};

/// 入站命令的 tagged union（小值传递，无堆分配/虚函数）
using WorkerCommand =
    std::variant<AddClientCommand, RemoveClientCommand,
                 DisconnectClientCommand, ResumeClientCommand,
                 InputCommand>;

// ============================== 出站消息（Worker -> Gateway） ==============================

/// @brief 出站消息类型
enum class OutboundType : uint8_t {
    BindClient = 0,    ///< 绑定 fd↔房间↔实体（不发包，只更新会话状态，关键）
    SendToFd = 1,      ///< 单播给某个 fd
    BroadcastRoom = 2, ///< 向房间内全员广播（可排除某 fd）
};

/// @brief 一条出站消息
struct WorkerOutbound {
    OutboundType type = OutboundType::SendToFd;
    socket_t fd = BLOB_INVALID_SOCKET;        ///< 单播目标 / 绑定目标
    socket_t exceptFd = BLOB_INVALID_SOCKET;  ///< 广播时排除的 fd
    std::string roomCode;                     ///< 广播房间码
    EntityId entityId = kInvalidEntityId;     ///< 绑定用：玩家实体 ID
    std::string packet;                       ///< 已 packPacket 的完整数据包
};

/// @brief Worker 运行统计（快照式，供 /api/status、/metrics、控制台日志使用）
struct WorkerStats {
    std::size_t rooms = 0;        ///< 当前房间数
    std::size_t players = 0;      ///< 在线玩家数
    std::size_t disconnectedPlayers = 0; ///< 断线等待重连的玩家数
    std::size_t entities = 0;     ///< 全部房间实体总数
    uint64_t inboundDropped = 0;  ///< 入站队列丢弃命令数
    uint64_t outboundDropped = 0; ///< 出站队列丢弃消息数
    std::size_t inboundDepth = 0; ///< 入站队列积压
    std::size_t outboundDepth = 0;///< 出站队列积压
    double tickAvgMs = 0.0;      ///< 逻辑帧平均耗时
    double tickP95Ms = 0.0;      ///< 逻辑帧 P95 耗时
    double tickMaxMs = 0.0;      ///< 逻辑帧最大耗时
    uint64_t tickOver50 = 0;      ///< 耗时超过 50ms 的帧数
    std::size_t tickSamples = 0;  ///< 采样帧数
    uint64_t snapshotPackets = 0; ///< 已发送 Snapshot 包数
    uint64_t aoiBytes = 0;        ///< AOI 增量同步实际字节数
    uint64_t fullSnapshotBytes = 0; ///< 若全量广播所需的估算字节数
    uint64_t snapshotSamples = 0; ///< 快照采样次数
    uint64_t aoiEntitySamples = 0;///< AOI 平均实体数累计
    uint64_t fullEntitySamples = 0; ///< 全量实体数累计
    uint64_t aiTakeoverTicks = 0; ///< 断线 AI 托管 tick 次数
    uint64_t inputCommands = 0;   ///< 收到的移动指令数
};

/// @brief 房间逻辑线程（游戏世界唯一管理者）
class RoomWorker {
public:
    /// @brief 构造（不启动线程，需再调 start）
    RoomWorker(uint32_t seed, float worldSize,
               int countdownSec, int durationSec, int shrinkStartSec,
               int reconnectGraceMs,
               DataStore* store,
               std::size_t inboundCapacity = 4096,    ///< 入站队列容量
               std::size_t outboundCapacity = 32768); ///< 出站队列容量
    ~RoomWorker();

    // 持有线程/队列，不可拷贝
    RoomWorker(const RoomWorker&) = delete;
    RoomWorker& operator=(const RoomWorker&) = delete;

    void start();  ///< 启动逻辑线程
    void stop();   ///< 关闭队列并 join 线程

    // ---- 入站投递（Gateway 调用；全部 tryPush，满则计数丢弃并返回 false） ----
    bool postAddClient(const AddClientCommand& command);
    bool postRemoveClient(socket_t fd);
    bool postDisconnectClient(socket_t fd);
    bool postResumeClient(socket_t fd, PlayerId playerId,
                          const std::string& roomCode);
    bool postInput(socket_t fd, float dirX, float dirY);

    /// @brief 非阻塞取出一条出站消息（Gateway 每轮循环调用）
    bool pollOutbound(WorkerOutbound& out);
    /// @brief 读取一份统计快照（计数器是 atomic，可安全跨线程读）
    WorkerStats stats() const;

private:
    /// @brief Worker 眼中的一个玩家会话（含 AOI 差分状态）
    struct Member {
        /// @brief 上次已同步给该玩家的实体视图（用于差分比较）
        struct EntityView {
            float x = 0.0f;
            float y = 0.0f;
            float radius = 0.0f;
            int score = 0;

            /// 位置变化 >0.01、半径变化 >0.01 或分数变化才认为需要同步
            bool differs(const Entity& entity) const {
                return std::fabs(x - entity.pos.x) > 0.01f ||
                       std::fabs(y - entity.pos.y) > 0.01f ||
                       std::fabs(radius - entity.radius) > 0.01f ||
                       score != entity.score;
            }
        };

        PlayerId playerId = kInvalidPlayerId;
        socket_t fd = BLOB_INVALID_SOCKET;  ///< 当前连接（断线期间为无效值）
        std::string username;
        std::string roomCode;
        EntityId entityId = kInvalidEntityId;
        bool connected = true;              ///< 是否在线（false=断线托管中）
        std::chrono::steady_clock::time_point disconnectedAt{}; ///< 断线时刻（超时判定）
        std::vector<EntityId> visible;      ///< 本次视野查询结果
        std::unordered_map<EntityId, EntityView> known; ///< 已告诉过客户端的实体
    };

    void run();                  // 逻辑线程主循环（30Hz 固定节拍+补帧）
    void handleCommand(WorkerCommand& command); // 入站命令分发
    void handleAddClient(const AddClientCommand& command);
    void handleRemoveClient(socket_t fd);
    void handleDisconnectClient(socket_t fd);
    void handleResumeClient(const ResumeClientCommand& command);
    void handleInput(const InputCommand& command);
    void tickRooms(float dt);    // 每帧推进所有房间 + 同步 + 结算

    void emit(WorkerOutbound outbound, bool critical); ///< 出站统一入口（关键/非关键不同投递策略）
    std::size_t sendPacket(socket_t fd, proto::S2C msgId,
                           const std::string& payload); ///< 组单播包并入出站队列
    void broadcastPacket(const Room* room, proto::S2C msgId,
                         const std::string& payload,
                         socket_t exceptFd = BLOB_INVALID_SOCKET); ///< 组广播包并入队
    void persistMatch(Room* room, const std::vector<Standing>& standings); ///< 战绩落盘
    void refreshStats();        // 重新统计房间/玩家/实体数到 atomic
    void recordTickDuration(uint64_t microseconds); // 记录一帧耗时到环形数组
    void sendAoiUpdate(Room* room, socket_t fd, Member& member,
                       const SpatialGrid& grid);    // 单玩家 AOI 增量同步
    void applyAiTakeover(Room* room, Member& member); // 断线玩家的简单 AI
    void expireDisconnectedPlayers();   // 清理超过重连宽限期的玩家

    Member* findMemberByEntity(const Room* room, EntityId entityId);

    // ---- 配置与大厅 ----
    uint32_t seed_;
    float worldSize_;
    int countdownSec_;
    int durationSec_;
    int shrinkStartSec_;
    int reconnectGraceMs_;
    DataStore* store_ = nullptr;  ///< 存储层（不拥有，由 Gateway 持有；内部自带锁）
    Lobby lobby_;                 ///< 房间注册表（Worker 拥有全部房间）
    /// 所有房间共用同一套网格尺寸；复用它避免每次快照都重新分配格子数组
    SpatialGrid aoiGrid_;

    // ---- 两条跨线程队列 ----
    BoundedQueue<WorkerCommand> inbound_;   ///< 网络侧 -> 逻辑（tryPush，满则丢）
    BoundedQueue<WorkerOutbound> outbound_; ///< 逻辑 -> 网络侧（关键消息 waitPush）
    // ---- 玩家索引表（仅本线程访问，无需锁） ----
    std::unordered_map<PlayerId, Member> members_;
    std::unordered_map<socket_t, PlayerId> playerByFd_;

    // ---- 线程与跨线程监控计数 ----
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> inboundDropped_{0};
    std::atomic<uint64_t> outboundDropped_{0};
    std::atomic<std::size_t> roomCount_{0};
    std::atomic<std::size_t> playerCount_{0};
    std::atomic<std::size_t> disconnectedPlayerCount_{0};
    std::atomic<std::size_t> entityCount_{0};
    std::atomic<uint64_t> aoiBytes_{0};
    std::atomic<uint64_t> fullSnapshotBytes_{0};
    std::atomic<uint64_t> snapshotSamples_{0};
    std::atomic<uint64_t> aoiEntitySamples_{0};
    std::atomic<uint64_t> fullEntitySamples_{0};
    std::atomic<uint64_t> aiTakeoverTicks_{0};
    std::atomic<uint64_t> inputCommands_{0};

    // ---- 帧耗时采样（300 格环形数组，主线程据此算 P95） ----
    static constexpr std::size_t kTickWindow = 300;
    std::array<std::atomic<uint32_t>, kTickWindow> tickSamplesUs_{};
    std::atomic<std::size_t> tickSampleCount_{0};
    std::atomic<std::size_t> tickSampleIndex_{0};
    std::atomic<uint64_t> snapshotPackets_{0};

    /// 快照发送节拍累积器：30 tick/s 与 20 快照/s 不是整数倍，
    /// 用时间累积器才能得到真正的 20Hz（平均 50ms 一次）。
    float snapshotAccum_ = 0.0f;
};

}  // namespace blob
