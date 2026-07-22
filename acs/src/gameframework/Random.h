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
//   - 時刻 seed は acs::FClock::Ticks() (`platform/Time.h`) を利用する。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 決定論的 PRNG (xoshiro128**)。
 *
 * @details
 * 4×u32 state / 128bit period の xoshiro128** を採用し、gameplay・particle・
 * shuffle 等の用途で決定論的な再現性を提供する (暗号用途は対象外)。seed の拡散
 * には SplitMix64 を使い、低エントロピ入力でも初期状態のビット分布を良好に保つ。
 * 非コピー / 非ムーブで state を 1 箇所にとどめ、全 API noexcept。
 */
class FRandom {
public:
    /** 起動時刻 (FClock::Ticks) を seed にして構築する。 */
    FRandom() noexcept;

    /**
     * 明示した u64 seed で構築する (決定論再現用)。
     *
     * @param seed 初期化に使う seed。
     */
    explicit FRandom(u64 seed) noexcept;

    /** コピー禁止 (state を 1 箇所にとどめるため)。 */
    FRandom(const FRandom&)            = delete;

    /** コピー代入も禁止。 */
    FRandom& operator=(const FRandom&) = delete;

    /** ムーブ禁止 (state を 1 箇所にとどめるため)。 */
    FRandom(FRandom&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FRandom& operator=(FRandom&&)      = delete;

    /**
     * 任意の u64 で再 seed する (SplitMix64 で 4 個の u32 state に拡散)。
     *
     * @param seed 拡散元の seed (0 でも全 0 state にはならない)。
     */
    void Seed(u64 seed) noexcept;

    /**
     * xoshiro128** から 32bit を 1 個取り出す (全 API の最下層)。
     *
     * @return 生成した 32bit 乱数。
     */
    u32 NextU32() noexcept;

    /**
     * [0, 1) の f32 を返す。
     *
     * @details 上位 24bit を仮数に詰めて 1/2^24 でスケールするため 1.0 は返さない。
     * @return [0, 1) の一様乱数。
     */
    f32 NextF32Unit() noexcept;

    /**
     * [min, max] inclusive の整数を返す。
     *
     * @details max < min なら swap して扱う。剰余方式 (gameplay 用途では偏り無視)。
     * @param min 範囲の下限 (含む)。
     * @param max 範囲の上限 (含む)。
     * @return [min, max] の一様整数。
     */
    i32 RangeInt(i32 min, i32 max) noexcept;

    /**
     * [min, max) の f32 を返す。
     *
     * @details NaN/inf 入力は呼び出し側責務。min>max でも線形補間で動作はする。
     * @param min 範囲の下限 (含む)。
     * @param max 範囲の上限 (含まない)。
     * @return [min, max) の一様乱数。
     */
    f32 RangeF32(f32 min, f32 max) noexcept;

    /**
     * 確率 true_probability で true を返す。
     *
     * @details true_probability は [0,1] にクランプ。0 で常に false、1 で常に true。
     * @param true_probability true を返す確率 (既定 0.5)。
     * @return 抽選結果の bool。
     */
    bool NextBool(f32 true_probability = 0.5f) noexcept;

    /**
     * 円板内の一様な点をサンプルする。
     *
     * @details rejection sampling ([-1,1]^2 box で採り円内に入るまで再試行)。一様性は
     * 厳密で平均試行 ~4/π ≒ 1.27 回。
     * @param radius 円板の半径 (既定 1.0)。
     * @return 半径 radius の円板内の点。
     */
    FVec2 PointInCircle(f32 radius = 1.0f) noexcept;

    /**
     * 円周上の一様な点をサンプルする。
     *
     * @details 角度を [0, 2π) で一様に取り (cos, sin) で円周上へ写す。
     * @param radius 円の半径 (既定 1.0)。
     * @return 半径 radius の円周上の点。
     */
    FVec2 PointOnCircle(f32 radius = 1.0f) noexcept;

    /**
     * 配列を Fisher-Yates シャッフルする (in-place, O(n))。
     *
     * @details TArray の Size()/operator[] のみ要求する。
     * @tparam T 配列要素型 (ムーブ可能であること)。
     * @param a シャッフル対象の配列 (in-place で並べ替える)。
     */
    template<typename T>
    void Shuffle(TArray<T>& a) noexcept;

    /**
     * 重みに比例して index を選択する。
     *
     * @details 累積和方式 O(count)。負の重みは 0 扱い。合計が 0 / count==0 なら 0 を返す。
     * @param weights 各 index の重み配列 (非負想定)。
     * @param count weights の要素数。
     * @return 選ばれた index ([0, count))。
     */
    u32 WeightedChoice(const f32* weights, u32 count) noexcept;

    /**
     * プロセス内シングルトンを返す (lazy init、時刻 seed)。
     *
     * @details スレッド安全性は呼び出し側責務 (別スレッドは別 FRandom を持つこと)。
     * @return プロセス共有の FRandom インスタンス。
     */
    static FRandom& Global() noexcept;

private:
    /** xoshiro128** state[0] (全 0 state は禁止、SplitMix64 が回避)。 */
    u32 m_S0 = 0;

    /** xoshiro128** state[1]。 */
    u32 m_S1 = 0;

    /** xoshiro128** state[2]。 */
    u32 m_S2 = 0;

    /** xoshiro128** state[3]。 */
    u32 m_S3 = 0;
};

/**
 * 配列を Fisher-Yates シャッフルする (in-place, O(n))。
 *
 * @tparam T 配列要素型 (ムーブ可能であること)。
 * @param a シャッフル対象の配列 (in-place で並べ替える)。
 */
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
