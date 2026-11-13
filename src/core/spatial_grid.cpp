#include "core/spatial_grid.h"

#include <algorithm>
#include <cmath>

namespace blob {

SpatialGrid::SpatialGrid(float width, float height, float cellSize)
    : width_(std::max(width, 1.0f)),       // 防御非法（<=0）尺寸
      height_(std::max(height, 1.0f)),
      cellSize_(std::max(cellSize, 1.0f)),
      columns_(static_cast<std::size_t>(
          std::ceil(width_ / cellSize_))), // 列数向上取整，覆盖世界右/上边缘
      rows_(static_cast<std::size_t>(
          std::ceil(height_ / cellSize_))),
      cells_(columns_ * rows_) {}          // 一次性分配全部格子

/// 横向格子坐标夹取到 [0, columns_-1]，防止实体在边界时算出越界下标
int SpatialGrid::clampCellX(int x) const {
    if (x < 0) return 0;
    if (x >= static_cast<int>(columns_)) {
        return static_cast<int>(columns_) - 1;
    }
    return x;
}

/// 纵向格子坐标夹取到 [0, rows_-1]
int SpatialGrid::clampCellY(int y) const {
    if (y < 0) return 0;
    if (y >= static_cast<int>(rows_)) {
        return static_cast<int>(rows_) - 1;
    }
    return y;
}

void SpatialGrid::rebuild(const std::vector<Entity>& entities) {
    // 1) 清空所有格子（保留容量，只清内容，避免反复释放/申请内存）
    for (auto& cell : cells_) {
        cell.clear();
    }
    // 2) 逐个登记存活实体到其位置所在的格子
    for (const Entity& entity : entities) {
        if (!entity.alive) continue;
        // 连续坐标除以格边长取整即格子坐标，再做边界夹取
        const int x = clampCellX(
            static_cast<int>(entity.pos.x / cellSize_));
        const int y = clampCellY(
            static_cast<int>(entity.pos.y / cellSize_));
        cells_[cellIndex(x, y)].push_back(CellEntry{entity.id, entity.pos});
    }
}

void SpatialGrid::queryCircle(const Vec2& center, float radius,
                              std::vector<EntityId>& out) const {
    out.clear();
    if (radius <= 0.0f) return;

    // 1) 由圆的外接矩形算出覆盖到的格子范围（4 个角分别换算并夹取）
    const int minX = clampCellX(
        static_cast<int>((center.x - radius) / cellSize_));
    const int maxX = clampCellX(
        static_cast<int>((center.x + radius) / cellSize_));
    const int minY = clampCellY(
        static_cast<int>((center.y - radius) / cellSize_));
    const int maxY = clampCellY(
        static_cast<int>((center.y + radius) / cellSize_));
    const float radiusSq = radius * radius;  // 用平方距离判定，避免 sqrt

    // 2) 只遍历覆盖格子内的条目，再做精确圆形判定
    //    （格子是方形粗筛，角落条目可能在圆外，必须二次确认）
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            for (const CellEntry& entry : cells_[cellIndex(x, y)]) {
                if (entry.pos.distanceSq(center) <= radiusSq) {
                    out.push_back(entry.id);
                }
            }
        }
    }
}

}  // namespace blob
