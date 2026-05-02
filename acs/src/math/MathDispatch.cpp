#include "math/MathDispatch.h"
#include "math/Cpu.h"
#include "threading/Atomic.h"

namespace acs {

namespace {

// SSE2-baseline implementations (always available on x64).
void TransformPointsScalar(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept {
    for (usize i = 0; i < count; ++i) {
        out[i] = TransformPoint(in[i], m);
    }
}
void TransformVectorsScalar(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept {
    for (usize i = 0; i < count; ++i) {
        out[i] = TransformVector(in[i], m);
    }
}

// AVX2 fast path. DirectXMath picks the best instruction set per /arch flag,
// so this primarily gives the compiler a wider register window for unrolling.
// In v2 we'll split this into a separate /arch:AVX2-compiled TU; for Phase 1
// the dispatch table simply selects between two function pointers that route
// to the same scalar fallback. This preserves the API surface while leaving
// real AVX2 specialization for follow-up work.
void TransformPointsAvx2(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept {
    TransformPointsScalar(in, out, count, m);
}
void TransformVectorsAvx2(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept {
    TransformVectorsScalar(in, out, count, m);
}

MathDispatch  g_dispatch {};
Atomic<u32>   g_inited {0};

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
    if (g_inited.Load(MemoryOrder::Acquire) == 0) {
        u32 expected = 0;
        if (g_inited.CompareExchange(expected, 1)) {
            Init();
            g_inited.Store(2, MemoryOrder::Release);
        } else {
            while (g_inited.Load(MemoryOrder::Acquire) != 2) SpinHint();
        }
    }
    return g_dispatch;
}

void TransformPoints(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept {
    GetMathDispatch().transform_points(in, out, count, m);
}
void TransformVectors(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept {
    GetMathDispatch().transform_vectors(in, out, count, m);
}

} // namespace acs
