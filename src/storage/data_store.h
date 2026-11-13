// =============================================================================
//  data_store.h —— 账号、会话、战绩与排行榜存储接口
//
//  这是“抽象接口层”：业务代码（Gateway / RoomWorker）只依赖 DataStore，
//  不依赖具体存储方式。当前实现是 TSV 文件（FileDataStore），
//  将来换成 SQLite/MySQL 时只需新增一个子类，上层代码一行不改（依赖倒置）。
//
//  线程约定：实现类需自带线程安全（FileDataStore 内部用一把 mutex 保护），
//  因为登录/鉴权在主线程调用、写战绩在 RoomWorker 线程调用。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace blob {

/// 玩家 ID 类型（全局唯一，0 为无效）
using PlayerId = uint64_t;
constexpr PlayerId kInvalidPlayerId = 0;

/// @brief 玩家资料
struct PlayerProfile {
    PlayerId    id = kInvalidPlayerId;  ///< 玩家 ID
    std::string username;               ///< 用户名（大小写不敏感唯一）
    uint64_t    createdAtMs = 0;        ///< 账号创建时间（毫秒时间戳）
};

/// @brief 登录成功结果：资料 + 新签发的 Session Token
struct LoginResult {
    PlayerProfile profile;
    std::string   token;
};

/// @brief 一局比赛中一名玩家的成绩（写库用）
struct MatchPlayerStanding {
    PlayerId    playerId = kInvalidPlayerId;
    std::string username;
    int         rank = 0;    ///< 名次（从 1 开始）
    int         score = 0;
    int         kills = 0;
    int         deaths = 0;
};

/// @brief 一条个人历史战绩（/api/matches 返回用）
struct MatchHistoryEntry {
    std::string matchId;      ///< 对局 ID（房间码-种子）
    std::string roomCode;     ///< 房间码
    uint64_t    endedAtMs = 0;///< 对局结束时间戳
    int         playerCount = 0;
    int         rank = 0;
    int         score = 0;
    int         kills = 0;
    int         deaths = 0;
};

/// @brief 排行榜一行（按玩家聚合）
struct LeaderboardEntry {
    PlayerId    playerId = kInvalidPlayerId;
    std::string username;
    int         totalScore = 0;  ///< 累计总分
    int         games = 0;       ///< 总场次
    int         wins = 0;        ///< 胜场（rank==1）
    int         bestScore = 0;   ///< 单场最高分
};

/// @brief 存储层运行统计（/api/status、/metrics 用）
struct StoreStats {
    std::size_t players = 0;      ///< 玩家数
    std::size_t sessions = 0;     ///< 有效会话（token）数
    std::size_t matches = 0;      ///< 对局主记录数
    std::size_t matchPlayers = 0; ///< 对局明细行数
};

/// @brief 存储层抽象接口
class DataStore {
public:
    virtual ~DataStore() = default;

    /// @brief 启动时加载全部持久化数据
    /// @param[out] error 失败原因
    /// @return true=成功（文件不存在也视为首次运行的成功）
    virtual bool load(std::string& error) = 0;

    /// @brief 用户名登录（开发环境免密码；新用户名自动注册）
    /// @return 成功返回资料+token；用户名非法返回 nullopt
    virtual std::optional<LoginResult> login(const std::string& username) = 0;

    /// @brief 用 Session Token 鉴权
    /// @return token 有效返回玩家资料；无效/过期返回 nullopt
    virtual std::optional<PlayerProfile> authenticate(
        const std::string& token) const = 0;

    /// @brief 记录一局比赛结果（要求幂等：同一 matchId 只记录一次）
    /// @param matchId 对局唯一 ID
    /// @param roomCode 房间码
    /// @param endedAtMs 结束时间戳
    /// @param standings 全体玩家名次
    virtual bool recordMatch(
        const std::string& matchId,
        const std::string& roomCode,
        uint64_t endedAtMs,
        const std::vector<MatchPlayerStanding>& standings) = 0;

    /// @brief 查询某玩家的历史战绩（按结束时间倒序，最多 limit 条）
    virtual std::vector<MatchHistoryEntry> matchesFor(
        PlayerId playerId, std::size_t limit) const = 0;

    /// @brief 查询累计排行榜（按总分降序，最多 limit 条）
    virtual std::vector<LeaderboardEntry> leaderboard(
        std::size_t limit) const = 0;

    /// @brief 查询存储层统计指标
    virtual StoreStats stats() const = 0;
};

}  // namespace blob
