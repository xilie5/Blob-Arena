// =============================================================================
//  lobby.h —— 公共匹配房间与私有码房注册表
//
//  Lobby 拥有全部 Room 的生命周期，并提供三种进房方式：
//    quickMatch()      快速匹配：塞进未满的公共房，没有就新建公共房；
//    createPrivateRoom() 创建私有房：返回一个 4 位房间码；
//    joinByCode(code)  凭房间码加入（也被 Worker 内部用来按码取房间）。
//  空房间由 pruneEmptyRooms() 及时销毁以释放整份世界内存。
//
//  线程归属：仅在 RoomWorker 线程被访问，内部无锁；持有 unique_ptr 不可拷贝。
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/room.h"

namespace blob {

class Lobby {
public:
    /// @brief 构造大厅（新建房间时使用的世界尺寸/对局参数在此统一配置）
    Lobby(uint32_t seed, float worldWidth, float worldHeight,
          int countdownSec = cfg::kArenaCountdownSec,
          int durationSec = cfg::kArenaDurationSec,
          int shrinkStartSec = cfg::kArenaShrinkStartSec);
    ~Lobby();

    Lobby(const Lobby&) = delete;
    Lobby& operator=(const Lobby&) = delete;

    /// 快速匹配：优先加入尚未开局的公共房，否则创建新房。
    Room* quickMatch();

    /// 创建私有码房，返回新房码（失败返回空串）。
    std::string createPrivateRoom();

    /// 凭码查房间；不存在返回 nullptr。
    Room* joinByCode(const std::string& code);
    /// 销毁所有没有真人玩家的空房间。
    void pruneEmptyRooms();

    const std::vector<Room*>& rooms() const { return roomList_; } ///< 全部房间（遍历用）
    std::size_t codeRoomCount() const { return rooms_.size(); }  ///< 房间总数

private:
    Room* createRoom(bool publicMatch);  ///< 内部建房间（生成码+种子+入表）
    std::string generateUniqueCode();    ///< 生成不冲突的 4 位房间码
    uint32_t nextRng();                  ///< 大厅自己的 xorshift 随机数
    void rebuildRoomList();              ///< 由 map 重建裸指针列表

    uint32_t seed_;            ///< 大厅种子（随机数初始化用）
    float worldWidth_;         ///< 新房世界宽
    float worldHeight_;        ///< 新房世界高
    int countdownSec_;         ///< 新房倒计时
    int durationSec_;          ///< 新房时长
    int shrinkStartSec_;       ///< 新房缩圈开始时刻
    uint32_t rngState_;        ///< 随机流状态
    /// 房间码 -> 房间（unique_ptr 拥有所有权）
    std::unordered_map<std::string, std::unique_ptr<Room>> rooms_;
    /// 房间裸指针列表：每帧要遍历全部房间，用 vector 比遍历 hashmap 更直观高效
    std::vector<Room*> roomList_;
};

}  // namespace blob
