// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "math/Mat.h"

#include <cmath>

namespace acs {

/**
 * inverse-transpose normal matrix を安全に構築する。
 *
 * @details Singular scale では法線変換自体が数学的に定義できない。旧sceneの zero-axis
 * scale や非有限値を Inverse へ渡して GPU CB を NaN/Inf で汚染しないよう、相対行列式を
 * 検査し、定義不能なら deterministic な identity fallback を返す。
 */
inline FMat4 MakeSafeNormalMatrix(const FMat4& model) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            if (!std::isfinite(model.m[row][col])) return FMat4::Identity();
        }
    }

    const f32 m00 = model.m[0][0], m01 = model.m[0][1], m02 = model.m[0][2];
    const f32 m10 = model.m[1][0], m11 = model.m[1][1], m12 = model.m[1][2];
    const f32 m20 = model.m[2][0], m21 = model.m[2][1], m22 = model.m[2][2];
    const f32 determinant =
        m00 * (m11 * m22 - m12 * m21)
      - m01 * (m10 * m22 - m12 * m20)
      + m02 * (m10 * m21 - m11 * m20);
    const f32 row0_len = std::sqrt(m00 * m00 + m01 * m01 + m02 * m02);
    const f32 row1_len = std::sqrt(m10 * m10 + m11 * m11 + m12 * m12);
    const f32 row2_len = std::sqrt(m20 * m20 + m21 * m21 + m22 * m22);
    const f32 determinant_scale = row0_len * row1_len * row2_len;
    if (!std::isfinite(determinant) || !std::isfinite(determinant_scale) ||
        determinant_scale <= 0.0f ||
        std::fabs(determinant) <= determinant_scale * 1.0e-6f) {
        return FMat4::Identity();
    }

    const FMat4 normal = Transpose(Inverse(model));
    for (u32 row = 0; row < 4; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            if (!std::isfinite(normal.m[row][col])) return FMat4::Identity();
        }
    }
    return normal;
}

} // namespace acs
