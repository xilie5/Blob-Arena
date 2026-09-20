// =============================================================================
//  vec2.h —— 二维向量（头文件实现，零依赖）
// =============================================================================
#pragma once

#include <cmath>

namespace blob {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
    Vec2 operator*(float s) const { return Vec2(x * s, y * s); }

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }

    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }

    float lengthSq() const { return x * x + y * y; }
    float length() const { return std::sqrt(lengthSq()); }

    // 归一化；零向量返回 (0,0)，调用方需自行判断
    Vec2 normalized() const {
        const float len = length();
        if (len <= 1e-6f) {
            return Vec2(0.0f, 0.0f);
        }
        return Vec2(x / len, y / len);
    }

    bool isZero() const { return lengthSq() <= 1e-12f; }

    float distanceSq(const Vec2& o) const { return (*this - o).lengthSq(); }
    float distance(const Vec2& o) const { return std::sqrt(distanceSq(o)); }
};

inline Vec2 lerp(const Vec2& a, const Vec2& b, float t) {
    return Vec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace blob
