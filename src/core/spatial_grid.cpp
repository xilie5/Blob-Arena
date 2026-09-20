#include "core/spatial_grid.h"

#include <algorithm>
#include <cmath>

namespace blob {

SpatialGrid::SpatialGrid(float width, float height, float cellSize)
    : width_(std::max(width, 1.0f)),
      height_(std::max(height, 1.0f)),
      cellSize_(std::max(cellSize, 1.0f)),
      columns_(static_cast<std::size_t>(
          std::ceil(width_ / cellSize_))),
      rows_(static_cast<std::size_t>(
          std::ceil(height_ / cellSize_))),
      cells_(columns_ * rows_) {}

int SpatialGrid::clampCellX(int x) const {
    if (x < 0) return 0;
    if (x >= static_cast<int>(columns_)) {
        return static_cast<int>(columns_) - 1;
    }
    return x;
}

int SpatialGrid::clampCellY(int y) const {
    if (y < 0) return 0;
    if (y >= static_cast<int>(rows_)) {
        return static_cast<int>(rows_) - 1;
    }
    return y;
}

void SpatialGrid::rebuild(const std::vector<Entity>& entities) {
    for (auto& cell : cells_) {
        cell.clear();
    }
    for (const Entity& entity : entities) {
        if (!entity.alive) continue;
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

    const int minX = clampCellX(
        static_cast<int>((center.x - radius) / cellSize_));
    const int maxX = clampCellX(
        static_cast<int>((center.x + radius) / cellSize_));
    const int minY = clampCellY(
        static_cast<int>((center.y - radius) / cellSize_));
    const int maxY = clampCellY(
        static_cast<int>((center.y + radius) / cellSize_));
    const float radiusSq = radius * radius;

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
