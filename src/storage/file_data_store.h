// =============================================================================
//  file_data_store.h —— 无外部依赖的 TSV 文件持久化实现
//
//  数据保存在 data/ 目录的 4 个 TSV 文件（制表符分隔的文本表）：
//    players.tsv        id ⇥ username ⇥ createdAtMs
//    sessions.tsv       token ⇥ playerId ⇥ createdAtMs
//    matches.tsv        matchId ⇥ roomCode ⇥ endedAtMs ⇥ playerCount
//    match_players.tsv  matchId ⇥ playerId ⇥ username ⇥ rank ⇥ score ⇥ kills ⇥ deaths
//
//  写入策略：只追加（append-only），不重写全文件——崩溃不毁历史，写入开销小。
//  启动时把 4 个文件完整读入内存，之后查询全部走内存哈希表/数组。
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

/// @brief 基于 TSV 文件的 DataStore 实现（final：不再被继承）
class FileDataStore final : public DataStore {
public:
    /// @brief 构造（仅记录目录，真正加载在 load()）
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
    /// 会话记录（token -> playerId + 创建时间）
    struct SessionRecord {
        PlayerId playerId = kInvalidPlayerId;
        uint64_t createdAtMs = 0;
    };

    /// 对局主记录
    struct MatchRecord {
        std::string matchId;
        std::string roomCode;
        uint64_t endedAtMs = 0;
        int playerCount = 0;
    };

    /// 对局-玩家明细记录（每局每人一行，扁平结构便于聚合）
    struct MatchPlayerRecord {
        std::string matchId;
        PlayerId playerId = kInvalidPlayerId;
        std::string username;
        int rank = 0;
        int score = 0;
        int kills = 0;
        int deaths = 0;
    };

    std::string pathFor(const std::string& filename) const; // 目录 + 文件名拼接
    // 启动加载四张表（任一失败通过 error 返回并中止启动）
    bool loadPlayers(std::string& error);
    bool loadSessions(std::string& error);
    bool loadMatches(std::string& error);
    bool loadMatchPlayers(std::string& error);

    // 追加写单条记录（append-only，失败返回 false 并填 error）
    bool appendPlayer(const PlayerProfile& profile, std::string& error);
    bool appendSession(const std::string& token, const SessionRecord& session,
                       std::string& error);
    bool appendMatch(const MatchRecord& match, std::string& error);
    bool appendMatchPlayer(const MatchPlayerRecord& record, std::string& error);

    std::string generateToken() const;                           // 生成 32 位十六进制随机 token
    static std::string normalizeUsername(const std::string& username); // 用户名归一化
    static uint64_t nowMillis();                                 // 当前毫秒时间戳

    std::string directory_;     ///< 数据目录（如 "data"）
    PlayerId nextPlayerId_ = 1; ///< 下一个玩家 ID（加载历史后推进到 max+1）
    std::unordered_map<PlayerId, PlayerProfile> players_;        ///< id -> 资料
    std::unordered_map<std::string, PlayerId> playerIdByUsername_; ///< 归一化名 -> id
    std::unordered_map<std::string, SessionRecord> sessions_;    ///< token -> 会话
    std::vector<MatchRecord> matches_;                           ///< 对局主记录
    std::vector<MatchPlayerRecord> matchPlayers_;                ///< 对局明细
    std::unordered_set<std::string> recordedMatchIds_;           ///< 已记录对局 ID（幂等去重）
    mutable std::mutex mutex_;  ///< 保护全部内存数据；mutable 允许 const 查询方法加锁
};

}  // namespace blob
