// =============================================================================
//  data_store.h —— 账号、会话、战绩与排行榜存储接口
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace blob {

using PlayerId = uint64_t;
constexpr PlayerId kInvalidPlayerId = 0;

struct PlayerProfile {
    PlayerId    id = kInvalidPlayerId;
    std::string username;
    uint64_t    createdAtMs = 0;
};

struct LoginResult {
    PlayerProfile profile;
    std::string   token;
};

struct MatchPlayerStanding {
    PlayerId    playerId = kInvalidPlayerId;
    std::string username;
    int         rank = 0;
    int         score = 0;
    int         kills = 0;
    int         deaths = 0;
};

struct MatchHistoryEntry {
    std::string matchId;
    std::string roomCode;
    uint64_t    endedAtMs = 0;
    int         playerCount = 0;
    int         rank = 0;
    int         score = 0;
    int         kills = 0;
    int         deaths = 0;
};

struct LeaderboardEntry {
    PlayerId    playerId = kInvalidPlayerId;
    std::string username;
    int         totalScore = 0;
    int         games = 0;
    int         wins = 0;
    int         bestScore = 0;
};

struct StoreStats {
    std::size_t players = 0;
    std::size_t sessions = 0;
    std::size_t matches = 0;
    std::size_t matchPlayers = 0;
};

class DataStore {
public:
    virtual ~DataStore() = default;

    virtual bool load(std::string& error) = 0;
    virtual std::optional<LoginResult> login(const std::string& username) = 0;
    virtual std::optional<PlayerProfile> authenticate(
        const std::string& token) const = 0;

    virtual bool recordMatch(
        const std::string& matchId,
        const std::string& roomCode,
        uint64_t endedAtMs,
        const std::vector<MatchPlayerStanding>& standings) = 0;

    virtual std::vector<MatchHistoryEntry> matchesFor(
        PlayerId playerId, std::size_t limit) const = 0;
    virtual std::vector<LeaderboardEntry> leaderboard(
        std::size_t limit) const = 0;
    virtual StoreStats stats() const = 0;
};

}  // namespace blob
