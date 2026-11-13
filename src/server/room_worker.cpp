#include "server/room_worker.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>

#include "core/game_config.h"
#include "protocol/protocol.h"

namespace blob {

namespace {

/// @brief 组装 EntityAdd 消息载荷（一个实体的完整外观信息）
/// @details 字段顺序必须与客户端 net.js 的解析顺序严格一致：
///          id(u32) kind(u8) color(u32) radius(f32) x(f32) y(f32) score(i32) name(str8)
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
      // 宽限期非法时回落到配置默认值
      reconnectGraceMs_(reconnectGraceMs > 0 ? reconnectGraceMs
                                             : cfg::kReconnectGraceMs),
      store_(store),
      lobby_(seed, worldSize, worldSize, countdownSec, durationSec,
             shrinkStartSec),
      aoiGrid_(worldSize, worldSize, cfg::kAoiCellSize),
      inbound_(inboundCapacity),
      outbound_(outboundCapacity) {}

RoomWorker::~RoomWorker() {
    stop();  // RAII：析构前保证逻辑线程已 join
}

void RoomWorker::start() {
    // CAS 防止重复 start：只有 false->true 成功者启动线程
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    inbound_.reopen();
    outbound_.reopen();
    thread_ = std::thread([this] { run(); });
}

void RoomWorker::stop() {
    // CAS 防止重复 stop
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    inbound_.close();   // 唤醒 waitPop，让主循环看到 running_=false 退出
    outbound_.close();
    if (thread_.joinable()) {
        thread_.join(); // 等待逻辑线程结束，期间不得销毁本对象
    }
}

// ---- 入站投递：全部非阻塞。队列满说明 Worker 处理不过来，丢命令并计数 ----

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
    ++inboundDropped_;   // 移动指令是可丢弃的高频消息，丢一帧不影响游戏
    return false;
}

bool RoomWorker::pollOutbound(WorkerOutbound& out) {
    // 0ms 超时：非阻塞，主线程每轮循环尽可能抽干
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

    // 从环形数组复制采样并排序，计算 avg/P95/max（只读快照，不影响逻辑线程）
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
            if (sample > 50000u) ++out.tickOver50;  // 超过 50ms 的卡顿帧
        }
        out.tickAvgMs =
            static_cast<double>(total) / static_cast<double>(count) / 1000.0;
        // P95：排序后 95% 位置的样本
        out.tickP95Ms =
            static_cast<double>(samples[(samples.size() * 95) / 100]) / 1000.0;
        out.tickMaxMs =
            static_cast<double>(samples.back()) / 1000.0;
    }
    return out;
}

// ============================== 逻辑线程主循环 ==============================

void RoomWorker::run() {
    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::microseconds(1000000 / cfg::kTickRate); // 33.3ms
    auto nextTick = Clock::now();

    while (running_.load()) {
        // 1) 处理入站命令：最多等 3ms。既保证有命令立刻响应，又保证帧节拍不被饿死
        WorkerCommand command;
        if (inbound_.waitPop(command, std::chrono::milliseconds(3))) {
            handleCommand(command);
        }

        // 2) 固定步长推进 + 追赶补帧：
        //    若上一帧被命令处理拖慢，当前时刻可能已超过多个 nextTick，
        //    连续补帧把时间追回来；guard 上限 4 防止长时间卡顿后爆发式补帧。
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
        // 落后过多（补帧上限都追不上）：放弃历史欠账，节拍重置为“现在+一帧”
        if (nextTick <= now) {
            nextTick = now + interval;
        }
    }
}

void RoomWorker::handleCommand(WorkerCommand& command) {
    // 用 get_if 识别 variant 当前持有的命令类型并分发
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
    // 合法性与重复性校验：fd/playerId 有效、玩家未在服、fd 未占用
    if (command.fd == BLOB_INVALID_SOCKET ||
        command.playerId == kInvalidPlayerId ||
        members_.find(command.playerId) != members_.end() ||
        playerByFd_.find(command.fd) != playerByFd_.end()) {
        return;
    }

    // 按进房方式从大厅取房间
    Room* room = nullptr;
    if (command.mode == JoinMode::Quick) {
        room = lobby_.quickMatch();
    } else if (command.mode == JoinMode::Create) {
        const std::string code = lobby_.createPrivateRoom();
        room = lobby_.joinByCode(code);
    } else {
        room = lobby_.joinByCode(command.roomCode);
    }

    // 房间不存在或已不可加入：回错误码 3
    if (room == nullptr || !room->canJoin()) {
        proto::Writer writer;
        writer.u8(3);
        writer.str8("room unavailable");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }

    // 在世界生成玩家实体；房间满返回无效 ID，回错误码 1
    const EntityId entityId = room->addPlayer(command.username);
    if (entityId == kInvalidEntityId) {
        proto::Writer writer;
        writer.u8(1);
        writer.str8("room is full");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }

    // 建立玩家会话并登记两张索引表
    Member member;
    member.playerId = command.playerId;
    member.fd = command.fd;
    member.username = command.username;
    member.roomCode = room->code();
    member.entityId = entityId;
    member.connected = true;
    members_[command.playerId] = std::move(member);
    playerByFd_[command.fd] = command.playerId;

    // ① BindClient：先让 Gateway 完成 fd↔房间↔实体 的会话绑定（关键消息）
    WorkerOutbound bind;
    bind.type = OutboundType::BindClient;
    bind.fd = command.fd;
    bind.roomCode = room->code();
    bind.entityId = entityId;
    emit(std::move(bind), true);

    // ② JoinOk：告诉客户端“你是谁、世界多大、帧率、你的颜色、房间种子”
    const Entity* self = room->world().find(entityId);
    proto::Writer join;
    join.u32(entityId);
    join.f32(room->world().width());
    join.f32(room->world().height());
    join.u16(static_cast<uint16_t>(cfg::kTickRate));
    join.u32(self == nullptr ? 0xFFFFFFu : self->color);
    join.u32(room->seed());
    sendPacket(command.fd, proto::S2C::JoinOk, join.data());

    // ③ RoomJoined：房间码、世界尺寸、人数上限、是否公共
    proto::Writer roomJoined;
    roomJoined.str8(room->code());
    roomJoined.f32(room->world().width());
    roomJoined.f32(room->world().height());
    roomJoined.u8(static_cast<uint8_t>(room->maxPlayers()));
    roomJoined.u8(room->isPublic() ? 1u : 0u);
    sendPacket(command.fd, proto::S2C::RoomJoined, roomJoined.data());

    // ④ 立即推一次 AOI，让玩家入房第一时间看到视野内实体
    aoiGrid_.rebuild(room->world().entities());
    sendAoiUpdate(room, command.fd, members_[command.playerId], aoiGrid_);
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
    // 主动离开：从世界移除实体（原因 Leave）、删除会话、清理空房
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
    // 断线但不删玩家：实体保留，标记离线、记录断线时刻、解绑 fd，
    // 之后由 applyAiTakeover 临时托管，expireDisconnectedPlayers 负责超时清理
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
    // 玩家不存在或声称的房间与实际不符：拒绝（错误码 6）
    if (it == members_.end() ||
        it->second.roomCode != command.roomCode) {
        proto::Writer writer;
        writer.u8(6);
        writer.str8("resume unavailable");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }
    Room* room = lobby_.joinByCode(command.roomCode);
    // 房间或实体已不存在（可能已超时清理）：拒绝
    if (room == nullptr ||
        room->world().find(it->second.entityId) == nullptr) {
        proto::Writer writer;
        writer.u8(6);
        writer.str8("resume unavailable");
        sendPacket(command.fd, proto::S2C::Error, writer.data());
        return;
    }

    Member& member = it->second;
    // 若旧 fd 仍意外登记着，先解除旧绑定
    if (member.connected &&
        member.fd != BLOB_INVALID_SOCKET) {
        playerByFd_.erase(member.fd);
    }
    // 绑定新 fd，恢复在线，清空 AOI 缓存（强制下一帧重新全量下发视野）
    member.fd = command.fd;
    member.connected = true;
    member.disconnectedAt = {};
    member.visible.clear();
    member.known.clear();
    playerByFd_[command.fd] = command.playerId;

    // 重新绑定会话（关键）
    WorkerOutbound bind;
    bind.type = OutboundType::BindClient;
    bind.fd = command.fd;
    bind.roomCode = room->code();
    bind.entityId = member.entityId;
    emit(std::move(bind), true);

    // ResumeOk：房间码 + 原实体 ID + 世界尺寸
    proto::Writer resume;
    resume.str8(room->code());
    resume.u32(member.entityId);
    resume.f32(room->world().width());
    resume.f32(room->world().height());
    sendPacket(command.fd, proto::S2C::ResumeOk, resume.data());

    // 重发当前视野全部实体，无缝接管
    aoiGrid_.rebuild(room->world().entities());
    sendAoiUpdate(room, command.fd, member, aoiGrid_);
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
        // Room 内部还会校验对局阶段（非 Playing 拒绝）
        room->setInput(it->second.entityId, command.dirX, command.dirY);
    }
}

// ============================== 每帧房间推进 ==============================

void RoomWorker::tickRooms(float dt) {
    // 快照节拍：30 tick/s 与 20 快照/s 不是整数倍，用时间累积器才能得到
    // 平均 20Hz（约每 50ms 一次），而不是被截断成每 tick 一次。
    snapshotAccum_ += dt;
    bool sendSnapshot = false;
    if (snapshotAccum_ + 1e-6f >= cfg::kSnapshotInterval) {
        snapshotAccum_ -= cfg::kSnapshotInterval;
        if (snapshotAccum_ < 0.0f) {
            snapshotAccum_ = 0.0f;
        }
        sendSnapshot = true;
    }

    // 1) 先清理断线超时玩家
    expireDisconnectedPlayers();

    // 2) 逐房间推进
    for (Room* room : lobby_.rooms()) {
        // 2a) 对房内断线成员执行 AI 托管
        for (auto& entry : members_) {
            Member& member = entry.second;
            if (member.roomCode == room->code() && !member.connected) {
                applyAiTakeover(room, member);
            }
        }
        room->step(dt);  // 2b) 状态机 + 世界推进

        // 2c) 击杀事件：广播 KillFeed{killer,victim}（关键消息，不丢）
        std::vector<KillEvent> kills;
        room->drainKills(kills);
        for (const KillEvent& kill : kills) {
            proto::Writer writer;
            writer.u32(kill.killer);
            writer.u32(kill.victim);
            broadcastPacket(room, proto::S2C::KillFeed, writer.data());
        }

        // 2d) 实体生灭事件：不再使用全房广播，改由每个玩家的 AOI 差分表达，
        //     因此取出后直接丢弃（AOI 会精确告诉每个玩家“你视野内”的增删）
        std::vector<EntityEvent> events;
        room->drainEvents(events);
        // 实体生灭由每个玩家的 AOI 差分处理，避免全房广播。
        events.clear();

        // 2e) 对局状态变化：广播 MatchState（阶段/剩余秒/人数/缩圈半径）
        MatchInfo info;
        if (room->consumeMatchState(info)) {
            proto::Writer writer;
            writer.u8(static_cast<uint8_t>(info.phase));
            writer.u16(info.remainSec);
            writer.u16(info.playerCount);
            writer.f32(info.shrinkRadius);
            broadcastPacket(room, proto::S2C::MatchState, writer.data());
        }

        // 2f) 对局结算：先落盘，再广播 MatchResult
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

        // 2g) 20Hz AOI 增量快照：重建网格后，为每个在线成员做视野差分
        if (sendSnapshot && room->playerCount() > 0) {
            aoiGrid_.rebuild(room->world().entities());
            for (auto& entry : members_) {
                if (entry.second.roomCode == room->code() &&
                    entry.second.connected) {
                    sendAoiUpdate(room, entry.second.fd, entry.second, aoiGrid_);
                }
            }
        }
    }

    // 3) 房间内可能已无人，统一回收；刷新监控计数
    lobby_.pruneEmptyRooms();
    refreshStats();
}

void RoomWorker::emit(WorkerOutbound outbound, bool critical) {
    if (critical) {
        // 关键消息（BindClient/单播/非快照广播）：最多等 100ms，尽量不丢
        if (!outbound_.waitPush(std::move(outbound),
                                std::chrono::milliseconds(100))) {
            ++outboundDropped_;
        }
    } else if (!outbound_.tryPush(std::move(outbound))) {
        // 非关键消息（高频快照）：满了立刻丢，绝不阻塞游戏线程
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
    emit(std::move(outbound), true);  // 单播默认关键消息
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
    // Snapshot 量最大且可丢，标记为非关键；其余广播关键
    const bool critical = msgId != proto::S2C::Snapshot;
    emit(std::move(outbound), critical);
}

void RoomWorker::sendAoiUpdate(Room* room, socket_t fd, Member& member,
                               const SpatialGrid& grid) {
    const Entity* self = room->world().find(member.entityId);
    if (self == nullptr || !self->alive) return;  // 自己已不存在则无法定视野中心

    // 1) 圆形 AOI 查询：以自己为心、kAoiRadius(450) 为半径
    grid.queryCircle(self->pos, cfg::kAoiRadius, member.visible);
    std::unordered_set<EntityId> visibleSet(
        member.visible.begin(), member.visible.end());

    // 2) 统计采样：对比“全量广播”的字节与实体数，量化 AOI 节省比例
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

    // 3) 视野内实体：新出现的发 EntityAdd；已存在但状态变化的收集进 changed
    for (EntityId id : member.visible) {
        const Entity* entity = room->world().find(id);
        if (entity == nullptr || !entity->alive) continue;

        auto known = member.known.find(id);
        if (known == member.known.end()) {
            // 新进入视野：发完整实体信息并记入 known
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

    // 4) 已离开视野的实体（known 里有、visible 里没有）：发 EntityRemove
    for (auto it = member.known.begin(); it != member.known.end();) {
        if (visibleSet.find(it->first) != visibleSet.end()) {
            ++it;
            continue;
        }
        proto::Writer remove;
        remove.u32(it->first);
        remove.u8(proto::kRemoveReasonAoi);  // 原因：离开视野（实体本身还活着）
        aoiBytes_ += sendPacket(fd, proto::S2C::EntityRemove,
                                remove.data());
        it = member.known.erase(it);
    }

    // 5) 发送本帧变化实体快照（只有位置/半径/分数变化的实体才打包）
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
    // 非 Playing 阶段（倒计时等）：输入清零，托管角色保持静止
    if (!room->acceptsInput()) {
        room->setInput(member.entityId, 0.0f, 0.0f);
        return;
    }
    const Entity* self = room->world().find(member.entityId);
    if (self == nullptr || !self->alive) return;

    // 简单托管 AI：找全图最近的食物并游过去
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
        // 朝食物的方向向量（setInput 内部会归一化）
        const Vec2 direction = nearestFood->pos - self->pos;
        room->setInput(member.entityId, direction.x, direction.y);
        return;
    }

    // 没有食物（极端情况）：按秒在 8 个固定方向间循环游走，避免原地不动
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
    // 先收集超时玩家（不在遍历 members_ 时直接删除）
    for (const auto& entry : members_) {
        const Member& member = entry.second;
        if (member.connected) continue;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - member.disconnectedAt);
        if (elapsed.count() >= reconnectGraceMs_) {
            expired.push_back(entry.first);  // 超过重连宽限期（默认 10 秒）
        }
    }
    // 统一移除：从世界删实体、解绑 fd、删会话、回收空房
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
    // 把按实体 ID 组织的 Standing 关联回玩家账号信息（playerId/username）
    std::vector<MatchPlayerStanding> rows;
    rows.reserve(standings.size());
    for (std::size_t i = 0; i < standings.size(); ++i) {
        Member* member = findMemberByEntity(room, standings[i].id);
        if (member == nullptr) {
            continue;  // 排名实体找不到对应在线会话（异常数据），跳过
        }
        MatchPlayerStanding row;
        row.playerId = member->playerId;
        row.username = member->username;
        row.rank = static_cast<int>(i + 1);  // 名次即排序下标 +1
        row.score = standings[i].score;
        row.kills = standings[i].kills;
        row.deaths = standings[i].deaths;
        rows.push_back(std::move(row));
    }
    if (!rows.empty()) {
        // 以“当前系统时间毫秒”作为对局结束时间戳落盘；
        // store 内部按 matchId 幂等去重，重复结算不会写两次。
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
    // 汇总所有房间的玩家数与实体数
    std::size_t players = 0;
    std::size_t disconnected = 0;
    std::size_t entities = 0;
    for (const Room* room : lobby_.rooms()) {
        players += room->playerCount();
        entities += room->entityCount();
    }
    // 统计当前处于断线托管状态的成员数
    for (const auto& entry : members_) {
        if (!entry.second.connected) ++disconnected;
    }
    // 写入 atomic 供主线程无锁读取
    roomCount_ = lobby_.rooms().size();
    playerCount_ = players;
    disconnectedPlayerCount_ = disconnected;

    entityCount_ = entities;
}

void RoomWorker::recordTickDuration(uint64_t microseconds) {
    // 写入环形数组（300 格循环覆盖），值截断到 uint32 范围
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
    // 在同房间成员中按实体 ID 反查（结算时把实体成绩关联回账号）
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
