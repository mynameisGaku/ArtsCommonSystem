// ACS Math — Runtime CPU dispatch table for batch operations.
//
// Per-element ops live inline in Vec/Mat/Quat headers (compiled at SSE2 baseline
// — DirectXMath chooses the best path based on /arch flags). Bulk operations
// (transform N points, dot N vector pairs) get split across SSE / AVX2
// translation units and dispatched at startup based on detected CPU features.
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"

namespace acs {

struct MathDispatch {
    void (*transform_points)(const Vec3* in, Vec3* out, usize count, const Mat4& m) = nullptr;
    void (*transform_vectors)(const Vec3* in, Vec3* out, usize count, const Mat4& m) = nullptr;
};

const MathDispatch& GetMathDispatch() noexcept;

void TransformPoints(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept;
void TransformVectors(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept;

} // namespace acs
