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

bool readFileBinary(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream stream;
    stream << in.rdbuf();
    out = stream.str();
    return true;
}

uint32_t nowMillis() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

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
            return true;
        }
        value.push_back(c);
    }
    return false;
}

std::string bearerToken(const net::HttpRequest& request) {
    const std::string authorization = request.header("authorization");
    const std::string prefix = "Bearer ";
    if (authorization.size() <= prefix.size() ||
        authorization.compare(0, prefix.size(), prefix) != 0) {
        return std::string();
    }
    return authorization.substr(prefix.size());
}

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
        return fallback;
    }
}

uint16_t packetMessageId(const std::string& packet) {
    if (packet.size() < proto::kHeaderSize) return 0;
    const auto* bytes =
        reinterpret_cast<const uint8_t*>(packet.data());
    return static_cast<uint16_t>(bytes[4] |
                                 (static_cast<uint16_t>(bytes[5]) << 8));
}

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
    stop();
}

bool Gateway::start() {
    if (!net::initialize()) {
        std::printf("[gateway] WSAStartup failed\n");
        return false;
    }
    poller_.reset(net::Poller::create());
    if (!poller_ || !setupListener()) {
        return false;
    }

    store_.reset(new FileDataStore(options_.dataDir));
    std::string storeError;
    if (!store_->load(storeError)) {
        std::printf("[gateway] data store load failed: %s\n",
                    storeError.c_str());
        return false;
    }

    worker_.reset(new RoomWorker(
        options_.seed, static_cast<float>(options_.worldSize),
        options_.arenaCountdownSec, options_.arenaDurationSec,
        options_.arenaShrinkStartSec, options_.reconnectGraceMs,
        store_.get()));
    worker_->start();

    const std::size_t reactorCount =
        std::clamp<std::size_t>(options_.ioThreads, 1, 16);
    reactors_.reserve(reactorCount);
    for (std::size_t i = 0; i < reactorCount; ++i) {
        auto reactor = std::make_unique<net::IoReactor>(
            i,
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

    const Clock::time_point now = Clock::now();
    nextTick_ = now;
    nextStats_ = now + std::chrono::seconds(5);
    running_ = true;

    std::printf("[gateway] acceptor=1 ioReactors=%zu roomWorkers=1 tick=%dHz world=%u\n",
                reactors_.size(), cfg::kTickRate, options_.worldSize);
    std::printf("[gateway] listening on http://127.0.0.1:%u (web root: %s, data: %s)\n",
                static_cast<unsigned>(options_.port), options_.webRoot.c_str(),
                options_.dataDir.c_str());
    return true;
}

bool Gateway::setupListener() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenFd_ == BLOB_INVALID_SOCKET) return false;

    int yes = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes),
               static_cast<int>(sizeof(yes)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(options_.port);
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
    const Duration tickInterval(1000000 / cfg::kTickRate);
    while (running_) {
        drainWorkerOutbound();

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
            if (result.fd == listenFd_) acceptConnections();
        }

        drainWorkerOutbound();
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
    while (true) {
        sockaddr_in address{};
        socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
        const socket_t fd =
            ::accept(listenFd_, reinterpret_cast<sockaddr*>(&address),
                     &addressLength);
        if (fd == BLOB_INVALID_SOCKET) break;

        bool overLimit = false;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            overLimit = sessions_.size() >= options_.maxConnections;
        }
        if (overLimit) {
            net::closeSocket(fd);
            continue;
        }

        net::setNonBlocking(fd);
        net::setNoDelay(fd);
        auto session = std::make_shared<ClientSession>();
        const std::size_t reactorIndex = nextReactor_ % reactors_.size();
        nextReactor_ = (nextReactor_ + 1) % reactors_.size();
        session->reactorId = reactorIndex;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            sessions_[fd] = session;
        }
        if (!reactors_[reactorIndex]->assign(fd)) {
            eraseSession(fd);
            net::closeSocket(fd);
            continue;
        }
        ++totalConnections_;
    }
}

void Gateway::onClientClosed(socket_t fd) {
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
    auto packet = std::make_shared<std::string>(
        proto::packPacket(static_cast<uint16_t>(msgId), 0, payload));
    sendRawPacket(fd, session, std::move(packet));
}

void Gateway::broadcastPacketInRoom(const std::string& roomCode,
                                    const std::string& packet,
                                    socket_t exceptFd) {
    std::vector<std::pair<socket_t, SessionPtr>> recipients;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (const auto& entry : sessions_) {
            const SessionPtr& session = entry.second;
            std::lock_guard<std::mutex> sessionLock(session->mutex);
            if (!session->joined || session->wantClose ||
                session->roomCode != roomCode || entry.first == exceptFd) {
                continue;
            }
            recipients.emplace_back(entry.first, session);
        }
    }
    for (auto& recipient : recipients) {
        sendRawPacket(recipient.first, recipient.second,
                      std::make_shared<std::string>(packet));
    }
}

void Gateway::drainWorkerOutbound() {
    if (!worker_) return;
    WorkerOutbound outbound;
    while (worker_->pollOutbound(outbound)) {
        SessionPtr session = findSession(outbound.fd);
        if (outbound.type == OutboundType::BindClient) {
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

        broadcastPacketInRoom(outbound.roomCode, outbound.packet,
                              outbound.exceptFd);
    }
}

void Gateway::onTick() {
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

    if (now >= nextStats_) {
        broadcastStats();
        logStats();
        nextStats_ += std::chrono::seconds(5);
    }
}

void Gateway::broadcastStats() {
    if (!worker_) return;
    const WorkerStats stats = worker_->stats();
    proto::Writer writer;
    writer.u16(static_cast<uint16_t>(
        std::min<std::size_t>(stats.players, 0xFFFFu)));
    writer.u16(static_cast<uint16_t>(
        std::min<std::size_t>(stats.entities, 0xFFFFu)));
    writer.u16(static_cast<uint16_t>(cfg::kTickRate));
    const std::string payload = writer.data();

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

    std::printf(
        "[worker] workerTickMs: avg=%.2f p95=%.2f max=%.2f over50=%llu/%zu\n",
        workerStats.tickAvgMs, workerStats.tickP95Ms, workerStats.tickMaxMs,
        static_cast<unsigned long long>(workerStats.tickOver50),
        workerStats.tickSamples);
    std::fflush(stdout);
}

void Gateway::onPacket(socket_t fd, uint16_t msgId,
                       const std::string& payload) {
    SessionPtr session = findSession(fd);
    if (!session) return;

    switch (static_cast<proto::C2S>(msgId)) {
        case proto::C2S::Auth: {
            proto::Reader reader(payload.data(), payload.size());
            const auto profile = store_->authenticate(reader.str8());
            if (!profile.has_value()) {
                proto::Writer writer;
                writer.u8(4);
                writer.str8("invalid token");
                sendMessage(fd, session, proto::S2C::Error, writer.data());
                requestClose(fd, session);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(session->mutex);
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

        case proto::C2S::Join:
        case proto::C2S::CreateRoom:
        case proto::C2S::JoinRoom: {
            AddClientCommand command;
            command.fd = fd;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (session->joined || session->pendingJoin ||
                    session->playerId == kInvalidPlayerId) {
                    return;
                }
                session->pendingJoin = true;
                command.playerId = session->playerId;
                command.username = session->username;
            }
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

        case proto::C2S::Input: {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (!session->joined) return;
            }
            proto::Reader reader(payload.data(), payload.size());
            const uint32_t seq = reader.u32();
            const int8_t dx = reader.i8();
            const int8_t dy = reader.i8();
            if (reader.ok()) {
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->inputSeq = seq;
                }
                worker_->postInput(fd, static_cast<float>(dx) / 127.0f,
                                   static_cast<float>(dy) / 127.0f);
            }
            return;
        }

        case proto::C2S::Ping: {
            proto::Reader reader(payload.data(), payload.size());
            const uint32_t clientTime = reader.u32();
            proto::Writer writer;
            writer.u32(clientTime);
            writer.u32(nowMillis());
            sendMessage(fd, session, proto::S2C::Pong, writer.data());
            return;
        }

        case proto::C2S::Leave:
            if (worker_) worker_->postRemoveClient(fd);
            requestClose(fd, session);
            return;
    }
}

net::HttpResult Gateway::onHttpRequest(socket_t fd,
                                       const net::HttpRequest& request) {
    (void)fd;
    std::string path = request.path;
    if (path.empty() || path == "/") path = "/index.html";

    if (path == "/health") {
        return jsonResult(200, "OK", "{\"status\":\"ok\"}");
    }

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
        return jsonResult(
            200, "OK",
            "{\"playerId\":" + std::to_string(login->profile.id) +
                ",\"username\":\"" +
                jsonEscape(login->profile.username) + "\",\"token\":\"" +
                jsonEscape(login->token) + "\"}");
    }

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

    if (path == "/api/matches") {
        const auto profile = store_->authenticate(bearerToken(request));
        if (!profile.has_value()) {
            return jsonResult(401, "Unauthorized",
                              "{\"error\":\"invalid_token\"}");
        }
        const auto matches =
            store_->matchesFor(profile->id, queryLimit(request.query, 20, 100));
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

    if (request.method != "GET") {
        return jsonResult(405, "Method Not Allowed",
                          "{\"error\":\"method_not_allowed\"}");
    }

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
    if (urlPath.find("..") != std::string::npos) return std::string();
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
