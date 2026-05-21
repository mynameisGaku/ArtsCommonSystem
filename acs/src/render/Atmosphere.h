// SPDX-License-Identifier: Apache-2.0
// Physical atmospheric scattering (Hillaire 2020 / Bruneton 風) — Phase 34c
//
// Rayleigh + Mie 単散乱を per-direction で CPU 評価し equirect 画像に焼く。
// `ImageBasedLighting::LoadEquirectHdrFromMemory` に通せば env cubemap →
// irradiance → prefilter の IBL chain が一気に物理ベースの sky で構築される。
//
// 物理パラメータ (Earth、Bruneton 2008):
//   - 地表半径 6360 km、大気上端 6420 km (厚さ 60 km)
//   - Rayleigh: β = (5.802, 13.558, 33.1) ×10⁻⁶ m⁻¹、scale height 8 km
//   - Mie:      β = 3.996 ×10⁻⁶ m⁻¹、absorption 4.4 ×10⁻⁶ m⁻¹、scale height 1.2 km、g=0.8
//
// 簡易: 単散乱のみ (multi-scatter / aerial perspective / ozone は未含)。
// Hillaire LUT 版へのアップグレードは Phase 35+ (GPU で実装したい場合)。
#pragma once

#include "foundation/Result.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs {

struct AtmosphereParams {
    Vec3 sun_dir       = Vec3{0.4f, 0.7f, 0.4f};  // 太陽方角 (天頂方向 +Y、正規化前提)
    Vec3 sun_intensity = Vec3{22.0f, 22.0f, 22.0f}; // sun ピーク輝度 (W/m²/sr 相当)
    Vec3 ground_albedo = Vec3{0.10f, 0.12f, 0.10f}; // ground bounce (現状は使わず)
    u32  ray_steps     = 32;                       // view ray 沿いのサンプル数
    u32  sun_steps     = 8;                        // 各 sample から sun への光線でのサンプル数 (透過率)
};

class Atmosphere {
public:
    // CPU で equirect 画像を焼く。`out_rgba_float` は w*h*4 個の float、上から下、
    // v=0 が +Y 天頂、v=1 が -Y 天底 (sIBL Archive 規約と一致)。
    // 戻り値の Array は呼び出し側に渡される (move)。
    static Array<f32> BakeEquirect(u32 width, u32 height,
                                    const AtmosphereParams& params) noexcept;
};

} // namespace acs
