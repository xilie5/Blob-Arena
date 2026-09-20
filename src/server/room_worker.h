// =============================================================================
//  room_worker.h —— 独立房间逻辑线程
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

enum class JoinMode : uint8_t {
    Quick = 0,
    Create = 1,
    Code = 2,
};

struct AddClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
    PlayerId playerId = kInvalidPlayerId;
    std::string username;
    JoinMode mode = JoinMode::Quick;
    std::string roomCode;
};

struct RemoveClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
};

struct DisconnectClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
};

struct ResumeClientCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
    PlayerId playerId = kInvalidPlayerId;
    std::string roomCode;
};

struct InputCommand {
    socket_t fd = BLOB_INVALID_SOCKET;
    float dirX = 0.0f;
    float dirY = 0.0f;
};

using WorkerCommand =
    std::variant<AddClientCommand, RemoveClientCommand,
                 DisconnectClientCommand, ResumeClientCommand,
                 InputCommand>;

enum class OutboundType : uint8_t {
    BindClient = 0,
    SendToFd = 1,
    BroadcastRoom = 2,
};

struct WorkerOutbound {
    OutboundType type = OutboundType::SendToFd;
    socket_t fd = BLOB_INVALID_SOCKET;
    socket_t exceptFd = BLOB_INVALID_SOCKET;
    std::string roomCode;
    EntityId entityId = kInvalidEntityId;
    std::string packet;
};

struct WorkerStats {
    std::size_t rooms = 0;
    std::size_t players = 0;
    std::size_t disconnectedPlayers = 0;
    std::size_t entities = 0;
    uint64_t inboundDropped = 0;
    uint64_t outboundDropped = 0;
    std::size_t inboundDepth = 0;
    std::size_t outboundDepth = 0;
    double tickAvgMs = 0.0;
    double tickP95Ms = 0.0;
    double tickMaxMs = 0.0;
    uint64_t tickOver50 = 0;
    std::size_t tickSamples = 0;
    uint64_t snapshotPackets = 0;
    uint64_t aoiBytes = 0;
    uint64_t fullSnapshotBytes = 0;
    uint64_t snapshotSamples = 0;
    uint64_t aoiEntitySamples = 0;
    uint64_t fullEntitySamples = 0;
    uint64_t aiTakeoverTicks = 0;
    uint64_t inputCommands = 0;
};

class RoomWorker {
public:
    RoomWorker(uint32_t seed, float worldSize,
               int countdownSec, int durationSec, int shrinkStartSec,
               int reconnectGraceMs,
               DataStore* store,
               std::size_t inboundCapacity = 4096,
               std::size_t outboundCapacity = 32768);
    ~RoomWorker();

    RoomWorker(const RoomWorker&) = delete;
    RoomWorker& operator=(const RoomWorker&) = delete;

    void start();
    void stop();

    bool postAddClient(const AddClientCommand& command);
    bool postRemoveClient(socket_t fd);
    bool postDisconnectClient(socket_t fd);
    bool postResumeClient(socket_t fd, PlayerId playerId,
                          const std::string& roomCode);
    bool postInput(socket_t fd, float dirX, float dirY);

    bool pollOutbound(WorkerOutbound& out);
    WorkerStats stats() const;

private:
    struct Member {
        struct EntityView {
            float x = 0.0f;
            float y = 0.0f;
            float radius = 0.0f;
            int score = 0;

            bool differs(const Entity& entity) const {
                return std::fabs(x - entity.pos.x) > 0.01f ||
                       std::fabs(y - entity.pos.y) > 0.01f ||
                       std::fabs(radius - entity.radius) > 0.01f ||
                       score != entity.score;
            }
        };

        PlayerId playerId = kInvalidPlayerId;
        socket_t fd = BLOB_INVALID_SOCKET;
        std::string username;
        std::string roomCode;
        EntityId entityId = kInvalidEntityId;
        bool connected = true;
        std::chrono::steady_clock::time_point disconnectedAt{};
        std::vector<EntityId> visible;
        std::unordered_map<EntityId, EntityView> known;
    };

    void run();
    void handleCommand(WorkerCommand& command);
    void handleAddClient(const AddClientCommand& command);
    void handleRemoveClient(socket_t fd);
    void handleDisconnectClient(socket_t fd);
    void handleResumeClient(const ResumeClientCommand& command);
    void handleInput(const InputCommand& command);
    void tickRooms(float dt);

    void emit(WorkerOutbound outbound, bool critical);
    std::size_t sendPacket(socket_t fd, proto::S2C msgId,
                           const std::string& payload);
    void broadcastPacket(const Room* room, proto::S2C msgId,
                         const std::string& payload,
                         socket_t exceptFd = BLOB_INVALID_SOCKET);
    void persistMatch(Room* room, const std::vector<Standing>& standings);
    void refreshStats();
    void recordTickDuration(uint64_t microseconds);
    void sendAoiUpdate(Room* room, socket_t fd, Member& member,
                       const SpatialGrid& grid);
    void applyAiTakeover(Room* room, Member& member);
    void expireDisconnectedPlayers();

    Member* findMemberByEntity(const Room* room, EntityId entityId);

    uint32_t seed_;
    float worldSize_;
    int countdownSec_;
    int durationSec_;
    int shrinkStartSec_;
    int reconnectGraceMs_;
    DataStore* store_ = nullptr;
    Lobby lobby_;

    BoundedQueue<WorkerCommand> inbound_;
    BoundedQueue<WorkerOutbound> outbound_;
    std::unordered_map<PlayerId, Member> members_;
    std::unordered_map<socket_t, PlayerId> playerByFd_;

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

    static constexpr std::size_t kTickWindow = 300;
    std::array<std::atomic<uint32_t>, kTickWindow> tickSamplesUs_{};
    std::atomic<std::size_t> tickSampleCount_{0};
    std::atomic<std::size_t> tickSampleIndex_{0};
    std::atomic<uint64_t> snapshotPackets_{0};
};

}  // namespace blob
