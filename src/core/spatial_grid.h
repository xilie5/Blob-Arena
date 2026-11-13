// =============================================================================
//  spatial_grid.h —— 均匀网格空间索引
//
//  要解决的性能问题：
//    “查询某玩家 450 半径内有哪些实体”若两两比较，复杂度 O(N²)，
//    场上 600+ 实体、30 帧/秒时无法承受。
//
//  思路：
//    把正方形地图按 cellSize 切成均匀格子，每个实体登记进自己所在格；
//    圆形查询时只遍历“圆覆盖到的几格”，再用精确距离二次确认。
//    复杂度从 O(N²) 降到 O(覆盖格子内实体数)。
//
//  使用方式（RoomWorker 每次发快照前）：
//    SpatialGrid grid(世界宽, 世界高, kAoiCellSize);
//    grid.rebuild(world.entities());          // 每帧重建（实体持续移动）
//    grid.queryCircle(center, radius, out);   // 查询视野内实体
// =============================================================================
#pragma once

#include <cstddef>
#include <vector>

#include "core/entity.h"
#include "core/vec2.h"

namespace blob {

class SpatialGrid {
public:
    /// @brief 构造网格
    /// @param width    世界宽度
    /// @param height   世界高度
    /// @param cellSize 单元格边长（项目用 kAoiCellSize=300）
    SpatialGrid(float width, float height, float cellSize);

    /// @brief 用当前全部存活实体重建网格（先清空所有格子再重新登记）
    /// @note 实体每帧都在移动，网格不做增量维护，每帧整体重建最简单且足够快
    void rebuild(const std::vector<Entity>& entities);

    /// @brief 查询圆形区域内的实体 ID
    /// @param center 圆心（玩家自己的位置）
    /// @param radius 半径（AOI 视野 450）
    /// @param[out] out 结果列表（进入时会被清空）
    void queryCircle(const Vec2& center, float radius,
                     std::vector<EntityId>& out) const;

    std::size_t columns() const { return columns_; }  ///< 横向格数
    std::size_t rows() const { return rows_; }        ///< 纵向格数
    float cellSize() const { return cellSize_; }      ///< 格边长

private:
    /// @brief 格子内条目：只存 ID 和查询时的位置快照
    struct CellEntry {
        EntityId id = kInvalidEntityId;
        Vec2 pos;
    };

    /// 二维格子坐标 -> 拉平后一维数组下标（y*列数+x）
    std::size_t cellIndex(int x, int y) const {
        return static_cast<std::size_t>(y) * columns_ +
               static_cast<std::size_t>(x);
    }
    int clampCellX(int x) const;  ///< 横向格子坐标边界夹取
    int clampCellY(int y) const;  ///< 纵向格子坐标边界夹取

    float width_;       ///< 世界宽
    float height_;      ///< 世界高
    float cellSize_;    ///< 单元格边长
    std::size_t columns_ = 1;  ///< 横向格数（ceil(宽/格边长)）
    std::size_t rows_ = 1;     ///< 纵向格数
    std::vector<std::vector<CellEntry>> cells_;  ///< 拉平的格子数组，大小 列*行
};

}  // namespace blob
