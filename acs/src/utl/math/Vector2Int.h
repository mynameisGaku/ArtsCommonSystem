#pragma once
#include "MathCommon.h"

namespace ACSU_Math
{
    struct Vector2Int
    {
        int x, y;
        Vector2Int() : x(0), y(0) {}
        Vector2Int(int _x, int _y) : x(_x), y(_y) {}

        static inline Vector2Int zero() { return Vector2Int(0, 0); }
        static inline Vector2Int one() { return Vector2Int(1, 1); }

        friend inline Vector2Int operator+(const Vector2Int& a, const Vector2Int& b) { return Vector2Int(a.x + b.x, a.y + b.y); }
        friend inline Vector2Int operator-(const Vector2Int& a, const Vector2Int& b) { return Vector2Int(a.x - b.x, a.y - b.y); }
    };
}