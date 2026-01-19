#pragma once
#include "Mathf.h"

namespace ACSU_Math
{
    struct Vector2;

    struct Vector3
    {
        float x, y, z;
        Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
        Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

        static inline Vector3 zero() { return Vector3(0.0f, 0.0f, 0.0f); }
        static inline Vector3 one() { return Vector3(1.0f, 1.0f, 1.0f); }
        static inline Vector3 up() { return Vector3(0.0f, 1.0f, 0.0f); }
        static inline Vector3 down() { return Vector3(0.0f, -1.0f, 0.0f); }
        static inline Vector3 left() { return Vector3(-1.0f, 0.0f, 0.0f); }
        static inline Vector3 right() { return Vector3(1.0f, 0.0f, 0.0f); }
        static inline Vector3 forward() { return Vector3(0.0f, 0.0f, 1.0f); }
        static inline Vector3 back() { return Vector3(0.0f, 0.0f, -1.0f); }
        static inline Vector3 positiveInfinity() { return Vector3(Mathf::Infinity, Mathf::Infinity, Mathf::Infinity); }
        static inline Vector3 negativeInfinity() { return Vector3(Mathf::NegativeInfinity, Mathf::NegativeInfinity, Mathf::NegativeInfinity); }

        inline float& operator[](int index) { return index == 0 ? x : (index == 1 ? y : z); }
        inline const float& operator[](int index) const { return index == 0 ? x : (index == 1 ? y : z); }

        inline float magnitude() const { return Mathf::Sqrt(x * x + y * y + z * z); }
        inline float sqrMagnitude() const { return x * x + y * y + z * z; }
        inline Vector3 normalized() const { float mag = magnitude(); return (mag > 1.0e-5f) ? *this / mag : zero(); }
        inline void Normalize() { float mag = magnitude(); if (mag > 1.0e-5f) { x /= mag; y /= mag; z /= mag; } else { x = y = z = 0.0f; } }
        inline void Scale(const Vector3& scale) { x *= scale.x; y *= scale.y; z *= scale.z; }

        static inline float Dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        static inline Vector3 Cross(const Vector3& lhs, const Vector3& rhs) { return Vector3(lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x); }
        static inline float Angle(const Vector3& from, const Vector3& to) {
            float denom = Mathf::Sqrt(from.sqrMagnitude() * to.sqrMagnitude());
            if (denom < 1.0e-15f) return 0.0f;
            float dot = Mathf::Clamp(Dot(from, to) / denom, -1.0f, 1.0f);
            return Mathf::Acos(dot) * Mathf::Rad2Deg;
        }
        static inline float SignedAngle(const Vector3& from, const Vector3& to, const Vector3& axis) {
            Vector3 c = Cross(from, to);
            float angle = Angle(from, to);
            return angle * Mathf::Sign(Dot(axis, c));
        }

        static inline Vector3 Lerp(const Vector3& a, const Vector3& b, float t) {
            t = Mathf::Clamp01(t);
            return Vector3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
        }
        static inline Vector3 LerpUnclamped(const Vector3& a, const Vector3& b, float t) {
            return Vector3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
        }
        static inline Vector3 MoveTowards(const Vector3& current, const Vector3& target, float maxDistanceDelta) {
            Vector3 toVector = target - current;
            float dist = toVector.magnitude();
            if (dist <= maxDistanceDelta || dist == 0.0f) return target;
            return current + toVector / dist * maxDistanceDelta;
        }
        static inline Vector3 ClampMagnitude(const Vector3& vector, float maxLength) {
            float sqrMag = vector.sqrMagnitude();
            if (sqrMag > maxLength * maxLength) return vector.normalized() * maxLength;
            return vector;
        }
        static inline Vector3 Project(const Vector3& vector, const Vector3& onNormal) {
            float sqrMag = Dot(onNormal, onNormal);
            if (sqrMag < Mathf::Epsilon) return zero();
            return onNormal * (Dot(vector, onNormal) / sqrMag);
        }
        static inline Vector3 ProjectOnPlane(const Vector3& vector, const Vector3& planeNormal) { return vector - Project(vector, planeNormal); }
        static inline Vector3 Reflect(const Vector3& inDirection, const Vector3& inNormal) { return inDirection - 2.0f * Dot(inNormal, inDirection) * inNormal; }

        static inline Vector3 RotateTowards(const Vector3& current, const Vector3& target, float maxRadiansDelta, float maxMagnitudeDelta);
        static inline Vector3 SmoothDamp(const Vector3& current, const Vector3& target, Vector3& currentVelocity, float smoothTime, float maxSpeed = Mathf::Infinity, float deltaTime = MATH_DELTATIME()) {
            float vx = currentVelocity.x; float vy = currentVelocity.y; float vz = currentVelocity.z;
            float ox = Mathf::SmoothDamp(current.x, target.x, vx, smoothTime, maxSpeed, deltaTime);
            float oy = Mathf::SmoothDamp(current.y, target.y, vy, smoothTime, maxSpeed, deltaTime);
            float oz = Mathf::SmoothDamp(current.z, target.z, vz, smoothTime, maxSpeed, deltaTime);
            currentVelocity.x = vx; currentVelocity.y = vy; currentVelocity.z = vz;
            return Vector3(ox, oy, oz);
        }

        Matrix4x4 ToTranslateMatrix() const;
        Matrix4x4 ToScalingMatrix() const;
        Matrix4x4 ToRotationMatrix() const;

        friend inline bool operator==(const Vector3& lhs, const Vector3& rhs) { return (lhs - rhs).sqrMagnitude() < 9.99999944e-11f; }
        friend inline bool operator!=(const Vector3& lhs, const Vector3& rhs) { return !(lhs == rhs); }
        friend inline Vector3 operator+(const Vector3& a, const Vector3& b) { return Vector3(a.x + b.x, a.y + b.y, a.z + b.z); }
        friend inline Vector3 operator-(const Vector3& a, const Vector3& b) { return Vector3(a.x - b.x, a.y - b.y, a.z - b.z); }
        friend inline Vector3 operator-(const Vector3& a) { return Vector3(-a.x, -a.y, -a.z); }
        friend inline Vector3 operator*(const Vector3& a, float d) { return Vector3(a.x * d, a.y * d, a.z * d); }
        friend inline Vector3 operator*(float d, const Vector3& a) { return Vector3(a.x * d, a.y * d, a.z * d); }
        friend inline Vector3 operator/(const Vector3& a, float d) { return Vector3(a.x / d, a.y / d, a.z / d); }

        explicit Vector3(const Vector2& v);
        operator Vector2() const;
        Vector2 ToVector2() const;

        operator DxLib::VECTOR() const
        {
            DxLib::VECTOR vec;
            vec.x = x; vec.y = y; vec.z = z;
            return vec;
        }

        // Vector3 v = dxVec;
        Vector3(const DxLib::VECTOR& vec) : x(vec.x), y(vec.y), z(vec.z) {}
    };
}