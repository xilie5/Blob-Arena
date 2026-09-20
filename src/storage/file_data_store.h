// =============================================================================
//  file_data_store.h —— 无外部依赖的 TSV 文件持久化实现
// =============================================================================
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "storage/data_store.h"

namespace blob {

class FileDataStore final : public DataStore {
public:
    explicit FileDataStore(std::string directory);

    bool load(std::string& error) override;
    std::optional<LoginResult> login(const std::string& username) override;
    std::optional<PlayerProfile> authenticate(
        const std::string& token) const override;

    bool recordMatch(
        const std::string& matchId,
        const std::string& roomCode,
        uint64_t endedAtMs,
        const std::vector<MatchPlayerStanding>& standings) override;

    std::vector<MatchHistoryEntry> matchesFor(
        PlayerId playerId, std::size_t limit) const override;
    std::vector<LeaderboardEntry> leaderboard(
        std::size_t limit) const override;
    StoreStats stats() const override;

private:
    struct SessionRecord {
        PlayerId playerId = kInvalidPlayerId;
        uint64_t createdAtMs = 0;
    };

    struct MatchRecord {
        std::string matchId;
        std::string roomCode;
        uint64_t endedAtMs = 0;
        int playerCount = 0;
    };

    struct MatchPlayerRecord {
        std::string matchId;
        PlayerId playerId = kInvalidPlayerId;
        std::string username;
        int rank = 0;
        int score = 0;
        int kills = 0;
        int deaths = 0;
    };

    std::string pathFor(const std::string& filename) const;
    bool loadPlayers(std::string& error);
    bool loadSessions(std::string& error);
    bool loadMatches(std::string& error);
    bool loadMatchPlayers(std::string& error);

    bool appendPlayer(const PlayerProfile& profile, std::string& error);
    bool appendSession(const std::string& token, const SessionRecord& session,
                       std::string& error);
    bool appendMatch(const MatchRecord& match, std::string& error);
    bool appendMatchPlayer(const MatchPlayerRecord& record, std::string& error);

    std::string generateToken() const;
    static std::string normalizeUsername(const std::string& username);
    static uint64_t nowMillis();

    std::string directory_;
    PlayerId nextPlayerId_ = 1;
    std::unordered_map<PlayerId, PlayerProfile> players_;
    std::unordered_map<std::string, PlayerId> playerIdByUsername_;
    std::unordered_map<std::string, SessionRecord> sessions_;
    std::vector<MatchRecord> matches_;
    std::vector<MatchPlayerRecord> matchPlayers_;
    std::unordered_set<std::string> recordedMatchIds_;
    mutable std::mutex mutex_;
};

}  // namespace blob
