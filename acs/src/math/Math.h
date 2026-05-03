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

// ---- 数学定数 ----
inline constexpr f32 kPi      = 3.14159265358979323846f;   // 円周率
inline constexpr f32 kTwoPi   = 6.28318530717958647692f;   // 2π
inline constexpr f32 kHalfPi  = 1.57079632679489661923f;   // π/2
inline constexpr f32 kInvPi   = 0.31830988618379067154f;   // 1/π
inline constexpr f32 kEpsilon = 1.0e-6f;                    // 比較用イプシロン
inline constexpr f32 kDeg2Rad = 0.01745329251994329577f;   // 度→ラジアン
inline constexpr f32 kRad2Deg = 57.2957795130823208768f;   // ラジアン→度

// ---- スカラ関数 ----（<cmath> 委譲）
ACS_FORCEINLINE f32 Abs   (f32 v) noexcept { return v < 0 ? -v : v; }
ACS_FORCEINLINE f32 Sqrt  (f32 v) noexcept { return ::sqrtf(v); }
ACS_FORCEINLINE f32 Sin   (f32 v) noexcept { return ::sinf(v); }
ACS_FORCEINLINE f32 Cos   (f32 v) noexcept { return ::cosf(v); }
ACS_FORCEINLINE f32 Tan   (f32 v) noexcept { return ::tanf(v); }
ACS_FORCEINLINE f32 ASin  (f32 v) noexcept { return ::asinf(v); }
ACS_FORCEINLINE f32 ACos  (f32 v) noexcept { return ::acosf(v); }
ACS_FORCEINLINE f32 ATan2 (f32 y, f32 x) noexcept { return ::atan2f(y, x); }
ACS_FORCEINLINE f32 Pow   (f32 b, f32 e) noexcept { return ::powf(b, e); }
ACS_FORCEINLINE f32 Exp   (f32 v) noexcept { return ::expf(v); }
ACS_FORCEINLINE f32 Log   (f32 v) noexcept { return ::logf(v); }
ACS_FORCEINLINE f32 Floor (f32 v) noexcept { return ::floorf(v); }
ACS_FORCEINLINE f32 Ceil  (f32 v) noexcept { return ::ceilf(v); }
ACS_FORCEINLINE f32 Round (f32 v) noexcept { return ::roundf(v); }
ACS_FORCEINLINE f32 Mod   (f32 a, f32 b) noexcept { return ::fmodf(a, b); }

ACS_FORCEINLINE f32 ToRadians(f32 deg) noexcept { return deg * kDeg2Rad; }
ACS_FORCEINLINE f32 ToDegrees(f32 rad) noexcept { return rad * kRad2Deg; }

// 線形補間（t=0 で a、t=1 で b）
ACS_FORCEINLINE f32 Lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }
// [0,1] にクランプ
ACS_FORCEINLINE f32 Saturate(f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// 近似一致判定
ACS_FORCEINLINE bool IsNearlyEqual(f32 a, f32 b, f32 eps = kEpsilon) noexcept {
    f32 d = a - b; return (d < 0 ? -d : d) <= eps;
}

} // namespace acs
