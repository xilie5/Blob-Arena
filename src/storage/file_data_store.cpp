#include "storage/file_data_store.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace blob {

namespace {

/// @brief 确保目录存在（不存在则创建；已存在视为成功）
bool ensureDirectory(const std::string& directory) {
    if (directory.empty()) {
        return true;
    }
#ifdef _WIN32
    const int rc = ::_mkdir(directory.c_str());
#else
    const int rc = ::mkdir(directory.c_str(), 0755);
#endif
    return rc == 0 || errno == EEXIST;
}

/// @brief TSV 字段转义：%、制表符、回车、换行编码成 %XX 十六进制
/// @details 防止用户名中的特殊字符破坏列/行结构
std::string escapeField(const std::string& value) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if (c == '%' || c == '\t' || c == '\r' || c == '\n') {
            out << '%' << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(c);
        } else {
            out << static_cast<char>(c);
        }
    }
    return out.str();
}

/// 单个十六进制字符转数值（非法返回 -1）
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// @brief TSV 字段转义的逆过程（%XX 还原为原字节）
std::string unescapeField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hexValue(value[i + 1]);
            const int lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

/// @brief 按制表符切分一行（保留空列）
std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find('\t', start);
        if (pos == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

/// 解析 uint64，要求整串都是数字（成功返回 true）
bool parseU64(const std::string& text, uint64_t& out) {
    try {
        std::size_t consumed = 0;
        out = std::stoull(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

/// 解析 int，要求整串都是数字（成功返回 true）
bool parseInt(const std::string& text, int& out) {
    try {
        std::size_t consumed = 0;
        out = std::stoi(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

}  // namespace

FileDataStore::FileDataStore(std::string directory)
    : directory_(std::move(directory)) {}

/// @brief 拼出目录下文件的完整路径（自动补目录分隔符）
std::string FileDataStore::pathFor(const std::string& filename) const {
    if (directory_.empty()) {
        return filename;
    }
    const char last = directory_.back();
    return directory_ + (last == '/' || last == '\\' ? "" : "/") + filename;
}

bool FileDataStore::load(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);  // 加载期间禁止并发访问
    if (!ensureDirectory(directory_)) {
        error = "cannot create data directory";
        return false;
    }
    // 四张表全部加载成功才算启动成功（宁可不启动也不带脏数据运行）
    return loadPlayers(error) && loadSessions(error) &&
           loadMatches(error) && loadMatchPlayers(error);
}

bool FileDataStore::loadPlayers(std::string& error) {
    std::ifstream in(pathFor("players.tsv"));
    if (!in) {
        return true;  // 文件不存在：首次运行，视为正常
    }
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty()) continue;
        const auto fields = splitTabs(line);
        if (fields.size() != 3) {
            error = "invalid players.tsv line " + std::to_string(lineNumber);
            return false;
        }
        PlayerProfile profile;
        if (!parseU64(fields[0], profile.id)) {
            error = "invalid player id";
            return false;
        }
        profile.username = unescapeField(fields[1]);
        if (!parseU64(fields[2], profile.createdAtMs)) {
            error = "invalid player created_at";
            return false;
        }
        players_[profile.id] = profile;
        playerIdByUsername_[normalizeUsername(profile.username)] = profile.id;
        // 推进 ID 水位，保证后续新建 ID 不与历史冲突
        nextPlayerId_ = std::max(nextPlayerId_, profile.id + 1);
    }
    return true;
}

bool FileDataStore::loadSessions(std::string& error) {
    std::ifstream in(pathFor("sessions.tsv"));
    if (!in) {
        return true;
    }
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty()) continue;
        const auto fields = splitTabs(line);
        if (fields.size() != 3) {
            error = "invalid sessions.tsv line " + std::to_string(lineNumber);
            return false;
        }
        SessionRecord session;
        session.playerId = 0;
        if (!parseU64(fields[1], session.playerId) ||
            !parseU64(fields[2], session.createdAtMs)) {
            error = "invalid session record";
            return false;
        }
        sessions_[fields[0]] = session;  // 第 1 列是 token
    }
    return true;
}

bool FileDataStore::loadMatches(std::string& error) {
    std::ifstream in(pathFor("matches.tsv"));
    if (!in) {
        return true;
    }
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty()) continue;
        const auto fields = splitTabs(line);
        if (fields.size() != 4) {
            error = "invalid matches.tsv line " + std::to_string(lineNumber);
            return false;
        }
        MatchRecord record;
        record.matchId = unescapeField(fields[0]);
        record.roomCode = unescapeField(fields[1]);
        if (!parseU64(fields[2], record.endedAtMs) ||
            !parseInt(fields[3], record.playerCount)) {
            error = "invalid match record";
            return false;
        }
        matches_.push_back(record);
        recordedMatchIds_.insert(record.matchId);  // 载入幂等集合
    }
    return true;
}

bool FileDataStore::loadMatchPlayers(std::string& error) {
    std::ifstream in(pathFor("match_players.tsv"));
    if (!in) {
        return true;
    }
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty()) continue;
        const auto fields = splitTabs(line);
        if (fields.size() != 7) {
            error = "invalid match_players.tsv line " +
                    std::to_string(lineNumber);
            return false;
        }
        MatchPlayerRecord record;
        record.matchId = unescapeField(fields[0]);
        if (!parseU64(fields[1], record.playerId) ||
            !parseInt(fields[3], record.rank) ||
            !parseInt(fields[4], record.score) ||
            !parseInt(fields[5], record.kills) ||
            !parseInt(fields[6], record.deaths)) {
            error = "invalid match player record";
            return false;
        }
        record.username = unescapeField(fields[2]);
        matchPlayers_.push_back(record);
    }
    return true;
}

std::optional<LoginResult> FileDataStore::login(
    const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string clean = normalizeUsername(username);
    if (clean.empty() || clean.size() > 24) {
        return std::nullopt;  // 用户名非法（空或超 24 字符）
    }

    PlayerId playerId = kInvalidPlayerId;
    auto existing = playerIdByUsername_.find(clean);
    if (existing == playerIdByUsername_.end()) {
        // 新用户：分配 ID、落盘 players.tsv、写入内存两张表
        PlayerProfile profile;
        profile.id = nextPlayerId_++;
        profile.username = username;
        profile.createdAtMs = nowMillis();
        std::string error;
        if (!appendPlayer(profile, error)) {
            return std::nullopt;  // 磁盘写入失败，登录失败
        }
        players_[profile.id] = profile;
        playerIdByUsername_[clean] = profile.id;
        playerId = profile.id;
    } else {
        // 老用户：直接取已存在的 ID（同名归一化后复用账号）
        playerId = existing->second;
    }

    // 无论新老用户，每次登录都签发一个新的随机 token
    const std::string token = generateToken();
    SessionRecord session;
    session.playerId = playerId;
    session.createdAtMs = nowMillis();
    std::string error;
    if (!appendSession(token, session, error)) {
        return std::nullopt;
    }
    sessions_[token] = session;
    return LoginResult{players_[playerId], token};
}

std::optional<PlayerProfile> FileDataStore::authenticate(
    const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto session = sessions_.find(token);
    if (session == sessions_.end()) {
        return std::nullopt;  // token 不存在
    }
    auto player = players_.find(session->second.playerId);
    if (player == players_.end()) {
        return std::nullopt;  // 会话指向的玩家已不存在（数据异常）
    }
    return player->second;
}

bool FileDataStore::recordMatch(
    const std::string& matchId,
    const std::string& roomCode,
    uint64_t endedAtMs,
    const std::vector<MatchPlayerStanding>& standings) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 幂等保护：空 ID/空排名/重复对局 ID 全部拒绝（防止结算重入导致重复记录）
    if (matchId.empty() || standings.empty() ||
        recordedMatchIds_.find(matchId) != recordedMatchIds_.end()) {
        return false;
    }

    // 1) 追加对局主记录
    MatchRecord match;
    match.matchId = matchId;
    match.roomCode = roomCode;
    match.endedAtMs = endedAtMs;
    match.playerCount = static_cast<int>(standings.size());

    std::string error;
    if (!appendMatch(match, error)) {
        return false;
    }
    // 2) 逐玩家追加明细行
    for (const MatchPlayerStanding& standing : standings) {
        MatchPlayerRecord record;
        record.matchId = matchId;
        record.playerId = standing.playerId;
        record.username = standing.username;
        record.rank = standing.rank;
        record.score = standing.score;
        record.kills = standing.kills;
        record.deaths = standing.deaths;
        if (!appendMatchPlayer(record, error)) {
            return false;
        }
        matchPlayers_.push_back(record);
    }
    matches_.push_back(match);
    recordedMatchIds_.insert(matchId);  // 标记已记录
    return true;
}

std::vector<MatchHistoryEntry> FileDataStore::matchesFor(
    PlayerId playerId, std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MatchHistoryEntry> out;
    // 从扁平明细中筛出该玩家，再关联对局主记录补全房间码/时间等
    for (const MatchPlayerRecord& record : matchPlayers_) {
        if (record.playerId != playerId) {
            continue;
        }
        auto match = std::find_if(
            matches_.begin(), matches_.end(),
            [&](const MatchRecord& item) {
                return item.matchId == record.matchId;
            });
        if (match == matches_.end()) {
            continue;
        }
        MatchHistoryEntry entry;
        entry.matchId = match->matchId;
        entry.roomCode = match->roomCode;
        entry.endedAtMs = match->endedAtMs;
        entry.playerCount = match->playerCount;
        entry.rank = record.rank;
        entry.score = record.score;
        entry.kills = record.kills;
        entry.deaths = record.deaths;
        out.push_back(std::move(entry));
    }
    // 按结束时间倒序（最近的在前）并截断到 limit
    std::sort(out.begin(), out.end(),
              [](const MatchHistoryEntry& a, const MatchHistoryEntry& b) {
                  return a.endedAtMs > b.endedAtMs;
              });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

std::vector<LeaderboardEntry> FileDataStore::leaderboard(
    std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // 以玩家为 key 聚合全部明细
    std::unordered_map<PlayerId, LeaderboardEntry> aggregate;
    for (const MatchPlayerRecord& record : matchPlayers_) {
        LeaderboardEntry& entry = aggregate[record.playerId];
        entry.playerId = record.playerId;
        entry.username = record.username;
        entry.totalScore += record.score;
        entry.games += 1;
        entry.wins += record.rank == 1 ? 1 : 0;  // 第 1 名算胜场
        entry.bestScore = std::max(entry.bestScore, record.score);
    }

    std::vector<LeaderboardEntry> out;
    out.reserve(aggregate.size());
    for (auto& item : aggregate) {
        out.push_back(std::move(item.second));
    }
    // 排序：总分 -> 胜场 -> ID（ID 兜底保证顺序确定）
    std::sort(out.begin(), out.end(),
              [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
                  if (a.totalScore != b.totalScore) {
                      return a.totalScore > b.totalScore;
                  }
                  if (a.wins != b.wins) {
                      return a.wins > b.wins;
                  }
                  return a.playerId < b.playerId;
              });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

StoreStats FileDataStore::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StoreStats stats;
    stats.players = players_.size();
    stats.sessions = sessions_.size();
    stats.matches = matches_.size();
    stats.matchPlayers = matchPlayers_.size();
    return stats;
}

// ------------------------------ 追加写 ------------------------------

bool FileDataStore::appendPlayer(const PlayerProfile& profile,
                                 std::string& error) {
    std::ofstream out(pathFor("players.tsv"), std::ios::app);  // app=只追加
    if (!out) {
        error = "cannot open players.tsv";
        return false;
    }
    out << profile.id << '\t' << escapeField(profile.username) << '\t'
        << profile.createdAtMs << '\n';
    return static_cast<bool>(out);
}

bool FileDataStore::appendSession(const std::string& token,
                                  const SessionRecord& session,
                                  std::string& error) {
    std::ofstream out(pathFor("sessions.tsv"), std::ios::app);
    if (!out) {
        error = "cannot open sessions.tsv";
        return false;
    }
    out << token << '\t' << session.playerId << '\t'
        << session.createdAtMs << '\n';
    return static_cast<bool>(out);
}

bool FileDataStore::appendMatch(const MatchRecord& match,
                                std::string& error) {
    std::ofstream out(pathFor("matches.tsv"), std::ios::app);
    if (!out) {
        error = "cannot open matches.tsv";
        return false;
    }
    out << escapeField(match.matchId) << '\t' << escapeField(match.roomCode)
        << '\t' << match.endedAtMs << '\t' << match.playerCount << '\n';
    return static_cast<bool>(out);
}

bool FileDataStore::appendMatchPlayer(const MatchPlayerRecord& record,
                                      std::string& error) {
    std::ofstream out(pathFor("match_players.tsv"), std::ios::app);
    if (!out) {
        error = "cannot open match_players.tsv";
        return false;
    }
    out << escapeField(record.matchId) << '\t' << record.playerId << '\t'
        << escapeField(record.username) << '\t' << record.rank << '\t'
        << record.score << '\t' << record.kills << '\t'
        << record.deaths << '\n';
    return static_cast<bool>(out);
}

std::string FileDataStore::generateToken() const {
    // thread_local：每线程独立随机引擎，避免多线程争用同一个 RNG
    static thread_local std::mt19937_64 rng(std::random_device{}());
    const uint64_t first = rng();
    const uint64_t second = rng();
    // 两个 64 位数拼成 32 个十六进制字符（128 位熵），几乎不可能碰撞
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << first
        << std::setw(16) << second;
    return out.str();
}

std::string FileDataStore::normalizeUsername(
    const std::string& username) {
    std::string out;
    out.reserve(username.size());
    for (unsigned char c : username) {
        if (c < 0x20u || c == 0x7Fu) {
            continue;  // 丢弃控制字符
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');  // 转小写
        }
        out.push_back(static_cast<char>(c));
    }
    // 去掉首尾空格（注意：先处理字符再 trim）
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

uint64_t FileDataStore::nowMillis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace blob
