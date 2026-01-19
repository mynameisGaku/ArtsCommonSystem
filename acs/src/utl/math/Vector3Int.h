#pragma once
#include "MathCommon.h"

namespace ACSU_Math
{
    struct Vector3Int
    {
        int x, y, z;
        Vector3Int() : x(0), y(0), z(0) {}
        Vector3Int(int _x, int _y, int _z) : x(_x), y(_y), z(_z) {}

        static inline Vector3Int zero() { return Vector3Int(0, 0, 0); }
        static inline Vector3Int one() { return Vector3Int(1, 1, 1); }

        friend inline Vector3Int operator+(const Vector3Int& a, const Vector3Int& b) { return Vector3Int(a.x + b.x, a.y + b.y, a.z + b.z); }
        friend inline Vector3Int operator-(const Vector3Int& a, const Vector3Int& b) { return Vector3Int(a.x - b.x, a.y - b.y, a.z - b.z); }
    };
}