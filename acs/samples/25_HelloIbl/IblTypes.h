// SPDX-License-Identifier: Apache-2.0
// HelloIbl — 共通定数 + 小さなユーティリティ。
//
// equirect 解像度や動的球の数のような「描画ロジックと baker / equirect builder
// の両方が参照する constexpr」をここにまとめる。Halton(i, b) sequence は
// TAA sub-pixel jitter で使う low-discrepancy 1D シーケンス (constexpr)。
//
// すべて `inline constexpr` / inline 関数として header に置くので、複数 TU から
// include しても 1 つに resolve される (C++17)。
#pragma once

#include "math/Vec.h"
#include "foundation/Types.h"

namespace helloibl {

// CPU で焼く equirect HDR の解像度。SH9 計算 / Studio HDR / Atmosphere preset で
// 共有。SH9 は低周波なので小さめで十分。
inline constexpr acs::u32 kEquirectWidth  = 256;
inline constexpr acs::u32 kEquirectHeight = 128;

// Phase 34f-3: 動的球 (公転発光オーブ) の数。color pass + motion pass の両方が参照。
inline constexpr acs::u32 kDynCount = 3;

// Phase 35-3b/3c + 35-3d: ガラス球のワールド配置。clear (左) / frosted (右)。
inline constexpr acs::Vec3 kGlassPos       {-1.4f, 1.5f, 0.0f};
inline constexpr acs::Vec3 kFrostedGlassPos{ 1.4f, 1.5f, 0.0f};

// Halton(i, b) ∈ [0, 1): low-discrepancy 1D sequence used as TAA sub-pixel jitter.
// 2 つの異なる base (典型的に 2, 3) で xy ペアを作る (Halton(2,3) sequence)。
inline constexpr acs::f32 Halton(acs::u32 i, acs::u32 b) noexcept {
    acs::f32 f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= static_cast<acs::f32>(b);
        r += f * static_cast<acs::f32>(i % b);
        i /= b;
    }
    return r;
}

} // namespace helloibl
