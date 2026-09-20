#include "server/room_worker.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>

#include "core/game_config.h"
#include "protocol/protocol.h"

namespace blob {

namespace {

std::string buildEntityAdd(const Entity& entity) {
    proto::Writer writer;
    writer.u32(entity.id);
    writer.u8(static_cast<uint8_t>(entity.kind));
    writer.u32(entity.color);
    writer.f32(entity.radius);
    writer.f32(entity.pos.x);
    writer.f32(entity.pos.y);
    writer.i32(entity.score);
    writer.str8(entity.name);
    return writer.data();
}

}  // namespace

RoomWorker::RoomWorker(uint32_t seed, float worldSize,
                       int countdownSec, int durationSec, int shrinkStartSec,
                       int reconnectGraceMs,
                       DataStore* store,
                       std::size_t inboundCapacity,
                       std::size_t outboundCapacity)
    : seed_(seed),
      worldSize_(worldSize),
      countdownSec_(countdownSec),
      durationSec_(durationSec),
      shrinkStartSec_(shrinkStartSec),
      reconnectGraceMs_(reconnectGraceMs > 0 ? reconnectGraceMs
                                             : cfg::kReconnectGraceMs),
      store_(store),
      lobby_(seed, worldSize, worldSize, countdownSec, durationSec,
             shrinkStartSec),
      inbound_(inboundCapacity),
      outbound_(outboundCapacity) {}

RoomWorker::~RoomWorker() {
    stop();
}

void RoomWorker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    inbound_.reopen();
    outbound_.reopen();
    thread_ = std::thread([this] { run(); });
}

void RoomWorker::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    inbound_.close();
    outbound_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool RoomWorker::postAddClient(const AddClientCommand& command) {
    if (inbound_.tryPush(WorkerCommand{command})) {
        return true;
    }
    ++inboundDropped_;
    return false;
}

bool RoomWorker::postRemoveClient(socket_t fd) {
    if (inbound_.tryPush(WorkerCommand{RemoveClientCommand{fd}})) {
        return true;
    }
    ++inboundDropped_;
    return false;
}

bool RoomWorker::postDisconnectClient(socket_t fd) {
    if (inbound_.tryPush(WorkerCommand{DisconnectClientCommand{fd}})) {
        return true;
    }
    ++inboundDropped_;
    return false;
}

bool RoomWorker::postResumeClient(socket_t fd, PlayerId playerId,
                                  const std::string& roomCode) {
    if (inbound_.tryPush(WorkerCommand{
            ResumeClientCommand{fd, playerId, roomCode}})) {
        return true;
    }
    ++inboundDropped_;
    return false;
}

bool RoomWorker::postInput(socket_t fd, float dirX, float dirY) {
    if (inbound_.tryPush(WorkerCommand{InputCommand{fd, dirX, dirY}})) {
        return true;
    }
    ++inboundDropped_;
    return false;
}

bool RoomWorker::pollOutbound(WorkerOutbound& out) {
    return outbound_.waitPop(out, std::chrono::milliseconds(0));
}

WorkerStats RoomWorker::stats() const {
    WorkerStats out;
    out.rooms = roomCount_.load();
    out.players = playerCount_.load();
    out.disconnectedPlayers = disconnectedPlayerCount_.load();
    out.entities = entityCount_.load();
    out.inboundDropped = inboundDropped_.load();
    out.outboundDropped = outboundDropped_.load();
    out.inboundDepth = inbound_.size();
    out.outboundDepth = outbound_.size();

    const std::size_t count = tickSampleCount_.load();
    out.tickSamples = count;
    out.snapshotPackets = snapshotPackets_.load();
    out.aoiBytes = aoiBytes_.load();
    out.fullSnapshotBytes = fullSnapshotBytes_.load();
    out.snapshotSamples = snapshotSamples_.load();
    out.aoiEntitySamples = aoiEntitySamples_.load();
    out.fullEntitySamples = fullEntitySamples_.load();
    out.aiTakeoverTicks = aiTakeoverTicks_.load();
    out.inputCommands = inputCommands_.load();
    if (count > 0) {
        std::vector<uint32_t> samples;
        samples.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            samples.push_back(tickSamplesUs_[i].load());
        }
        std::sort(samples.begin(), samples.end());
        uint64_t total = 0;
        for (uint32_t sample : samples) {
            total += sample;
            if (sample > 50000u) ++out.tickOver50;
        }
        out.tickAvgMs =
            static_cast<double>(total) / static_cast<double>(count) / 1000.0;
        out.tickP95Ms =
            static_cast<double>(samples[(samples.size() * 95) / 100]) / 1000.0;
        out.tickMaxMs =
            static_cast<double>(samples.back()) / 1000.0;
    }
    return out;
}

void RoomWorker::run() {
    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::microseconds(1000000 / cfg::kTickRate);
    auto nextTick = Clock::now();

    while (running_.load()) {
        WorkerCommand command;
        if (inbound_.waitPop(command, std::chrono::milliseconds(3))) {
            handleCommand(command);
        }

        const auto now = Clock::now();
        int guard = 0;
        while (running_.load() && now >= nextTick && guard < 4) {
            const auto tickStart = Clock::now();
            tickRooms(cfg::kTickSeconds);
            const auto tickEnd = Clock::now();
            recordTickDuration(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    tickEnd - tickStart)
                    .count()));
            nextTick += interval;
            ++guard;
        }
        if (nextTick <= now) {
            nextTick = now + interval;
        }
    }
}

void RoomWorker::handleCommand(WorkerCommand& command) {
    if (auto* add = std::get_if<AddClientCommand>(&command)) {
        handleAddClient(*add);
    } else if (auto* remove = std::get_if<RemoveClientCommand>(&command)) {
        handleRemoveClient(remove->fd);
    } else if (auto* disconnect =
                   std::get_if<DisconnectClientCommand>(&command)) {
        handleDisconnectClient(disconnect->fd);
    } else if (auto* resume = std::get_if<ResumeClientCommand>(&command)) {
        handleResumeClient(*resume);
    } else if (auto* input = std::get_if<InputCommand>(&command)) {
        handleInput(*input);
    }
}

void RoomWorker::handleAddClient(const AddClientCommand& command) {
    if (command.fd == BLOB_INVALID_SOCKET ||
        command.playerId == kInvalidPlayerId ||
        members_.find(command.playerId) != members_.end() ||
        playerByFd_.find(command.fd) != playerByFd_.end()) {
        return;
    }

    Room* room = nullptr;
    if (command.mode == JoinMode::Quick) {
        room = lobby_.quickMatch();
    } else if (command.mode == JoinMode::Create) {
        const std::string code = lobby_.createPrivateRoom();
        room = lobby_.joinByCode(code);
    } else {
        room = lobby_.joinByCode(command.roomCode);
    }

    if (room == nullptr || !room->canJoin()) {
        proto::Writer writer;
        writer.u8(3);
        writer.str8("room unavailable");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }

    const EntityId entityId = room->addPlayer(command.username);
    if (entityId == kInvalidEntityId) {
        proto::Writer writer;
        writer.u8(1);
        writer.str8("room is full");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }

    Member member;
    member.playerId = command.playerId;
    member.fd = command.fd;
    member.username = command.username;
    member.roomCode = room->code();
    member.entityId = entityId;
    member.connected = true;
    members_[command.playerId] = std::move(member);
    playerByFd_[command.fd] = command.playerId;

    WorkerOutbound bind;
    bind.type = OutboundType::BindClient;
    bind.fd = command.fd;
    bind.roomCode = room->code();
    bind.entityId = entityId;
    emit(std::move(bind), true);

    const Entity* self = room->world().find(entityId);
    proto::Writer join;
    join.u32(entityId);
    join.f32(room->world().width());
    join.f32(room->world().height());
    join.u16(static_cast<uint16_t>(cfg::kTickRate));
    join.u32(self == nullptr ? 0xFFFFFFu : self->color);
    join.u32(room->seed());
    sendPacket(command.fd, proto::S2C::JoinOk, join.data());

    proto::Writer roomJoined;
    roomJoined.str8(room->code());
    roomJoined.f32(room->world().width());
    roomJoined.f32(room->world().height());
    roomJoined.u8(static_cast<uint8_t>(room->maxPlayers()));
    roomJoined.u8(room->isPublic() ? 1u : 0u);
    sendPacket(command.fd, proto::S2C::RoomJoined, roomJoined.data());

    SpatialGrid grid(room->world().width(), room->world().height(),
                     cfg::kAoiCellSize);
    grid.rebuild(room->world().entities());
    sendAoiUpdate(room, command.fd, members_[command.playerId], grid);
    refreshStats();
}

void RoomWorker::handleRemoveClient(socket_t fd) {
    auto fdIt = playerByFd_.find(fd);
    if (fdIt == playerByFd_.end()) {
        return;
    }
    const PlayerId playerId = fdIt->second;
    auto it = members_.find(playerId);
    if (it == members_.end()) {
        playerByFd_.erase(fdIt);
        return;
    }
    Room* room = lobby_.joinByCode(it->second.roomCode);
    if (room != nullptr) {
        room->removePlayer(it->second.entityId,
                           proto::kRemoveReasonLeave);
    }
    members_.erase(it);
    playerByFd_.erase(fdIt);
    lobby_.pruneEmptyRooms();
    refreshStats();
}

void RoomWorker::handleDisconnectClient(socket_t fd) {
    auto fdIt = playerByFd_.find(fd);
    if (fdIt == playerByFd_.end()) return;
    auto it = members_.find(fdIt->second);
    if (it == members_.end()) {
        playerByFd_.erase(fdIt);
        return;
    }
    Member& member = it->second;
    member.connected = false;
    member.fd = BLOB_INVALID_SOCKET;
    member.disconnectedAt = std::chrono::steady_clock::now();
    member.visible.clear();
    member.known.clear();
    playerByFd_.erase(fdIt);
}

void RoomWorker::handleResumeClient(const ResumeClientCommand& command) {
    if (command.fd == BLOB_INVALID_SOCKET ||
        command.playerId == kInvalidPlayerId) {
        return;
    }
    auto it = members_.find(command.playerId);
    if (it == members_.end() ||
        it->second.roomCode != command.roomCode) {
        proto::Writer writer;
        writer.u8(6);
        writer.str8("resume unavailable");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }
    Room* room = lobby_.joinByCode(command.roomCode);
    if (room == nullptr ||
        room->world().find(it->second.entityId) == nullptr) {
        proto::Writer writer;
        writer.u8(6);
        writer.str8("resume unavailable");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }

    Member& member = it->second;
    if (member.connected &&
        member.fd != BLOB_INVALID_SOCKET) {
        playerByFd_.erase(member.fd);
    }
    member.fd = command.fd;
    member.connected = true;
    member.disconnectedAt = {};
    member.visible.clear();
    member.known.clear();
    playerByFd_[command.fd] = command.playerId;

    WorkerOutbound bind;
    bind.type = OutboundType::BindClient;
    bind.fd = command.fd;
    bind.roomCode = room->code();
    bind.entityId = member.entityId;
    emit(std::move(bind), true);

    proto::Writer resume;
    resume.str8(room->code());
    resume.u32(member.entityId);
    resume.f32(room->world().width());
    resume.f32(room->world().height());
    sendPacket(command.fd, proto::S2C::ResumeOk, resume.data());

    SpatialGrid grid(room->world().width(), room->world().height(),
                     cfg::kAoiCellSize);
    grid.rebuild(room->world().entities());
    sendAoiUpdate(room, command.fd, member, grid);
    refreshStats();
}

void RoomWorker::handleInput(const InputCommand& command) {
    auto fdIt = playerByFd_.find(command.fd);
    if (fdIt == playerByFd_.end()) {
        return;
    }
    auto it = members_.find(fdIt->second);
    if (it == members_.end() || !it->second.connected) return;
    ++inputCommands_;
    Room* room = lobby_.joinByCode(it->second.roomCode);
    if (room != nullptr) {
        room->setInput(it->second.entityId, command.dirX, command.dirY);
    }
}

void RoomWorker::tickRooms(float dt) {
    static const int snapshotTicks =
        std::max(1, static_cast<int>(cfg::kTickRate * cfg::kSnapshotInterval));
    static int snapshotCounter = 0;

    expireDisconnectedPlayers();

    for (Room* room : lobby_.rooms()) {
        for (auto& entry : members_) {
            Member& member = entry.second;
            if (member.roomCode == room->code() && !member.connected) {
                applyAiTakeover(room, member);
            }
        }
        room->step(dt);

        std::vector<KillEvent> kills;
        room->drainKills(kills);
        for (const KillEvent& kill : kills) {
            proto::Writer writer;
            writer.u32(kill.killer);
            writer.u32(kill.victim);
            broadcastPacket(room, proto::S2C::KillFeed, writer.data());
        }

        std::vector<EntityEvent> events;
        room->drainEvents(events);
        // 实体生灭由每个玩家的 AOI 差分处理，避免全房广播。
        events.clear();

        MatchInfo info;
        if (room->consumeMatchState(info)) {
            proto::Writer writer;
            writer.u8(static_cast<uint8_t>(info.phase));
            writer.u16(info.remainSec);
            writer.u16(info.playerCount);
            writer.f32(info.shrinkRadius);
            broadcastPacket(room, proto::S2C::MatchState, writer.data());
        }

        std::vector<Standing> standings;
        if (room->consumeMatchResult(standings)) {
            persistMatch(room, standings);
            proto::Writer writer;
            writer.u16(static_cast<uint16_t>(
                std::min<std::size_t>(standings.size(), 0xFFFFu)));
            for (const Standing& standing : standings) {
                writer.u32(standing.id);
                writer.i32(standing.score);
                writer.u16(static_cast<uint16_t>(
                    std::min(standing.kills, static_cast<int>(0xFFFF))));
                writer.u16(static_cast<uint16_t>(
                    std::min(standing.deaths, static_cast<int>(0xFFFF))));
                writer.f32(standing.radius);
                writer.str8(standing.name);
            }
            broadcastPacket(room, proto::S2C::MatchResult, writer.data());
        }

        ++snapshotCounter;
        if (snapshotCounter % snapshotTicks == 0 &&
            room->playerCount() > 0) {
            SpatialGrid grid(room->world().width(),
                             room->world().height(),
                             cfg::kAoiCellSize);
            grid.rebuild(room->world().entities());
            for (auto& entry : members_) {
                if (entry.second.roomCode == room->code() &&
                    entry.second.connected) {
                    sendAoiUpdate(room, entry.second.fd, entry.second, grid);
                }
            }
        }
    }

    lobby_.pruneEmptyRooms();
    refreshStats();
}

void RoomWorker::emit(WorkerOutbound outbound, bool critical) {
    if (critical) {
        if (!outbound_.waitPush(std::move(outbound),
                                std::chrono::milliseconds(100))) {
            ++outboundDropped_;
        }
    } else if (!outbound_.tryPush(std::move(outbound))) {
        ++outboundDropped_;
    }
}

std::size_t RoomWorker::sendPacket(socket_t fd, proto::S2C msgId,
                                   const std::string& payload) {
    WorkerOutbound outbound;
    outbound.type = OutboundType::SendToFd;
    outbound.fd = fd;
    outbound.packet = proto::packPacket(
        static_cast<uint16_t>(msgId), 0, payload);
    const std::size_t packetSize = outbound.packet.size();
    emit(std::move(outbound), true);
    return packetSize;
}

void RoomWorker::broadcastPacket(const Room* room, proto::S2C msgId,
                                 const std::string& payload,
                                 socket_t exceptFd) {
    WorkerOutbound outbound;
    outbound.type = OutboundType::BroadcastRoom;
    outbound.roomCode = room->code();
    outbound.exceptFd = exceptFd;
    outbound.packet = proto::packPacket(
        static_cast<uint16_t>(msgId), 0, payload);
    const bool critical = msgId != proto::S2C::Snapshot;
    emit(std::move(outbound), critical);
}

void RoomWorker::sendAoiUpdate(Room* room, socket_t fd, Member& member,
                               const SpatialGrid& grid) {
    const Entity* self = room->world().find(member.entityId);
    if (self == nullptr || !self->alive) return;

    grid.queryCircle(self->pos, cfg::kAoiRadius, member.visible);
    std::unordered_set<EntityId> visibleSet(
        member.visible.begin(), member.visible.end());

    std::size_t aliveCount = 0;
    for (const Entity& entity : room->world().entities()) {
        if (entity.alive) ++aliveCount;
    }
    fullEntitySamples_ += aliveCount;
    aoiEntitySamples_ += member.visible.size();
    ++snapshotSamples_;
    fullSnapshotBytes_ += proto::kHeaderSize + 6 + aliveCount * 20;

    std::vector<const Entity*> changed;
    changed.reserve(member.visible.size());

    for (EntityId id : member.visible) {
        const Entity* entity = room->world().find(id);
        if (entity == nullptr || !entity->alive) continue;

        auto known = member.known.find(id);
        if (known == member.known.end()) {
            aoiBytes_ += sendPacket(fd, proto::S2C::EntityAdd,
                                    buildEntityAdd(*entity));
            member.known.emplace(
                id, Member::EntityView{
                        entity->pos.x, entity->pos.y,
                        entity->radius, entity->score});
            continue;
        }
        if (known->second.differs(*entity)) {
            changed.push_back(entity);
            known->second = Member::EntityView{
                entity->pos.x, entity->pos.y,
                entity->radius, entity->score};
        }
    }

    for (auto it = member.known.begin(); it != member.known.end();) {
        if (visibleSet.find(it->first) != visibleSet.end()) {
            ++it;
            continue;
        }
        proto::Writer remove;
        remove.u32(it->first);
        remove.u8(proto::kRemoveReasonAoi);
        aoiBytes_ += sendPacket(fd, proto::S2C::EntityRemove,
                                remove.data());
        it = member.known.erase(it);
    }

    proto::Writer snapshot;
    snapshot.u32(room->world().tick());
    snapshot.u16(static_cast<uint16_t>(
        std::min<std::size_t>(changed.size(), 0xFFFFu)));
    for (const Entity* entity : changed) {
        snapshot.u32(entity->id);
        snapshot.f32(entity->pos.x);
        snapshot.f32(entity->pos.y);
        snapshot.f32(entity->radius);
        snapshot.i32(entity->score);
    }
    aoiBytes_ += sendPacket(fd, proto::S2C::Snapshot,
                            snapshot.data());
    ++snapshotPackets_;
}

void RoomWorker::applyAiTakeover(Room* room, Member& member) {
    ++aiTakeoverTicks_;
    if (!room->acceptsInput()) {
        room->setInput(member.entityId, 0.0f, 0.0f);
        return;
    }
    const Entity* self = room->world().find(member.entityId);
    if (self == nullptr || !self->alive) return;

    const Entity* nearestFood = nullptr;
    float nearestDistanceSq = 1e30f;
    for (const Entity& entity : room->world().entities()) {
        if (!entity.alive || entity.kind != EntityKind::Food) continue;
        const float distanceSq = self->pos.distanceSq(entity.pos);
        if (distanceSq < nearestDistanceSq) {
            nearestDistanceSq = distanceSq;
            nearestFood = &entity;
        }
    }

    if (nearestFood != nullptr) {
        const Vec2 direction = nearestFood->pos - self->pos;
        room->setInput(member.entityId, direction.x, direction.y);
        return;
    }

    static constexpr float kDirections[][2] = {
        {1.0f, 0.0f}, {0.7071f, 0.7071f},
        {0.0f, 1.0f}, {-0.7071f, 0.7071f},
        {-1.0f, 0.0f}, {-0.7071f, -0.7071f},
        {0.0f, -1.0f}, {0.7071f, -0.7071f},
    };
    const std::size_t index =
        (room->world().tick() / static_cast<uint32_t>(cfg::kTickRate)) % 8u;
    room->setInput(member.entityId, kDirections[index][0],
                   kDirections[index][1]);
}

void RoomWorker::expireDisconnectedPlayers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<PlayerId> expired;
    for (const auto& entry : members_) {
        const Member& member = entry.second;
        if (member.connected) continue;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - member.disconnectedAt);
        if (elapsed.count() >= reconnectGraceMs_) {
            expired.push_back(entry.first);
        }
    }
    for (PlayerId playerId : expired) {
        auto it = members_.find(playerId);
        if (it == members_.end()) continue;
        Room* room = lobby_.joinByCode(it->second.roomCode);
        if (room != nullptr) {
            room->removePlayer(it->second.entityId,
                               proto::kRemoveReasonLeave);
        }
        if (it->second.fd != BLOB_INVALID_SOCKET) {
            playerByFd_.erase(it->second.fd);
        }
        members_.erase(it);
    }
    if (!expired.empty()) {
        lobby_.pruneEmptyRooms();
        refreshStats();
    }
}

void RoomWorker::persistMatch(Room* room,
                              const std::vector<Standing>& standings) {
    if (store_ == nullptr) {
        return;
    }
    std::vector<MatchPlayerStanding> rows;
    rows.reserve(standings.size());
    for (std::size_t i = 0; i < standings.size(); ++i) {
        Member* member = findMemberByEntity(room, standings[i].id);
        if (member == nullptr) {
            continue;
        }
        MatchPlayerStanding row;
        row.playerId = member->playerId;
        row.username = member->username;
        row.rank = static_cast<int>(i + 1);
        row.score = standings[i].score;
        row.kills = standings[i].kills;
        row.deaths = standings[i].deaths;
        rows.push_back(std::move(row));
    }
    if (!rows.empty()) {
        store_->recordMatch(room->matchId(), room->code(),
                            static_cast<uint64_t>(
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::system_clock::now()
                                        .time_since_epoch())
                                    .count()),
                            rows);
    }
}

void RoomWorker::refreshStats() {
    std::size_t players = 0;
    std::size_t disconnected = 0;
    std::size_t entities = 0;
    for (const Room* room : lobby_.rooms()) {
        players += room->playerCount();
        entities += room->entityCount();
    }
    for (const auto& entry : members_) {
        if (!entry.second.connected) ++disconnected;
    }
    roomCount_ = lobby_.rooms().size();
    playerCount_ = players;
    disconnectedPlayerCount_ = disconnected;

    entityCount_ = entities;
}

void RoomWorker::recordTickDuration(uint64_t microseconds) {
    const std::size_t index = tickSampleIndex_.load();
    tickSamplesUs_[index].store(
        static_cast<uint32_t>(std::min<uint64_t>(
            microseconds, static_cast<uint64_t>(0xFFFFFFFFu))));
    tickSampleIndex_.store((index + 1) % kTickWindow);
    const std::size_t count = tickSampleCount_.load();
    if (count < kTickWindow) {
        tickSampleCount_.store(count + 1);
    }
}

RoomWorker::Member* RoomWorker::findMemberByEntity(const Room* room,
                                                   EntityId entityId) {
    for (auto& entry : members_) {
        Member& member = entry.second;
        if (member.roomCode == room->code() &&
            member.entityId == entityId) {
            return &member;
        }
    }
    return nullptr;
}

}  // namespace blob
