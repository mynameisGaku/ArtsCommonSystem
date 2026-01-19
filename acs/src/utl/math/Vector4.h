#pragma once
#include "Mathf.h"

namespace ACSU_Math
{
    struct Vector4
    {
        float x, y, z, w;
        Vector4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
        Vector4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

        static inline Vector4 zero() { return Vector4(0.0f, 0.0f, 0.0f, 0.0f); }
        static inline Vector4 one() { return Vector4(1.0f, 1.0f, 1.0f, 1.0f); }

        inline float& operator[](int index) { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
        inline const float& operator[](int index) const { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }

        inline float magnitude() const { return Mathf::Sqrt(x * x + y * y + z * z + w * w); }
        inline float sqrMagnitude() const { return x * x + y * y + z * z + w * w; }
        inline Vector4 normalized() const { float mag = magnitude(); return (mag > 1.0e-5f) ? *this / mag : zero(); }
        inline void Normalize() { float mag = magnitude(); if (mag > 1.0e-5f) { x /= mag; y /= mag; z /= mag; w /= mag; } else { x = y = z = w = 0.0f; } }

        static inline float Dot(const Vector4& a, const Vector4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
        static inline Vector4 Lerp(const Vector4& a, const Vector4& b, float t) {
            t = Mathf::Clamp01(t);
            return Vector4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        }
        static inline Vector4 LerpUnclamped(const Vector4& a, const Vector4& b, float t) {
            return Vector4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        }

        friend inline bool operator==(const Vector4& lhs, const Vector4& rhs) { return (lhs - rhs).sqrMagnitude() < 9.99999944e-11f; }
        friend inline bool operator!=(const Vector4& lhs, const Vector4& rhs) { return !(lhs == rhs); }
        friend inline Vector4 operator+(const Vector4& a, const Vector4& b) { return Vector4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
        friend inline Vector4 operator-(const Vector4& a, const Vector4& b) { return Vector4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
        friend inline Vector4 operator*(const Vector4& a, float d) { return Vector4(a.x * d, a.y * d, a.z * d, a.w * d); }
        friend inline Vector4 operator*(float d, const Vector4& a) { return Vector4(a.x * d, a.y * d, a.z * d, a.w * d); }
        friend inline Vector4 operator/(const Vector4& a, float d) { return Vector4(a.x / d, a.y / d, a.z / d, a.w / d); }
    };
}