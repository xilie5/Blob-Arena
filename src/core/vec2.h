// =============================================================================
//  vec2.h —— 二维向量（头文件实现，零依赖）
//
//  游戏中所有坐标、移动方向、距离判定都基于 Vec2。纯头文件内联实现，
//  不引入任何 .cpp，编译期可内联展开，性能与手写 float[2] 相当。
// =============================================================================
#pragma once

#include <cmath>

namespace blob {

/// @brief 二维浮点向量/坐标点
struct Vec2 {
    float x = 0.0f;  ///< x 分量（横轴坐标/方向）
    float y = 0.0f;  ///< y 分量（纵轴坐标/方向）

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
    Vec2 operator*(float s) const { return Vec2(x * s, y * s); }   // 数乘（限定向量*标量）

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }

    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }

    /// 长度的平方（不开根号）；比较距离大小时用它可省掉一次 sqrt
    float lengthSq() const { return x * x + y * y; }
    /// 向量长度（欧几里得模长）
    float length() const { return std::sqrt(lengthSq()); }

    /// @brief 归一化为单位向量（长度变为 1，方向不变）
    /// @return 单位向量；零向量返回 (0,0)，调用方可用 isZero() 自行判断
    /// @note 阈值 1e-6 防止除零产生 NaN/Inf
    Vec2 normalized() const {
        const float len = length();
        if (len <= 1e-6f) {
            return Vec2(0.0f, 0.0f);
        }
        return Vec2(x / len, y / len);
    }

    /// 是否为（近似）零向量
    bool isZero() const { return lengthSq() <= 1e-12f; }

    /// 到另一点距离的平方（碰撞/视野判定优先使用，避免 sqrt）
    float distanceSq(const Vec2& o) const { return (*this - o).lengthSq(); }
    /// 到另一点的距离
    float distance(const Vec2& o) const { return std::sqrt(distanceSq(o)); }
};

/// @brief 线性插值：t=0 返回 a，t=1 返回 b，中间均匀过渡
inline Vec2 lerp(const Vec2& a, const Vec2& b, float t) {
    return Vec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

/// @brief 把 v 夹取到 [lo, hi] 区间（小于 lo 返回 lo，大于 hi 返回 hi）
inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace blob
