// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — 決定論的 PRNG (xoshiro128**) 実装
//
// アルゴリズム出典:
//   - xoshiro128** : Blackman & Vigna, "Scrambled linear pseudorandom number
//     generators" (2018). 4×u32 state, period 2^128 - 1, BigCrush パス。
//   - SplitMix64   : Steele et al., "Fast Splittable Pseudorandom Number
//     Generators" (OOPSLA 2014). 任意の u64 seed を独立性の高い 64bit ストリーム
//     に展開し、xoshiro state の bootstrap に最適。
#include "gameframework/Random.h"

#include "math/Math.h"        // Sqrt / Sin / Cos / kTwoPi
#include "platform/Time.h"    // Clock::Ticks

namespace acs::game {

namespace {

// SplitMix64: 1 step で次の 64bit を生成。0 seed でも分布が偏らないのが利点。
inline u64 SplitMix64Step(u64& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;          // golden ratio constant
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// 32bit 左ローテート (xoshiro128** のコアプリミティブ)
inline u32 Rotl32(u32 x, int k) noexcept {
    return (x << k) | (x >> (32 - k));
}

} // namespace

// ---------------------------------------------------------------------------
// 構築 / 種まき
// ---------------------------------------------------------------------------
FRandom::FRandom() noexcept {
    // 時刻 (QPC tick) を seed に使う。FrameTimer 起動順に依存せず単独で使える。
    Seed(Clock::Ticks());
}

FRandom::FRandom(u64 seed) noexcept {
    Seed(seed);
}

void FRandom::Seed(u64 seed) noexcept {
    // SplitMix64 で 2 個の u64 を取り、4 個の u32 に分解。
    // seed=0 のときも SplitMix の constant 加算で z≠0 が保証され、
    // xoshiro の "全 0 state 禁止" 制約も自動的に満たされる。
    u64 sm = seed;
    const u64 a = SplitMix64Step(sm);
    const u64 b = SplitMix64Step(sm);
    m_S0 = static_cast<u32>(a & 0xFFFFFFFFULL);
    m_S1 = static_cast<u32>(a >> 32);
    m_S2 = static_cast<u32>(b & 0xFFFFFFFFULL);
    m_S3 = static_cast<u32>(b >> 32);
    // 保険: 4 個全部 0 (理論上 SplitMix 経由では発生しないが) のときは固定値で回避。
    if ((m_S0 | m_S1 | m_S2 | m_S3) == 0u) {
        m_S0 = 0xDEADBEEFu;
        m_S1 = 0xCAFEBABEu;
        m_S2 = 0xFEEDF00Du;
        m_S3 = 0x12345678u;
    }
}

// ---------------------------------------------------------------------------
// raw 出力 (xoshiro128**)
// ---------------------------------------------------------------------------
u32 FRandom::NextU32() noexcept {
    // xoshiro128** の "**" バリアント: state の線形進行に
    // 非線形スクランブラを乗せて高品質 32bit を出力。
    const u32 result = Rotl32(m_S1 * 5u, 7) * 9u;

    const u32 t = m_S1 << 9;
    m_S2 ^= m_S0;
    m_S3 ^= m_S1;
    m_S1 ^= m_S2;
    m_S0 ^= m_S3;
    m_S2 ^= t;
    m_S3 = Rotl32(m_S3, 11);

    return result;
}

// ---------------------------------------------------------------------------
// 一般ユース
// ---------------------------------------------------------------------------
f32 FRandom::NextF32Unit() noexcept {
    // 上位 24bit を仮数に: f32 の mantissa は 23bit + implicit 1 で 24bit 精度。
    // (x >> 8) で 0..(2^24 - 1)、× 2^-24 で [0, 1)。1.0 を絶対に返さない。
    const u32 bits = NextU32() >> 8;
    return static_cast<f32>(bits) * (1.0f / 16777216.0f); // 1/2^24
}

i32 FRandom::RangeInt(i32 min, i32 max) noexcept {
    // max < min は swap して扱う (defensive、API の使い心地優先)。
    if (max < min) {
        const i32 t = min; min = max; max = t;
    }
    // span = max - min + 1, 1 以上が保証される。
    // (max - min) は最大 2^31-1-(-2^31)=2^32-1 で u32 に収まるよう u32 で計算。
    const u32 span = static_cast<u32>(max - min) + 1u;
    if (span == 0u) {
        // span オーバーフロー (min=INT32_MIN, max=INT32_MAX)。
        // フル 32bit 範囲を返してよい。
        return static_cast<i32>(NextU32());
    }
    // Lemire の高速 unbiased 法でなく、用途的にバイアス無視できる剰余方式を採用。
    // (gameplay 用途では 2^32 / span の偏りは検知不能)
    const u32 r = NextU32() % span;
    return min + static_cast<i32>(r);
}

f32 FRandom::RangeF32(f32 min, f32 max) noexcept {
    // [min, max): max==min なら min 固定、min>max は呼び出し側の bug 扱いだが
    // 線形補間で動作はする ([max, min) を返すことになる)。
    return min + (max - min) * NextF32Unit();
}

bool FRandom::NextBool(f32 true_probability) noexcept {
    // 範囲外はクランプ。0 → 常に false、1 → 常に true (浮動小数の境界に注意:
    // NextF32Unit() < 1.0f が保証されているので 1.0f との比較で常に true)。
    if (true_probability <= 0.0f) return false;
    if (true_probability >= 1.0f) return true;
    return NextF32Unit() < true_probability;
}

FVec2 FRandom::PointInCircle(f32 radius) noexcept {
    // Rejection sampling: [-1,1]^2 box でサンプル、単位円内に入るまで再試行。
    // 一様性は厳密。平均試行回数は 4/π ≒ 1.273 回 (期待値)、最大は確率的に小。
    // ループ脱出保証: bool 評価が常に確定する有限プロセス (ハード上限を持たない
    //   のは PRNG が同一値を無限連続返さないという仮定下、xoshiro なら担保)。
    for (;;) {
        const f32 x = NextF32Unit() * 2.0f - 1.0f;
        const f32 y = NextF32Unit() * 2.0f - 1.0f;
        const f32 d2 = x * x + y * y;
        if (d2 <= 1.0f) {
            return FVec2{ x * radius, y * radius };
        }
    }
}

FVec2 FRandom::PointOnCircle(f32 radius) noexcept {
    // 角度を [0, 2π) で一様にサンプル、(cos, sin) で円周上の点へ。
    const f32 theta = NextF32Unit() * kTwoPi;
    return FVec2{ Cos(theta) * radius, Sin(theta) * radius };
}

u32 FRandom::WeightedChoice(const f32* weights, u32 count) noexcept {
    if (count == 0u || weights == nullptr) return 0u;
    // 累積和を取り、[0, total) の一様乱数で線形探索。
    f32 total = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        // 負値は 0 として扱う (defensive、署名上は f32 だが NaN は呼び出し側責務)。
        const f32 w = weights[i] > 0.0f ? weights[i] : 0.0f;
        total += w;
    }
    if (total <= 0.0f) return 0u; // 全 0 → 先頭 index を返す
    const f32 pick = NextF32Unit() * total;
    f32 acc = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        const f32 w = weights[i] > 0.0f ? weights[i] : 0.0f;
        acc += w;
        if (pick < acc) return i;
    }
    // 浮動小数誤差で末端を超えたとき: 最後の index にフォールバック。
    return count - 1u;
}

FRandom& FRandom::Global() noexcept {
    // C++11 magic statics でスレッド安全な lazy init。
    // 用途上、複数スレッドからの並行呼び出しは未保護 (上記コメント参照)。
    static FRandom s_global{};
    return s_global;
}

} // namespace acs::game
