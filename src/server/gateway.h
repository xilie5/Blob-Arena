// =============================================================================
//  gateway.h —— Acceptor、会话路由与 Room Worker 协调器
// =============================================================================
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/game_config.h"
#include "net/io_reactor.h"
#include "net/poller.h"
#include "protocol/protocol.h"
#include "server/room_worker.h"
#include "storage/data_store.h"

namespace blob {

struct ServerOptions {
    uint16_t    port = 8080;
    std::string webRoot = "web";
    std::string dataDir = "data";
    uint32_t    seed = 20260919u;
    std::size_t maxConnections = 4096;
    uint32_t    worldSize = static_cast<uint32_t>(cfg::kArenaWorldSize);
    int         arenaCountdownSec = cfg::kArenaCountdownSec;
    int         arenaDurationSec = cfg::kArenaDurationSec;
    int         arenaShrinkStartSec = cfg::kArenaShrinkStartSec;
    int         reconnectGraceMs = cfg::kReconnectGraceMs;
    std::size_t ioThreads = 2;
};

class Gateway {
public:
    explicit Gateway(const ServerOptions& options);
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    bool start();
    void run();
    void stop();

private:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::microseconds;

    struct ClientSession {
        mutable std::mutex mutex;
        std::size_t reactorId = 0;
        bool joined = false;
        bool pendingJoin = false;
        bool wantClose = false;
        EntityId entityId = kInvalidEntityId;
        PlayerId playerId = kInvalidPlayerId;
        std::string username;
        std::string roomCode;
        uint32_t inputSeq = 0;
        std::size_t queuedBytes = 0;
    };

    using SessionPtr = std::shared_ptr<ClientSession>;

    bool setupListener();
    std::string resolveWebPath(const std::string& urlPath, bool& found) const;

    void acceptConnections();
    void onClientClosed(socket_t fd);
    void onPacket(socket_t fd, uint16_t msgId, const std::string& payload);
    net::HttpResult onHttpRequest(socket_t fd,
                                  const net::HttpRequest& request);

    SessionPtr findSession(socket_t fd) const;
    void eraseSession(socket_t fd);
    void requestClose(socket_t fd, const SessionPtr& session);
    bool sendRawPacket(socket_t fd, const SessionPtr& session,
                       std::shared_ptr<std::string> packet);
    void sendMessage(socket_t fd, const SessionPtr& session,
                     proto::S2C msgId, const std::string& payload);
    void broadcastPacketInRoom(const std::string& roomCode,
                               const std::string& packet,
                               socket_t exceptFd);

    void drainWorkerOutbound();
    void onTick();
    void broadcastStats();
    void logStats();

    ServerOptions options_;
    std::unique_ptr<net::Poller> poller_;
    std::vector<std::unique_ptr<net::IoReactor>> reactors_;
    std::size_t nextReactor_ = 0;
    socket_t listenFd_ = BLOB_INVALID_SOCKET;
    bool running_ = false;

    mutable std::mutex sessionsMutex_;
    std::unordered_map<socket_t, SessionPtr> sessions_;
    std::vector<net::PollResult> pollResults_;

    std::unique_ptr<DataStore> store_;
    std::unique_ptr<RoomWorker> worker_;

    Clock::time_point nextTick_;
    Clock::time_point nextStats_;
    uint32_t totalConnections_ = 0;
    uint32_t tickSeq_ = 0;
    uint64_t slowClientDisconnects_ = 0;

    static constexpr std::size_t kMaxPendingOutBytes = 1024 * 1024;
    static constexpr std::size_t kTickSampleWindow = 300;
    std::array<float, kTickSampleWindow> tickSamples_{};
    std::size_t tickSampleCount_ = 0;
    std::size_t tickSampleIndex_ = 0;
    Clock::time_point lastTickAt_{};
    bool tickTimingStarted_ = false;
};

}  // namespace blob
