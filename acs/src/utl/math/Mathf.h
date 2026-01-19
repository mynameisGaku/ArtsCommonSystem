#pragma once
#include "MathCommon.h"

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

        static inline float Abs(float v) { return std::fabs(v); }
        static inline int Abs(int v) { return v < 0 ? -v : v; }
        static inline float Min(float a, float b) { return std::fmin(a, b); }
        static inline float Max(float a, float b) { return std::fmax(a, b); }
        static inline int Min(int a, int b) { return a < b ? a : b; }
        static inline int Max(int a, int b) { return a > b ? a : b; }
        static inline float Sign(float v) { return v >= 0.0f ? 1.0f : -1.0f; }
        static inline float Clamp(float value, float min, float max) { return std::fmin(std::fmax(value, min), max); }
        static inline int Clamp(int value, int min, int max) { return value < min ? min : (value > max ? max : value); }
        static inline float Clamp01(float value) { return Clamp(value, 0.0f, 1.0f); }

        static inline float Lerp(float a, float b, float t) { t = Clamp01(t); return a + (b - a) * t; }
        static inline float LerpUnclamped(float a, float b, float t) { return a + (b - a) * t; }
        static inline float InverseLerp(float a, float b, float value) { if (a != b) return Clamp01((value - a) / (b - a)); return 0.0f; }

        static inline float SmoothStep(float from, float to, float t) { t = Clamp01(t); t = t * t * (3.0f - 2.0f * t); return to * t + from * (1.0f - t); }
        static inline float Repeat(float t, float length) { return t - std::floor(t / length) * length; }
        static inline float PingPong(float t, float length) { t = Repeat(t, length * 2.0f); return length - Abs(t - length); }

        static inline float DeltaAngle(float current, float target) {
            float delta = Repeat((target - current), 360.0f);
            if (delta > 180.0f) delta -= 360.0f;
            return delta;
        }

        static inline float MoveTowards(float current, float target, float maxDelta) {
            if (Abs(target - current) <= maxDelta) return target;
            return current + Sign(target - current) * maxDelta;
        }

        static inline float MoveTowardsAngle(float current, float target, float maxDelta) {
            float delta = DeltaAngle(current, target);
            if (-maxDelta < delta && delta < maxDelta) return target;
            target = current + delta;
            return MoveTowards(current, target, maxDelta);
        }

        static inline bool Approximately(float a, float b) {
            return Abs(b - a) < Max(1.0e-6f * Max(Abs(a), Abs(b)), Epsilon * 8.0f);
        }

        static inline float Sin(float v) { return std::sinf(v); }
        static inline float Cos(float v) { return std::cosf(v); }
        static inline float Tan(float v) { return std::tanf(v); }
        static inline float Asin(float v) { return std::asinf(v); }
        static inline float Acos(float v) { return std::acosf(v); }
        static inline float Atan(float v) { return std::atanf(v); }
        static inline float Atan2(float y, float x) { return std::atan2f(y, x); }
        static inline float Sqrt(float v) { return std::sqrtf(v); }
        static inline float Pow(float f, float p) { return std::powf(f, p); }
        static inline float Exp(float power) { return std::expf(power); }
        static inline float Log(float f) { return std::logf(f); }
        static inline float Log10(float f) { return std::log10f(f); }
        static inline float Floor(float f) { return std::floorf(f); }
        static inline float Ceil(float f) { return std::ceilf(f); }
        static inline float Round(float f) { return std::roundf(f); }
        static inline int FloorToInt(float f) { return static_cast<int>(std::floorf(f)); }
        static inline int CeilToInt(float f) { return static_cast<int>(std::ceilf(f)); }
        static inline int RoundToInt(float f) { return static_cast<int>(std::roundf(f)); }

        static inline float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed, float deltaTime) {
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
            if ((originalTo - current > 0.0f) == (output > originalTo)) {
                output = originalTo;
                currentVelocity = (output - originalTo) / deltaTime;
            }
            return output;
        }

        static inline float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed = Infinity) {
            return SmoothDamp(current, target, currentVelocity, smoothTime, maxSpeed, MATH_DELTATIME());
        }

        static inline float SmoothDampAngle(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed, float deltaTime) {
            target = current + DeltaAngle(current, target);
            return SmoothDamp(current, target, currentVelocity, smoothTime, maxSpeed, deltaTime);
        }

        static inline float SmoothDampAngle(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed = Infinity) {
            return SmoothDampAngle(current, target, currentVelocity, smoothTime, maxSpeed, MATH_DELTATIME());
        }

        static inline float LerpAngle(float a, float b, float t) {
            float delta = Repeat((b - a), 360.0f);
            if (delta > 180.0f) delta -= 360.0f;
            return a + delta * Clamp01(t);
        }
    };
}