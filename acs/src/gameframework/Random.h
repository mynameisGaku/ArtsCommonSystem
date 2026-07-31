// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/RandomSnapshot.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * xoshiro128** で再現可能なゲーム用乱数列を進める値型。
 *
 * @details 暗号用途には使わず、共有入口Globalの同時利用は呼び出し側で防ぐ。
 */
class FRandom {
public:
    /** 起動時刻をseedにして乱数列を初期化する。 */
    FRandom() noexcept;

    /**
     * 指定seedから再現可能な乱数列を初期化する。
     *
     * @param seed 初期状態へ拡散する値。
     */
    explicit FRandom(u64 seed) noexcept;

    /** 内部状態の意図しない複製を防ぐ。 */
    FRandom(const FRandom&) = delete;

    /** 内部状態の意図しない複製を防ぐ。 */
    FRandom& operator=(const FRandom&) = delete;

    /** 内部状態の意図しない移動を防ぐ。 */
    FRandom(FRandom&&) = delete;

    /** 内部状態の意図しない移動を防ぐ。 */
    FRandom& operator=(FRandom&&) = delete;

    /**
     * 指定seedから内部状態を再初期化する。
     *
     * @param seed 初期状態へ拡散する値。
     */
    void Seed(u64 seed) noexcept;

    /**
     * 乱数列から32bit値を1個生成する。
     *
     * @return 次の32bit値。
     */
    u32 NextU32() noexcept;

    /**
     * 指定回数だけ乱数列を進める。
     *
     * @param draw_count 消費する32bit値の個数。
     * @return 1,048,576回以内ならtrue。上限超過時は状態を維持する。
     */
    bool TryDiscard(u64 draw_count) noexcept;

    /**
     * 現在の再生位置を改変検出付きの値へ保存する。
     *
     * @return 現在の内部状態と検査値。
     */
    FRandomSnapshot CaptureSnapshot() const noexcept;

    /**
     * 検査済みの再生位置へ状態を戻す。
     *
     * @param snapshot CaptureSnapshotで取得した値。
     * @return 版、予約値、状態、検査値が正しければtrue。失敗時は状態を維持する。
     */
    bool TryRestoreSnapshot(const FRandomSnapshot& snapshot) noexcept;

    /**
     * 0以上1未満のf32値を1個生成する。
     *
     * @return 上位24bitを使った0以上1未満の値。
     */
    f32 NextF32Unit() noexcept;

    /**
     * [min, max]の整数を既存の1回剰余方式で生成する。
     *
     * @details maxがmin未満なら交換する。既存の値列と消費回数を維持する。
     * @param min 両端を含む範囲の一方。
     * @param max 両端を含む範囲の一方。
     * @return 交換後の範囲に含まれる整数。
     */
    i32 RangeInt(i32 min, i32 max) noexcept;

    /**
     * min以上max未満のf32値を既存方式で生成する。
     *
     * @param min 範囲の下限。
     * @param max 範囲の上限。
     * @return 線形補間した値。
     */
    f32 RangeF32(f32 min, f32 max) noexcept;

    /**
     * 指定確率でtrueを生成する。
     *
     * @param true_probability trueとなる確率。0から1へ制限して扱う。
     * @return 抽選結果。
     */
    bool NextBool(f32 true_probability = 0.5f) noexcept;

    /**
     * 円板内の一様な点を生成する。
     *
     * @param radius 円板の半径。
     * @return 指定円板内の点。
     */
    FVec2 PointInCircle(f32 radius = 1.0f) noexcept;

    /**
     * 円周上の一様な点を生成する。
     *
     * @param radius 円の半径。
     * @return 指定円周上の点。
     */
    FVec2 PointOnCircle(f32 radius = 1.0f) noexcept;

    /**
     * TArrayを既存の32bit剰余方式で並べ替える。
     *
     * @tparam T 配列要素型。
     * @param values その場で並べ替える配列。
     */
    template<typename T>
    void Shuffle(TArray<T>& values) noexcept;

    /**
     * 重みに比例したindexを既存の24bit方式で生成する。
     *
     * @param weights 各indexの重み。
     * @param count 要素数。
     * @return 選ばれたindex。入力が使えない場合は0。
     */
    u32 WeightedChoice(const f32* weights, u32 count) noexcept;

    /**
     * 検査済み重みに比例したindexを53bit精度で生成する。
     *
     * @param weights 0以上の有限な重み配列。
     * @param count 1から4,096までの要素数。
     * @param out_index 成功時だけ選択結果を書き込む値。
     * @return 全入力が有効で正の合計があればtrue。失敗時は状態と出力を維持する。
     */
    bool TryWeightedIndex(const f32* weights, u32 count, u32& out_index) noexcept;

    /**
     * min以上max未満の有限値で出力配列を埋める。
     *
     * @param values 成功時だけ全要素を更新する配列。
     * @param count 0から4,096までの要素数。0ではvaluesがnullでもよい。
     * @param min 有限な下限。
     * @param max 有限かつmin以上の上限。
     * @return 全入力が有効ならtrue。失敗時は状態と出力を維持する。
     */
    bool TryFillRangeF32(f32* values, u32 count, f32 min, f32 max) noexcept;

    /**
     * 偏りを除いた[min, max]の整数で出力配列を埋める。
     *
     * @param values 成功時だけ全要素を更新する配列。
     * @param count 0から4,096までの要素数。0ではvaluesがnullでもよい。
     * @param min 両端を含む範囲の下限。
     * @param max 両端を含む範囲の上限。
     * @return 全入力が有効ならtrue。失敗時は状態と出力を維持する。
     */
    bool TryFillRangeIntUnbiased(i32* values, u32 count, i32 min, i32 max) noexcept;

    /**
     * 64bit棄却法でindex配列をその場で並べ替える。
     *
     * @param indices 成功時だけ並べ替える配列。
     * @param count 0から4,096までの要素数。0ではindicesがnullでもよい。
     * @return 全入力が有効ならtrue。失敗時は状態と配列を維持する。
     */
    bool TryShuffleIndicesUnbiased(u32* indices, u32 count) noexcept;

    /**
     * 呼び出し側が単一threadに閉じて使う共有乱数列を返す。
     *
     * @return process内の共有インスタンス。
     */
    static FRandom& Global() noexcept;

private:
    /** 1回の検査済み配列操作で扱う最大要素数。 */
    static constexpr u32 kMaximumBatchCount = 4096u;

    /** 1回のTryDiscardで許可する最大消費数。 */
    static constexpr u64 kMaximumDiscardCount = 1048576u;

    /** xoshiro128** の第0状態。 */
    u32 m_S0 = 0u;

    /** xoshiro128** の第1状態。 */
    u32 m_S1 = 0u;

    /** xoshiro128** の第2状態。 */
    u32 m_S2 = 0u;

    /** xoshiro128** の第3状態。 */
    u32 m_S3 = 0u;
};

/**
 * TArrayを既存の32bit剰余方式で並べ替える。
 *
 * @tparam T 配列要素型。
 * @param values その場で並べ替える配列。
 */
template<typename T>
void FRandom::Shuffle(TArray<T>& values) noexcept {
    /** 並べ替える要素数。 */
    const usize count = values.Size();
    if (count < 2u) return;
    /** 末尾から確定する配列位置。 */
    for (usize index = count - 1u; index > 0u; --index) {
        /** 現在位置までから選んだ交換位置。 */
        const u32 selected = NextU32() % static_cast<u32>(index + 1u);
        if (static_cast<usize>(selected) != index) {
            /** 交換中に保持する要素。 */
            T temporary = static_cast<T&&>(values[index]);
            values[index] = static_cast<T&&>(values[selected]);
            values[selected] = static_cast<T&&>(temporary);
        }
    }
}

} // namespace acs::game
