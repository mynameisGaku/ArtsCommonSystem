// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — Easing functions
//
// 全関数は `f32 (*)(f32)` の関数ポインタとして使える: 入力 t in [0,1]、
// 戻り値は補間進度 (Back/Elastic は一時的に [0,1] を外れる)。
//
// 使い方:
//   #include "gameframework/Easing.h"
//   using namespace acs::game;
//   f32 e = Easing::OutCubic(0.5f);          // 直接呼び出し
//
//   // FTween に渡す:
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

/** π/2。Sine 系イージングの角度スケールに使う。 */
inline constexpr f32 kHalfPi  = 1.57079632679f;

/** Back イージングの overshoot 係数 c1 (≈1.70158)。 */
inline constexpr f32 kBackC1  = 1.70158f;

/** InOutBack 用の overshoot 係数 c2 (= c1 * 1.525)。 */
inline constexpr f32 kBackC2  = kBackC1 * 1.525f;

/** In/OutBack 用の係数 c3 (= c1 + 1)。 */
inline constexpr f32 kBackC3  = kBackC1 + 1.0f;

/** In/OutElastic の角周波数 c4 (= 2π/3)。 */
inline constexpr f32 kElasC4  = 6.28318530718f / 3.0f;

/** InOutElastic の角周波数 c5 (= 2π/4.5)。 */
inline constexpr f32 kElasC5  = 6.28318530718f / 4.5f;

/** Bounce イージングの放物線係数 n1 (= 7.5625)。 */
inline constexpr f32 kBounceN1 = 7.5625f;

/** Bounce イージングの区間分母 d1 (= 2.75)。 */
inline constexpr f32 kBounceD1 = 2.75f;

/**
 * 線形 (補間なし)。
 *
 * @param t 入力進度 [0,1]。
 * @return t をそのまま返す。
 */
inline f32 Linear(f32 t) noexcept { return t; }

/**
 * 二次の ease-in (t^2)。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InQuad   (f32 t) noexcept { return t * t; }

/**
 * 二次の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 OutQuad  (f32 t) noexcept { return 1.0f - (1.0f - t) * (1.0f - t); }

/**
 * 二次の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutQuad(f32 t) noexcept {
    return t < 0.5f ? 2.0f * t * t
                     : 1.0f - 0.5f * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f);
}

/**
 * 三次の ease-in (t^3)。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InCubic   (f32 t) noexcept { return t * t * t; }

/**
 * 三次の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 OutCubic  (f32 t) noexcept {
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u;
}

/**
 * 三次の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutCubic(f32 t) noexcept {
    if (t < 0.5f) return 4.0f * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u;
}

/**
 * 四次の ease-in (t^4)。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InQuart   (f32 t) noexcept { return t * t * t * t; }

/**
 * 四次の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 OutQuart  (f32 t) noexcept {
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u * u;
}

/**
 * 四次の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutQuart(f32 t) noexcept {
    if (t < 0.5f) return 8.0f * t * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u * u;
}

/**
 * 五次の ease-in (t^5)。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InQuint   (f32 t) noexcept { return t * t * t * t * t; }

/**
 * 五次の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 OutQuint  (f32 t) noexcept {
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

/**
 * 五次の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutQuint(f32 t) noexcept {
    if (t < 0.5f) return 16.0f * t * t * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u * u * u;
}

/**
 * 正弦の ease-in。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InSine   (f32 t) noexcept { return 1.0f - Cos(t * kHalfPi); }

/**
 * 正弦の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 OutSine  (f32 t) noexcept { return Sin(t * kHalfPi); }

/**
 * 正弦の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutSine(f32 t) noexcept { return -(Cos(kPi * t) - 1.0f) * 0.5f; }

/**
 * 指数の ease-in (2^(10t-10))。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度 (t<=0 で 0)。
 */
inline f32 InExpo   (f32 t) noexcept { return t <= 0.0f ? 0.0f : Pow(2.0f, 10.0f * t - 10.0f); }

/**
 * 指数の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度 (t>=1 で 1)。
 */
inline f32 OutExpo  (f32 t) noexcept { return t >= 1.0f ? 1.0f : 1.0f - Pow(2.0f, -10.0f * t); }

/**
 * 指数の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度 (端点で 0 / 1 にクランプ)。
 */
inline f32 InOutExpo(f32 t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f
            ? 0.5f * Pow(2.0f, 20.0f * t - 10.0f)
            : (2.0f - Pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;
}

/**
 * 円弧の ease-in。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InCirc   (f32 t) noexcept { return 1.0f - Sqrt(1.0f - t * t); }

/**
 * 円弧の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 OutCirc  (f32 t) noexcept { return Sqrt(1.0f - (t - 1.0f) * (t - 1.0f)); }

/**
 * 円弧の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutCirc(f32 t) noexcept {
    if (t < 0.5f) return (1.0f - Sqrt(1.0f - 4.0f * t * t)) * 0.5f;
    const f32 u = -2.0f * t + 2.0f;
    return (Sqrt(1.0f - u * u) + 1.0f) * 0.5f;
}

/**
 * 戻りつき (overshoot) の ease-in。
 *
 * @details 開始側で一時的に [0,1] を下に外れる。
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InBack   (f32 t) noexcept { return kBackC3 * t * t * t - kBackC1 * t * t; }

/**
 * 行き過ぎ (overshoot) の ease-out。
 *
 * @details 終端付近で一時的に 1 を超える。
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 OutBack  (f32 t) noexcept {
    const f32 u = t - 1.0f;
    return 1.0f + kBackC3 * u * u * u + kBackC1 * u * u;
}

/**
 * overshoot の ease-in-out。
 *
 * @details 両端で一時的に [0,1] を外れる。
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutBack(f32 t) noexcept {
    if (t < 0.5f) {
        const f32 u = 2.0f * t;
        return 0.5f * (u * u * ((kBackC2 + 1.0f) * u - kBackC2));
    }
    const f32 u = 2.0f * t - 2.0f;
    return 0.5f * (u * u * ((kBackC2 + 1.0f) * u + kBackC2) + 2.0f);
}

/**
 * 弾性振動の ease-in。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度 (端点で 0 / 1 にクランプ)。
 */
inline f32 InElastic(f32 t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return -Pow(2.0f, 10.0f * t - 10.0f) * Sin((t * 10.0f - 10.75f) * kElasC4);
}

/**
 * 弾性振動の ease-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度 (端点で 0 / 1 にクランプ)。
 */
inline f32 OutElastic(f32 t) noexcept {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return Pow(2.0f, -10.0f * t) * Sin((t * 10.0f - 0.75f) * kElasC4) + 1.0f;
}

/**
 * 弾性振動の ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度 (端点で 0 / 1 にクランプ)。
 */
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

/**
 * 跳ね返りの ease-out。
 *
 * @details 4 つの放物線区間で減衰しながら跳ねる。In/InOutBounce の基礎関数。
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
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

/**
 * 跳ね返りの ease-in (OutBounce の鏡像)。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InBounce(f32 t) noexcept { return 1.0f - OutBounce(1.0f - t); }

/**
 * 跳ね返りの ease-in-out。
 *
 * @param t 入力進度 [0,1]。
 * @return 補間進度。
 */
inline f32 InOutBounce(f32 t) noexcept {
    return t < 0.5f
            ? (1.0f - OutBounce(1.0f - 2.0f * t)) * 0.5f
            : (1.0f + OutBounce(2.0f * t - 1.0f)) * 0.5f;
}

/** イージング関数ポインタ型 (FTween API で使う、f32 (*)(f32) noexcept)。 */
using EasingFn = f32 (*)(f32) noexcept;

} // namespace acs::game::Easing
