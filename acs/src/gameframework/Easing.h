// SPDX-License-Identifier: Apache-2.0
// GameFramework のイージング関数と型付きイージングカタログ。
//
// 従来の直接関数は、FTweenManager と互換性のある `f32 (*)(f32) noexcept`
// シグネチャを維持する。直接呼び出しは全域関数であり、有限入力を [0,1] に
// clamp、NaN を 0、無限大を符号に対応する端点へ写像する。
// 無効入力を診断する必要がある場合は TryEvaluate を使用する。
#pragma once

#include "foundation/Types.h"
#include "math/Math.h"

#include <cmath>

namespace acs::game::Easing {

/** FTweenManager と互換性のあるイージング関数ポインタ。 */
using EasingFn = f32 (*)(f32) noexcept;

/**
 * 型付きカタログが公開する全イージング曲線。
 * テーブル・エディタとの安定した連携のため数値IDを固定する。新しい要素は Count の
 * 直前へ追加し、新しい明示IDを割り当てる。
 */
enum class EEasingType : u8 {
    Linear       = 0,
    InQuad       = 1,
    OutQuad      = 2,
    InOutQuad    = 3,
    InCubic      = 4,
    OutCubic     = 5,
    InOutCubic   = 6,
    InQuart      = 7,
    OutQuart     = 8,
    InOutQuart   = 9,
    InQuint      = 10,
    OutQuint     = 11,
    InOutQuint   = 12,
    InSine       = 13,
    OutSine      = 14,
    InOutSine    = 15,
    InExpo       = 16,
    OutExpo      = 17,
    InOutExpo    = 18,
    InCirc       = 19,
    OutCirc      = 20,
    InOutCirc    = 21,
    InBack       = 22,
    OutBack      = 23,
    InOutBack    = 24,
    InElastic    = 25,
    OutElastic   = 26,
    InOutElastic = 27,
    InBounce     = 28,
    OutBounce    = 29,
    InOutBounce  = 30,
    SmoothStep   = 31,
    SmootherStep = 32,
    Count        = 33,
};

/** checkedイージングAPIが返す安定したエラーコード。 */
enum class EEasingError : u8 {
    None = 0,
    InvalidType = 1,
    NonFiniteInput = 2,
    NullName = 3,
    UnknownName = 4,
    InvalidSampleCount = 5,
    NullOutput = 6,
    NonFiniteResult = 7,
};

/** checkedイージングカタログ操作の結果。 */
struct FEasingResult {
    EEasingError error = EEasingError::None;

    bool Succeeded() const noexcept {
        return error == EEasingError::None;
    }

    static const char* ErrorName(EEasingError value) noexcept {
        switch (value) {
        case EEasingError::None:           return "None";
        case EEasingError::InvalidType:    return "InvalidType";
        case EEasingError::NonFiniteInput: return "NonFiniteInput";
        case EEasingError::NullName:       return "NullName";
        case EEasingError::UnknownName:    return "UnknownName";
        case EEasingError::InvalidSampleCount: return "InvalidSampleCount";
        case EEasingError::NullOutput:         return "NullOutput";
        case EEasingError::NonFiniteResult:    return "NonFiniteResult";
        default:                           return "InvalidError";
        }
    }
};

// 一括サンプリングの最小点数。開始端点と終了端点の両方を必ず含める。
inline constexpr usize kMinSampleCount = 2u;

// 誤入力による単一呼び出しの過大な CPU 使用を防ぐ上限。
inline constexpr usize kMaxSampleCount = 65536u;

static_assert(
    kMaxSampleCount <= 16777216u,
    "Sample indices must remain exactly representable as f32 integers.");

/** 正弦系で使用する pi/2 の係数。 */
inline constexpr f32 kHalfPi = 1.57079632679f;

/** Penner 形式の Back オーバーシュート定数。 */
inline constexpr f32 kBackC1 = 1.70158f;
inline constexpr f32 kBackC2 = kBackC1 * 1.525f;
inline constexpr f32 kBackC3 = kBackC1 + 1.0f;

/** 弾性系で使用する角度定数。 */
inline constexpr f32 kElasC4 = 6.28318530718f / 3.0f;
inline constexpr f32 kElasC5 = 6.28318530718f / 4.5f;

/** バウンス系の区分式で使用する定数。 */
inline constexpr f32 kBounceN1 = 7.5625f;
inline constexpr f32 kBounceD1 = 2.75f;

namespace Detail {

/**
 * 直接イージング呼び出しを全域関数にし、有限値を定義域へclampする。
 * NaNには方向がないため、決定的に開始端点へ写像する。
 */
inline f32 NormalizeInput(f32 t) noexcept {
    if (!std::isfinite(t)) {
        return t > 0.0f ? 1.0f : 0.0f;
    }
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t;
}

inline bool NameEquals(const char* lhs, const char* rhs) noexcept {
    if (lhs == rhs) return lhs != nullptr;
    if (lhs == nullptr || rhs == nullptr) return false;
    while (*lhs != '\0' && *lhs == *rhs) {
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

// 端点を厳密に保ちながら [0,1] 上の等間隔時刻を返す。
inline f32 UniformSampleTime(usize index, usize sample_count) noexcept {
    if (index == 0u) return 0.0f;
    if (index + 1u == sample_count) return 1.0f;
    return static_cast<f32>(index) /
           static_cast<f32>(sample_count - 1u);
}

// カタログ関数の評価結果も検証し、失敗時は出力を変更しない。
inline FEasingResult TryEvaluateFunction(
    EasingFn function, f32 t, f32& out_value) noexcept {
    if (function == nullptr) return {EEasingError::InvalidType};
    if (!std::isfinite(t)) return {EEasingError::NonFiniteInput};

    const f32 evaluated = function(Saturate(t));
    if (!std::isfinite(evaluated)) {
        return {EEasingError::NonFiniteResult};
    }
    out_value = evaluated;
    return {};
}

// 決定的なカタログ関数を2回評価し、書き込み前に全点を検証する。
inline FEasingResult TrySampleFunction(
    EasingFn function, f32* out_values, usize sample_count) noexcept {
    if (function == nullptr) return {EEasingError::InvalidType};
    if (sample_count < kMinSampleCount ||
        sample_count > kMaxSampleCount) {
        return {EEasingError::InvalidSampleCount};
    }
    if (out_values == nullptr) return {EEasingError::NullOutput};

    for (usize index = 0u; index < sample_count; ++index) {
        const f32 t = UniformSampleTime(index, sample_count);
        if (!std::isfinite(function(t))) {
            return {EEasingError::NonFiniteResult};
        }
    }
    for (usize index = 0u; index < sample_count; ++index) {
        const f32 t = UniformSampleTime(index, sample_count);
        out_values[index] = function(t);
    }
    return {};
}

} // namespace Detail

/** 線形補間。 */
inline f32 Linear(f32 t) noexcept {
    return Detail::NormalizeInput(t);
}

/** 2次イーズイン。 */
inline f32 InQuad(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t;
}

/** 2次イーズアウト。 */
inline f32 OutQuad(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const f32 u = 1.0f - t;
    return 1.0f - u * u;
}

/** 2次イーズインアウト。 */
inline f32 InOutQuad(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return 2.0f * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u;
}

/** 3次イーズイン。 */
inline f32 InCubic(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * t;
}

/** 3次イーズアウト。 */
inline f32 OutCubic(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u;
}

/** 3次イーズインアウト。 */
inline f32 InOutCubic(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return 4.0f * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u;
}

/** 4次イーズイン。 */
inline f32 InQuart(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * t * t;
}

/** 4次イーズアウト。 */
inline f32 OutQuart(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u * u;
}

/** 4次イーズインアウト。 */
inline f32 InOutQuart(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return 8.0f * t * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u * u;
}

/** 5次イーズイン。 */
inline f32 InQuint(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * t * t * t;
}

/** 5次イーズアウト。 */
inline f32 OutQuint(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const f32 u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

/** 5次イーズインアウト。 */
inline f32 InOutQuint(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return 16.0f * t * t * t * t * t;
    const f32 u = -2.0f * t + 2.0f;
    return 1.0f - 0.5f * u * u * u * u * u;
}

/** 正弦イーズイン。 */
inline f32 InSine(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - Cos(t * kHalfPi);
}

/** 正弦イーズアウト。 */
inline f32 OutSine(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return Sin(t * kHalfPi);
}

/** 正弦イーズインアウト。 */
inline f32 InOutSine(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return -(Cos(kPi * t) - 1.0f) * 0.5f;
}

/** 指数イーズイン。 */
inline f32 InExpo(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return Pow(2.0f, 10.0f * t - 10.0f);
}

/** 指数イーズアウト。 */
inline f32 OutExpo(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - Pow(2.0f, -10.0f * t);
}

/** 指数イーズインアウト。 */
inline f32 InOutExpo(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f
        ? 0.5f * Pow(2.0f, 20.0f * t - 10.0f)
        : (2.0f - Pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;
}

/** 円弧イーズイン。 */
inline f32 InCirc(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - Sqrt(1.0f - t * t);
}

/** 円弧イーズアウト。 */
inline f32 OutCirc(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const f32 u = t - 1.0f;
    return Sqrt(1.0f - u * u);
}

/** 円弧イーズインアウト。 */
inline f32 InOutCirc(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return (1.0f - Sqrt(1.0f - 4.0f * t * t)) * 0.5f;
    const f32 u = -2.0f * t + 2.0f;
    return (Sqrt(1.0f - u * u) + 1.0f) * 0.5f;
}

/** Back（オーバーシュート）イーズイン。 */
inline f32 InBack(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return kBackC3 * t * t * t - kBackC1 * t * t;
}

/** Back（オーバーシュート）イーズアウト。 */
inline f32 OutBack(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const f32 u = t - 1.0f;
    return 1.0f + kBackC3 * u * u * u + kBackC1 * u * u;
}

/** Back（オーバーシュート）イーズインアウト。 */
inline f32 InOutBack(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) {
        const f32 u = 2.0f * t;
        return 0.5f * (u * u * ((kBackC2 + 1.0f) * u - kBackC2));
    }
    const f32 u = 2.0f * t - 2.0f;
    return 0.5f * (u * u * ((kBackC2 + 1.0f) * u + kBackC2) + 2.0f);
}

/** 弾性イーズイン。 */
inline f32 InElastic(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return -Pow(2.0f, 10.0f * t - 10.0f)
        * Sin((t * 10.0f - 10.75f) * kElasC4);
}

/** 弾性イーズアウト。 */
inline f32 OutElastic(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return Pow(2.0f, -10.0f * t)
        * Sin((t * 10.0f - 0.75f) * kElasC4) + 1.0f;
}

/** 弾性イーズインアウト。 */
inline f32 InOutElastic(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) {
        return -0.5f * Pow(2.0f, 20.0f * t - 10.0f)
            * Sin((20.0f * t - 11.125f) * kElasC5);
    }
    return Pow(2.0f, -20.0f * t + 10.0f)
        * Sin((20.0f * t - 11.125f) * kElasC5) * 0.5f + 1.0f;
}

/** 区分式によるバウンスイーズアウト。 */
inline f32 OutBounce(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 1.0f / kBounceD1) {
        return kBounceN1 * t * t;
    }
    if (t < 2.0f / kBounceD1) {
        t -= 1.5f / kBounceD1;
        return kBounceN1 * t * t + 0.75f;
    }
    if (t < 2.5f / kBounceD1) {
        t -= 2.25f / kBounceD1;
        return kBounceN1 * t * t + 0.9375f;
    }
    t -= 2.625f / kBounceD1;
    return kBounceN1 * t * t + 0.984375f;
}

/** バウンスイーズイン。 */
inline f32 InBounce(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - OutBounce(1.0f - t);
}

/** バウンスイーズインアウト。 */
inline f32 InOutBounce(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f
        ? (1.0f - OutBounce(1.0f - 2.0f * t)) * 0.5f
        : (1.0f + OutBounce(2.0f * t - 1.0f)) * 0.5f;
}

/** 両端で1階微分が0になる3次Hermite smooth step。 */
inline f32 SmoothStep(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/** 両端で1階・2階微分が0になる5次smooth step。 */
inline f32 SmootherStep(f32 t) noexcept {
    t = Detail::NormalizeInput(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/** typeに対応するFTween互換関数を返す。無効値ではnullptr。 */
inline EasingFn GetFunction(EEasingType type) noexcept {
    switch (type) {
    case EEasingType::Linear:       return &Linear;
    case EEasingType::InQuad:       return &InQuad;
    case EEasingType::OutQuad:      return &OutQuad;
    case EEasingType::InOutQuad:    return &InOutQuad;
    case EEasingType::InCubic:      return &InCubic;
    case EEasingType::OutCubic:     return &OutCubic;
    case EEasingType::InOutCubic:   return &InOutCubic;
    case EEasingType::InQuart:      return &InQuart;
    case EEasingType::OutQuart:     return &OutQuart;
    case EEasingType::InOutQuart:   return &InOutQuart;
    case EEasingType::InQuint:      return &InQuint;
    case EEasingType::OutQuint:     return &OutQuint;
    case EEasingType::InOutQuint:   return &InOutQuint;
    case EEasingType::InSine:       return &InSine;
    case EEasingType::OutSine:      return &OutSine;
    case EEasingType::InOutSine:    return &InOutSine;
    case EEasingType::InExpo:       return &InExpo;
    case EEasingType::OutExpo:      return &OutExpo;
    case EEasingType::InOutExpo:    return &InOutExpo;
    case EEasingType::InCirc:       return &InCirc;
    case EEasingType::OutCirc:      return &OutCirc;
    case EEasingType::InOutCirc:    return &InOutCirc;
    case EEasingType::InBack:       return &InBack;
    case EEasingType::OutBack:      return &OutBack;
    case EEasingType::InOutBack:    return &InOutBack;
    case EEasingType::InElastic:    return &InElastic;
    case EEasingType::OutElastic:   return &OutElastic;
    case EEasingType::InOutElastic: return &InOutElastic;
    case EEasingType::InBounce:     return &InBounce;
    case EEasingType::OutBounce:    return &OutBounce;
    case EEasingType::InOutBounce:  return &InOutBounce;
    case EEasingType::SmoothStep:   return &SmoothStep;
    case EEasingType::SmootherStep: return &SmootherStep;
    default:                         return nullptr;
    }
}

/** typeのcanonicalな安定名を返す。無効値では"Invalid"。 */
inline const char* GetName(EEasingType type) noexcept {
    switch (type) {
    case EEasingType::Linear:       return "Linear";
    case EEasingType::InQuad:       return "InQuad";
    case EEasingType::OutQuad:      return "OutQuad";
    case EEasingType::InOutQuad:    return "InOutQuad";
    case EEasingType::InCubic:      return "InCubic";
    case EEasingType::OutCubic:     return "OutCubic";
    case EEasingType::InOutCubic:   return "InOutCubic";
    case EEasingType::InQuart:      return "InQuart";
    case EEasingType::OutQuart:     return "OutQuart";
    case EEasingType::InOutQuart:   return "InOutQuart";
    case EEasingType::InQuint:      return "InQuint";
    case EEasingType::OutQuint:     return "OutQuint";
    case EEasingType::InOutQuint:   return "InOutQuint";
    case EEasingType::InSine:       return "InSine";
    case EEasingType::OutSine:      return "OutSine";
    case EEasingType::InOutSine:    return "InOutSine";
    case EEasingType::InExpo:       return "InExpo";
    case EEasingType::OutExpo:      return "OutExpo";
    case EEasingType::InOutExpo:    return "InOutExpo";
    case EEasingType::InCirc:       return "InCirc";
    case EEasingType::OutCirc:      return "OutCirc";
    case EEasingType::InOutCirc:    return "InOutCirc";
    case EEasingType::InBack:       return "InBack";
    case EEasingType::OutBack:      return "OutBack";
    case EEasingType::InOutBack:    return "InOutBack";
    case EEasingType::InElastic:    return "InElastic";
    case EEasingType::OutElastic:   return "OutElastic";
    case EEasingType::InOutElastic: return "InOutElastic";
    case EEasingType::InBounce:     return "InBounce";
    case EEasingType::OutBounce:    return "OutBounce";
    case EEasingType::InOutBounce:  return "InOutBounce";
    case EEasingType::SmoothStep:   return "SmoothStep";
    case EEasingType::SmootherStep: return "SmootherStep";
    default:                         return "Invalid";
    }
}

/**
 * easing type の canonical 名を checked 取得する。
 *
 * 無効な enum 値では InvalidType を返し、out_name を変更しない。
 */
inline FEasingResult TryGetName(
    EEasingType type, const char*& out_name) noexcept {
    if (GetFunction(type) == nullptr) {
        return {EEasingError::InvalidType};
    }
    out_name = GetName(type);
    return {};
}

/**
 * canonical easing 名を checked 解析する。
 *
 * null は NullName、空文字列と未知名は UnknownName。失敗時は out_type を
 * 変更しない。比較は大文字小文字を区別する。
 */
inline FEasingResult TryParseNameChecked(
    const char* name, EEasingType& out_type) noexcept {
    if (name == nullptr) {
        return {EEasingError::NullName};
    }

    for (u32 value = 0u;
         value < static_cast<u32>(EEasingType::Count);
         ++value) {
        const EEasingType type = static_cast<EEasingType>(value);
        if (Detail::NameEquals(name, GetName(type))) {
            out_type = type;
            return {};
        }
    }
    return {EEasingError::UnknownName};
}

/**
 * canonical easing 名を互換 bool API で解析する。
 * 失敗時は out_type を変更しない。比較は大文字小文字を区別する。
 */
inline bool TryParseName(
    const char* name, EEasingType& out_type) noexcept {
    return TryParseNameChecked(name, out_type).Succeeded();
}

/**
 * 型付きイージング曲線を評価する。
 * 非有限入力・結果と無効typeを拒否し、失敗時はout_valueを変更しない。
 * 有限入力は[0,1]へclampする。
 */
inline FEasingResult TryEvaluate(
    EEasingType type, f32 t, f32& out_value) noexcept {
    const EasingFn function = GetFunction(type);
    if (function == nullptr) return {EEasingError::InvalidType};
    return Detail::TryEvaluateFunction(function, t, out_value);
}

// 型付き曲線を [0,1] 上で等間隔に一括サンプリングする。
// 失敗時は出力配列を変更しない。成功時は先頭が t=0、末尾が t=1 になる。
inline FEasingResult TrySampleCurve(
    EEasingType type, f32* out_values, usize sample_count) noexcept {
    const EasingFn function = GetFunction(type);
    if (function == nullptr) return {EEasingError::InvalidType};
    return Detail::TrySampleFunction(function, out_values, sample_count);
}

/** typeを評価し、typeまたは入力が無効ならfallbackを返す。 */
inline f32 Evaluate(
    EEasingType type, f32 t, f32 fallback = 0.0f) noexcept {
    f32 value = fallback;
    const FEasingResult result = TryEvaluate(type, t, value);
    return result.Succeeded() ? value : fallback;
}

static_assert(
    static_cast<u32>(EEasingType::Count) == 33u,
    "Update the easing catalog when adding or removing an easing type.");

} // namespace acs::game::Easing
