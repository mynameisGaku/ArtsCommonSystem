#pragma once

#include <Pch.h>

#ifndef MATH_DELTATIME
#define MATH_DELTATIME() (1.0f / 60.0f)
#endif

namespace ACSU_Math
{
    struct Mathf
    {
        static constexpr float PI = 3.14159265358979323846f;
        static constexpr float Deg2Rad = PI / 180.0f;
        static constexpr float Rad2Deg = 180.0f / PI;

        static constexpr float Infinity = std::numeric_limits<float>::infinity();
        static constexpr float NegativeInfinity = -std::numeric_limits<float>::infinity();

        static constexpr float Epsilon = 1.4012984643248170709237295832899e-45f;

        static inline float Abs(float v)
        {
            return std::fabs(v);
        }

        static inline int Abs(int v)
        {
            return v < 0 ? -v : v;
        }

        static inline float Min(float a, float b)
        {
            return std::fmin(a, b);
        }

        static inline float Max(float a, float b)
        {
            return std::fmax(a, b);
        }

        static inline int Min(int a, int b)
        {
            return a < b ? a : b;
        }

        static inline int Max(int a, int b)
        {
            return a > b ? a : b;
        }

        static inline float Sign(float v)
        {
            return v >= 0.0f ? 1.0f : -1.0f;
        }

        static inline float Clamp(float value, float min, float max)
        {
            return std::fmin(std::fmax(value, min), max);
        }

        static inline int Clamp(int value, int min, int max)
        {
            return value < min ? min : (value > max ? max : value);
        }

        static inline float Clamp01(float value)
        {
            return Clamp(value, 0.0f, 1.0f);
        }

        static inline float Lerp(float a, float b, float t)
        {
            t = Clamp01(t);
            return a + (b - a) * t;
        }

        static inline float LerpUnclamped(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        static inline float InverseLerp(float a, float b, float value)
        {
            if (a != b)
            {
                return Clamp01((value - a) / (b - a));
            }
            return 0.0f;
        }

        static inline float SmoothStep(float from, float to, float t)
        {
            t = Clamp01(t);
            t = t * t * (3.0f - 2.0f * t);
            return to * t + from * (1.0f - t);
        }

        static inline float Repeat(float t, float length)
        {
            return t - std::floor(t / length) * length;
        }

        static inline float PingPong(float t, float length)
        {
            t = Repeat(t, length * 2.0f);
            return length - Abs(t - length);
        }

        static inline float DeltaAngle(float current, float target)
        {
            float delta = Repeat((target - current), 360.0f);
            if (delta > 180.0f)
            {
                delta -= 360.0f;
            }
            return delta;
        }

        static inline float MoveTowards(float current, float target, float maxDelta)
        {
            if (Abs(target - current) <= maxDelta)
            {
                return target;
            }
            return current + Sign(target - current) * maxDelta;
        }

        static inline float MoveTowardsAngle(float current, float target, float maxDelta)
        {
            float delta = DeltaAngle(current, target);
            if (-maxDelta < delta && delta < maxDelta)
            {
                return target;
            }
            target = current + delta;
            return MoveTowards(current, target, maxDelta);
        }

        static inline bool Approximately(float a, float b)
        {
            return Abs(b - a) < Max(1.0e-6f * Max(Abs(a), Abs(b)), Epsilon * 8.0f);
        }

        static inline float Sin(float v)
        {
            return std::sinf(v);
        }

        static inline float Cos(float v)
        {
            return std::cosf(v);
        }

        static inline float Tan(float v)
        {
            return std::tanf(v);
        }

        static inline float Asin(float v)
        {
            return std::asinf(v);
        }

        static inline float Acos(float v)
        {
            return std::acosf(v);
        }

        static inline float Atan(float v)
        {
            return std::atanf(v);
        }

        static inline float Atan2(float y, float x)
        {
            return std::atan2f(y, x);
        }

        static inline float Sqrt(float v)
        {
            return std::sqrtf(v);
        }

        static inline float Pow(float f, float p)
        {
            return std::powf(f, p);
        }

        static inline float Exp(float power)
        {
            return std::expf(power);
        }

        static inline float Log(float f)
        {
            return std::logf(f);
        }

        static inline float Log10(float f)
        {
            return std::log10f(f);
        }

        static inline float Floor(float f)
        {
            return std::floorf(f);
        }

        static inline float Ceil(float f)
        {
            return std::ceilf(f);
        }

        static inline float Round(float f)
        {
            return std::roundf(f);
        }

        static inline int FloorToInt(float f)
        {
            return static_cast<int>(std::floorf(f));
        }

        static inline int CeilToInt(float f)
        {
            return static_cast<int>(std::ceilf(f));
        }

        static inline int RoundToInt(float f)
        {
            return static_cast<int>(std::roundf(f));
        }

        static inline float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed, float deltaTime)
        {
            smoothTime = Max(0.0001f, smoothTime);
            float omega = 2.0f / smoothTime;

            float x = omega * deltaTime;
            float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

            float change = current - target;
            float originalTo = target;

            float maxChange = maxSpeed * smoothTime;
            change = Clamp(change, -maxChange, maxChange);
            target = current - change;

            float temp = (currentVelocity + omega * change) * deltaTime;
            currentVelocity = (currentVelocity - omega * temp) * exp;

            float output = target + (change + temp) * exp;

            if ((originalTo - current > 0.0f) == (output > originalTo))
            {
                output = originalTo;
                currentVelocity = (output - originalTo) / deltaTime;
            }

            return output;
        }

        static inline float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed)
        {
            float deltaTime = MATH_DELTATIME();
            return SmoothDamp(current, target, currentVelocity, smoothTime, maxSpeed, deltaTime);
        }

        static inline float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime)
        {
            return SmoothDamp(current, target, currentVelocity, smoothTime, Infinity, MATH_DELTATIME());
        }

        static inline float SmoothDampAngle(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed, float deltaTime)
        {
            target = current + DeltaAngle(current, target);
            return SmoothDamp(current, target, currentVelocity, smoothTime, maxSpeed, deltaTime);
        }

        static inline float SmoothDampAngle(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed)
        {
            return SmoothDampAngle(current, target, currentVelocity, smoothTime, maxSpeed, MATH_DELTATIME());
        }

        static inline float SmoothDampAngle(float current, float target, float& currentVelocity, float smoothTime)
        {
            return SmoothDampAngle(current, target, currentVelocity, smoothTime, Infinity, MATH_DELTATIME());
        }

        static inline float LerpAngle(float a, float b, float t)
        {
            float delta = Repeat((b - a), 360.0f);
            if (delta > 180.0f)
            {
                delta -= 360.0f;
            }
            return a + delta * Clamp01(t);
        }
    };

    struct Vector2
    {
        float x;
        float y;

        Vector2()
            : x(0.0f), y(0.0f)
        {
        }

        Vector2(float _x, float _y)
            : x(_x), y(_y)
        {
        }

        static inline Vector2 zero()
        {
            return Vector2(0.0f, 0.0f);
        }

        static inline Vector2 one()
        {
            return Vector2(1.0f, 1.0f);
        }

        static inline Vector2 up()
        {
            return Vector2(0.0f, 1.0f);
        }

        static inline Vector2 down()
        {
            return Vector2(0.0f, -1.0f);
        }

        static inline Vector2 left()
        {
            return Vector2(-1.0f, 0.0f);
        }

        static inline Vector2 right()
        {
            return Vector2(1.0f, 0.0f);
        }

        static inline Vector2 positiveInfinity()
        {
            return Vector2(Mathf::Infinity, Mathf::Infinity);
        }

        static inline Vector2 negativeInfinity()
        {
            return Vector2(Mathf::NegativeInfinity, Mathf::NegativeInfinity);
        }

        inline float& operator[](int index)
        {
            return index == 0 ? x : y;
        }

        inline const float& operator[](int index) const
        {
            return index == 0 ? x : y;
        }

        inline float magnitude() const
        {
            return Mathf::Sqrt(x * x + y * y);
        }

        inline float sqrMagnitude() const
        {
            return x * x + y * y;
        }

        inline Vector2 normalized() const
        {
            float mag = magnitude();
            if (mag > 1.0e-5f)
            {
                return *this / mag;
            }
            return zero();
        }

        inline void Normalize()
        {
            float mag = magnitude();
            if (mag > 1.0e-5f)
            {
                x /= mag;
                y /= mag;
            }
            else
            {
                x = 0.0f;
                y = 0.0f;
            }
        }

        inline void Scale(const Vector2& scale)
        {
            x *= scale.x;
            y *= scale.y;
        }

        static inline float Dot(const Vector2& a, const Vector2& b)
        {
            return a.x * b.x + a.y * b.y;
        }

        static inline float Angle(const Vector2& from, const Vector2& to)
        {
            float denom = Mathf::Sqrt(from.sqrMagnitude() * to.sqrMagnitude());
            if (denom < 1.0e-15f)
            {
                return 0.0f;
            }
            float dot = Dot(from, to) / denom;
            dot = Mathf::Clamp(dot, -1.0f, 1.0f);
            return Mathf::Acos(dot) * Mathf::Rad2Deg;
        }

        static inline float SignedAngle(const Vector2& from, const Vector2& to)
        {
            float unsignedAngle = Angle(from, to);
            float sign = Mathf::Sign(from.x * to.y - from.y * to.x);
            return unsignedAngle * sign;
        }

        static inline Vector2 Perpendicular(const Vector2& inDirection)
        {
            return Vector2(-inDirection.y, inDirection.x);
        }

        static inline Vector2 Lerp(const Vector2& a, const Vector2& b, float t)
        {
            t = Mathf::Clamp01(t);
            return Vector2(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t
            );
        }

        static inline Vector2 LerpUnclamped(const Vector2& a, const Vector2& b, float t)
        {
            return Vector2(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t
            );
        }

        static inline Vector2 MoveTowards(const Vector2& current, const Vector2& target, float maxDistanceDelta)
        {
            Vector2 toVector = target - current;
            float dist = toVector.magnitude();
            if (dist <= maxDistanceDelta || dist == 0.0f)
            {
                return target;
            }
            return current + toVector / dist * maxDistanceDelta;
        }

        static inline Vector2 ClampMagnitude(const Vector2& vector, float maxLength)
        {
            float sqrMag = vector.sqrMagnitude();
            float maxSqr = maxLength * maxLength;
            if (sqrMag > maxSqr)
            {
                float mag = Mathf::Sqrt(sqrMag);
                return vector / mag * maxLength;
            }
            return vector;
        }

        static inline Vector2 Reflect(const Vector2& inDirection, const Vector2& inNormal)
        {
            return inDirection - 2.0f * Dot(inNormal, inDirection) * inNormal;
        }

        static inline Vector2 SmoothDamp(const Vector2& current, const Vector2& target, Vector2& currentVelocity, float smoothTime, float maxSpeed, float deltaTime)
        {
            float vx = currentVelocity.x;
            float vy = currentVelocity.y;

            float ox = Mathf::SmoothDamp(current.x, target.x, vx, smoothTime, maxSpeed, deltaTime);
            float oy = Mathf::SmoothDamp(current.y, target.y, vy, smoothTime, maxSpeed, deltaTime);

            currentVelocity.x = vx;
            currentVelocity.y = vy;

            return Vector2(ox, oy);
        }

        static inline Vector2 SmoothDamp(const Vector2& current, const Vector2& target, Vector2& currentVelocity, float smoothTime, float maxSpeed)
        {
            return SmoothDamp(current, target, currentVelocity, smoothTime, maxSpeed, MATH_DELTATIME());
        }

        static inline Vector2 SmoothDamp(const Vector2& current, const Vector2& target, Vector2& currentVelocity, float smoothTime)
        {
            return SmoothDamp(current, target, currentVelocity, smoothTime, Mathf::Infinity, MATH_DELTATIME());
        }

        friend inline bool operator==(const Vector2& lhs, const Vector2& rhs)
        {
            Vector2 diff = lhs - rhs;
            return diff.sqrMagnitude() < 9.99999944e-11f;
        }

        friend inline bool operator!=(const Vector2& lhs, const Vector2& rhs)
        {
            return !(lhs == rhs);
        }

        friend inline Vector2 operator+(const Vector2& a, const Vector2& b)
        {
            return Vector2(a.x + b.x, a.y + b.y);
        }

        friend inline Vector2 operator-(const Vector2& a, const Vector2& b)
        {
            return Vector2(a.x - b.x, a.y - b.y);
        }

        friend inline Vector2 operator-(const Vector2& a)
        {
            return Vector2(-a.x, -a.y);
        }

        friend inline Vector2 operator*(const Vector2& a, float d)
        {
            return Vector2(a.x * d, a.y * d);
        }

        friend inline Vector2 operator*(float d, const Vector2& a)
        {
            return Vector2(a.x * d, a.y * d);
        }

        friend inline Vector2 operator/(const Vector2& a, float d)
        {
            return Vector2(a.x / d, a.y / d);
        }
    };

    struct Vector3
    {
        float x;
        float y;
        float z;

        Vector3()
            : x(0.0f), y(0.0f), z(0.0f)
        {
        }

        Vector3(float _x, float _y, float _z)
            : x(_x), y(_y), z(_z)
        {
        }

        static inline Vector3 zero()
        {
            return Vector3(0.0f, 0.0f, 0.0f);
        }

        static inline Vector3 one()
        {
            return Vector3(1.0f, 1.0f, 1.0f);
        }

        static inline Vector3 up()
        {
            return Vector3(0.0f, 1.0f, 0.0f);
        }

        static inline Vector3 down()
        {
            return Vector3(0.0f, -1.0f, 0.0f);
        }

        static inline Vector3 left()
        {
            return Vector3(-1.0f, 0.0f, 0.0f);
        }

        static inline Vector3 right()
        {
            return Vector3(1.0f, 0.0f, 0.0f);
        }

        static inline Vector3 forward()
        {
            return Vector3(0.0f, 0.0f, 1.0f);
        }

        static inline Vector3 back()
        {
            return Vector3(0.0f, 0.0f, -1.0f);
        }

        static inline Vector3 positiveInfinity()
        {
            return Vector3(Mathf::Infinity, Mathf::Infinity, Mathf::Infinity);
        }

        static inline Vector3 negativeInfinity()
        {
            return Vector3(Mathf::NegativeInfinity, Mathf::NegativeInfinity, Mathf::NegativeInfinity);
        }

        inline float& operator[](int index)
        {
            if (index == 0)
            {
                return x;
            }
            if (index == 1)
            {
                return y;
            }
            return z;
        }

        inline const float& operator[](int index) const
        {
            if (index == 0)
            {
                return x;
            }
            if (index == 1)
            {
                return y;
            }
            return z;
        }

        inline float magnitude() const
        {
            return Mathf::Sqrt(x * x + y * y + z * z);
        }

        inline float sqrMagnitude() const
        {
            return x * x + y * y + z * z;
        }

        inline Vector3 normalized() const
        {
            float mag = magnitude();
            if (mag > 1.0e-5f)
            {
                return *this / mag;
            }
            return zero();
        }

        inline void Normalize()
        {
            float mag = magnitude();
            if (mag > 1.0e-5f)
            {
                x /= mag;
                y /= mag;
                z /= mag;
            }
            else
            {
                x = 0.0f;
                y = 0.0f;
                z = 0.0f;
            }
        }

        inline void Scale(const Vector3& scale)
        {
            x *= scale.x;
            y *= scale.y;
            z *= scale.z;
        }

        static inline float Dot(const Vector3& a, const Vector3& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        static inline Vector3 Cross(const Vector3& lhs, const Vector3& rhs)
        {
            return Vector3(
                lhs.y * rhs.z - lhs.z * rhs.y,
                lhs.z * rhs.x - lhs.x * rhs.z,
                lhs.x * rhs.y - lhs.y * rhs.x
            );
        }

        static inline float Angle(const Vector3& from, const Vector3& to)
        {
            float denom = Mathf::Sqrt(from.sqrMagnitude() * to.sqrMagnitude());
            if (denom < 1.0e-15f)
            {
                return 0.0f;
            }
            float dot = Dot(from, to) / denom;
            dot = Mathf::Clamp(dot, -1.0f, 1.0f);
            return Mathf::Acos(dot) * Mathf::Rad2Deg;
        }

        static inline float SignedAngle(const Vector3& from, const Vector3& to, const Vector3& axis)
        {
            Vector3 c = Cross(from, to);
            float angle = Angle(from, to);
            float sign = Mathf::Sign(Dot(axis, c));
            return angle * sign;
        }

        static inline Vector3 Lerp(const Vector3& a, const Vector3& b, float t)
        {
            t = Mathf::Clamp01(t);
            return Vector3(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t
            );
        }

        static inline Vector3 LerpUnclamped(const Vector3& a, const Vector3& b, float t)
        {
            return Vector3(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t
            );
        }

        static inline Vector3 MoveTowards(const Vector3& current, const Vector3& target, float maxDistanceDelta)
        {
            Vector3 toVector = target - current;
            float dist = toVector.magnitude();
            if (dist <= maxDistanceDelta || dist == 0.0f)
            {
                return target;
            }
            return current + toVector / dist * maxDistanceDelta;
        }

        static inline Vector3 ClampMagnitude(const Vector3& vector, float maxLength)
        {
            float sqrMag = vector.sqrMagnitude();
            float maxSqr = maxLength * maxLength;
            if (sqrMag > maxSqr)
            {
                float mag = Mathf::Sqrt(sqrMag);
                return vector / mag * maxLength;
            }
            return vector;
        }

        static inline Vector3 Project(const Vector3& vector, const Vector3& onNormal)
        {
            float sqrMag = Dot(onNormal, onNormal);
            if (sqrMag < Mathf::Epsilon)
            {
                return zero();
            }
            float dot = Dot(vector, onNormal);
            return onNormal * (dot / sqrMag);
        }

        static inline Vector3 ProjectOnPlane(const Vector3& vector, const Vector3& planeNormal)
        {
            return vector - Project(vector, planeNormal);
        }

        static inline Vector3 Reflect(const Vector3& inDirection, const Vector3& inNormal)
        {
            return inDirection - 2.0f * Dot(inNormal, inDirection) * inNormal;
        }

        static inline Vector3 RotateTowards(const Vector3& current, const Vector3& target, float maxRadiansDelta, float maxMagnitudeDelta)
        {
            float lenCurrent = current.magnitude();
            float lenTarget = target.magnitude();

            if (lenCurrent > 1.0e-15f && lenTarget > 1.0e-15f)
            {
                Vector3 from = current / lenCurrent;
                Vector3 to = target / lenTarget;

                float dot = Dot(from, to);
                dot = Mathf::Clamp(dot, -1.0f, 1.0f);

                float angle = std::acosf(dot);
                if (angle == 0.0f)
                {
                    return MoveTowards(current, target, maxMagnitudeDelta);
                }

                float t = std::fmin(1.0f, maxRadiansDelta / angle);

                float sinAngle = std::sinf(angle);
                float coeff0 = std::sinf((1.0f - t) * angle) / sinAngle;
                float coeff1 = std::sinf(t * angle) / sinAngle;
                Vector3 newDir = from * coeff0 + to * coeff1;

                float newLen = Mathf::MoveTowards(lenCurrent, lenTarget, maxMagnitudeDelta);
                return newDir * newLen;
            }

            return MoveTowards(current, target, maxMagnitudeDelta);
        }

        static inline Vector3 SmoothDamp(const Vector3& current, const Vector3& target, Vector3& currentVelocity, float smoothTime, float maxSpeed, float deltaTime)
        {
            float vx = currentVelocity.x;
            float vy = currentVelocity.y;
            float vz = currentVelocity.z;

            float ox = Mathf::SmoothDamp(current.x, target.x, vx, smoothTime, maxSpeed, deltaTime);
            float oy = Mathf::SmoothDamp(current.y, target.y, vy, smoothTime, maxSpeed, deltaTime);
            float oz = Mathf::SmoothDamp(current.z, target.z, vz, smoothTime, maxSpeed, deltaTime);

            currentVelocity.x = vx;
            currentVelocity.y = vy;
            currentVelocity.z = vz;

            return Vector3(ox, oy, oz);
        }

        static inline Vector3 SmoothDamp(const Vector3& current, const Vector3& target, Vector3& currentVelocity, float smoothTime, float maxSpeed)
        {
            return SmoothDamp(current, target, currentVelocity, smoothTime, maxSpeed, MATH_DELTATIME());
        }

        static inline Vector3 SmoothDamp(const Vector3& current, const Vector3& target, Vector3& currentVelocity, float smoothTime)
        {
            return SmoothDamp(current, target, currentVelocity, smoothTime, Mathf::Infinity, MATH_DELTATIME());
        }

        friend inline bool operator==(const Vector3& lhs, const Vector3& rhs)
        {
            Vector3 diff = lhs - rhs;
            return diff.sqrMagnitude() < 9.99999944e-11f;
        }

        friend inline bool operator!=(const Vector3& lhs, const Vector3& rhs)
        {
            return !(lhs == rhs);
        }

        friend inline Vector3 operator+(const Vector3& a, const Vector3& b)
        {
            return Vector3(a.x + b.x, a.y + b.y, a.z + b.z);
        }

        friend inline Vector3 operator-(const Vector3& a, const Vector3& b)
        {
            return Vector3(a.x - b.x, a.y - b.y, a.z - b.z);
        }

        friend inline Vector3 operator-(const Vector3& a)
        {
            return Vector3(-a.x, -a.y, -a.z);
        }

        friend inline Vector3 operator*(const Vector3& a, float d)
        {
            return Vector3(a.x * d, a.y * d, a.z * d);
        }

        friend inline Vector3 operator*(float d, const Vector3& a)
        {
            return Vector3(a.x * d, a.y * d, a.z * d);
        }

        friend inline Vector3 operator/(const Vector3& a, float d)
        {
            return Vector3(a.x / d, a.y / d, a.z / d);
        }
    };

    struct Vector4
    {
        float x;
        float y;
        float z;
        float w;

        Vector4()
            : x(0.0f), y(0.0f), z(0.0f), w(0.0f)
        {
        }

        Vector4(float _x, float _y, float _z, float _w)
            : x(_x), y(_y), z(_z), w(_w)
        {
        }

        static inline Vector4 zero()
        {
            return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        static inline Vector4 one()
        {
            return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        inline float& operator[](int index)
        {
            if (index == 0)
            {
                return x;
            }
            if (index == 1)
            {
                return y;
            }
            if (index == 2)
            {
                return z;
            }
            return w;
        }

        inline const float& operator[](int index) const
        {
            if (index == 0)
            {
                return x;
            }
            if (index == 1)
            {
                return y;
            }
            if (index == 2)
            {
                return z;
            }
            return w;
        }

        inline float magnitude() const
        {
            return Mathf::Sqrt(x * x + y * y + z * z + w * w);
        }

        inline float sqrMagnitude() const
        {
            return x * x + y * y + z * z + w * w;
        }

        inline Vector4 normalized() const
        {
            float mag = magnitude();
            if (mag > 1.0e-5f)
            {
                return *this / mag;
            }
            return zero();
        }

        inline void Normalize()
        {
            float mag = magnitude();
            if (mag > 1.0e-5f)
            {
                x /= mag;
                y /= mag;
                z /= mag;
                w /= mag;
            }
            else
            {
                x = 0.0f;
                y = 0.0f;
                z = 0.0f;
                w = 0.0f;
            }
        }

        static inline float Dot(const Vector4& a, const Vector4& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }

        static inline Vector4 Lerp(const Vector4& a, const Vector4& b, float t)
        {
            t = Mathf::Clamp01(t);
            return Vector4(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t,
                a.w + (b.w - a.w) * t
            );
        }

        static inline Vector4 LerpUnclamped(const Vector4& a, const Vector4& b, float t)
        {
            return Vector4(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t,
                a.w + (b.w - a.w) * t
            );
        }

        friend inline bool operator==(const Vector4& lhs, const Vector4& rhs)
        {
            Vector4 diff = lhs - rhs;
            return diff.sqrMagnitude() < 9.99999944e-11f;
        }

        friend inline bool operator!=(const Vector4& lhs, const Vector4& rhs)
        {
            return !(lhs == rhs);
        }

        friend inline Vector4 operator+(const Vector4& a, const Vector4& b)
        {
            return Vector4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
        }

        friend inline Vector4 operator-(const Vector4& a, const Vector4& b)
        {
            return Vector4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
        }

        friend inline Vector4 operator-(const Vector4& a)
        {
            return Vector4(-a.x, -a.y, -a.z, -a.w);
        }

        friend inline Vector4 operator*(const Vector4& a, float d)
        {
            return Vector4(a.x * d, a.y * d, a.z * d, a.w * d);
        }

        friend inline Vector4 operator*(float d, const Vector4& a)
        {
            return Vector4(a.x * d, a.y * d, a.z * d, a.w * d);
        }

        friend inline Vector4 operator/(const Vector4& a, float d)
        {
            return Vector4(a.x / d, a.y / d, a.z / d, a.w / d);
        }
    };

    struct Quaternion
    {
        float x;
        float y;
        float z;
        float w;

        Quaternion()
            : x(0.0f), y(0.0f), z(0.0f), w(1.0f)
        {
        }

        Quaternion(float _x, float _y, float _z, float _w)
            : x(_x), y(_y), z(_z), w(_w)
        {
        }

        static inline Quaternion identity()
        {
            return Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        }

        static inline float Dot(const Quaternion& a, const Quaternion& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }

        inline float sqrMagnitude() const
        {
            return x * x + y * y + z * z + w * w;
        }

        inline Quaternion normalized() const
        {
            float mag = std::sqrtf(sqrMagnitude());
            if (mag > 1.0e-15f)
            {
                return Quaternion(x / mag, y / mag, z / mag, w / mag);
            }
            return identity();
        }

        inline void Normalize()
        {
            float mag = std::sqrtf(sqrMagnitude());
            if (mag > 1.0e-15f)
            {
                x /= mag;
                y /= mag;
                z /= mag;
                w /= mag;
            }
            else
            {
                x = 0.0f;
                y = 0.0f;
                z = 0.0f;
                w = 1.0f;
            }
        }

        static inline Quaternion Inverse(const Quaternion& q)
        {
            float sqr = q.sqrMagnitude();
            if (sqr > 1.0e-15f)
            {
                float inv = 1.0f / sqr;
                return Quaternion(-q.x * inv, -q.y * inv, -q.z * inv, q.w * inv);
            }
            return identity();
        }

        static inline float Angle(const Quaternion& a, const Quaternion& b)
        {
            float dot = std::fabs(Dot(a, b));
            dot = Mathf::Clamp(dot, -1.0f, 1.0f);
            return 2.0f * std::acosf(dot) * Mathf::Rad2Deg;
        }

        static inline Quaternion AngleAxis(float angleDegrees, const Vector3& axis)
        {
            float rad = angleDegrees * Mathf::Deg2Rad * 0.5f;
            float s = std::sinf(rad);
            Vector3 n = axis.normalized();
            return Quaternion(n.x * s, n.y * s, n.z * s, std::cosf(rad));
        }

        static inline Quaternion Euler(float xDegrees, float yDegrees, float zDegrees)
        {
            float xr = xDegrees * Mathf::Deg2Rad * 0.5f;
            float yr = yDegrees * Mathf::Deg2Rad * 0.5f;
            float zr = zDegrees * Mathf::Deg2Rad * 0.5f;

            float sx = std::sinf(xr);
            float cx = std::cosf(xr);
            float sy = std::sinf(yr);
            float cy = std::cosf(yr);
            float sz = std::sinf(zr);
            float cz = std::cosf(zr);

            Quaternion qz(0.0f, 0.0f, sz, cz);
            Quaternion qx(sx, 0.0f, 0.0f, cx);
            Quaternion qy(0.0f, sy, 0.0f, cy);

            return (qy * (qx * qz)).normalized();
        }

        static inline Quaternion Euler(const Vector3& eulerDegrees)
        {
            return Euler(eulerDegrees.x, eulerDegrees.y, eulerDegrees.z);
        }

        inline Vector3 eulerAngles() const
        {
            Quaternion q = normalized();

            float xx = q.x * q.x;
            float yy = q.y * q.y;
            float zz = q.z * q.z;
            float xy = q.x * q.y;
            float xz = q.x * q.z;
            float yz = q.y * q.z;
            float wx = q.w * q.x;
            float wy = q.w * q.y;
            float wz = q.w * q.z;

            float m00 = 1.0f - 2.0f * (yy + zz);
            float m01 = 2.0f * (xy - wz);
            float m02 = 2.0f * (xz + wy);

            float m10 = 2.0f * (xy + wz);
            float m11 = 1.0f - 2.0f * (xx + zz);
            float m12 = 2.0f * (yz - wx);

            float m20 = 2.0f * (xz - wy);
            float m21 = 2.0f * (yz + wx);
            float m22 = 1.0f - 2.0f * (xx + yy);

            float xRad = std::asinf(Mathf::Clamp(m21, -1.0f, 1.0f));
            float zRad;
            float yRad;

            if (std::fabs(m21) > 0.999999f)
            {
                zRad = 0.0f;
                yRad = std::atan2f(m02, m00);
            }
            else
            {
                zRad = std::atan2f(-m01, m11);
                yRad = std::atan2f(-m20, m22);
            }

            Vector3 e(xRad * Mathf::Rad2Deg, yRad * Mathf::Rad2Deg, zRad * Mathf::Rad2Deg);

            auto norm360 = [](float a)
                {
                    a = std::fmod(a, 360.0f);
                    if (a < 0.0f)
                    {
                        a += 360.0f;
                    }
                    return a;
                };

            e.x = norm360(e.x);
            e.y = norm360(e.y);
            e.z = norm360(e.z);
            return e;
        }

        static inline Quaternion LookRotation(const Vector3& forward, const Vector3& up)
        {
            Vector3 f = forward.normalized();
            if (f.sqrMagnitude() < 1.0e-15f)
            {
                return identity();
            }

            Vector3 u = up.normalized();
            Vector3 r = Vector3::Cross(u, f);
            if (r.sqrMagnitude() < 1.0e-15f)
            {
                u = std::fabs(f.y) < 0.999f ? Vector3::up() : Vector3::right();
                r = Vector3::Cross(u, f);
            }
            r.Normalize();
            u = Vector3::Cross(f, r);

            float m00 = r.x;
            float m01 = u.x;
            float m02 = f.x;
            float m10 = r.y;
            float m11 = u.y;
            float m12 = f.y;
            float m20 = r.z;
            float m21 = u.z;
            float m22 = f.z;

            float trace = m00 + m11 + m22;
            Quaternion q;

            if (trace > 0.0f)
            {
                float s = std::sqrtf(trace + 1.0f) * 2.0f;
                q.w = 0.25f * s;
                q.x = (m21 - m12) / s;
                q.y = (m02 - m20) / s;
                q.z = (m10 - m01) / s;
            }
            else if (m00 > m11 && m00 > m22)
            {
                float s = std::sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
                q.w = (m21 - m12) / s;
                q.x = 0.25f * s;
                q.y = (m01 + m10) / s;
                q.z = (m02 + m20) / s;
            }
            else if (m11 > m22)
            {
                float s = std::sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
                q.w = (m02 - m20) / s;
                q.x = (m01 + m10) / s;
                q.y = 0.25f * s;
                q.z = (m12 + m21) / s;
            }
            else
            {
                float s = std::sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
                q.w = (m10 - m01) / s;
                q.x = (m02 + m20) / s;
                q.y = (m12 + m21) / s;
                q.z = 0.25f * s;
            }

            return q.normalized();
        }

        static inline Quaternion LookRotation(const Vector3& forward)
        {
            return LookRotation(forward, Vector3::up());
        }

        static inline Quaternion SlerpUnclamped(const Quaternion& a, const Quaternion& b, float t)
        {
            Quaternion qb = b;
            float cosOmega = Dot(a, b);

            if (cosOmega < 0.0f)
            {
                cosOmega = -cosOmega;
                qb.x = -qb.x;
                qb.y = -qb.y;
                qb.z = -qb.z;
                qb.w = -qb.w;
            }

            const float kEps = 1.0e-6f;
            if (cosOmega > 1.0f - kEps)
            {
                Quaternion q(
                    a.x + (qb.x - a.x) * t,
                    a.y + (qb.y - a.y) * t,
                    a.z + (qb.z - a.z) * t,
                    a.w + (qb.w - a.w) * t
                );
                return q.normalized();
            }

            float omega = std::acosf(Mathf::Clamp(cosOmega, -1.0f, 1.0f));
            float sinOmega = std::sinf(omega);

            float k0 = std::sinf((1.0f - t) * omega) / sinOmega;
            float k1 = std::sinf(t * omega) / sinOmega;

            Quaternion q(
                a.x * k0 + qb.x * k1,
                a.y * k0 + qb.y * k1,
                a.z * k0 + qb.z * k1,
                a.w * k0 + qb.w * k1
            );

            return q.normalized();
        }

        static inline Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t)
        {
            return SlerpUnclamped(a, b, Mathf::Clamp01(t));
        }

        static inline Quaternion LerpUnclamped(const Quaternion& a, const Quaternion& b, float t)
        {
            Quaternion q(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t,
                a.w + (b.w - a.w) * t
            );
            return q.normalized();
        }

        static inline Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t)
        {
            return LerpUnclamped(a, b, Mathf::Clamp01(t));
        }

        static inline Quaternion RotateTowards(const Quaternion& from, const Quaternion& to, float maxDegreesDelta)
        {
            float angle = Angle(from, to);
            if (angle == 0.0f)
            {
                return to;
            }
            float t = std::fmin(1.0f, maxDegreesDelta / angle);
            return SlerpUnclamped(from, to, t);
        }

        friend inline bool operator==(const Quaternion& lhs, const Quaternion& rhs)
        {
            return Dot(lhs, rhs) > 0.999999f;
        }

        friend inline bool operator!=(const Quaternion& lhs, const Quaternion& rhs)
        {
            return !(lhs == rhs);
        }

        friend inline Quaternion operator*(const Quaternion& a, const Quaternion& b)
        {
            return Quaternion(
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
            );
        }

        friend inline Vector3 operator*(const Quaternion& rotation, const Vector3& point)
        {
            float num = rotation.x * 2.0f;
            float num2 = rotation.y * 2.0f;
            float num3 = rotation.z * 2.0f;

            float num4 = rotation.x * num;
            float num5 = rotation.y * num2;
            float num6 = rotation.z * num3;

            float num7 = rotation.x * num2;
            float num8 = rotation.x * num3;
            float num9 = rotation.y * num3;

            float num10 = rotation.w * num;
            float num11 = rotation.w * num2;
            float num12 = rotation.w * num3;

            Vector3 result;
            result.x = (1.0f - (num5 + num6)) * point.x + (num7 - num12) * point.y + (num8 + num11) * point.z;
            result.y = (num7 + num12) * point.x + (1.0f - (num4 + num6)) * point.y + (num9 - num10) * point.z;
            result.z = (num8 - num11) * point.x + (num9 + num10) * point.y + (1.0f - (num4 + num5)) * point.z;
            return result;
        }
    };

    struct Matrix4x4
    {
        float m00; float m01; float m02; float m03;
        float m10; float m11; float m12; float m13;
        float m20; float m21; float m22; float m23;
        float m30; float m31; float m32; float m33;

        Matrix4x4()
            : m00(1.0f), m01(0.0f), m02(0.0f), m03(0.0f)
            , m10(0.0f), m11(1.0f), m12(0.0f), m13(0.0f)
            , m20(0.0f), m21(0.0f), m22(1.0f), m23(0.0f)
            , m30(0.0f), m31(0.0f), m32(0.0f), m33(1.0f)
        {
        }

        static inline Matrix4x4 identity()
        {
            return Matrix4x4();
        }

        static inline Matrix4x4 zero()
        {
            Matrix4x4 m;
            m.m00 = m.m01 = m.m02 = m.m03 = 0.0f;
            m.m10 = m.m11 = m.m12 = m.m13 = 0.0f;
            m.m20 = m.m21 = m.m22 = m.m23 = 0.0f;
            m.m30 = m.m31 = m.m32 = m.m33 = 0.0f;
            return m;
        }

        inline float& operator()(int row, int col)
        {
            return (&m00)[row * 4 + col];
        }

        inline const float& operator()(int row, int col) const
        {
            return (&m00)[row * 4 + col];
        }

        static inline Matrix4x4 Translate(const Vector3& v)
        {
            Matrix4x4 m = identity();
            m.m03 = v.x;
            m.m13 = v.y;
            m.m23 = v.z;
            return m;
        }

        static inline Matrix4x4 Scale(const Vector3& v)
        {
            Matrix4x4 m = identity();
            m.m00 = v.x;
            m.m11 = v.y;
            m.m22 = v.z;
            return m;
        }

        static inline Matrix4x4 Rotate(const Quaternion& q)
        {
            Quaternion n = q.normalized();

            float xx = n.x * n.x;
            float yy = n.y * n.y;
            float zz = n.z * n.z;
            float xy = n.x * n.y;
            float xz = n.x * n.z;
            float yz = n.y * n.z;
            float wx = n.w * n.x;
            float wy = n.w * n.y;
            float wz = n.w * n.z;

            Matrix4x4 m = identity();
            m.m00 = 1.0f - 2.0f * (yy + zz);
            m.m01 = 2.0f * (xy - wz);
            m.m02 = 2.0f * (xz + wy);

            m.m10 = 2.0f * (xy + wz);
            m.m11 = 1.0f - 2.0f * (xx + zz);
            m.m12 = 2.0f * (yz - wx);

            m.m20 = 2.0f * (xz - wy);
            m.m21 = 2.0f * (yz + wx);
            m.m22 = 1.0f - 2.0f * (xx + yy);

            return m;
        }

        static inline Matrix4x4 TRS(const Vector3& pos, const Quaternion& q, const Vector3& s)
        {
            Matrix4x4 r = Rotate(q);
            r.m00 *= s.x; r.m01 *= s.y; r.m02 *= s.z;
            r.m10 *= s.x; r.m11 *= s.y; r.m12 *= s.z;
            r.m20 *= s.x; r.m21 *= s.y; r.m22 *= s.z;

            r.m03 = pos.x;
            r.m13 = pos.y;
            r.m23 = pos.z;

            return r;
        }

        inline Vector4 GetColumn(int i) const
        {
            return Vector4((*this)(0, i), (*this)(1, i), (*this)(2, i), (*this)(3, i));
        }

        inline Vector4 GetRow(int i) const
        {
            return Vector4((*this)(i, 0), (*this)(i, 1), (*this)(i, 2), (*this)(i, 3));
        }

        inline void SetColumn(int i, const Vector4& v)
        {
            (*this)(0, i) = v.x;
            (*this)(1, i) = v.y;
            (*this)(2, i) = v.z;
            (*this)(3, i) = v.w;
        }

        inline void SetRow(int i, const Vector4& v)
        {
            (*this)(i, 0) = v.x;
            (*this)(i, 1) = v.y;
            (*this)(i, 2) = v.z;
            (*this)(i, 3) = v.w;
        }

        inline Vector3 MultiplyPoint3x4(const Vector3& v) const
        {
            Vector3 r;
            r.x = m00 * v.x + m01 * v.y + m02 * v.z + m03;
            r.y = m10 * v.x + m11 * v.y + m12 * v.z + m13;
            r.z = m20 * v.x + m21 * v.y + m22 * v.z + m23;
            return r;
        }

        inline Vector3 MultiplyPoint(const Vector3& v) const
        {
            float x = m00 * v.x + m01 * v.y + m02 * v.z + m03;
            float y = m10 * v.x + m11 * v.y + m12 * v.z + m13;
            float z = m20 * v.x + m21 * v.y + m22 * v.z + m23;
            float w = m30 * v.x + m31 * v.y + m32 * v.z + m33;

            if (w != 0.0f)
            {
                float invW = 1.0f / w;
                return Vector3(x * invW, y * invW, z * invW);
            }
            return Vector3(x, y, z);
        }

        inline Vector3 MultiplyVector(const Vector3& v) const
        {
            Vector3 r;
            r.x = m00 * v.x + m01 * v.y + m02 * v.z;
            r.y = m10 * v.x + m11 * v.y + m12 * v.z;
            r.z = m20 * v.x + m21 * v.y + m22 * v.z;
            return r;
        }

        static inline Matrix4x4 Transpose(const Matrix4x4& m)
        {
            Matrix4x4 t = zero();
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    t(r, c) = m(c, r);
                }
            }
            return t;
        }

        static inline Matrix4x4 Inverse(const Matrix4x4& m)
        {
            float a[4][8];

            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    a[r][c] = m(r, c);
                }
                for (int c = 0; c < 4; ++c)
                {
                    a[r][4 + c] = (r == c) ? 1.0f : 0.0f;
                }
            }

            for (int col = 0; col < 4; ++col)
            {
                int pivot = col;
                float maxAbs = std::fabs(a[col][col]);

                for (int r = col + 1; r < 4; ++r)
                {
                    float v = std::fabs(a[r][col]);
                    if (v > maxAbs)
                    {
                        maxAbs = v;
                        pivot = r;
                    }
                }

                if (maxAbs < 1.0e-20f)
                {
                    return zero();
                }

                if (pivot != col)
                {
                    for (int c = 0; c < 8; ++c)
                    {
                        std::swap(a[pivot][c], a[col][c]);
                    }
                }

                float invPivot = 1.0f / a[col][col];
                for (int c = 0; c < 8; ++c)
                {
                    a[col][c] *= invPivot;
                }

                for (int r = 0; r < 4; ++r)
                {
                    if (r == col)
                    {
                        continue;
                    }
                    float factor = a[r][col];
                    for (int c = 0; c < 8; ++c)
                    {
                        a[r][c] -= factor * a[col][c];
                    }
                }
            }

            Matrix4x4 inv = zero();
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    inv(r, c) = a[r][4 + c];
                }
            }
            return inv;
        }

        friend inline Matrix4x4 operator*(const Matrix4x4& a, const Matrix4x4& b)
        {
            Matrix4x4 r = zero();
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                    {
                        sum += a(row, k) * b(k, col);
                    }
                    r(row, col) = sum;
                }
            }
            return r;
        }

        friend inline Vector4 operator*(const Matrix4x4& m, const Vector4& v)
        {
            return Vector4(
                m.m00 * v.x + m.m01 * v.y + m.m02 * v.z + m.m03 * v.w,
                m.m10 * v.x + m.m11 * v.y + m.m12 * v.z + m.m13 * v.w,
                m.m20 * v.x + m.m21 * v.y + m.m22 * v.z + m.m23 * v.w,
                m.m30 * v.x + m.m31 * v.y + m.m32 * v.z + m.m33 * v.w
            );
        }

        inline void DecomposeTRS(Vector3& outPosition, Quaternion& outRotation, Vector3& outScale) const
        {
            outPosition.x = m03;
            outPosition.y = m13;
            outPosition.z = m23;

            Vector3 col0(m00, m10, m20);
            Vector3 col1(m01, m11, m21);
            Vector3 col2(m02, m12, m22);

            outScale.x = col0.magnitude();
            outScale.y = col1.magnitude();
            outScale.z = col2.magnitude();

            Vector3 normCol0 = (outScale.x > 1e-8f) ? (col0 / outScale.x) : Vector3::zero();
            Vector3 normCol1 = (outScale.y > 1e-8f) ? (col1 / outScale.y) : Vector3::zero();
            Vector3 normCol2 = (outScale.z > 1e-8f) ? (col2 / outScale.z) : Vector3::zero();

            Matrix4x4 rotMat = Matrix4x4::identity();
            rotMat.m00 = normCol0.x; rotMat.m01 = normCol1.x; rotMat.m02 = normCol2.x;
            rotMat.m10 = normCol0.y; rotMat.m11 = normCol1.y; rotMat.m12 = normCol2.y;
            rotMat.m20 = normCol0.z; rotMat.m21 = normCol1.z; rotMat.m22 = normCol2.z;

            outRotation = RotationMatrixToQuaternion(rotMat);
        }

    private:
        static inline Quaternion RotationMatrixToQuaternion(const Matrix4x4& m)
        {
            float trace = m.m00 + m.m11 + m.m22;
            Quaternion q;
            if (trace > 0.0f)
            {
                float s = 0.5f / std::sqrtf(trace + 1.0f);
                q.w = 0.25f / s;
                q.x = (m.m21 - m.m12) * s;
                q.y = (m.m02 - m.m20) * s;
                q.z = (m.m10 - m.m01) * s;
            }
            else
            {
                if (m.m00 > m.m11 && m.m00 > m.m22)
                {
                    float s = 2.0f * std::sqrtf(1.0f + m.m00 - m.m11 - m.m22);
                    q.w = (m.m21 - m.m12) / s;
                    q.x = 0.25f * s;
                    q.y = (m.m01 + m.m10) / s;
                    q.z = (m.m02 + m.m20) / s;
                }
                else if (m.m11 > m.m22)
                {
                    float s = 2.0f * std::sqrtf(1.0f + m.m11 - m.m00 - m.m22);
                    q.w = (m.m02 - m.m20) / s;
                    q.x = (m.m01 + m.m10) / s;
                    q.y = 0.25f * s;
                    q.z = (m.m12 + m.m21) / s;
                }
                else
                {
                    float s = 2.0f * std::sqrtf(1.0f + m.m22 - m.m00 - m.m11);
                    q.w = (m.m10 - m.m01) / s;
                    q.x = (m.m02 + m.m20) / s;
                    q.y = (m.m12 + m.m21) / s;
                    q.z = 0.25f * s;
                }
            }
            return q.normalized();
        }
    };

    struct Color
    {
        float r;
        float g;
        float b;
        float a;

        Color()
            : r(0.0f), g(0.0f), b(0.0f), a(1.0f)
        {
        }

        Color(float _r, float _g, float _b, float _a = 1.0f)
            : r(_r), g(_g), b(_b), a(_a)
        {
        }

        static inline Color black()
        {
            return Color(0.0f, 0.0f, 0.0f, 1.0f);
        }

        static inline Color white()
        {
            return Color(1.0f, 1.0f, 1.0f, 1.0f);
        }

        static inline Color red()
        {
            return Color(1.0f, 0.0f, 0.0f, 1.0f);
        }

        static inline Color green()
        {
            return Color(0.0f, 1.0f, 0.0f, 1.0f);
        }

        static inline Color blue()
        {
            return Color(0.0f, 0.0f, 1.0f, 1.0f);
        }

        static inline Color Lerp(const Color& a, const Color& b, float t)
        {
            t = Mathf::Clamp01(t);
            return Color(
                a.r + (b.r - a.r) * t,
                a.g + (b.g - a.g) * t,
                a.b + (b.b - a.b) * t,
                a.a + (b.a - a.a) * t
            );
        }

        static inline Color LerpUnclamped(const Color& a, const Color& b, float t)
        {
            return Color(
                a.r + (b.r - a.r) * t,
                a.g + (b.g - a.g) * t,
                a.b + (b.b - a.b) * t,
                a.a + (b.a - a.a) * t
            );
        }

        static inline void RGBToHSV(const Color& rgbColor, float& H, float& S, float& V)
        {
            float r = rgbColor.r;
            float g = rgbColor.g;
            float b = rgbColor.b;

            float maxc = Mathf::Max(r, Mathf::Max(g, b));
            float minc = Mathf::Min(r, Mathf::Min(g, b));
            V = maxc;

            float delta = maxc - minc;
            if (maxc == 0.0f)
            {
                S = 0.0f;
                H = 0.0f;
                return;
            }

            S = delta / maxc;

            if (delta == 0.0f)
            {
                H = 0.0f;
                return;
            }

            if (maxc == r)
            {
                H = (g - b) / delta;
            }
            else if (maxc == g)
            {
                H = 2.0f + (b - r) / delta;
            }
            else
            {
                H = 4.0f + (r - g) / delta;
            }

            H /= 6.0f;
            if (H < 0.0f)
            {
                H += 1.0f;
            }
        }

        static inline Color HSVToRGB(float H, float S, float V, bool hdr = false)
        {
            if (S == 0.0f)
            {
                return Color(V, V, V, 1.0f);
            }

            H = H - std::floor(H);
            float h = H * 6.0f;
            int sector = static_cast<int>(std::floor(h));
            float f = h - sector;

            float p = V * (1.0f - S);
            float q = V * (1.0f - S * f);
            float t = V * (1.0f - S * (1.0f - f));

            Color c;
            switch (sector)
            {
            case 0: c = Color(V, t, p, 1.0f); break;
            case 1: c = Color(q, V, p, 1.0f); break;
            case 2: c = Color(p, V, t, 1.0f); break;
            case 3: c = Color(p, q, V, 1.0f); break;
            case 4: c = Color(t, p, V, 1.0f); break;
            default: c = Color(V, p, q, 1.0f); break;
            }

            if (!hdr)
            {
                c.r = Mathf::Clamp01(c.r);
                c.g = Mathf::Clamp01(c.g);
                c.b = Mathf::Clamp01(c.b);
            }
            return c;
        }

        friend inline Color operator+(const Color& a, const Color& b)
        {
            return Color(a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a);
        }

        friend inline Color operator-(const Color& a, const Color& b)
        {
            return Color(a.r - b.r, a.g - b.g, a.b - b.b, a.a - b.a);
        }

        friend inline Color operator*(const Color& a, float d)
        {
            return Color(a.r * d, a.g * d, a.b * d, a.a * d);
        }

        friend inline Color operator*(float d, const Color& a)
        {
            return a * d;
        }

        friend inline Color operator*(const Color& a, const Color& b)
        {
            return Color(a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a);
        }

        friend inline Color operator/(const Color& a, float d)
        {
            return Color(a.r / d, a.g / d, a.b / d, a.a / d);
        }
    };

    struct Rect
    {
        float x;
        float y;
        float width;
        float height;

        Rect()
            : x(0.0f), y(0.0f), width(0.0f), height(0.0f)
        {
        }

        Rect(float _x, float _y, float _w, float _h)
            : x(_x), y(_y), width(_w), height(_h)
        {
        }

        inline float xMin() const
        {
            return x;
        }

        inline float yMin() const
        {
            return y;
        }

        inline float xMax() const
        {
            return x + width;
        }

        inline float yMax() const
        {
            return y + height;
        }

        inline bool Contains(const Vector2& point) const
        {
            return point.x >= xMin() && point.x < xMax() && point.y >= yMin() && point.y < yMax();
        }
    };

    struct Bounds
    {
        Vector3 center;
        Vector3 size;

        Bounds()
            : center(Vector3::zero()), size(Vector3::zero())
        {
        }

        Bounds(const Vector3& _center, const Vector3& _size)
            : center(_center), size(_size)
        {
        }

        inline Vector3 extents() const
        {
            return size * 0.5f;
        }

        inline Vector3 min() const
        {
            return center - extents();
        }

        inline Vector3 max() const
        {
            return center + extents();
        }

        inline bool Contains(const Vector3& point) const
        {
            Vector3 mn = min();
            Vector3 mx = max();
            return (point.x >= mn.x && point.x <= mx.x) &&
                (point.y >= mn.y && point.y <= mx.y) &&
                (point.z >= mn.z && point.z <= mx.z);
        }

        inline void Encapsulate(const Vector3& point)
        {
            Vector3 mn = min();
            Vector3 mx = max();

            mn.x = std::fmin(mn.x, point.x);
            mn.y = std::fmin(mn.y, point.y);
            mn.z = std::fmin(mn.z, point.z);

            mx.x = std::fmax(mx.x, point.x);
            mx.y = std::fmax(mx.y, point.y);
            mx.z = std::fmax(mx.z, point.z);

            center = (mn + mx) * 0.5f;
            size = (mx - mn);
        }

        inline void Expand(float amount)
        {
            size = size + Vector3(amount, amount, amount);
        }

        inline void Expand(const Vector3& amount)
        {
            size = size + amount;
        }
    };

    struct Ray
    {
        Vector3 origin;
        Vector3 direction;

        Ray()
            : origin(Vector3::zero()), direction(Vector3::forward())
        {
        }

        Ray(const Vector3& _origin, const Vector3& _direction)
            : origin(_origin), direction(_direction)
        {
        }

        inline Vector3 GetPoint(float distance) const
        {
            return origin + direction * distance;
        }
    };

    struct Plane
    {
        Vector3 normal;
        float distance;

        Plane()
            : normal(Vector3::up()), distance(0.0f)
        {
        }

        Plane(const Vector3& inNormal, float d)
            : normal(inNormal.normalized()), distance(d)
        {
        }

        Plane(const Vector3& inNormal, const Vector3& inPoint)
            : normal(inNormal.normalized()), distance(-Vector3::Dot(inNormal.normalized(), inPoint))
        {
        }

        Plane(const Vector3& a, const Vector3& b, const Vector3& c)
        {
            normal = Vector3::Cross(b - a, c - a).normalized();
            distance = -Vector3::Dot(normal, a);
        }

        inline float GetDistanceToPoint(const Vector3& point) const
        {
            return Vector3::Dot(normal, point) + distance;
        }

        inline bool GetSide(const Vector3& point) const
        {
            return GetDistanceToPoint(point) > 0.0f;
        }

        inline bool SameSide(const Vector3& p1, const Vector3& p2) const
        {
            float d1 = GetDistanceToPoint(p1);
            float d2 = GetDistanceToPoint(p2);
            return (d1 > 0.0f && d2 > 0.0f) || (d1 <= 0.0f && d2 <= 0.0f);
        }

        inline bool Raycast(const Ray& ray, float& enter) const
        {
            float denom = Vector3::Dot(normal, ray.direction);
            if (std::fabs(denom) < 1.0e-15f)
            {
                enter = 0.0f;
                return false;
            }
            enter = -(Vector3::Dot(normal, ray.origin) + distance) / denom;
            return enter >= 0.0f;
        }
    };

    struct Vector2Int
    {
        int x;
        int y;

        Vector2Int()
            : x(0), y(0)
        {
        }

        Vector2Int(int _x, int _y)
            : x(_x), y(_y)
        {
        }

        static inline Vector2Int zero()
        {
            return Vector2Int(0, 0);
        }

        static inline Vector2Int one()
        {
            return Vector2Int(1, 1);
        }

        friend inline Vector2Int operator+(const Vector2Int& a, const Vector2Int& b)
        {
            return Vector2Int(a.x + b.x, a.y + b.y);
        }

        friend inline Vector2Int operator-(const Vector2Int& a, const Vector2Int& b)
        {
            return Vector2Int(a.x - b.x, a.y - b.y);
        }
    };

    struct Vector3Int
    {
        int x;
        int y;
        int z;

        Vector3Int()
            : x(0), y(0), z(0)
        {
        }

        Vector3Int(int _x, int _y, int _z)
            : x(_x), y(_y), z(_z)
        {
        }

        static inline Vector3Int zero()
        {
            return Vector3Int(0, 0, 0);
        }

        static inline Vector3Int one()
        {
            return Vector3Int(1, 1, 1);
        }

        friend inline Vector3Int operator+(const Vector3Int& a, const Vector3Int& b)
        {
            return Vector3Int(a.x + b.x, a.y + b.y, a.z + b.z);
        }

        friend inline Vector3Int operator-(const Vector3Int& a, const Vector3Int& b)
        {
            return Vector3Int(a.x - b.x, a.y - b.y, a.z - b.z);
        }
    };
}
