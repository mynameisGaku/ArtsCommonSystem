#pragma once
#include "Mathf.h"

namespace ACSU_Math
{
    struct Vector3;

    struct Vector2
    {
        float x, y;
        Vector2() : x(0.0f), y(0.0f) {}
        Vector2(float _x, float _y) : x(_x), y(_y) {}

        static inline Vector2 zero() { return Vector2(0.0f, 0.0f); }
        static inline Vector2 one() { return Vector2(1.0f, 1.0f); }
        static inline Vector2 up() { return Vector2(0.0f, 1.0f); }
        static inline Vector2 down() { return Vector2(0.0f, -1.0f); }
        static inline Vector2 left() { return Vector2(-1.0f, 0.0f); }
        static inline Vector2 right() { return Vector2(1.0f, 0.0f); }
        static inline Vector2 positiveInfinity() { return Vector2(Mathf::Infinity, Mathf::Infinity); }
        static inline Vector2 negativeInfinity() { return Vector2(Mathf::NegativeInfinity, Mathf::NegativeInfinity); }

        inline float& operator[](int index) { return index == 0 ? x : y; }
        inline const float& operator[](int index) const { return index == 0 ? x : y; }

        inline float magnitude() const { return Mathf::Sqrt(x * x + y * y); }
        inline float sqrMagnitude() const { return x * x + y * y; }
        inline Vector2 normalized() const {
            float mag = magnitude();
            return (mag > 1.0e-5f) ? *this / mag : zero();
        }
        inline void Normalize() {
            float mag = magnitude();
            if (mag > 1.0e-5f) { x /= mag; y /= mag; }
            else { x = y = 0.0f; }
        }
        inline void Scale(const Vector2& scale) { x *= scale.x; y *= scale.y; }

        static inline float Dot(const Vector2& a, const Vector2& b) { return a.x * b.x + a.y * b.y; }

        static inline float Angle(const Vector2& from, const Vector2& to) {
            float denom = Mathf::Sqrt(from.sqrMagnitude() * to.sqrMagnitude());
            if (denom < 1.0e-15f) return 0.0f;
            float dot = Mathf::Clamp(Dot(from, to) / denom, -1.0f, 1.0f);
            return Mathf::Acos(dot) * Mathf::Rad2Deg;
        }

        static inline float SignedAngle(const Vector2& from, const Vector2& to) {
            return Angle(from, to) * Mathf::Sign(from.x * to.y - from.y * to.x);
        }

        static inline Vector2 Perpendicular(const Vector2& inDirection) { return Vector2(-inDirection.y, inDirection.x); }
        static inline Vector2 Lerp(const Vector2& a, const Vector2& b, float t) {
            t = Mathf::Clamp01(t);
            return Vector2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        }
        static inline Vector2 LerpUnclamped(const Vector2& a, const Vector2& b, float t) {
            return Vector2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        }
        static inline Vector2 MoveTowards(const Vector2& current, const Vector2& target, float maxDistanceDelta) {
            Vector2 toVector = target - current;
            float dist = toVector.magnitude();
            if (dist <= maxDistanceDelta || dist == 0.0f) return target;
            return current + toVector / dist * maxDistanceDelta;
        }
        static inline Vector2 ClampMagnitude(const Vector2& vector, float maxLength) {
            float sqrMag = vector.sqrMagnitude();
            if (sqrMag > maxLength * maxLength) return vector.normalized() * maxLength;
            return vector;
        }
        static inline Vector2 Reflect(const Vector2& inDirection, const Vector2& inNormal) {
            return inDirection - 2.0f * Dot(inNormal, inDirection) * inNormal;
        }
        static inline Vector2 SmoothDamp(const Vector2& current, const Vector2& target, Vector2& currentVelocity, float smoothTime, float maxSpeed = Mathf::Infinity, float deltaTime = MATH_DELTATIME()) {
            float vx = currentVelocity.x;
            float vy = currentVelocity.y;
            float ox = Mathf::SmoothDamp(current.x, target.x, vx, smoothTime, maxSpeed, deltaTime);
            float oy = Mathf::SmoothDamp(current.y, target.y, vy, smoothTime, maxSpeed, deltaTime);
            currentVelocity.x = vx; currentVelocity.y = vy;
            return Vector2(ox, oy);
        }

        friend inline bool operator==(const Vector2& lhs, const Vector2& rhs) { return (lhs - rhs).sqrMagnitude() < 9.99999944e-11f; }
        friend inline bool operator!=(const Vector2& lhs, const Vector2& rhs) { return !(lhs == rhs); }
        friend inline Vector2 operator+(const Vector2& a, const Vector2& b) { return Vector2(a.x + b.x, a.y + b.y); }
        friend inline Vector2 operator-(const Vector2& a, const Vector2& b) { return Vector2(a.x - b.x, a.y - b.y); }
        friend inline Vector2 operator-(const Vector2& a) { return Vector2(-a.x, -a.y); }
        friend inline Vector2 operator*(const Vector2& a, float d) { return Vector2(a.x * d, a.y * d); }
        friend inline Vector2 operator*(float d, const Vector2& a) { return Vector2(a.x * d, a.y * d); }
        friend inline Vector2 operator/(const Vector2& a, float d) { return Vector2(a.x / d, a.y / d); }

        explicit Vector2(const Vector3& v);
        operator Vector3() const;
        Vector3 ToVector3() const;
    };
}