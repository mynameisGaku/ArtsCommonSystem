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

// AVX2 経路 — Phase 1 では未実装でスカラフォールバック
// v2 で /arch:AVX2 でコンパイルした別 TU の実装に切り替える
void TransformPointsAvx2(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    TransformPointsScalar(in, out, count, m);
}
void TransformVectorsAvx2(const FVec3* in, FVec3* out, usize count, const FMat4& m) noexcept {
    TransformVectorsScalar(in, out, count, m);
}

MathDispatch  g_dispatch {};
Atomic<u32>   g_inited {0};

// CPU 機能を見て関数ポインタを差し替える
void Init() noexcept {
    if (HasAvx2()) {
        g_dispatch.transform_points  = &TransformPointsAvx2;
        g_dispatch.transform_vectors = &TransformVectorsAvx2;
    } else {
        g_dispatch.transform_points  = &TransformPointsScalar;
        g_dispatch.transform_vectors = &TransformVectorsScalar;
    }
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
