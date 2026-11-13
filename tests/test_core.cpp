// =============================================================================
//  test_core.cpp —— 纯逻辑测试
//
//  不依赖第三方测试框架，覆盖协议、权威世界、竞技场规则和房间匹配。
// =============================================================================
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "core/arena_match.h"
#include "core/game_config.h"
#include "core/world.h"
#include "core/spatial_grid.h"
#include "concurrency/bounded_queue.h"
#include "protocol/protocol.h"
#include "server/lobby.h"
#include "server/room.h"
#include "storage/file_data_store.h"

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(condition)) {                                                    \
            ++g_failures;                                                      \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__,            \
                        #condition);                                           \
        }                                                                      \
    } while (false)

void beginCase(const char* name) {
    g_case = name;
    std::printf("\n[ RUN      ] %s\n", name);
}

bool near(float a, float b, float epsilon = 0.01f) {
    return std::fabs(a - b) <= epsilon;
}

void testVec2() {
    beginCase("Vec2/基础运算与归一化");
    const blob::Vec2 a(3.0f, 4.0f);
    CHECK(near(a.length(), 5.0f));
    CHECK(near(a.normalized().length(), 1.0f));
    CHECK(blob::Vec2().normalized().isZero());
    CHECK(near(blob::lerp(blob::Vec2(0, 0), blob::Vec2(10, 20), 0.5f).x, 5.0f));
}

void testProtocol() {
    beginCase("协议/Writer Reader 往返与小端布局");
    blob::proto::Writer writer;
    writer.u8(0x12);
    writer.u16(0x3456);
    writer.u32(0x789ABCDE);
    writer.i32(-42);
    writer.f32(3.5f);
    writer.str8("arena");

    blob::proto::Reader reader(writer.data().data(), writer.size());
    CHECK(reader.u8() == 0x12);
    CHECK(reader.u16() == 0x3456);
    CHECK(reader.u32() == 0x789ABCDEu);
    CHECK(reader.i32() == -42);
    CHECK(near(reader.f32(), 3.5f));
    CHECK(reader.str8() == "arena");
    CHECK(reader.ok());
    CHECK(reader.remaining() == 0);

    const std::string packet =
        blob::proto::packPacket(0x1234, 7, writer.data());
    std::size_t offset = 0;
    uint16_t msgId = 0;
    uint16_t seq = 0;
    std::string payload;
    CHECK(blob::proto::unpackPacket(packet, offset, msgId, seq, payload));
    CHECK(msgId == 0x1234);
    CHECK(seq == 7);
    CHECK(payload == writer.data());
}

void testWorldInitialState() {
    beginCase("World/初始状态与随机确定性");
    blob::World a(123u, 1000.0f, 800.0f);
    blob::World b(123u, 1000.0f, 800.0f);
    CHECK(a.size() == 0);
    CHECK(a.playerCount() == 0);
    CHECK(a.nextRandom() == b.nextRandom());
    CHECK(a.nextRandom() == b.nextRandom());

    const blob::EntityId first = a.spawnPlayer("Alpha");
    const blob::EntityId second = b.spawnPlayer("Alpha");
    CHECK(first == second);
    CHECK(a.find(first) != nullptr);
    CHECK(a.find(first)->kind == blob::EntityKind::Player);
    CHECK(a.find(first)->pos.x >= 0.0f);
    CHECK(a.find(first)->pos.x <= 1000.0f);
}

void testMovementAndBounds() {
    beginCase("World/移动、方向归一化与边界夹紧");
    blob::World world(1u, 1000.0f, 1000.0f);
    const blob::EntityId id = world.spawnPlayer("Runner");
    blob::Entity* player = world.find(id);
    player->pos = blob::Vec2(200.0f, 200.0f);
    world.setMoveDir(id, blob::Vec2(3.0f, 4.0f));
    world.step(1.0f);
    CHECK(near(player->pos.x, 356.0f, 0.5f));
    CHECK(near(player->pos.y, 408.0f, 0.5f));

    world.setMoveDir(id, blob::Vec2(1.0f, 0.0f));
    for (int i = 0; i < 20; ++i) {
        world.step(1.0f);
    }
    CHECK(player->pos.x <= 1000.0f - player->radius + 0.01f);
}

void testFoodEatingAndScore() {
    beginCase("World/吃食物增加质量与分数");
    blob::World world(2u, 1000.0f, 1000.0f);
    world.setFoodSpawnEnabled(false);
    const blob::EntityId playerId = world.spawnPlayer("Eater");
    blob::Entity* player = world.find(playerId);
    player->pos = blob::Vec2(500.0f, 500.0f);
    const float oldRadius = player->radius;
    const blob::EntityId foodId = world.spawnFood(blob::Vec2(501.0f, 500.0f));

    world.step(blob::cfg::kTickSeconds);
    CHECK(world.find(foodId) == nullptr);
    player = world.find(playerId);
    CHECK(player->radius > oldRadius);
    CHECK(player->score == blob::cfg::kScoreFood);
    CHECK(player->eaten == 1);
    CHECK(world.foodEatenCount() == 1u);
}

void testPlayerEatingAndRespawn() {
    beginCase("World/玩家互吃、分数与原地重生");
    blob::World world(3u, 1000.0f, 1000.0f);
    world.setFoodSpawnEnabled(false);
    world.setBotSpawnEnabled(false);

    const blob::EntityId hunterId = world.spawnPlayer("Hunter");
    const blob::EntityId preyId = world.spawnPlayer("Prey");
    blob::Entity* hunter = world.find(hunterId);
    blob::Entity* prey = world.find(preyId);
    world.debugSetRadius(hunterId, 50.0f);
    world.debugSetRadius(preyId, 20.0f);
    hunter->pos = blob::Vec2(400.0f, 400.0f);
    prey->pos = blob::Vec2(405.0f, 400.0f);

    world.step(blob::cfg::kTickSeconds);
    hunter = world.find(hunterId);
    prey = world.find(preyId);
    CHECK(hunter->score == blob::cfg::kScorePlayer);
    CHECK(hunter->kills == 1);
    CHECK(prey->deaths == 1);
    CHECK(near(prey->radius, blob::cfg::kPlayerMinRadius, 0.01f));
    CHECK(prey->pos.distanceSq(hunter->pos) > 100.0f);
}

void testBotRules() {
    beginCase("World/NPC 吞噬矩阵与恐慌");
    blob::World world(4u, 1000.0f, 1000.0f);
    world.setFoodSpawnEnabled(false);
    world.setBotSpawnEnabled(false);

    const blob::EntityId botAId = world.spawnBot("a");
    const blob::EntityId botBId = world.spawnBot("b");
    blob::Entity* botA = world.find(botAId);
    blob::Entity* botB = world.find(botBId);
    world.debugSetRadius(botAId, 50.0f);
    world.debugSetRadius(botBId, 10.0f);
    botA->pos = blob::Vec2(400.0f, 400.0f);
    botB->pos = blob::Vec2(405.0f, 400.0f);
    world.step(blob::cfg::kTickSeconds);
    CHECK(world.find(botAId) != nullptr);
    CHECK(world.find(botBId) != nullptr);

    const blob::EntityId playerId = world.spawnPlayer("Predator");
    blob::Entity* player = world.find(playerId);
    world.debugSetRadius(playerId, 80.0f);
    player->pos = blob::Vec2(200.0f, 200.0f);
    blob::Entity* nearbyBot = world.find(botBId);
    nearbyBot->pos = blob::Vec2(450.0f, 200.0f);  // 在恐慌圈内，但不在吞噬范围内
    world.step(blob::cfg::kTickSeconds);
    CHECK(nearbyBot->aiState == 1);  // BotState::Panic
}

void testShrinkRespawnsPlayer() {
    beginCase("World/缩圈越界扣体积，玩家回到安全区");
    blob::World world(5u, 1000.0f, 1000.0f);
    world.setFoodSpawnEnabled(false);
    world.setBotSpawnEnabled(false);
    const blob::EntityId id = world.spawnPlayer("Out");
    blob::Entity* player = world.find(id);
    player->pos = blob::Vec2(900.0f, 900.0f);
    world.debugSetRadius(id, 14.01f);
    world.setShrinkDurationMs(1000u);
    world.setShrinkEnabled(true);
    for (int i = 0; i < 5; ++i) {
        world.step(0.1f);
    }
    player = world.find(id);
    CHECK(player != nullptr);
    CHECK(player->deaths >= 1);
    CHECK(player->radius >= blob::cfg::kPlayerMinRadius);
}

void testArenaStateMachine() {
    beginCase("ArenaMatch/Waiting → Countdown → Playing");
    blob::World world(10u, 1000.0f, 1000.0f);
    blob::ArenaMatch match(1u);
    blob::MatchInfo info;
    CHECK(match.phase() == blob::MatchPhase::Waiting);

    match.tick(world, 0.1f, 1);
    CHECK(match.phase() == blob::MatchPhase::Countdown);
    CHECK(match.acceptsPlayers());
    CHECK(!match.acceptsInput());

    for (int i = 0; i < blob::cfg::kArenaCountdownSec * blob::cfg::kTickRate;
         ++i) {
        match.tick(world, blob::cfg::kTickSeconds, 1);
    }
    CHECK(match.phase() == blob::MatchPhase::Playing);
    CHECK(match.acceptsInput());
    CHECK(match.consumeStateChanged(world, 1, info));
}

void testArenaStartsWhenFull() {
    beginCase("ArenaMatch/满 4 人立即开局");
    blob::World world(11u, 1000.0f, 1000.0f);
    blob::ArenaMatch match(2u);
    match.tick(world, blob::cfg::kTickSeconds, 1);
    CHECK(match.phase() == blob::MatchPhase::Countdown);
    match.tick(world, blob::cfg::kTickSeconds, blob::cfg::kArenaPlayers);
    CHECK(match.phase() == blob::MatchPhase::Playing);
}

void testArenaEndsAndRanks() {
    beginCase("ArenaMatch/180 秒结算并按分数排名");
    blob::World world(12u, 1000.0f, 1000.0f);
    world.setFoodSpawnEnabled(false);
    world.setBotSpawnEnabled(false);
    const blob::EntityId a = world.spawnPlayer("A");
    const blob::EntityId b = world.spawnPlayer("B");
    const blob::EntityId c = world.spawnPlayer("C");
    world.find(a)->score = 30;
    world.find(b)->score = 70;
    world.find(c)->score = 50;

    blob::ArenaMatch match(3u);
    match.debugStartPlaying();
    match.debugAdvance(static_cast<float>(blob::cfg::kArenaDurationSec));
    match.tick(world, 0.0f, 3);
    CHECK(match.phase() == blob::MatchPhase::Ended);
    CHECK(match.isOver());

    std::vector<blob::Standing> standings;
    CHECK(match.consumeResult(standings));
    CHECK(standings.size() == 3);
    CHECK(standings[0].id == b);
    CHECK(standings[1].id == c);
    CHECK(standings[2].id == a);
}

void testArenaConfigurableTiming() {
    beginCase("ArenaMatch/可配置倒计时与单局时长");
    blob::World world(13u, 1000.0f, 1000.0f);
    blob::ArenaMatch match(4u, 1, 3, 1);
    match.tick(world, 0.5f, 1);
    CHECK(match.phase() == blob::MatchPhase::Countdown);
    match.tick(world, 0.5f, 1);
    CHECK(match.phase() == blob::MatchPhase::Playing);
    match.debugAdvance(3.0f);
    match.tick(world, 0.0f, 1);
    CHECK(match.phase() == blob::MatchPhase::Ended);
}

void testRoomLifecycle() {
    beginCase("Room/4 人上限、倒计时和输入门控");
    blob::Room room("TEST", 77u, 1000.0f, 1000.0f, 4u, false);
    CHECK(room.canJoin());
    const blob::EntityId a = room.addPlayer("A");
    CHECK(a != blob::kInvalidEntityId);
    CHECK(room.setInput(a, 1.0f, 0.0f) == false);

    CHECK(room.addPlayer("B") != blob::kInvalidEntityId);
    CHECK(room.addPlayer("C") != blob::kInvalidEntityId);
    CHECK(room.addPlayer("D") != blob::kInvalidEntityId);
    CHECK(room.isFull());
    CHECK(room.addPlayer("E") == blob::kInvalidEntityId);

    room.step(blob::cfg::kTickSeconds);
    CHECK(room.acceptsInput());
    std::vector<blob::Standing> result;
    CHECK(!room.consumeMatchResult(result));
    CHECK(room.setInput(a, 1.0f, 0.0f));
}

void testLobbyQuickMatch() {
    beginCase("Lobby/快速匹配复用可加入房间，开局后创建新房");
    blob::Lobby lobby(88u, 1000.0f, 1000.0f);
    blob::Room* first = lobby.quickMatch();
    CHECK(first != nullptr);
    CHECK(first->isPublic());
    CHECK(lobby.quickMatch() == first);

    for (int i = 0; i < blob::cfg::kArenaPlayers; ++i) {
        CHECK(first->addPlayer("P") != blob::kInvalidEntityId);
    }
    first->step(blob::cfg::kTickSeconds);
    CHECK(!first->canJoin());

    blob::Room* second = lobby.quickMatch();
    CHECK(second != nullptr);
    CHECK(second != first);
    CHECK(lobby.rooms().size() == 2);

    // 两个房间各自持有独立 World：往 second 加人不会改变 first 的实体数
    const std::size_t firstEntities = first->world().size();
    const std::size_t secondEntities = second->world().size();
    CHECK(firstEntities > 0);
    CHECK(secondEntities > 0);
    CHECK(second->addPlayer("Q") != blob::kInvalidEntityId);
    CHECK(first->world().size() == firstEntities);
    CHECK(second->world().size() == secondEntities + 1);
}

void testLobbyPrune() {
    beginCase("Lobby/空房回收");
    blob::Lobby lobby(99u, 1000.0f, 1000.0f);
    const std::string code = lobby.createPrivateRoom();
    CHECK(!code.empty());
    blob::Room* room = lobby.joinByCode(code);
    CHECK(room != nullptr);
    const blob::EntityId id = room->addPlayer("A");
    CHECK(lobby.rooms().size() == 1);
    CHECK(room->removePlayer(id, blob::proto::kRemoveReasonLeave));
    lobby.pruneEmptyRooms();
    CHECK(lobby.rooms().empty());
    CHECK(lobby.joinByCode(code) == nullptr);
}

void testProtocolMessageIds() {
    beginCase("协议/消息号契约");
    CHECK(static_cast<uint16_t>(blob::proto::C2S::Auth) == 0x0001);
    CHECK(static_cast<uint16_t>(blob::proto::C2S::Join) == 0x0010);
    CHECK(static_cast<uint16_t>(blob::proto::C2S::Resume) == 0x0011);
    CHECK(static_cast<uint16_t>(blob::proto::C2S::Input) == 0x0020);
    CHECK(static_cast<uint16_t>(blob::proto::C2S::CreateRoom) == 0x0040);
    CHECK(static_cast<uint16_t>(blob::proto::C2S::JoinRoom) == 0x0041);
    CHECK(static_cast<uint16_t>(blob::proto::S2C::AuthOk) == 0x0080);
    CHECK(static_cast<uint16_t>(blob::proto::S2C::JoinOk) == 0x0081);
    CHECK(static_cast<uint16_t>(blob::proto::S2C::MatchState) == 0x0093);
    CHECK(static_cast<uint16_t>(blob::proto::S2C::MatchResult) == 0x0094);
    CHECK(static_cast<uint16_t>(blob::proto::S2C::ResumeOk) == 0x008B);
}

void testFileDataStore() {
    beginCase("FileDataStore/登录、鉴权、幂等结算、战绩与排行榜");
    const std::string directory = "p0_test_store";
    const std::vector<std::string> files = {
        "players.tsv",
        "sessions.tsv",
        "matches.tsv",
        "match_players.tsv",
    };
    for (const std::string& file : files) {
        std::remove((directory + "/" + file).c_str());
    }
#ifdef _WIN32
    ::_rmdir(directory.c_str());
#else
    ::rmdir(directory.c_str());
#endif

    blob::FileDataStore store(directory);
    std::string error;
    CHECK(store.load(error));

    const auto alice1 = store.login("Alice");
    const auto alice2 = store.login("alice");
    const auto bob = store.login("Bob");
    CHECK(alice1.has_value());
    CHECK(alice2.has_value());
    CHECK(bob.has_value());
    CHECK(alice1->profile.id == alice2->profile.id);
    CHECK(alice1->token != alice2->token);
    CHECK(store.authenticate(alice2->token).has_value());
    CHECK(!store.authenticate("bad-token").has_value());

    std::vector<blob::MatchPlayerStanding> rows;
    rows.push_back({alice1->profile.id, "Alice", 1, 100, 4, 1});
    rows.push_back({bob->profile.id, "Bob", 2, 40, 1, 3});
    CHECK(store.recordMatch("ROOM-1", "ROOM", 1000u, rows));
    CHECK(!store.recordMatch("ROOM-1", "ROOM", 1000u, rows));
    CHECK(store.stats().matches == 1u);
    CHECK(store.stats().matchPlayers == 2u);

    const auto history = store.matchesFor(alice1->profile.id, 10u);
    CHECK(history.size() == 1u);
    CHECK(history[0].matchId == "ROOM-1");
    CHECK(history[0].rank == 1);
    CHECK(history[0].score == 100);

    const auto leaderboard = store.leaderboard(10u);
    CHECK(leaderboard.size() == 2u);
    CHECK(leaderboard[0].playerId == alice1->profile.id);
    CHECK(leaderboard[0].totalScore == 100);
    CHECK(leaderboard[0].wins == 1);

    blob::FileDataStore reloaded(directory);
    CHECK(reloaded.load(error));
    CHECK(reloaded.authenticate(alice1->token).has_value());
    CHECK(reloaded.matchesFor(alice1->profile.id, 10u).size() == 1u);

    for (const std::string& file : files) {
        std::remove((directory + "/" + file).c_str());
    }
#ifdef _WIN32
    ::_rmdir(directory.c_str());
#else
    ::rmdir(directory.c_str());
#endif
}

void testBoundedQueue() {
    beginCase("BoundedQueue/容量、阻塞、关闭与并发生产消费");
    blob::BoundedQueue<int> queue(2);
    CHECK(queue.capacity() == 2u);
    CHECK(queue.tryPush(1));
    CHECK(queue.tryPush(2));
    CHECK(!queue.tryPush(3));
    int value = 0;
    CHECK(queue.waitPop(value, std::chrono::milliseconds(1)));
    CHECK(value == 1);
    CHECK(queue.waitPush(3, std::chrono::milliseconds(1)));

    blob::BoundedQueue<int> concurrent(64);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::thread producer([&] {
        for (int i = 0; i < 1000; ++i) {
            while (!concurrent.waitPush(i, std::chrono::milliseconds(10))) {
            }
            ++produced;
        }
    });
    std::thread consumer([&] {
        int item = 0;
        while (consumed.load() < 1000) {
            if (concurrent.waitPop(item, std::chrono::milliseconds(10))) {
                ++consumed;
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK(produced.load() == 1000);
    CHECK(consumed.load() == 1000);
    CHECK(queue.size() == 2u);
    queue.close();
    CHECK(queue.closed());
    CHECK(!queue.tryPush(4));
}

void testSpatialGrid() {
    beginCase("SpatialGrid/网格重建与半径查询");
    blob::World world(77u, 1000.0f, 1000.0f);
    world.setFoodSpawnEnabled(false);
    world.setBotSpawnEnabled(false);
    const blob::EntityId a = world.spawnPlayer("a");
    const blob::EntityId b = world.spawnPlayer("b");
    const blob::EntityId c = world.spawnPlayer("c");
    world.find(a)->pos = blob::Vec2(200.0f, 200.0f);
    world.find(b)->pos = blob::Vec2(250.0f, 220.0f);
    world.find(c)->pos = blob::Vec2(800.0f, 800.0f);

    blob::SpatialGrid grid(1000.0f, 1000.0f, 100.0f);
    grid.rebuild(world.entities());
    std::vector<blob::EntityId> hits;
    grid.queryCircle(blob::Vec2(200.0f, 200.0f), 100.0f, hits);
    CHECK(hits.size() == 2u);
    CHECK(std::find(hits.begin(), hits.end(), a) != hits.end());
    CHECK(std::find(hits.begin(), hits.end(), b) != hits.end());
    CHECK(std::find(hits.begin(), hits.end(), c) == hits.end());
}

}  // namespace

int main() {
    std::printf("===========================================\n");
    std::printf(" Blob Arena 核心逻辑测试\n");
    std::printf("===========================================\n");

    testVec2();
    testProtocol();
    testProtocolMessageIds();
    testWorldInitialState();
    testMovementAndBounds();
    testFoodEatingAndScore();
    testPlayerEatingAndRespawn();
    testBotRules();
    testShrinkRespawnsPlayer();
    testArenaStateMachine();
    testArenaStartsWhenFull();
    testArenaEndsAndRanks();
    testArenaConfigurableTiming();
    testRoomLifecycle();
    testLobbyQuickMatch();
    testLobbyPrune();
    testFileDataStore();
    testBoundedQueue();
    testSpatialGrid();

    std::printf("\n===========================================\n");
    if (g_failures == 0) {
        std::printf(" 全部通过：%d 项断言\n", g_checks);
    } else {
        std::printf(" 失败：%d / %d 项断言\n", g_failures, g_checks);
    }
    std::printf("===========================================\n");
    return g_failures == 0 ? 0 : 1;
}
