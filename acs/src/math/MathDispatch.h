// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — バッチ演算の CPU ディスパッチテーブル
// -----------------------------------------------------------------------------
// 1 要素ずつの計算は Vec/Mat/FQuat ヘッダにインライン記述（DirectXMath が
// /arch:* に応じて最適 SIMD を選ぶ）。一方、N 要素の一括変換などは関数
// ポインタテーブル経由でディスパッチし、起動時に検出した CPU 機能に応じて
// 最適な実装を選択する。
//
// 現状は SSE2 ベースライン経路のみ実装。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"

namespace acs {

/**
 * 起動時に CPU 機能から実装を選んで結線するバッチ演算の関数ポインタ群。
 */
struct MathDispatch {
    /** 点群を行列で一括変換する関数 (in→out、count 個、行列 m)。 */
    void (*transform_points) (const FVec3* in, FVec3* out, usize count, const FMat4& m) = nullptr;

    /** 方向ベクトル群を行列で一括変換する関数 (in→out、count 個、行列 m)。 */
    void (*transform_vectors)(const FVec3* in, FVec3* out, usize count, const FMat4& m) = nullptr;
};

/**
 * ディスパッチテーブルを返す (初回呼び出しで初期化)。
 *
 * @return 結線済みの MathDispatch への const 参照。
 */
const MathDispatch& GetMathDispatch() noexcept;

/**
 * 点群をディスパッチ越しに行列変換する利便関数。
 *
 * @param in 入力点配列。
 * @param out 出力点配列 (count 要素確保済み)。
 * @param count 変換する要素数。
 * @param m 変換行列。
 */
void TransformPoints (const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept;

/**
 * 方向ベクトル群をディスパッチ越しに行列変換する利便関数。
 *
 * @param in 入力ベクトル配列。
 * @param out 出力ベクトル配列 (count 要素確保済み)。
 * @param count 変換する要素数。
 * @param m 変換行列。
 */
void TransformVectors(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept;

} // namespace acs
