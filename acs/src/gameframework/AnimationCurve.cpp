// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FAnimationCurve 実装 (Phase 3)
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
#include "math/Math.h"

namespace acs::game {

// time 昇順を保ったまま挿入位置を返す (= 二分探索の lower_bound)
// 同 time の key があればその index を返し、上書き処理に流す。
static u32 LowerBoundByTime(const TArray<CurveKey>& keys, f32 time) noexcept {
    u32 lo = 0;
    u32 hi = static_cast<u32>(keys.Size());
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (keys[mid].time < time) lo = mid + 1u;
        else                       hi = mid;
    }
    return lo;
}

void FAnimationCurve::AddKey(f32 time, f32 value, ECurveInterpolation interp) noexcept {
    const u32 pos = LowerBoundByTime(m_Keys, time);
    // 同 time の key が既にある → 上書き (in_interp は維持して out_interp と value のみ更新)
    if (pos < m_Keys.Size() && m_Keys[pos].time == time) {
        m_Keys[pos].value      = value;
        m_Keys[pos].out_interp = interp;
        return;
    }

    // 末尾に追加してから後ろ向きに 1 個ずつ swap する (TArray は中間 insert を
    // 提供しないため。N が大きくない前提 = キー打ち数百個までを想定)
    CurveKey k;
    k.time        = time;
    k.value       = value;
    k.in_interp   = interp;
    k.out_interp  = interp;
    k.in_tangent  = 0.0f;
    k.out_tangent = 0.0f;
    m_Keys.PushBack(k);

    // バブルダウン: 直前要素より時間が小さければ swap (= sorted insert)
    for (u32 i = static_cast<u32>(m_Keys.Size()) - 1u; i > pos; --i) {
        const CurveKey tmp = m_Keys[i];
        m_Keys[i]     = m_Keys[i - 1u];
        m_Keys[i - 1u] = tmp;
    }
}

void FAnimationCurve::AddKeyHermite(f32 time, f32 value,
                                   f32 in_tangent, f32 out_tangent) noexcept {
    const u32 pos = LowerBoundByTime(m_Keys, time);
    if (pos < m_Keys.Size() && m_Keys[pos].time == time) {
        m_Keys[pos].value       = value;
        m_Keys[pos].in_tangent  = in_tangent;
        m_Keys[pos].out_tangent = out_tangent;
        m_Keys[pos].in_interp   = ECurveInterpolation::Hermite;
        m_Keys[pos].out_interp  = ECurveInterpolation::Hermite;
        return;
    }

    CurveKey k;
    k.time        = time;
    k.value       = value;
    k.in_tangent  = in_tangent;
    k.out_tangent = out_tangent;
    k.in_interp   = ECurveInterpolation::Hermite;
    k.out_interp  = ECurveInterpolation::Hermite;
    m_Keys.PushBack(k);

    for (u32 i = static_cast<u32>(m_Keys.Size()) - 1u; i > pos; --i) {
        const CurveKey tmp = m_Keys[i];
        m_Keys[i]      = m_Keys[i - 1u];
        m_Keys[i - 1u] = tmp;
    }
}

void FAnimationCurve::RemoveKey(u32 index) noexcept {
    if (index >= m_Keys.Size()) return;
    // 順序を保つため左詰め (TArray::RemoveAtSwap は順序を崩すので使えない)
    for (u32 i = index; i + 1u < m_Keys.Size(); ++i) {
        m_Keys[i] = m_Keys[i + 1u];
    }
    m_Keys.PopBack();
}

void FAnimationCurve::ClearKeys() noexcept {
    m_Keys.Clear();
}

f32 FAnimationCurve::Duration() const noexcept {
    // 仕様: 「末尾 key の time」を返す。key 0 個では 0。
    // (1 個でも「t==末尾 key.time」を返すことで Evaluate(Duration()) が末尾値になる)
    if (m_Keys.Size() == 0u) return 0.0f;
    return m_Keys[m_Keys.Size() - 1u].time;
}

f32 FAnimationCurve::ApplyWrap(f32 time) const noexcept {
    if (m_Keys.Size() < 2u) return time;

    const f32 t0  = m_Keys[0].time;
    const f32 t1  = m_Keys[m_Keys.Size() - 1u].time;
    const f32 dur = t1 - t0;
    if (dur <= 0.0f) return t0;

    if (time < t0) {
        switch (m_PreWrap) {
        case WrapMode::Clamp:    return t0;
        case WrapMode::Loop: {
            // (t0 - time) 分だけ右に折り返す。`Mod` は負数で実装差があるため
            // 必ず正の値を渡す形にする。
            const f32 diff   = t0 - time;
            const f32 cycles = Mod(diff, dur);
            return t1 - cycles;   // cycles==0 → t1, cycles→dur に近い → t0+
        }
        case WrapMode::PingPong: {
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
        case WrapMode::Clamp:    return t1;
        case WrapMode::Loop: {
            const f32 diff   = time - t1;
            const f32 cycles = Mod(diff, dur);
            return t0 + cycles;
        }
        case WrapMode::PingPong: {
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
    const u32 n = static_cast<u32>(m_Keys.Size());
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

f32 FAnimationCurve::InterpolateSegment(const CurveKey& k0, const CurveKey& k1,
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

f32 FAnimationCurve::Evaluate(f32 time) const noexcept {
    const u32 n = static_cast<u32>(m_Keys.Size());
    if (n == 0u) return 0.0f;
    if (n == 1u) return m_Keys[0].value;

    const f32 wrapped = ApplyWrap(time);
    const u32 i       = FindSegmentIndex(wrapped);
    const CurveKey& k0 = m_Keys[i];
    const CurveKey& k1 = m_Keys[i + 1u];
    const f32 dt = k1.time - k0.time;
    if (dt <= 0.0f) return k0.value;   // 退化 segment (同 time の key が混入した保険)
    const f32 t  = (wrapped - k0.time) / dt;
    return InterpolateSegment(k0, k1, t, dt);
}

} // namespace acs::game
