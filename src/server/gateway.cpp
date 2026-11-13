#include "server/gateway.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "net/platform.h"
#include "storage/file_data_store.h"
#include "version.h"

namespace blob {

using net::PollEvent;

namespace {

/// @brief 按文件扩展名返回 HTTP Content-Type
/// @details 静态资源服务使用；未知扩展名统一按二进制流下载处理
std::string mimeTypeFor(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const std::string ext = path.substr(dot + 1);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff2") return "font/woff2";
    if (ext == "woff") return "font/woff";
    if (ext == "ttf") return "font/ttf";
    return "application/octet-stream";
}

/// @brief 以二进制方式读取整个文件到字符串
/// @return true=读取成功
bool readFileBinary(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream stream;
    stream << in.rdbuf();
    out = stream.str();
    return true;
}

/// @brief 当前单调时钟毫秒数（Pong 时间戳使用，避免系统时间被回调影响）
uint32_t nowMillis() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// @brief JSON 字符串转义（手工拼 JSON 响应时用）
std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

/// @brief 极简 JSON 字符串字段提取（项目无 JSON 库，仅需解析 {"username":"x"}）
/// @param body JSON 文本  @param key 字段名  @param[out] out 提取出的值
/// @return true=找到并提取成功
/// @note 支持 \\n \\r \\t 等基本转义；仅用于受控的登录请求，不追求完整 JSON 语法
bool jsonStringField(const std::string& body, const std::string& key,
                     std::string& out) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = body.find(needle);
    if (keyPos == std::string::npos) return false;
    const std::size_t colon = body.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return false;
    const std::size_t quote = body.find('"', colon + 1);
    if (quote == std::string::npos) return false;

    std::string value;
    bool escaped = false;
    for (std::size_t i = quote + 1; i < body.size(); ++i) {
        const char c = body[i];
        if (escaped) {
            if (c == 'n') value.push_back('\n');
            else if (c == 'r') value.push_back('\r');
            else if (c == 't') value.push_back('\t');
            else value.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            out = std::move(value);
            return true;   // 遇到结束引号，提取完成
        }
        value.push_back(c);
    }
    return false;
}

/// @brief 从 Authorization 头提取 Bearer Token
/// @details 形如 "Authorization: Bearer abc123"，去掉前缀返回 "abc123"
std::string bearerToken(const net::HttpRequest& request) {
    const std::string authorization = request.header("authorization");
    const std::string prefix = "Bearer ";
    if (authorization.size() <= prefix.size() ||
        authorization.compare(0, prefix.size(), prefix) != 0) {
        return std::string();
    }
    return authorization.substr(prefix.size());
}

/// @brief 从 query 串解析 limit 参数（战绩/排行榜分页用）
/// @param fallback 缺省值  @param maximum 上限（夹取保护，防止一次请求拉全表）
std::size_t queryLimit(const std::string& query, std::size_t fallback,
                       std::size_t maximum) {
    const std::string key = "limit=";
    const std::size_t pos = query.find(key);
    if (pos == std::string::npos) return fallback;
    const std::size_t start = pos + key.size();
    const std::size_t end = query.find('&', start);
    try {
        const std::size_t value = static_cast<std::size_t>(
            std::stoull(query.substr(start, end - start)));
        return std::clamp<std::size_t>(value, 1, maximum);
    } catch (...) {
        return fallback;  // 非数字：静默回退默认值
    }
}

/// @brief 从一个已打包完成的业务包中读出消息号（第 4~5 字节，小端序）
/// @details Gateway 不拆包转发，但需要识别 Error 包以解除会话 pendingJoin
uint16_t packetMessageId(const std::string& packet) {
    if (packet.size() < proto::kHeaderSize) return 0;
    const auto* bytes =
        reinterpret_cast<const uint8_t*>(packet.data());
    return static_cast<uint16_t>(bytes[4] |
                                 (static_cast<uint16_t>(bytes[5]) << 8));
}

/// @brief 快速构造 JSON HttpResult（末尾补换行，curl 输出更友好）
net::HttpResult jsonResult(int status, const char* text,
                           const std::string& body) {
    net::HttpResult result;
    result.statusCode = status;
    result.statusText = text;
    result.body = body + "\n";
    return result;
}

}  // namespace

Gateway::Gateway(const ServerOptions& options) : options_(options) {}

Gateway::~Gateway() {
    stop();  // RAII：确保程序结束前所有线程已停止、资源已释放
}

bool Gateway::start() {
    // 1) 网络库初始化（Windows: WSAStartup + 高精度定时器）
    if (!net::initialize()) {
        std::printf("[gateway] WSAStartup failed\n");
        return false;
    }
    // 2) 主线程 poller 与监听 socket
    poller_.reset(net::Poller::create());
    if (!poller_ || !setupListener()) {
        return false;
    }

    // 3) 存储层：创建并加载历史数据（失败则拒绝启动，不带着脏数据运行）
    store_.reset(new FileDataStore(options_.dataDir));
    std::string storeError;
    if (!store_->load(storeError)) {
        std::printf("[gateway] data store load failed: %s\n",
                    storeError.c_str());
        return false;
    }

    // 4) 游戏逻辑线程（注入配置与存储指针），先于 IO 线程启动
    worker_.reset(new RoomWorker(
        options_.seed, static_cast<float>(options_.worldSize),
        options_.arenaCountdownSec, options_.arenaDurationSec,
        options_.arenaShrinkStartSec, options_.reconnectGraceMs,
        store_.get()));
    worker_->start();

    // 5) IO Reactor 线程池：数量夹取到 [1,16]
    const std::size_t reactorCount =
        std::clamp<std::size_t>(options_.ioThreads, 1, 16);
    reactors_.reserve(reactorCount);
    for (std::size_t i = 0; i < reactorCount; ++i) {
        auto reactor = std::make_unique<net::IoReactor>(
            i,
            // 三个回调把 IO 线程上的事件接回主线程处理
            [this](socket_t fd, uint16_t msgId, const std::string& payload) {
                onPacket(fd, msgId, payload);
            },
            [this](socket_t fd, const net::HttpRequest& request) {
                return onHttpRequest(fd, request);
            },
            [this](socket_t fd) { onClientClosed(fd); },
            16384, kMaxPendingOutBytes);
        reactor->start();
        reactors_.push_back(std::move(reactor));
    }

    // 6) 节拍初始化
    const Clock::time_point now = Clock::now();
    nextTick_ = now;
    nextStats_ = now + std::chrono::seconds(5);  // 5 秒后第一次统计输出
    running_ = true;

    std::printf("[gateway] acceptor=1 ioReactors=%zu roomWorkers=1 tick=%dHz world=%u\n",
                reactors_.size(), cfg::kTickRate, options_.worldSize);
    std::printf("[gateway] listening on http://127.0.0.1:%u (web root: %s, data: %s)\n",
                static_cast<unsigned>(options_.port), options_.webRoot.c_str(),
                options_.dataDir.c_str());
    return true;
}

bool Gateway::setupListener() {
    // 创建 TCP socket
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenFd_ == BLOB_INVALID_SOCKET) return false;

    // SO_REUSEADDR：服务器重启时可立即重新绑定同一端口（否则会停留 TIME_WAIT）
    int yes = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes),
               static_cast<int>(sizeof(yes)));

    // 绑定 0.0.0.0:port（监听本机所有网卡地址）
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(options_.port);
    // bind -> listen(backlog=SOMAXCONN) -> 非阻塞 -> 注册读事件，任一步失败即回滚
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(listenFd_, SOMAXCONN) != 0 ||
        !net::setNonBlocking(listenFd_) ||
        !poller_->add(listenFd_, PollEvent::Read)) {
        net::closeSocket(listenFd_);
        listenFd_ = BLOB_INVALID_SOCKET;
        return false;
    }
    return true;
}

void Gateway::stop() {
    if (!running_) return;
    running_ = false;
    // 停止顺序：先游戏线程（不再产生出站消息），再 IO 线程，最后释放其他资源
    if (worker_) worker_->stop();
    for (auto& reactor : reactors_) reactor->stop();
    reactors_.clear();
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.clear();
    }
    if (listenFd_ != BLOB_INVALID_SOCKET) {
        net::closeSocket(listenFd_);
        listenFd_ = BLOB_INVALID_SOCKET;
    }
    store_.reset();
    net::shutdown();
    std::printf("[gateway] stopped\n");
}

void Gateway::run() {
    const Duration tickInterval(1000000 / cfg::kTickRate);  // 33.3ms
    while (running_) {
        // 1) 先抽一次 Worker 出站，尽快把游戏消息送出去
        drainWorkerOutbound();

        // 2) poll 等待监听 socket 就绪，超时时间 clamp 到 [0,20]ms
        //    （既要及时 accept，又不能让主线程睡过 30Hz 统计节拍）
        Clock::time_point now = Clock::now();
        long long waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               nextTick_ - now)
                               .count();
        waitMs = std::clamp(waitMs, 0LL, 20LL);

        pollResults_.clear();
        const int ready = poller_->wait(pollResults_,
                                        static_cast<int>(waitMs));
        if (ready < 0) break;
        for (const net::PollResult& result : pollResults_) {
            // 主线程的 poller 只盯监听 socket，就绪即有新连接
            if (result.fd == listenFd_) acceptConnections();
        }

        // 3) 接入后再抽一次出站（accept 循环中 Worker 可能又产出了消息）
        drainWorkerOutbound();
        // 4) 30Hz 节拍（带最多 4 帧的追赶保护，逻辑同 RoomWorker）
        now = Clock::now();
        int guard = 0;
        while (running_ && now >= nextTick_ && guard < 4) {
            onTick();
            nextTick_ += tickInterval;
            ++guard;
        }
        if (nextTick_ <= now) nextTick_ = now + tickInterval;
    }
}

void Gateway::acceptConnections() {
    // 监听 socket 是非阻塞的：循环 accept 直到拿不到新连接为止
    while (true) {
        sockaddr_in address{};
        socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
        const socket_t fd =
            ::accept(listenFd_, reinterpret_cast<sockaddr*>(&address),
                     &addressLength);
        if (fd == BLOB_INVALID_SOCKET) break;  // 没有更多积压连接

        // 过载保护：会话数达到上限直接拒绝（关闭新 fd）
        bool overLimit = false;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            overLimit = sessions_.size() >= options_.maxConnections;
        }
        if (overLimit) {
            net::closeSocket(fd);
            continue;
        }

        // 新连接统一设为非阻塞 + 关闭 Nagle
        net::setNonBlocking(fd);
        net::setNoDelay(fd);
        // 建立会话并 round-robin 分配 IO Reactor（连接与线程永久绑定）
        auto session = std::make_shared<ClientSession>();
        const std::size_t reactorIndex = nextReactor_ % reactors_.size();
        nextReactor_ = (nextReactor_ + 1) % reactors_.size();
        session->reactorId = reactorIndex;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            sessions_[fd] = session;
        }
        // 投递 AddSocket；失败则回滚（删会话、关 fd）
        if (!reactors_[reactorIndex]->assign(fd)) {
            eraseSession(fd);
            net::closeSocket(fd);
            continue;
        }
        ++totalConnections_;
    }
}

void Gateway::onClientClosed(socket_t fd) {
    // 由 IO Reactor 线程回调；仅做会话清理 + 通知 Worker 断线
    const bool existed = findSession(fd) != nullptr;
    eraseSession(fd);
    if (existed && worker_) worker_->postDisconnectClient(fd);
}

Gateway::SessionPtr Gateway::findSession(socket_t fd) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(fd);
    return it == sessions_.end() ? nullptr : it->second;
}

void Gateway::eraseSession(socket_t fd) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_.erase(fd);
}

void Gateway::requestClose(socket_t fd, const SessionPtr& session) {
    if (!session) return;
    // 标记关闭并查出归属的 Reactor，然后投递 CloseSocket 命令
    std::size_t reactorId = 0;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->wantClose = true;
        reactorId = session->reactorId;
    }
    if (reactorId < reactors_.size()) reactors_[reactorId]->postClose(fd);
}

bool Gateway::sendRawPacket(socket_t fd, const SessionPtr& session,
                            std::shared_ptr<std::string> packet) {
    if (!session || !packet) return false;
#ifdef _WIN32
    const socket_t invalidSocket = INVALID_SOCKET;
#else
    const socket_t invalidSocket = -1;
#endif
    if (fd == invalidSocket) return false;

    // 慢客户端保护 + 记账：积压超 1MB 直接踢掉，防止发送缓冲无限膨胀
    std::size_t reactorId = 0;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->wantClose) return false;
        if (session->queuedBytes + packet->size() > kMaxPendingOutBytes) {
            session->wantClose = true;
            reactorId = session->reactorId;
            ++slowClientDisconnects_;
            if (reactorId < reactors_.size()) reactors_[reactorId]->postClose(fd);
            return false;
        }
        session->queuedBytes += packet->size();
        reactorId = session->reactorId;
    }

    const std::size_t packetSize = packet->size();
    // weak_ptr：完成回调在 IO 线程执行，若会话已销毁则不触碰悬空对象
    std::weak_ptr<ClientSession> weak = session;
    const bool posted = reactorId < reactors_.size() &&
                        reactors_[reactorId]->postPacket(
                            fd, std::move(packet),
                            [weak, packetSize] {
                                auto locked = weak.lock();
                                if (!locked) return;
                                std::lock_guard<std::mutex> lock(locked->mutex);
                                if (locked->queuedBytes >= packetSize) {
                                    locked->queuedBytes -= packetSize;
                                } else {
                                    locked->queuedBytes = 0;
                                }
                            });
    // 投递失败（命令队列满）：回滚记账并关闭连接
    if (!posted) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->queuedBytes >= packetSize) {
            session->queuedBytes -= packetSize;
        } else {
            session->queuedBytes = 0;
        }
        session->wantClose = true;
        if (reactorId < reactors_.size()) reactors_[reactorId]->postClose(fd);
        return false;
    }
    return true;
}

void Gateway::sendMessage(socket_t fd, const SessionPtr& session,
                          proto::S2C msgId, const std::string& payload) {
    // 打包为 shared_ptr 字符串：跨线程投递时共享同一份字节，避免大包拷贝
    auto packet = std::make_shared<std::string>(
        proto::packPacket(static_cast<uint16_t>(msgId), 0, payload));
    sendRawPacket(fd, session, std::move(packet));
}

void Gateway::broadcastPacketInRoom(const std::string& roomCode,
                                    const std::string& packet,
                                    socket_t exceptFd) {
    // 持锁阶段只做“筛选”，确定收件人快照；实际投递放到锁外，避免持锁做 IO
    std::vector<std::pair<socket_t, SessionPtr>> recipients;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (const auto& entry : sessions_) {
            const SessionPtr& session = entry.second;
            std::lock_guard<std::mutex> sessionLock(session->mutex);
            // 跳过：未入房、待关闭、别的房间、被排除者
            if (!session->joined || session->wantClose ||
                session->roomCode != roomCode || entry.first == exceptFd) {
                continue;
            }
            recipients.emplace_back(entry.first, session);
        }
    }
    // 广播包只读：所有收件人共享同一份字节，避免每个玩家复制一次大包。
    // shared_ptr 的引用计数是原子的，跨线程共享只读 string 是安全的。
    const auto sharedPacket = std::make_shared<std::string>(packet);
    for (auto& recipient : recipients) {
        sendRawPacket(recipient.first, recipient.second, sharedPacket);
    }
}

void Gateway::drainWorkerOutbound() {
    if (!worker_) return;
    WorkerOutbound outbound;
    // 非阻塞循环抽干 Worker 出站队列
    while (worker_->pollOutbound(outbound)) {
        SessionPtr session = findSession(outbound.fd);
        if (outbound.type == OutboundType::BindClient) {
            // 绑定消息不下发网络，只更新主线程会话状态
            if (!session) continue;
            std::lock_guard<std::mutex> lock(session->mutex);
            session->joined = true;
            session->pendingJoin = false;
            session->roomCode = outbound.roomCode;
            session->entityId = outbound.entityId;
            continue;
        }

        if (outbound.type == OutboundType::SendToFd) {
            if (!session) continue;
            // 若收到 Error 包，说明入房失败，解除 pendingJoin 允许重试
            if (packetMessageId(outbound.packet) ==
                static_cast<uint16_t>(proto::S2C::Error)) {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->pendingJoin = false;
            }
            sendRawPacket(outbound.fd, session,
                          std::make_shared<std::string>(
                              std::move(outbound.packet)));
            continue;
        }

        // 其余按房间广播
        broadcastPacketInRoom(outbound.roomCode, outbound.packet,
                              outbound.exceptFd);
    }
}

void Gateway::onTick() {
    // 记录主线程（协调器）帧间隔，用于统计自身节拍抖动
    const Clock::time_point now = Clock::now();
    if (tickTimingStarted_) {
        const float ms =
            std::chrono::duration<float, std::milli>(now - lastTickAt_)
                .count();
        tickSamples_[tickSampleIndex_] = ms;
        tickSampleIndex_ = (tickSampleIndex_ + 1) % kTickSampleWindow;
        tickSampleCount_ =
            std::min(tickSampleCount_ + 1, kTickSampleWindow);
    }
    tickTimingStarted_ = true;
    lastTickAt_ = now;
    ++tickSeq_;

    // 每 5 秒做一次统计广播 + 控制台日志
    if (now >= nextStats_) {
        broadcastStats();
        logStats();
        nextStats_ += std::chrono::seconds(5);
    }
}

void Gateway::broadcastStats() {
    if (!worker_) return;
    const WorkerStats stats = worker_->stats();
    // Stats 载荷：在线玩家数(u16) 实体数(u16) tick 率(u16)
    proto::Writer writer;
    writer.u16(static_cast<uint16_t>(
        std::min<std::size_t>(stats.players, 0xFFFFu)));
    writer.u16(static_cast<uint16_t>(
        std::min<std::size_t>(stats.entities, 0xFFFFu)));
    writer.u16(static_cast<uint16_t>(cfg::kTickRate));
    const std::string payload = writer.data();

    // 筛选已入房玩家（持锁只筛选），锁外逐个发送
    std::vector<std::pair<socket_t, SessionPtr>> recipients;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (const auto& entry : sessions_) {
            std::lock_guard<std::mutex> sessionLock(entry.second->mutex);
            if (entry.second->joined) {
                recipients.push_back(entry);
            }
        }
    }
    for (auto& recipient : recipients) {
        sendMessage(recipient.first, recipient.second, proto::S2C::Stats,
                    payload);
    }
}

void Gateway::logStats() {
    const WorkerStats workerStats = worker_ ? worker_->stats() : WorkerStats{};
    const StoreStats storeStats = store_ ? store_->stats() : StoreStats{};
    std::size_t clientCount = 0;
    std::size_t reactorDepth = 0;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        clientCount = sessions_.size();
    }
    for (const auto& reactor : reactors_) {
        reactorDepth += reactor->commandDepth();
    }
    // 主统计行：连接、线程、队列、房间/玩家/实体、丢弃计数、存储计数等
    std::printf(
        "[gateway] ioClients=%zu reactors=%zu reactorCmds=%zu rooms=%zu "
        "players=%zu disconnected=%zu entities=%zu frames=%u slow=%llu inDrop=%llu "
        "outDrop=%llu workerQ=%zu/%zu storedPlayers=%zu storedMatches=%zu "
        "aoiBytes=%llu fullBytes=%llu aoiEntities=%llu fullEntities=%llu\n",
        clientCount, reactors_.size(), reactorDepth, workerStats.rooms,
        workerStats.players, workerStats.disconnectedPlayers,
        workerStats.entities,
        static_cast<unsigned>(tickSeq_),
        static_cast<unsigned long long>(slowClientDisconnects_),
        static_cast<unsigned long long>(workerStats.inboundDropped),
        static_cast<unsigned long long>(workerStats.outboundDropped),
        workerStats.inboundDepth, workerStats.outboundDepth,
        storeStats.players, storeStats.matches,
        static_cast<unsigned long long>(workerStats.aoiBytes),
        static_cast<unsigned long long>(workerStats.fullSnapshotBytes),
        static_cast<unsigned long long>(workerStats.aoiEntitySamples),
        static_cast<unsigned long long>(workerStats.fullEntitySamples));
    std::fflush(stdout);

    // AOI 带宽节省比例与平均实体数（量化空间裁剪的收益）
    if (workerStats.fullSnapshotBytes > 0) {
        const double ratio =
            1.0 - static_cast<double>(workerStats.aoiBytes) /
                      static_cast<double>(workerStats.fullSnapshotBytes);
        std::printf("[worker] bandwidthReduction=%.2f%% aoiAvgEntities=%.1f "
                    "fullAvgEntities=%.1f\n",
                    ratio * 100.0,
                    workerStats.snapshotSamples == 0
                        ? 0.0
                        : static_cast<double>(workerStats.aoiEntitySamples) /
                              static_cast<double>(workerStats.snapshotSamples),
                    workerStats.snapshotSamples == 0
                        ? 0.0
                        : static_cast<double>(workerStats.fullEntitySamples) /
                              static_cast<double>(workerStats.snapshotSamples));
        std::fflush(stdout);
    }

    // 协调器（主线程）帧间隔统计
    if (tickSampleCount_ == 0) return;
    std::vector<float> samples(tickSamples_.begin(),
                               tickSamples_.begin() + tickSampleCount_);
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (float sample : samples) sum += sample;
    const float average = static_cast<float>(sum / samples.size());
    const float p95 = samples[(samples.size() * 95) / 100];
    const float maximum = samples.back();
    const std::size_t over50 = static_cast<std::size_t>(
        std::count_if(samples.begin(), samples.end(),
                      [](float sample) { return sample > 50.0f; }));
    std::printf("[gateway] coordinatorTickMs: avg=%.1f p95=%.1f max=%.1f over50=%zu/%zu\n",
                average, p95, maximum, over50, samples.size());
    std::fflush(stdout);

    // Worker 帧耗时（已在 Worker::stats 中排序计算好）
    std::printf(
        "[worker] workerTickMs: avg=%.2f p95=%.2f max=%.2f over50=%llu/%zu\n",
        workerStats.tickAvgMs, workerStats.tickP95Ms, workerStats.tickMaxMs,
        static_cast<unsigned long long>(workerStats.tickOver50),
        workerStats.tickSamples);
    std::fflush(stdout);
}

// ============================== 业务包入口（在 IO 线程被回调） ==============================

void Gateway::onPacket(socket_t fd, uint16_t msgId,
                       const std::string& payload) {
    SessionPtr session = findSession(fd);
    if (!session) return;

    switch (static_cast<proto::C2S>(msgId)) {
        // ---- 鉴权：校验 token，绑定 playerId/username ----
        case proto::C2S::Auth: {
            proto::Reader reader(payload.data(), payload.size());
            const auto profile = store_->authenticate(reader.str8());
            if (!profile.has_value()) {
                // token 无效：错误码 4 并关闭连接
                proto::Writer writer;
                writer.u8(4);
                writer.str8("invalid token");
                sendMessage(fd, session, proto::S2C::Error, writer.data());
                requestClose(fd, session);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                // 已鉴权/已入房的重复 Auth 忽略
                if (session->joined ||
                    session->playerId != kInvalidPlayerId) {
                    return;
                }
                session->playerId = profile->id;
                session->username = profile->username;
            }
            sendMessage(fd, session, proto::S2C::AuthOk, std::string());
            return;
        }

        // ---- 进房（快速匹配/创建私房/凭码加入共用主体逻辑） ----
        case proto::C2S::Join:
        case proto::C2S::CreateRoom:
        case proto::C2S::JoinRoom: {
            AddClientCommand command;
            command.fd = fd;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                // 必须已鉴权、未入房、没有正在处理的入房请求
                if (session->joined || session->pendingJoin ||
                    session->playerId == kInvalidPlayerId) {
                    return;
                }
                session->pendingJoin = true;  // 防重复点击提交
                command.playerId = session->playerId;
                command.username = session->username;
            }
            // 区分三种进房方式；JoinRoom 还要读出房间码
            if (static_cast<proto::C2S>(msgId) == proto::C2S::CreateRoom) {
                command.mode = JoinMode::Create;
            } else if (static_cast<proto::C2S>(msgId) ==
                       proto::C2S::JoinRoom) {
                command.mode = JoinMode::Code;
                proto::Reader reader(payload.data(), payload.size());
                command.roomCode = reader.str8();
            } else {
                command.mode = JoinMode::Quick;
            }
            // 投递失败（Worker 队列满）：解除 pending 并回错误码 5
            if (!worker_->postAddClient(command)) {
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->pendingJoin = false;
                }
                proto::Writer writer;
                writer.u8(5);
                writer.str8("server busy");
                sendMessage(fd, session, proto::S2C::Error, writer.data());
            }
            return;
        }

        // ---- 断线重连 ----
        case proto::C2S::Resume: {
            PlayerId playerId = kInvalidPlayerId;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (session->joined || session->pendingJoin ||
                    session->playerId == kInvalidPlayerId) {
                    return;
                }
                session->pendingJoin = true;
                playerId = session->playerId;
            }
            proto::Reader reader(payload.data(), payload.size());
            const std::string roomCode = reader.str8();
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->roomCode = roomCode;
            }
            if (!worker_->postResumeClient(fd, playerId, roomCode)) {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->pendingJoin = false;
            }
            return;
        }

        // ---- 高频移动输入 ----
        case proto::C2S::Input: {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (!session->joined) return;  // 未入房的输入直接丢弃
            }
            proto::Reader reader(payload.data(), payload.size());
            const uint32_t seq = reader.u32();   // 客户端序号（记录，可用于乱序/丢包分析）
            const int8_t dx = reader.i8();       // -127~127 的方向分量
            const int8_t dy = reader.i8();
            if (reader.ok()) {
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->inputSeq = seq;
                }
                // 归一化到约 -1~1，用 int8 传输节省带宽
                worker_->postInput(fd, static_cast<float>(dx) / 127.0f,
                                   static_cast<float>(dy) / 127.0f);
            }
            return;
        }

        // ---- 心跳：原样回显客户端时间并附上服务端时间 ----
        case proto::C2S::Ping: {
            proto::Reader reader(payload.data(), payload.size());
            const uint32_t clientTime = reader.u32();
            proto::Writer writer;
            writer.u32(clientTime);
            writer.u32(nowMillis());
            sendMessage(fd, session, proto::S2C::Pong, writer.data());
            return;
        }

        // ---- 主动离开：通知 Worker 移除并关闭连接 ----
        case proto::C2S::Leave:
            if (worker_) worker_->postRemoveClient(fd);
            requestClose(fd, session);
            return;
    }
}

// ============================== HTTP 入口（在 IO 线程被回调） ==============================

net::HttpResult Gateway::onHttpRequest(socket_t fd,
                                       const net::HttpRequest& request) {
    (void)fd;
    // 根路径映射到首页
    std::string path = request.path;
    if (path.empty() || path == "/") path = "/index.html";

    // ---- 存活探针（Docker HEALTHCHECK / 负载均衡用，无需鉴权） ----
    if (path == "/health") {
        return jsonResult(200, "OK", "{\"status\":\"ok\"}");
    }

    // ---- 用户名登录（开发环境免密码，新用户名自动注册） ----
    if (path == "/api/login") {
        if (request.method != "POST") {
            return jsonResult(405, "Method Not Allowed",
                              "{\"error\":\"method_not_allowed\"}");
        }
        std::string username;
        if (!jsonStringField(request.body, "username", username)) {
            return jsonResult(400, "Bad Request",
                              "{\"error\":\"username_required\"}");
        }
        const auto login = store_->login(username);
        if (!login.has_value()) {
            return jsonResult(400, "Bad Request",
                              "{\"error\":\"invalid_username\"}");
        }
        // 手工拼 JSON：playerId / username / token
        return jsonResult(
            200, "OK",
            "{\"playerId\":" + std::to_string(login->profile.id) +
                ",\"username\":\"" +
                jsonEscape(login->profile.username) + "\",\"token\":\"" +
                jsonEscape(login->token) + "\"}");
    }

    // ---- 查询本人资料（Bearer Token 鉴权） ----
    if (path == "/api/profile") {
        const auto profile = store_->authenticate(bearerToken(request));
        if (!profile.has_value()) {
            return jsonResult(401, "Unauthorized",
                              "{\"error\":\"invalid_token\"}");
        }
        return jsonResult(
            200, "OK",
            "{\"playerId\":" + std::to_string(profile->id) +
                ",\"username\":\"" + jsonEscape(profile->username) +
                "\",\"createdAtMs\":" +
                std::to_string(profile->createdAtMs) + "}");
    }

    // ---- 查询个人战绩（鉴权 + limit 分页） ----
    if (path == "/api/matches") {
        const auto profile = store_->authenticate(bearerToken(request));
        if (!profile.has_value()) {
            return jsonResult(401, "Unauthorized",
                              "{\"error\":\"invalid_token\"}");
        }
        const auto matches =
            store_->matchesFor(profile->id, queryLimit(request.query, 20, 100));
        // 手工拼战绩数组 JSON
        std::string body = "{\"matches\":[";
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i > 0) body += ',';
            const MatchHistoryEntry& match = matches[i];
            body +=
                "{\"matchId\":\"" + jsonEscape(match.matchId) +
                "\",\"roomCode\":\"" + jsonEscape(match.roomCode) +
                "\",\"endedAtMs\":" + std::to_string(match.endedAtMs) +
                ",\"playerCount\":" + std::to_string(match.playerCount) +
                ",\"rank\":" + std::to_string(match.rank) +
                ",\"score\":" + std::to_string(match.score) +
                ",\"kills\":" + std::to_string(match.kills) +
                ",\"deaths\":" + std::to_string(match.deaths) + "}";
        }
        body += "]}";
        return jsonResult(200, "OK", body);
    }

    // ---- 排行榜（公开接口，无需登录） ----
    if (path == "/api/leaderboard") {
        const auto entries =
            store_->leaderboard(queryLimit(request.query, 20, 100));
        std::string body = "{\"leaderboard\":[";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) body += ',';
            const LeaderboardEntry& entry = entries[i];
            body +=
                "{\"playerId\":" + std::to_string(entry.playerId) +
                ",\"username\":\"" + jsonEscape(entry.username) +
                "\",\"totalScore\":" + std::to_string(entry.totalScore) +
                ",\"games\":" + std::to_string(entry.games) +
                ",\"wins\":" + std::to_string(entry.wins) +
                ",\"bestScore\":" + std::to_string(entry.bestScore) + "}";
        }
        body += "]}";
        return jsonResult(200, "OK", body);
    }

    // ---- 运行状态 JSON（运维排查用） ----
    if (path == "/api/status") {
        const WorkerStats workerStats = worker_->stats();
        const StoreStats storeStats = store_->stats();
        std::size_t clientCount = 0;
        std::size_t reactorDepth = 0;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            clientCount = sessions_.size();
        }
        for (const auto& reactor : reactors_) {
            reactorDepth += reactor->commandDepth();
        }
        return jsonResult(
            200, "OK",
            "{\"rooms\":" + std::to_string(workerStats.rooms) +
                ",\"version\":\"" + kProjectVersion + "\"" +
                ",\"players\":" + std::to_string(workerStats.players) +
                ",\"disconnectedPlayers\":" +
                std::to_string(workerStats.disconnectedPlayers) +
                ",\"entities\":" + std::to_string(workerStats.entities) +
                ",\"tickRate\":" + std::to_string(cfg::kTickRate) +
                ",\"ioReactors\":" + std::to_string(reactors_.size()) +
                ",\"ioClients\":" + std::to_string(clientCount) +
                ",\"reactorCommands\":" + std::to_string(reactorDepth) +
                ",\"inboundDropped\":" +
                std::to_string(workerStats.inboundDropped) +
                ",\"outboundDropped\":" +
                std::to_string(workerStats.outboundDropped) +
                ",\"slowClientDisconnects\":" +
                std::to_string(slowClientDisconnects_) +
                ",\"aiTakeoverTicks\":" +
                std::to_string(workerStats.aiTakeoverTicks) +
                ",\"inputCommands\":" +
                std::to_string(workerStats.inputCommands) +
                ",\"reconnectGraceMs\":" +
                std::to_string(options_.reconnectGraceMs) +
                ",\"workerTickAvgMs\":" +
                std::to_string(workerStats.tickAvgMs) +
                ",\"workerTickP95Ms\":" +
                std::to_string(workerStats.tickP95Ms) +
                ",\"workerTickMaxMs\":" +
                std::to_string(workerStats.tickMaxMs) +
                ",\"snapshotPackets\":" +
                std::to_string(workerStats.snapshotPackets) +
                ",\"aoiBytes\":" +
                std::to_string(workerStats.aoiBytes) +
                ",\"fullSnapshotBytes\":" +
                std::to_string(workerStats.fullSnapshotBytes) +
                ",\"aoiEntitySamples\":" +
                std::to_string(workerStats.aoiEntitySamples) +
                ",\"fullEntitySamples\":" +
                std::to_string(workerStats.fullEntitySamples) +
                ",\"storedPlayers\":" +
                std::to_string(storeStats.players) +
                ",\"storedMatches\":" +
                std::to_string(storeStats.matches) + "}");
    }

    // ---- Prometheus 文本指标（监控系统抓取） ----
    if (path == "/metrics") {
        const WorkerStats workerStats = worker_ ? worker_->stats()
                                                : WorkerStats{};
        const StoreStats storeStats = store_ ? store_->stats()
                                             : StoreStats{};
        std::size_t clientCount = 0;
        std::size_t reactorDepth = 0;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            clientCount = sessions_.size();
        }
        for (const auto& reactor : reactors_) {
            reactorDepth += reactor->commandDepth();
        }
        const double reduction =
            workerStats.fullSnapshotBytes == 0
                ? 0.0
                : 1.0 -
                      static_cast<double>(workerStats.aoiBytes) /
                          static_cast<double>(workerStats.fullSnapshotBytes);

        // 按 Prometheus  exposition 格式输出：# TYPE 声明 + 指标行
        std::ostringstream metrics;
        metrics << "# TYPE blob_rooms gauge\n"
                << "blob_rooms " << workerStats.rooms << "\n"
                << "# TYPE blob_players gauge\n"
                << "blob_players " << workerStats.players << "\n"
                << "# TYPE blob_disconnected_players gauge\n"
                << "blob_disconnected_players "
                << workerStats.disconnectedPlayers << "\n"
                << "# TYPE blob_entities gauge\n"
                << "blob_entities " << workerStats.entities << "\n"
                << "# TYPE blob_io_reactors gauge\n"
                << "blob_io_reactors " << reactors_.size() << "\n"
                << "# TYPE blob_io_clients gauge\n"
                << "blob_io_clients " << clientCount << "\n"
                << "# TYPE blob_reactor_commands gauge\n"
                << "blob_reactor_commands " << reactorDepth << "\n"
                << "# TYPE blob_worker_tick_avg_ms gauge\n"
                << "blob_worker_tick_avg_ms " << workerStats.tickAvgMs << "\n"
                << "# TYPE blob_worker_tick_p95_ms gauge\n"
                << "blob_worker_tick_p95_ms " << workerStats.tickP95Ms << "\n"
                << "# TYPE blob_worker_tick_max_ms gauge\n"
                << "blob_worker_tick_max_ms " << workerStats.tickMaxMs << "\n"
                << "# TYPE blob_worker_tick_over_50_total counter\n"
                << "blob_worker_tick_over_50_total "
                << workerStats.tickOver50 << "\n"
                << "# TYPE blob_inbound_dropped_total counter\n"
                << "blob_inbound_dropped_total "
                << workerStats.inboundDropped << "\n"
                << "# TYPE blob_outbound_dropped_total counter\n"
                << "blob_outbound_dropped_total "
                << workerStats.outboundDropped << "\n"
                << "# TYPE blob_slow_client_disconnects_total counter\n"
                << "blob_slow_client_disconnects_total "
                << slowClientDisconnects_ << "\n"
                << "# TYPE blob_ai_takeover_ticks_total counter\n"
                << "blob_ai_takeover_ticks_total "
                << workerStats.aiTakeoverTicks << "\n"
                << "# TYPE blob_aoi_bandwidth_reduction_ratio gauge\n"
                << "blob_aoi_bandwidth_reduction_ratio " << reduction << "\n"
                << "# TYPE blob_stored_players gauge\n"
                << "blob_stored_players " << storeStats.players << "\n"
                << "# TYPE blob_stored_matches gauge\n"
                << "blob_stored_matches " << storeStats.matches << "\n"
                << "# TYPE blob_total_connections_total counter\n"
                << "blob_total_connections_total " << totalConnections_ << "\n";
        // 构建信息指标（带版本 label），用于确认线上版本
        metrics << "# TYPE blob_build_info gauge\n"
                << "blob_build_info{version=\"" << kProjectVersion
                << "\"} 1\n";
        net::HttpResult result;
        result.statusCode = 200;
        result.statusText = "OK";
        result.contentType =
            "text/plain; version=0.0.4; charset=utf-8";
        result.body = metrics.str();
        return result;
    }

    // ---- 其余路径：静态资源（仅允许 GET） ----
    if (request.method != "GET") {
        return jsonResult(405, "Method Not Allowed",
                          "{\"error\":\"method_not_allowed\"}");
    }

    // 解析磁盘路径并读取文件
    bool found = false;
    const std::string fullPath = resolveWebPath(path, found);
    if (!found) {
        net::HttpResult result;
        result.statusCode = 404;
        result.statusText = "Not Found";
        result.contentType = "text/plain; charset=utf-8";
        result.body = "404 Not Found";
        return result;
    }
    net::HttpResult result;
    result.contentType = mimeTypeFor(fullPath);
    if (!readFileBinary(fullPath, result.body)) {
        // 文件存在但读取失败（权限/IO 错误）：500
        result.statusCode = 500;
        result.statusText = "Internal Server Error";
        result.contentType = "text/plain; charset=utf-8";
        result.body = "read failed";
    }
    return result;
}

std::string Gateway::resolveWebPath(const std::string& urlPath,
                                    bool& found) const {
    found = false;
    // 安全：拒绝任何含 ".." 的路径，防止目录穿越读到系统文件
    if (urlPath.find("..") != std::string::npos) return std::string();
    // 兼容从项目根目录或 bin/ 目录启动：依次尝试 webRoot 及其上两级
    const std::string candidates[] = {
        options_.webRoot,
        "../" + options_.webRoot,
        "../../" + options_.webRoot,
    };
    for (const std::string& base : candidates) {
        const std::string full = base + urlPath;
        std::ifstream probe(full, std::ios::binary);
        if (probe.good()) {
            found = true;
            return full;
        }
    }
    return std::string();
}

}  // namespace blob
