// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — Easing functions (Phase 3)
//
// 全関数は `f32 (*)(f32)` の関数ポインタとして使える: 入力 t in [0,1]、
// 戻り値は補間進度 (Back/Elastic は一時的に [0,1] を外れる)。
//
// 使い方:
//   #include "gameframework/Easing.h"
//   using namespace acs::game;
//   f32 e = Easing::OutCubic(0.5f);          // 直接呼び出し
//
//   // Tween に渡す:
//   tweens.Tween(&player.x, 0.0f, 100.0f, 0.5f, Easing::InOutQuad);
//
// 命名規約: `<Pace>Curve` で 30 関数。
//   Pace  ∈ { Linear, In, Out, InOut } (Linear は 1 個のみ)
//   Curve ∈ { Linear, Quad, Cubic, Quart, Quint, Sine, Expo, Circ, Back,
//             Elastic, Bounce } (10 個)
//
// 数式: Penner-style。`sinf`/`powf`/`sqrtf` を使うので constexpr 不可、`inline` のみ。
#pragma once

#include "foundation/Types.h"
#include "math/Math.h"

namespace acs::game::Easing {

// ---- 内部定数 ----
inline constexpr f32 kHalfPi  = 1.57079632679f;
inline constexpr f32 kBackC1  = 1.70158f;
inline constexpr f32 kBackC2  = kBackC1 * 1.525f;
inline constexpr f32 kBackC3  = kBackC1 + 1.0f;
inline constexpr f32 kElasC4  = 6.28318530718f / 3.0f;    // 2π/3
inline constexpr f32 kElasC5  = 6.28318530718f / 4.5f;
inline constexpr f32 kBounceN1 = 7.5625f;
inline constexpr f32 kBounceD1 = 2.75f;

// ---- Linear ----------------------------------------------------------------
inline f32 Linear(f32 t) noexcept { return t; }

// ---- Quad (t^2) ------------------------------------------------------------
inline f32 InQuad   (f32 t) noexcept { return t * t; }
inline f32 OutQuad  (f32 t) noexcept { return 1.0f - (1.0f - t) * (1.0f - t); }
inline f32 InOutQuad(f32 t) noexcept {
    return t < 0.5f ? 2.0f * t * t
                     : 1.0f - 0.5f * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f);
}

// ---- Cubic (t^3) -----------------------------------------------------------
inline f32 InCubic   (f32 t) noexcept { return t * t * t; }
inline f32 OutCubic  (f32 t) noexcept {
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u;
}
inline f32 InOutCubic(f32 t) noexcept {
    if (t < 0.5f) return 4.0f * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u;
}

// ---- Quart (t^4) -----------------------------------------------------------
inline f32 InQuart   (f32 t) noexcept { return t * t * t * t; }
inline f32 OutQuart  (f32 t) noexcept {
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u * u;
}
inline f32 InOutQuart(f32 t) noexcept {
    if (t < 0.5f) return 8.0f * t * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u * u;
}

// ---- Quint (t^5) -----------------------------------------------------------
inline f32 InQuint   (f32 t) noexcept { return t * t * t * t * t; }
inline f32 OutQuint  (f32 t) noexcept {
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}
inline f32 InOutQuint(f32 t) noexcept {
    if (t < 0.5f) return 16.0f * t * t * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u * u * u;
}

// ---- Sine ------------------------------------------------------------------
inline f32 InSine   (f32 t) noexcept { return 1.0f - Cos(t * kHalfPi); }
inline f32 OutSine  (f32 t) noexcept { return Sin(t * kHalfPi); }
inline f32 InOutSine(f32 t) noexcept { return -(Cos(kPi * t) - 1.0f) * 0.5f; }

// ---- Expo (2^(10*t-10)) ----------------------------------------------------
inline f32 InExpo   (f32 t) noexcept { return t <= 0.0f ? 0.0f : Pow(2.0f, 10.0f * t - 10.0f); }
inline f32 OutExpo  (f32 t) noexcept { return t >= 1.0f ? 1.0f : 1.0f - Pow(2.0f, -10.0f * t); }
inline f32 InOutExpo(f32 t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f
            ? 0.5f * Pow(2.0f, 20.0f * t - 10.0f)
            : (2.0f - Pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;
}

// ---- Circ ------------------------------------------------------------------
inline f32 InCirc   (f32 t) noexcept { return 1.0f - Sqrt(1.0f - t * t); }
inline f32 OutCirc  (f32 t) noexcept { return Sqrt(1.0f - (t - 1.0f) * (t - 1.0f)); }
inline f32 InOutCirc(f32 t) noexcept {
    if (t < 0.5f) return (1.0f - Sqrt(1.0f - 4.0f * t * t)) * 0.5f;
    const f32 u = -2.0f * t + 2.0f;
    return (Sqrt(1.0f - u * u) + 1.0f) * 0.5f;
}

// ---- Back (overshoot) ------------------------------------------------------
inline f32 InBack   (f32 t) noexcept { return kBackC3 * t * t * t - kBackC1 * t * t; }
inline f32 OutBack  (f32 t) noexcept {
    const f32 u = t - 1.0f;
    return 1.0f + kBackC3 * u * u * u + kBackC1 * u * u;
}
inline f32 InOutBack(f32 t) noexcept {
    if (t < 0.5f) {
        const f32 u = 2.0f * t;
        return 0.5f * (u * u * ((kBackC2 + 1.0f) * u - kBackC2));
    }
    const f32 u = 2.0f * t - 2.0f;
    return 0.5f * (u * u * ((kBackC2 + 1.0f) * u + kBackC2) + 2.0f);
}

// ---- Elastic ---------------------------------------------------------------
inline f32 InElastic(f32 t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return -Pow(2.0f, 10.0f * t - 10.0f) * Sin((t * 10.0f - 10.75f) * kElasC4);
}
inline f32 OutElastic(f32 t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return Pow(2.0f, -10.0f * t) * Sin((t * 10.0f - 0.75f) * kElasC4) + 1.0f;
}
inline f32 InOutElastic(f32 t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) {
        return -0.5f * Pow(2.0f, 20.0f * t - 10.0f) *
                Sin((20.0f * t - 11.125f) * kElasC5);
    }
    return Pow(2.0f, -20.0f * t + 10.0f) *
            Sin((20.0f * t - 11.125f) * kElasC5) * 0.5f + 1.0f;
}

// ---- Bounce ----------------------------------------------------------------
inline f32 OutBounce(f32 t) noexcept {
    if (t < 1.0f / kBounceD1) {
        return kBounceN1 * t * t;
    } else if (t < 2.0f / kBounceD1) {
        t -= 1.5f / kBounceD1;
        return kBounceN1 * t * t + 0.75f;
    } else if (t < 2.5f / kBounceD1) {
        t -= 2.25f / kBounceD1;
        return kBounceN1 * t * t + 0.9375f;
    }
    t -= 2.625f / kBounceD1;
    return kBounceN1 * t * t + 0.984375f;
}
inline f32 InBounce(f32 t) noexcept { return 1.0f - OutBounce(1.0f - t); }
inline f32 InOutBounce(f32 t) noexcept {
    return t < 0.5f
            ? (1.0f - OutBounce(1.0f - 2.0f * t)) * 0.5f
            : (1.0f + OutBounce(2.0f * t - 1.0f)) * 0.5f;
}

// 関数ポインタ型 alias (Tween API で使う)
using EasingFn = f32 (*)(f32) noexcept;

} // namespace acs::game::Easing
