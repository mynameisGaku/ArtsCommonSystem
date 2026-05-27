// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — 決定論的 PRNG (xoshiro128**)
//
// 設計判断:
//   - <random> 不使用 (ACS STL 禁止方針)。
//   - xoshiro128** を採用: 4×u32 state / 128bit period / BigCrush パス /
//     1 出力あたり ~3 cycle。一般用途 (gameplay, particle, shuffle 等) の
//     決定論再現性とゲーム要求の品質を両立し、暗号用途は対象外。
//   - SplitMix64 を seed 拡散専用に使用: 1 個の u64 seed から 4 個の u32
//     state を埋める。SplitMix は 0 seed を引っ張らず、低エントロピ入力
//     (例: time tick) でも初期状態のビット分布が良好。
//   - 非コピー / 非ムーブ: state を持ち回しても意味的に意義が薄く、
//     コピー検出を強制したいので削除。
//   - 全 noexcept: 失敗パスを持たない (state は内部固定、外部 IO なし)。
//
// 使い方:
//   acs::game::FRandom r(0x12345678ULL);
//   f32 t = r.NextF32Unit();                 // [0,1)
//   i32 d = r.RangeInt(1, 6);                // dice roll
//   acs::FVec2 p = r.PointInCircle(50.0f);    // 円板内一様
//   acs::game::FRandom::Global().NextBool();  // 簡易ユースケース
//
// 注意:
//   - Global() はプロセス内で 1 個の静的インスタンス。マルチスレッドからの
//     呼び出しは未保護 (ACS 規約: 別スレッドは別 FRandom を持つこと)。
//   - 時刻 seed は acs::Clock::Ticks() (`platform/Time.h`) を利用する。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

class FRandom {
public:
    // 既定: 起動時刻ベースで seed する。決定論再現が必要なら u64 を明示。
    FRandom() noexcept;
    explicit FRandom(u64 seed) noexcept;

    // 非コピー / 非ムーブ (state は 1 箇所にとどめる)
    FRandom(const FRandom&)            = delete;
    FRandom& operator=(const FRandom&) = delete;
    FRandom(FRandom&&)                 = delete;
    FRandom& operator=(FRandom&&)      = delete;

    // 任意の u64 で再 seed (SplitMix64 で 4 個の u32 に拡散)
    void Seed(u64 seed) noexcept;

    // ---- raw 出力 ----------------------------------------------------------
    // xoshiro128** から 32bit を 1 個取り出す。全 API の最下層。
    u32 NextU32() noexcept;

    // ---- 一般ユース --------------------------------------------------------
    // [0, 1) の f32。上位 24bit を仮数に詰めて 0x1p-24 でスケール。
    f32 NextF32Unit() noexcept;

    // [min, max] inclusive。max < min なら swap して扱う。
    i32 RangeInt(i32 min, i32 max) noexcept;

    // [min, max) の f32。NaN/inf は呼び出し側責務。
    f32 RangeF32(f32 min, f32 max) noexcept;

    // true_probability を [0,1] でクランプして bool を返す。
    bool NextBool(f32 true_probability = 0.5f) noexcept;

    // 円板内一様サンプル (rejection sampling: 2x2 box → 円内採用)。
    // 一様性は完璧、平均試行 ~4/π ≒ 1.27 回で polar 公式より速いことが多い。
    FVec2 PointInCircle(f32 radius = 1.0f) noexcept;

    // 円周上一様サンプル (角度 [0, 2π) から sin/cos)。
    FVec2 PointOnCircle(f32 radius = 1.0f) noexcept;

    // Fisher-Yates シャッフル (in-place, O(n))。TArray の Size()/[] のみ要求。
    template<typename T>
    void Shuffle(TArray<T>& a) noexcept;

    // 重み付き index 選択。weights は非負想定、合計が 0 なら 0 を返す。
    // O(count) の累積和方式。count==0 のときは 0 を返す (defensive)。
    u32 WeightedChoice(const f32* weights, u32 count) noexcept;

    // プロセス内シングルトン (lazy init、時刻 seed)。スレッド安全性は呼び出し側責務。
    static FRandom& Global() noexcept;

private:
    // xoshiro128** state: 4 個の u32、全 0 は禁止 (SplitMix64 が回避)。
    u32 m_S0 = 0;
    u32 m_S1 = 0;
    u32 m_S2 = 0;
    u32 m_S3 = 0;
};

// ---------------------------------------------------------------------------
// template 実装: ヘッダで完結させる (TArray<T> のインスタンス化を許す)
// ---------------------------------------------------------------------------
template<typename T>
void FRandom::Shuffle(TArray<T>& a) noexcept {
    const usize n = a.Size();
    if (n < 2) return;
    // i = n-1 ... 1 の各位置に対し、[0, i] の一様乱数 j を選び swap。
    // 0 < n は ACS TArray が usize の正値を返す前提。
    for (usize i = n - 1; i > 0; --i) {
        // RangeInt は i32 を返すので usize にキャストして比較。i は 32bit 範囲想定。
        const u32 j = NextU32() % static_cast<u32>(i + 1u);
        if (static_cast<usize>(j) != i) {
            T tmp     = static_cast<T&&>(a[i]);
            a[i]      = static_cast<T&&>(a[j]);
            a[j]      = static_cast<T&&>(tmp);
        }
    }
}

} // namespace acs::game
