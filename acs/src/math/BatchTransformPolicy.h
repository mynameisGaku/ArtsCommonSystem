// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Mat.h"
#include "math/Vec.h"

namespace acs {

/** バッチ内の各要素を点または方向として扱うコンパイル時方針。 */
enum class EBatchTransformPolicy : u8 {
    /** 行列の平行移動を適用する点。 */
    Point,

    /** 行列の平行移動を適用しない方向ベクトル。 */
    Vector
};

/**
 * 種別分岐と関数ポインタ呼び出しを除去してバッチ変換する。
 *
 * @tparam Policy 点または方向を選ぶコンパイル時方針。
 * @param Input Count 個の入力配列。
 * @param Output Count 個を格納できる出力配列。
 * @param Count 変換する要素数。
 * @param Matrix 各要素へ適用する行列。
 */
template<EBatchTransformPolicy Policy>
ACS_FORCEINLINE void TransformBatchStatic(const FVec3* Input, FVec3* Output, usize Count, const FMat4& Matrix) noexcept
{
    static_assert(Policy == EBatchTransformPolicy::Point || Policy == EBatchTransformPolicy::Vector, "未対応のバッチ変換方針です");
    // 現在変換する要素位置。
    for (usize Index = 0; Index < Count; ++Index) {
        if constexpr (Policy == EBatchTransformPolicy::Point) {
            Output[Index] = TransformPoint(Input[Index], Matrix);
        } else {
            Output[Index] = TransformVector(Input[Index], Matrix);
        }
    }
}

/**
 * 要素数もコンパイル時に決まる固定配列をバッチ変換する。
 *
 * @tparam Policy 点または方向を選ぶコンパイル時方針。
 * @tparam Count 入出力配列の要素数。
 * @param Input 固定長の入力配列。
 * @param Output 固定長の出力配列。
 * @param Matrix 各要素へ適用する行列。
 */
template<EBatchTransformPolicy Policy, usize Count>
ACS_FORCEINLINE void TransformBatchStatic(const FVec3 (&Input)[Count], FVec3 (&Output)[Count], const FMat4& Matrix) noexcept
{
    TransformBatchStatic<Policy>(Input, Output, Count, Matrix);
}

} // namespace acs
