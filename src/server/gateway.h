// =============================================================================
//  gateway.h —— Acceptor、会话路由与 Room Worker 协调器
//
//  Gateway 运行在主线程，是整个服务器的“总装车间 + 调度中心”：
//    1. 持有监听 socket，accept 新连接并轮询分配给某个 IoReactor；
//    2. 维护全局会话表 sessions_（fd -> ClientSession）；
//    3. 作为 IO 回调的落点，把业务包投递进 RoomWorker 的入站队列；
//    4. 以 30Hz 节奏从 RoomWorker 出站队列取消息，转回对应 IoReactor 发出；
//    5. 处理 HTTP 业务（登录、战绩、排行榜、静态文件、状态、指标）。
//
//  它绝不直接读写游戏世界——World 只属于 RoomWorker 线程。
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

/// @brief 启动配置（运行期可被命令行覆盖的一层；规则默认值引用 cfg::）
struct ServerOptions {
    // ---- 基础设施项：默认值直接写死 ----
    uint16_t    port = 8080;                  ///< 监听端口
    std::string webRoot = "web";              ///< 静态资源目录
    std::string dataDir = "data";             ///< 持久化数据目录
    uint32_t    seed = 20260919u;             ///< 世界随机种子（固定值便于复现）
    std::size_t maxConnections = 4096;        ///< 最大连接数（过载保护）

    // ---- 游戏规则项：默认值引用 cfg:: 单一数据源，命令行可临时覆盖 ----
    uint32_t    worldSize = static_cast<uint32_t>(cfg::kArenaWorldSize);
    int         arenaCountdownSec = cfg::kArenaCountdownSec;
    int         arenaDurationSec = cfg::kArenaDurationSec;
    int         arenaShrinkStartSec = cfg::kArenaShrinkStartSec;
    int         reconnectGraceMs = cfg::kReconnectGraceMs;

    std::size_t ioThreads = 2;  ///< IO Reactor 线程数（start 时 clamp 到 1~16）
};

/// @brief 网关/协调器（主线程）
class Gateway {
public:
    explicit Gateway(const ServerOptions& options);
    ~Gateway();

    // 持有线程/监听 socket 等独占资源，禁止拷贝
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    bool start();  ///< 初始化网络/存储/Worker/Reactor 并绑定监听端口
    void run();    ///< 进入主线程循环（接入、转发、30Hz 统计）
    void stop();   ///< 优雅停止全部线程并释放资源

private:
    using Clock = std::chrono::steady_clock;  ///< 单调时钟（不受系统时间回调影响）
    using Duration = std::chrono::microseconds;

    /// @brief 一条连接的会话状态（主线程维护，被多线程读取，自带锁）
    struct ClientSession {
        mutable std::mutex mutex;          ///< 保护本会话内字段
        std::size_t reactorId = 0;         ///< 永久绑定的 IO Reactor 编号
        bool joined = false;               ///< 是否已成功进入房间
        bool pendingJoin = false;          ///< 入房请求处理中（防重复提交）
        bool wantClose = false;            ///< 已标记关闭（后续发包直接拒绝）
        EntityId entityId = kInvalidEntityId; ///< 房间内对应的实体 ID
        PlayerId playerId = kInvalidPlayerId; ///< 登录后的玩家 ID
        std::string username;              ///< 玩家名
        std::string roomCode;              ///< 所在房间码
        uint32_t inputSeq = 0;             ///< 最近一次移动指令序号
        std::size_t queuedBytes = 0;       ///< 已投递但 IO 线程尚未发完的字节数
    };

    using SessionPtr = std::shared_ptr<ClientSession>; ///< 共享所有权（回调中用 weak 观察）

    bool setupListener();                                  ///< 创建/bind/listen 监听 socket
    std::string resolveWebPath(const std::string& urlPath, bool& found) const; ///< 静态文件路径解析

    void acceptConnections();                              ///< 接收积压的全部新连接
    void onClientClosed(socket_t fd);                      ///< IO 回调：连接关闭
    void onPacket(socket_t fd, uint16_t msgId, const std::string& payload); ///< IO 回调：业务包入口
    net::HttpResult onHttpRequest(socket_t fd,
                                  const net::HttpRequest& request); ///< IO 回调：HTTP 入口

    SessionPtr findSession(socket_t fd) const;             ///< 按 fd 查会话（加锁）
    void eraseSession(socket_t fd);                        ///< 删除会话（加锁）
    void requestClose(socket_t fd, const SessionPtr& session); ///< 请求 Reactor 关闭连接
    bool sendRawPacket(socket_t fd, const SessionPtr& session,
                       std::shared_ptr<std::string> packet); ///< 投递数据包（含慢客户端保护）
    void sendMessage(socket_t fd, const SessionPtr& session,
                     proto::S2C msgId, const std::string& payload); ///< 打包并投递 S2C 消息
    void broadcastPacketInRoom(const std::string& roomCode,
                               const std::string& packet,
                               socket_t exceptFd);         ///< 向房间内所有会话广播

    void drainWorkerOutbound();  ///< 抽干 Worker 出站队列并分发（绑定/单播/广播）
    void onTick();               ///< 主线程 30Hz 节拍：采样帧间隔、每 5s 统计
    void broadcastStats();       ///< 向全部已入房玩家广播 Stats
    void logStats();             ///< 打印控制台统计日志

    // ---- 配置与网络组件 ----
    ServerOptions options_;                       ///< 启动配置
    std::unique_ptr<net::Poller> poller_;        ///< 主线程 poller（只盯 listenFd_）
    std::vector<std::unique_ptr<net::IoReactor>> reactors_; ///< N 个 IO Reactor
    std::size_t nextReactor_ = 0;                ///< 轮询分配计数（round-robin）
    socket_t listenFd_ = BLOB_INVALID_SOCKET;    ///< 监听 socket
    bool running_ = false;                       ///< 主循环运行标志

    // ---- 会话表（fd -> 会话），多线程访问，用 sessionsMutex_ 保护 ----
    mutable std::mutex sessionsMutex_;
    std::unordered_map<socket_t, SessionPtr> sessions_;
    std::vector<net::PollResult> pollResults_;   ///< 主线程 wait 结果复用缓冲

    // ---- 存储与游戏逻辑 ----
    std::unique_ptr<DataStore> store_;           ///< 存储层（文件实现）
    std::unique_ptr<RoomWorker> worker_;         ///< 唯一的游戏逻辑线程

    // ---- 节拍与统计 ----
    Clock::time_point nextTick_;                 ///< 下一个 30Hz 到期时刻
    Clock::time_point nextStats_;                ///< 下一次 5 秒统计时刻
    uint32_t totalConnections_ = 0;              ///< 累计接入连接数
    uint32_t tickSeq_ = 0;                       ///< 主线程 tick 计数
    uint64_t slowClientDisconnects_ = 0;         ///< 因发送积压超限被踢的连接数

    static constexpr std::size_t kMaxPendingOutBytes = 1024 * 1024; ///< 单连接积压上限 1MB
    static constexpr std::size_t kTickSampleWindow = 300;           ///< 帧耗时采样窗口
    std::array<float, kTickSampleWindow> tickSamples_{};            ///< 最近 300 次帧间隔(ms)
    std::size_t tickSampleCount_ = 0;
    std::size_t tickSampleIndex_ = 0;
    Clock::time_point lastTickAt_{};
    bool tickTimingStarted_ = false;
};

}  // namespace blob
