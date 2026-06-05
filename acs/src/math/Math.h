// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — スカラ数学関数と定数
// -----------------------------------------------------------------------------
// 浮動小数の基本演算（sqrt / sin / cos など）を <cmath> ラッパとして提供。
// よく使う定数（π / ε / deg↔rad 変換係数）も定義。
// constexpr が効く関数（Lerp / Clamp / Saturate）はインライン化される。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

#include <cmath>

namespace acs {

/** 円周率 π。 */
inline constexpr f32 kPi      = 3.14159265358979323846f;

/** 2π (1 回転のラジアン)。 */
inline constexpr f32 kTwoPi   = 6.28318530717958647692f;

/** π/2 (直角のラジアン)。 */
inline constexpr f32 kHalfPi  = 1.57079632679489661923f;

/** 1/π (π での除算を乗算に置き換える用)。 */
inline constexpr f32 kInvPi   = 0.31830988618379067154f;

/** 浮動小数の近似一致判定に使う既定イプシロン。 */
inline constexpr f32 kEpsilon = 1.0e-6f;

/** 度→ラジアン変換係数 (π/180)。 */
inline constexpr f32 kDeg2Rad = 0.01745329251994329577f;

/** ラジアン→度変換係数 (180/π)。 */
inline constexpr f32 kRad2Deg = 57.2957795130823208768f;

/**
 * 絶対値を返す。
 *
 * @param v 入力値。
 * @return |v|。
 */
ACS_FORCEINLINE f32 Abs   (f32 v) noexcept { return v < 0 ? -v : v; }

/**
 * 平方根を返す (::sqrtf 委譲)。
 *
 * @param v 入力値 (非負を想定)。
 * @return √v。
 */
ACS_FORCEINLINE f32 Sqrt  (f32 v) noexcept { return ::sqrtf(v); }

/**
 * 正弦を返す (::sinf 委譲)。
 *
 * @param v 角度 (ラジアン)。
 * @return sin(v)。
 */
ACS_FORCEINLINE f32 Sin   (f32 v) noexcept { return ::sinf(v); }

/**
 * 余弦を返す (::cosf 委譲)。
 *
 * @param v 角度 (ラジアン)。
 * @return cos(v)。
 */
ACS_FORCEINLINE f32 Cos   (f32 v) noexcept { return ::cosf(v); }

/**
 * 正接を返す (::tanf 委譲)。
 *
 * @param v 角度 (ラジアン)。
 * @return tan(v)。
 */
ACS_FORCEINLINE f32 Tan   (f32 v) noexcept { return ::tanf(v); }

/**
 * 逆正弦を返す (::asinf 委譲)。
 *
 * @param v [-1,1] の入力値。
 * @return asin(v) (ラジアン)。
 */
ACS_FORCEINLINE f32 ASin  (f32 v) noexcept { return ::asinf(v); }

/**
 * 逆余弦を返す (::acosf 委譲)。
 *
 * @param v [-1,1] の入力値。
 * @return acos(v) (ラジアン)。
 */
ACS_FORCEINLINE f32 ACos  (f32 v) noexcept { return ::acosf(v); }

/**
 * 2 引数の逆正接を返す (::atan2f 委譲)。
 *
 * @param y y 成分。
 * @param x x 成分。
 * @return atan2(y, x) (ラジアン、象限を考慮)。
 */
ACS_FORCEINLINE f32 ATan2 (f32 y, f32 x) noexcept { return ::atan2f(y, x); }

/**
 * 累乗を返す (::powf 委譲)。
 *
 * @param b 底。
 * @param e 指数。
 * @return b の e 乗。
 */
ACS_FORCEINLINE f32 Pow   (f32 b, f32 e) noexcept { return ::powf(b, e); }

/**
 * 自然指数を返す (::expf 委譲)。
 *
 * @param v 指数。
 * @return e の v 乗。
 */
ACS_FORCEINLINE f32 Exp   (f32 v) noexcept { return ::expf(v); }

/**
 * 自然対数を返す (::logf 委譲)。
 *
 * @param v 入力値 (正を想定)。
 * @return ln(v)。
 */
ACS_FORCEINLINE f32 Log   (f32 v) noexcept { return ::logf(v); }

/**
 * 切り捨て (床関数) を返す (::floorf 委譲)。
 *
 * @param v 入力値。
 * @return v 以下の最大の整数値。
 */
ACS_FORCEINLINE f32 Floor (f32 v) noexcept { return ::floorf(v); }

/**
 * 切り上げ (天井関数) を返す (::ceilf 委譲)。
 *
 * @param v 入力値。
 * @return v 以上の最小の整数値。
 */
ACS_FORCEINLINE f32 Ceil  (f32 v) noexcept { return ::ceilf(v); }

/**
 * 四捨五入を返す (::roundf 委譲)。
 *
 * @param v 入力値。
 * @return v に最も近い整数値 (中間は 0 から離れる方向)。
 */
ACS_FORCEINLINE f32 Round (f32 v) noexcept { return ::roundf(v); }

/**
 * 浮動小数の剰余を返す (::fmodf 委譲)。
 *
 * @param a 被除数。
 * @param b 除数。
 * @return a を b で割った余り。
 */
ACS_FORCEINLINE f32 Mod   (f32 a, f32 b) noexcept { return ::fmodf(a, b); }

/**
 * 度をラジアンに変換する。
 *
 * @param deg 度。
 * @return ラジアン値 (deg * kDeg2Rad)。
 */
ACS_FORCEINLINE f32 ToRadians(f32 deg) noexcept { return deg * kDeg2Rad; }

/**
 * ラジアンを度に変換する。
 *
 * @param rad ラジアン。
 * @return 度値 (rad * kRad2Deg)。
 */
ACS_FORCEINLINE f32 ToDegrees(f32 rad) noexcept { return rad * kRad2Deg; }

/**
 * 線形補間を行う。
 *
 * @param a t=0 のときの値。
 * @param b t=1 のときの値。
 * @param t 補間係数 (クランプなし)。
 * @return a + (b - a) * t。
 */
ACS_FORCEINLINE f32 Lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

/**
 * 値を [0,1] にクランプする。
 *
 * @param v 入力値。
 * @return 0 未満なら 0、1 超なら 1、それ以外は v。
 */
ACS_FORCEINLINE f32 Saturate(f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/**
 * 2 値が許容誤差内で一致するかを返す。
 *
 * @param a 比較値 1。
 * @param b 比較値 2。
 * @param eps 許容誤差 (既定 kEpsilon)。
 * @return |a - b| <= eps なら true。
 */
ACS_FORCEINLINE bool IsNearlyEqual(f32 a, f32 b, f32 eps = kEpsilon) noexcept {
    const f32 d = a - b; return (d < 0 ? -d : d) <= eps;
}

} // namespace acs
