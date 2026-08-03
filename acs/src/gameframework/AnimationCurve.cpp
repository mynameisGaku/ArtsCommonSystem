// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FAnimationCurve 実装
//
// アルゴリズム:
//   ・AddKey:  内部 TArray を time 昇順に保つよう、二分探索で挿入位置を決める。
//              同 time の key が既にあれば上書き (新規生成ではなく値更新)。
//   ・Evaluate: time を WrapMode で [0, Duration] に折り返した後、segment を
//              二分探索で特定 → 左 key の out_interp で補間。
//   ・Hermite:  3 次エルミート公式
//                h00 =  2t^3 - 3t^2 + 1
//                h10 =      t^3 - 2t^2 + t
//                h01 = -2t^3 + 3t^2
//                h11 =      t^3 -  t^2
//              value = h00*p0 + h10*(m0*dt) + h01*p1 + h11*(m1*dt)
//              タンジェントを dt 倍するのは Unity と同じく「単位 1 秒あたりの傾き」
//              を保存しているため。これで key 間隔を変えても曲線形が直感的に保てる。
#include "gameframework/AnimationCurve.h"
#include "gameframework/Easing.h"
#include "foundation/EnumLookup.h"
#include "foundation/Move.h"
#include "math/Math.h"

#include <cmath>

namespace acs::game {

namespace {

/** 補間 enum の整数値と一致する名前 table。 */
constexpr const char* kInterpolationNames[]{"Step", "Linear", "Hermite"};
/** 補間 enum の妥当性と名前を生成する constexpr table。 */
constexpr TContiguousEnumLookup<ECurveInterpolation, 3u> kInterpolationLookup(kInterpolationNames);

/** wrap enum の整数値と一致する名前 table。 */
constexpr const char* kWrapModeNames[]{"Clamp", "Loop", "PingPong"};
/** wrap enum の妥当性と名前を生成する constexpr table。 */
constexpr TContiguousEnumLookup<FAnimationCurve::EWrapMode, 3u> kWrapModeLookup(kWrapModeNames);

/** 曲線 error enum の整数値と一致する名前 table。 */
constexpr const char* kCurveErrorNames[]{
    "None",
    "NullKeys",
    "TooManyKeys",
    "NonFiniteValue",
    "InvalidInterpolation",
    "InvalidEasingType",
    "InvalidSampleCount",
    "InvalidWrapMode",
    "UnsortedKeys",
    "DuplicateKeyTime",
    "AllocationFailure",
    "ResultOutOfRange",
};
/** 曲線 error 名を生成する constexpr table。 */
constexpr TContiguousEnumLookup<EAnimationCurveError, 12u> kCurveErrorLookup(kCurveErrorNames);

static_assert(kInterpolationLookup.Contains(ECurveInterpolation::Hermite));
static_assert(kWrapModeLookup.Contains(FAnimationCurve::EWrapMode::PingPong));
static_assert(kCurveErrorLookup.Contains(EAnimationCurveError::ResultOutOfRange));

bool IsFiniteCurveValue(f32 value) noexcept {
    return std::isfinite(value);
}

bool IsValidInterpolation(ECurveInterpolation interpolation) noexcept {
    return kInterpolationLookup.Contains(interpolation);
}

bool IsValidWrapMode(FAnimationCurve::EWrapMode mode) noexcept {
    return kWrapModeLookup.Contains(mode);
}

} // namespace

const char* FAnimationCurveResult::ErrorName(
    EAnimationCurveError error) noexcept {
    return kCurveErrorLookup.Name(error);
}

/**
 * time 昇順の key 列で time の挿入位置を二分探索で返す (lower_bound)。
 *
 * @details 同 time の key があればその index を返し、呼び出し側の上書き処理に流す。
 * @param keys time 昇順に並んだ key 列。
 * @param time 挿入位置を求める時刻 (秒)。
 * @return time 以上の最初の key の index。
 */
static u32 LowerBoundByTime(const TArray<FCurveKey>& keys, f32 time) noexcept {
    u32 lo = 0;
    u32 hi = static_cast<u32>(keys.Num());
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (keys[mid].time < time) lo = mid + 1u;
        else                       hi = mid;
    }
    return lo;
}

FAnimationCurveResult FAnimationCurve::TryAddKey(
    f32 time, f32 value, ECurveInterpolation interp) noexcept {
    FAnimationCurveResult result{};
    result.key_count = static_cast<u32>(m_Keys.Num());
    if (!IsFiniteCurveValue(time) || !IsFiniteCurveValue(value)) {
        result.error = EAnimationCurveError::NonFiniteValue;
        return result;
    }
    if (!IsValidInterpolation(interp)) {
        result.error = EAnimationCurveError::InvalidInterpolation;
        return result;
    }

    const u32 pos = LowerBoundByTime(m_Keys, time);
    if (pos < m_Keys.Num() && m_Keys[pos].time == time) {
        m_Keys[pos].value      = value;
        m_Keys[pos].out_interp = interp;
        result.key_index = pos;
        return result;
    }
    if (m_Keys.Num() >= kMaxKeys) {
        result.error = EAnimationCurveError::TooManyKeys;
        return result;
    }

    FCurveKey k;
    k.time        = time;
    k.value       = value;
    k.in_interp   = interp;
    k.out_interp  = interp;
    k.in_tangent  = 0.0f;
    k.out_tangent = 0.0f;
    if (!m_Keys.TryAdd(k)) {
        result.error = EAnimationCurveError::AllocationFailure;
        return result;
    }

    for (u32 i = static_cast<u32>(m_Keys.Num()) - 1u; i > pos; --i) {
        const FCurveKey tmp = m_Keys[i];
        m_Keys[i]     = m_Keys[i - 1u];
        m_Keys[i - 1u] = tmp;
    }
    result.key_index = pos;
    result.key_count = static_cast<u32>(m_Keys.Num());
    return result;
}

void FAnimationCurve::AddKey(
    f32 time, f32 value, ECurveInterpolation interp) noexcept {
    (void)TryAddKey(time, value, interp);
}

FAnimationCurveResult FAnimationCurve::TryAddKeyHermite(
    f32 time, f32 value,
    f32 in_tangent, f32 out_tangent) noexcept {
    FAnimationCurveResult result{};
    result.key_count = static_cast<u32>(m_Keys.Num());
    if (!IsFiniteCurveValue(time) || !IsFiniteCurveValue(value) ||
        !IsFiniteCurveValue(in_tangent) ||
        !IsFiniteCurveValue(out_tangent)) {
        result.error = EAnimationCurveError::NonFiniteValue;
        return result;
    }

    const u32 pos = LowerBoundByTime(m_Keys, time);
    if (pos < m_Keys.Num() && m_Keys[pos].time == time) {
        m_Keys[pos].value       = value;
        m_Keys[pos].in_tangent  = in_tangent;
        m_Keys[pos].out_tangent = out_tangent;
        m_Keys[pos].in_interp   = ECurveInterpolation::Hermite;
        m_Keys[pos].out_interp  = ECurveInterpolation::Hermite;
        result.key_index = pos;
        return result;
    }
    if (m_Keys.Num() >= kMaxKeys) {
        result.error = EAnimationCurveError::TooManyKeys;
        return result;
    }

    FCurveKey k;
    k.time        = time;
    k.value       = value;
    k.in_tangent  = in_tangent;
    k.out_tangent = out_tangent;
    k.in_interp   = ECurveInterpolation::Hermite;
    k.out_interp  = ECurveInterpolation::Hermite;
    if (!m_Keys.TryAdd(k)) {
        result.error = EAnimationCurveError::AllocationFailure;
        return result;
    }

    for (u32 i = static_cast<u32>(m_Keys.Num()) - 1u; i > pos; --i) {
        const FCurveKey tmp = m_Keys[i];
        m_Keys[i]      = m_Keys[i - 1u];
        m_Keys[i - 1u] = tmp;
    }
    result.key_index = pos;
    result.key_count = static_cast<u32>(m_Keys.Num());
    return result;
}

void FAnimationCurve::AddKeyHermite(
    f32 time, f32 value,
    f32 in_tangent, f32 out_tangent) noexcept {
    (void)TryAddKeyHermite(time, value, in_tangent, out_tangent);
}

FAnimationCurveResult FAnimationCurve::TrySetKeys(
    const FCurveKey* keys, u32 count,
    EWrapMode pre_wrap, EWrapMode post_wrap) noexcept {
    FAnimationCurveResult result{};
    result.key_count = count;
    if (count > kMaxKeys) {
        result.error = EAnimationCurveError::TooManyKeys;
        return result;
    }
    if (count != 0u && keys == nullptr) {
        result.error = EAnimationCurveError::NullKeys;
        return result;
    }
    if (!IsValidWrapMode(pre_wrap) || !IsValidWrapMode(post_wrap)) {
        result.error = EAnimationCurveError::InvalidWrapMode;
        return result;
    }

    for (u32 i = 0u; i < count; ++i) {
        result.key_index = i;
        const FCurveKey& key = keys[i];
        if (!IsFiniteCurveValue(key.time) ||
            !IsFiniteCurveValue(key.value) ||
            !IsFiniteCurveValue(key.in_tangent) ||
            !IsFiniteCurveValue(key.out_tangent)) {
            result.error = EAnimationCurveError::NonFiniteValue;
            return result;
        }
        if (!IsValidInterpolation(key.in_interp) ||
            !IsValidInterpolation(key.out_interp)) {
            result.error = EAnimationCurveError::InvalidInterpolation;
            return result;
        }
        if (i != 0u) {
            if (key.time < keys[i - 1u].time) {
                result.error = EAnimationCurveError::UnsortedKeys;
                return result;
            }
            if (key.time == keys[i - 1u].time) {
                result.error = EAnimationCurveError::DuplicateKeyTime;
                return result;
            }
        }
    }

    TArray<FCurveKey> staged(*m_Keys.GetAllocator());
    if (!staged.TrySetNum(count)) {
        result.error = EAnimationCurveError::AllocationFailure;
        return result;
    }
    for (u32 i = 0u; i < count; ++i) staged[i] = keys[i];

    m_Keys = Move(staged);
    m_PreWrap = pre_wrap;
    m_PostWrap = post_wrap;
    result.key_index = count == 0u ? 0u : count - 1u;
    return result;
}

FAnimationCurveResult FAnimationCurve::TrySetEasingPreset(
    Easing::EEasingType type, u32 sample_count) noexcept {
    FAnimationCurveResult result{};
    result.key_count = static_cast<u32>(m_Keys.Num());
    if (sample_count < 2u || sample_count > kMaxEasingPresetSamples) {
        result.error = EAnimationCurveError::InvalidSampleCount;
        return result;
    }
    if (Easing::GetFunction(type) == nullptr) {
        result.error = EAnimationCurveError::InvalidEasingType;
        return result;
    }

    TArray<FCurveKey> staged(*m_Keys.GetAllocator());
    if (!staged.TrySetNum(sample_count)) {
        result.error = EAnimationCurveError::AllocationFailure;
        return result;
    }

    const f32 denominator = static_cast<f32>(sample_count - 1u);
    for (u32 index = 0u; index < sample_count; ++index) {
        result.key_index = index;
        const f32 time = static_cast<f32>(index) / denominator;
        f32 value = 0.0f;
        const Easing::FEasingResult easing_result =
            Easing::TryEvaluate(type, time, value);
        if (!easing_result.Succeeded() || !IsFiniteCurveValue(value)) {
            result.error = EAnimationCurveError::InvalidEasingType;
            return result;
        }

        FCurveKey& key = staged[index];
        key.time = time;
        key.value = value;
        key.in_tangent = 0.0f;
        key.out_tangent = 0.0f;
        key.in_interp = ECurveInterpolation::Linear;
        key.out_interp = ECurveInterpolation::Linear;
    }

    m_Keys = Move(staged);
    m_PreWrap = EWrapMode::Clamp;
    m_PostWrap = EWrapMode::Clamp;
    result.key_index = sample_count - 1u;
    result.key_count = sample_count;
    return result;
}

void FAnimationCurve::RemoveKey(u32 index) noexcept {
    if (index >= m_Keys.Num()) return;
    // 順序を保つため左詰め (TArray::RemoveAtSwap は順序を崩すので使えない)
    for (u32 i = index; i + 1u < m_Keys.Num(); ++i) {
        m_Keys[i] = m_Keys[i + 1u];
    }
    m_Keys.Pop();
}

void FAnimationCurve::ClearKeys() noexcept {
    m_Keys.Reset();
}

FAnimationCurveResult FAnimationCurve::TrySetWrapModes(
    EWrapMode pre_wrap, EWrapMode post_wrap) noexcept {
    FAnimationCurveResult result{};
    result.key_count = static_cast<u32>(m_Keys.Num());
    if (!IsValidWrapMode(pre_wrap) || !IsValidWrapMode(post_wrap)) {
        result.error = EAnimationCurveError::InvalidWrapMode;
        return result;
    }
    m_PreWrap = pre_wrap;
    m_PostWrap = post_wrap;
    return result;
}

void FAnimationCurve::SetPreWrap(EWrapMode mode) noexcept {
    (void)TrySetWrapModes(mode, m_PostWrap);
}

void FAnimationCurve::SetPostWrap(EWrapMode mode) noexcept {
    (void)TrySetWrapModes(m_PreWrap, mode);
}

f32 FAnimationCurve::Duration() const noexcept {
    // 仕様: 「末尾 key の time」を返す。key 0 個では 0。
    // (1 個でも「t==末尾 key.time」を返すことで Evaluate(Duration()) が末尾値になる)
    if (m_Keys.Num() == 0u) return 0.0f;
    return m_Keys[m_Keys.Num() - 1u].time;
}

f32 FAnimationCurve::ApplyWrap(f32 time) const noexcept {
    if (m_Keys.Num() < 2u) return time;

    const f32 t0  = m_Keys[0].time;
    const f32 t1  = m_Keys[m_Keys.Num() - 1u].time;
    const f32 dur = t1 - t0;
    if (dur <= 0.0f) return t0;

    if (time < t0) {
        switch (m_PreWrap) {
        case EWrapMode::Clamp:    return t0;
        case EWrapMode::Loop: {
            // (t0 - time) 分だけ右に折り返す。`Mod` は負数で実装差があるため
            // 必ず正の値を渡す形にする。
            const f32 diff   = t0 - time;
            const f32 cycles = Mod(diff, dur);
            return t1 - cycles;   // cycles==0 → t1, cycles→dur に近い → t0+
        }
        case EWrapMode::PingPong: {
            const f32 diff   = t0 - time;
            const f32 period = dur * 2.0f;
            f32 m = Mod(diff, period);
            if (m < 0.0f) m += period;
            // [0, dur] は逆方向に進む (= t0 + m)
            // (dur, period) は順方向に戻る (= t1 - (m - dur))
            return m <= dur ? (t0 + m) : (t1 - (m - dur));
        }
        }
        return t0;
    }

    if (time > t1) {
        switch (m_PostWrap) {
        case EWrapMode::Clamp:    return t1;
        case EWrapMode::Loop: {
            const f32 diff   = time - t1;
            const f32 cycles = Mod(diff, dur);
            return t0 + cycles;
        }
        case EWrapMode::PingPong: {
            const f32 diff   = time - t1;
            const f32 period = dur * 2.0f;
            f32 m = Mod(diff, period);
            if (m < 0.0f) m += period;
            // [0, dur] は逆走 (= t1 - m), (dur, period) は順走 (= t0 + (m - dur))
            return m <= dur ? (t1 - m) : (t0 + (m - dur));
        }
        }
        return t1;
    }

    return time;   // 定義域内
}

u32 FAnimationCurve::FindSegmentIndex(f32 time) const noexcept {
    // 戻り値は左端 key の index。呼び出し側は m_Keys.Size() >= 2 を保証する。
    const u32 n = static_cast<u32>(m_Keys.Num());
    if (time <= m_Keys[0].time)         return 0u;
    if (time >= m_Keys[n - 1u].time)    return n - 2u;

    // 二分探索: m_Keys[lo].time <= time < m_Keys[lo+1].time となる lo
    u32 lo = 0;
    u32 hi = n - 1u;
    while (lo + 1u < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (m_Keys[mid].time <= time) lo = mid;
        else                          hi = mid;
    }
    return lo;
}

f32 FAnimationCurve::InterpolateSegment(const FCurveKey& k0, const FCurveKey& k1,
                                       f32 t, f32 dt) noexcept {
    switch (k0.out_interp) {
    case ECurveInterpolation::Step:
        return k0.value;
    case ECurveInterpolation::Linear:
        return k0.value + (k1.value - k0.value) * t;
    case ECurveInterpolation::Hermite: {
        // 3 次 Hermite。タンジェントは「単位 1 秒あたりの傾き」を保存している
        // ため、segment 長 dt で乗算してこの segment 内の傾きにスケールする。
        const f32 t2  = t * t;
        const f32 t3  = t2 * t;
        const f32 h00 =  2.0f * t3 - 3.0f * t2 + 1.0f;
        const f32 h10 =         t3 - 2.0f * t2 + t;
        const f32 h01 = -2.0f * t3 + 3.0f * t2;
        const f32 h11 =         t3 -        t2;
        const f32 m0  = k0.out_tangent * dt;
        const f32 m1  = k1.in_tangent  * dt;
        return h00 * k0.value + h10 * m0 + h01 * k1.value + h11 * m1;
    }
    }
    return k0.value;   // 想定外 enum 値の保険
}

FAnimationCurveResult FAnimationCurve::TryEvaluate(
    f32 time, f32& out_value) const noexcept {
    FAnimationCurveResult result{};
    const u32 n = static_cast<u32>(m_Keys.Num());
    result.key_count = n;
    if (!IsFiniteCurveValue(time)) {
        result.error = EAnimationCurveError::NonFiniteValue;
        return result;
    }
    if (n == 0u) {
        out_value = 0.0f;
        return result;
    }
    if (n == 1u) {
        out_value = m_Keys[0].value;
        return result;
    }

    const f32 wrapped = ApplyWrap(time);
    if (!IsFiniteCurveValue(wrapped)) {
        result.error = EAnimationCurveError::ResultOutOfRange;
        return result;
    }
    if (wrapped <= m_Keys[0].time) {
        out_value = m_Keys[0].value;
        return result;
    }
    if (wrapped >= m_Keys[n - 1u].time) {
        result.key_index = n - 1u;
        out_value = m_Keys[n - 1u].value;
        return result;
    }

    const u32 i       = FindSegmentIndex(wrapped);
    result.key_index = i;
    const FCurveKey& k0 = m_Keys[i];
    const FCurveKey& k1 = m_Keys[i + 1u];
    const f32 dt = k1.time - k0.time;
    const f32 offset = wrapped - k0.time;
    if (!IsFiniteCurveValue(dt) || dt <= 0.0f ||
        !IsFiniteCurveValue(offset)) {
        result.error = EAnimationCurveError::ResultOutOfRange;
        return result;
    }
    const f32 normalized = offset / dt;
    if (!IsFiniteCurveValue(normalized)) {
        result.error = EAnimationCurveError::ResultOutOfRange;
        return result;
    }
    const f32 value = InterpolateSegment(k0, k1, normalized, dt);
    if (!IsFiniteCurveValue(value)) {
        result.error = EAnimationCurveError::ResultOutOfRange;
        return result;
    }
    out_value = value;
    return result;
}

f32 FAnimationCurve::Evaluate(f32 time) const noexcept {
    f32 value = 0.0f;
    return TryEvaluate(time, value).Succeeded() ? value : 0.0f;
}

} // namespace acs::game
