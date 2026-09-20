// =============================================================================
//  spatial_grid.h —— 均匀网格空间索引
// =============================================================================
#pragma once

#include <cstddef>
#include <vector>

#include "core/entity.h"
#include "core/vec2.h"

namespace blob {

class SpatialGrid {
public:
    SpatialGrid(float width, float height, float cellSize);

    void rebuild(const std::vector<Entity>& entities);
    void queryCircle(const Vec2& center, float radius,
                     std::vector<EntityId>& out) const;

    std::size_t columns() const { return columns_; }
    std::size_t rows() const { return rows_; }
    float cellSize() const { return cellSize_; }

private:
    struct CellEntry {
        EntityId id = kInvalidEntityId;
        Vec2 pos;
    };

    std::size_t cellIndex(int x, int y) const {
        return static_cast<std::size_t>(y) * columns_ +
               static_cast<std::size_t>(x);
    }
    int clampCellX(int x) const;
    int clampCellY(int y) const;

    float width_;
    float height_;
    float cellSize_;
    std::size_t columns_ = 1;
    std::size_t rows_ = 1;
    std::vector<std::vector<CellEntry>> cells_;
};

}  // namespace blob
