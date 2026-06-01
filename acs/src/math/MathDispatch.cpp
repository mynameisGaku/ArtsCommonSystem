// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — MathDispatch 実装
// -----------------------------------------------------------------------------
// CPU 機能を見て関数ポインタテーブルを構築。
// 現状は SSE2 ベースライン経路のみ実装。AVX2 専用ビルド (/arch:AVX2 で
// 別 TU) は v2 で追加予定。
// =============================================================================
#include "math/MathDispatch.h"
#include "math/Cpu.h"
#include "threading/Atomic.h"

namespace acs {

namespace {

// SSE2 ベースライン実装（DirectXMath が /arch:SSE2 で動作）
void TransformPointsScalar(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    for (usize i = 0; i < count; ++i) {
        out[i] = TransformPoint(in[i], m);
    }
}
void TransformVectorsScalar(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    for (usize i = 0; i < count; ++i) {
        out[i] = TransformVector(in[i], m);
    }
}

MathDispatch  g_dispatch {};
TAtomic<u32>   g_inited {0};

// 関数ポインタを実装に差し替える。
//
// 注意: かつてここに「AVX2 経路」として中身がスカラ実装をそのまま呼ぶだけの偽関数を
// 置き、HasAvx2() で分岐していた。実体はベースライン (DirectXMath/SSE2) と同一で、
// AVX2 を名乗るだけの no-op スタブだったため削除した。AVX2 特化は /arch:AVX2 で
// コンパイルした別 TU の実装を用意できた時点で dispatch に正式に結線する。
// それまでは正しく動作するベースライン経路を全 CPU で使う (出力は常に正しい)。
void Init() noexcept {
    g_dispatch.transform_points  = &TransformPointsScalar;
    g_dispatch.transform_vectors = &TransformVectorsScalar;
}

} // namespace

const MathDispatch& GetMathDispatch() noexcept {
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
