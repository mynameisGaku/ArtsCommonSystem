// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — 共通型 / 定数。
//
// 4 体の sphere の素材種、配置定数、emissive オーブの定数、TAA 用 Halton
// シーケンス helper を 1 ヘッダにまとめる。`inline constexpr` で header
// only に置くため複数 TU に include しても 1 ストレージに resolve される。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace helloshowcase {

// ----- 4 体の sphere -----
inline constexpr acs::u32 kSphereCount = 4;
inline constexpr acs::f32 kSphereX[kSphereCount] = {-2.4f, -0.8f, 0.8f, 2.4f};

enum class EMaterialKind : acs::u8 { Gold, Ceramic, ClearGlass, FrostedGlass };
inline constexpr EMaterialKind kSphereKind[kSphereCount] = {
    EMaterialKind::Gold,
    EMaterialKind::Ceramic,
    EMaterialKind::ClearGlass,
    EMaterialKind::FrostedGlass,
};

inline constexpr acs::f32 kSphereRadius = 0.55f;     // MakeSphere の既定半径
inline constexpr acs::f32 kSphereScale  = 1.4f;      // 視認性のため 1.4 倍にスケール
inline constexpr acs::f32 kSphereY      = 0.6f;      // 中心 y (床から少し浮かす)
inline constexpr acs::f32 kSphereZ      = 0.0f;

// ----- 床 -----
inline constexpr acs::f32 kFloorY    = -0.2f;
inline constexpr acs::f32 kFloorSize = 12.0f;

// ----- emissive オーブ -----
inline constexpr acs::u32 kOrbCount        = 2;
inline constexpr acs::f32 kOrbRadius       = 0.5f;     // sphere scale で小さく
inline constexpr acs::f32 kOrbScale        = 0.35f;
inline constexpr acs::f32 kOrbY            = 0.9f;
inline constexpr acs::f32 kOrbOrbitRadius  = 3.5f;     // 床平面で公転
inline constexpr acs::f32 kOrbEmissiveStrength = 4.0f;

inline constexpr acs::FVec3 kOrbColors[kOrbCount] = {
    acs::FVec3{1.00f, 0.55f, 0.20f},     // orange
    acs::FVec3{0.30f, 0.85f, 1.00f},     // cyan
};

// Halton(i, b) ∈ [0, 1): TAA 用 low-discrepancy 1D sequence
constexpr acs::f32 Halton(acs::u32 i, acs::u32 b) noexcept {
    acs::f32 f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= static_cast<acs::f32>(b);
        r += f * static_cast<acs::f32>(i % b);
        i /= b;
    }
    return r;
}

} // namespace helloshowcase
