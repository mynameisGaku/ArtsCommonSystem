#pragma once
#include "Mathf.h"
#include "Vector3.h"

namespace ACSU_Math
{
    struct Matrix4x4; // 前方宣言

    struct Quaternion
    {
        float x, y, z, w;

        Quaternion() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}
        Quaternion(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

        static inline Quaternion identity() { return Quaternion(0.0f, 0.0f, 0.0f, 1.0f); }

        static inline float Dot(const Quaternion& a, const Quaternion& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }

        inline float magnitude() const { return Mathf::Sqrt(x * x + y * y + z * z + w * w); }

        inline Quaternion normalized() const {
            float mag = magnitude();
            if (mag > 1.0e-5f) return Quaternion(x / mag, y / mag, z / mag, w / mag);
            return identity();
        }
        static Quaternion Euler(float x, float y, float z);
        static inline Quaternion Euler(const Vector3& euler) { return Euler(euler.x, euler.y, euler.z); }
        static Quaternion AngleAxis(float angle, const Vector3& axis);
        static Quaternion LookRotation(const Vector3& forward, const Vector3& upwards = Vector3::up());
        static Quaternion FromToRotation(const Vector3& fromDirection, const Vector3& toDirection);
        static Quaternion SlerpUnclamped(const Quaternion& a, const Quaternion& b, float t);
        static inline Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t) {
            float dot = Dot(a, b);
            if (dot > 0.9995f) return SlerpUnclamped(a, b, t);
            return SlerpUnclamped(a, b, t);
        }

        static inline Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t) {
            t = Mathf::Clamp01(t);
            return Slerp(a, b, t);
        }
        static Quaternion Inverse(const Quaternion& rotation);
        Matrix4x4 ToMatrix() const;

        friend inline bool operator==(const Quaternion& lhs, const Quaternion& rhs) { return Dot(lhs, rhs) > 0.999999f; }
        friend inline bool operator!=(const Quaternion& lhs, const Quaternion& rhs) { return !(lhs == rhs); }

        friend inline Quaternion operator*(const Quaternion& a, const Quaternion& b) {
            return Quaternion(
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
            );
        }

        friend inline Vector3 operator*(const Quaternion& rotation, const Vector3& point) {
            float num = rotation.x * 2.0f; float num2 = rotation.y * 2.0f; float num3 = rotation.z * 2.0f;
            float num4 = rotation.x * num; float num5 = rotation.y * num2; float num6 = rotation.z * num3;
            float num7 = rotation.x * num2; float num8 = rotation.x * num3; float num9 = rotation.y * num3;
            float num10 = rotation.w * num; float num11 = rotation.w * num2; float num12 = rotation.w * num3;
            Vector3 result;
            result.x = (1.0f - (num5 + num6)) * point.x + (num7 - num12) * point.y + (num8 + num11) * point.z;
            result.y = (num7 + num12) * point.x + (1.0f - (num4 + num6)) * point.y + (num9 - num10) * point.z;
            result.z = (num8 - num11) * point.x + (num9 + num10) * point.y + (1.0f - (num4 + num5)) * point.z;
            return result;
        }
    };
}