// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — MathDispatch 実装
// -----------------------------------------------------------------------------
// CPU 機能を見て関数ポインタテーブルを構築。
// 現状は SSE2 ベースライン経路のみ実装。
// =============================================================================
#include "math/MathDispatch.h"
#include "math/Cpu.h"
#include "threading/Atomic.h"

namespace acs {

namespace {

/**
 * 点群を 1 要素ずつ行列変換する SSE2 ベースライン実装。
 *
 * @details DirectXMath が /arch:SSE2 で動作する経路で、全 CPU で正しく動く。
 * @param in 入力点配列。
 * @param out 出力点配列。
 * @param count 変換する要素数。
 * @param m 変換行列。
 */
void TransformPointsScalar(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    for (usize i = 0; i < count; ++i) {
        out[i] = TransformPoint(in[i], m);
    }
}

/**
 * 方向ベクトル群を 1 要素ずつ行列変換する SSE2 ベースライン実装。
 *
 * @param in 入力ベクトル配列。
 * @param out 出力ベクトル配列。
 * @param count 変換する要素数。
 * @param m 変換行列。
 */
void TransformVectorsScalar(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    for (usize i = 0; i < count; ++i) {
        out[i] = TransformVector(in[i], m);
    }
}

/** ディスパッチテーブルの実体 (Init が結線する)。 */
FMathDispatch  g_dispatch {};

/** 初期化状態 (0=未初期化, 1=初期化中, 2=完了)。 */
TAtomic<u32>   g_inited {0};

/**
 * ディスパッチ関数ポインタをベースライン実装に結線する。
 *
 * @details
 * 全 CPU で正しく動作する DirectXMath/SSE2 経路を割り当てる (出力は常に正しい)。
 * AVX2 特化は /arch:AVX2 でコンパイルした別 TU の実装を用意して結線する。
 */
void Init() noexcept {
    g_dispatch.transform_points  = &TransformPointsScalar;
    g_dispatch.transform_vectors = &TransformVectorsScalar;
}

} // namespace

const FMathDispatch& GetMathDispatch() noexcept {
    if (g_inited.Load(EMemoryOrder::Acquire) == 0) {
        u32 expected = 0;
        if (g_inited.CompareExchange(expected, 1)) {
            Init();
            g_inited.Store(2, EMemoryOrder::Release);
        } else {
            while (g_inited.Load(EMemoryOrder::Acquire) != 2) SpinHint();
        }
    }
    return g_dispatch;
}

void TransformPoints(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    GetMathDispatch().transform_points(in, out, count, m);
}
void TransformVectors(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    GetMathDispatch().transform_vectors(in, out, count, m);
}

} // namespace acs
