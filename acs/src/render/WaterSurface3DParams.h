// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "render/EWaterSurface3DProfile.h"

namespace acs {

/** 形状に依存せず、水面の波と光学特性を指定する値。 */
struct FWaterSurface3DParams {
    /** 浅い部分で散乱して見える色。 */
    FVec3 shallow_color{0.055f, 0.38f, 0.50f};

    /** 深い部分で散乱して見える色。 */
    FVec3 deep_color{0.008f, 0.055f, 0.16f};

    /** 1ワールド単位あたりのBeer-Lambert吸収係数。 */
    FVec3 absorption{0.34f, 0.13f, 0.040f};

    /** 1ワールド単位あたりの単一散乱係数。 */
    FVec3 scattering{0.018f, 0.050f, 0.085f};

    /** 光が前後どちらへ散りやすいかを表す値。 */
    f32 phase_anisotropy = 0.62f;

    /** 白波と接触泡の色。 */
    FVec3 foam_color{0.88f, 0.96f, 1.0f};

    /** 水面上の主な流れ方向。 */
    FVec2 flow_direction{0.92f, 0.38f};

    /** 水面反射の粗さ。 */
    f32 roughness = 0.105f;

    /** 細かな法線変化の強さ。 */
    f32 normal_strength = 0.82f;

    /** 1ワールド単位あたりの細かな法線模様の反復数。 */
    f32 normal_tiling = 0.075f;

    /** 画面上で背景を屈折させる強さ。 */
    f32 refraction_strength = 0.72f;

    /** 深度textureがない場合に使う水の光学距離。 */
    f32 optical_depth = 1.35f;

    /** 風波全体の最大変位。 */
    f32 wave_amplitude = 0.105f;

    /** 風波の空間周波数倍率。小さいほど長い波になる。 */
    f32 wave_scale = 0.78f;

    /** 風波と細かな法線の時間倍率。 */
    f32 wave_speed = 0.72f;

    /** 動的な波紋が広がる速度。 */
    f32 ripple_speed = 2.65f;

    /** 動的な波紋の波長。 */
    f32 ripple_wavelength = 0.52f;

    /** 動的な波紋が完全に消えるまでの秒数。 */
    f32 ripple_lifetime = 4.0f;

    /** 動的な波紋へ掛ける指数減衰。 */
    f32 ripple_damping = 0.78f;

    /** 白波と接触泡の量。 */
    f32 foam_intensity = 0.82f;

    /** 用途に合う安全な初期値を返し、未知値ではLakeを返す。 */
    static FWaterSurface3DParams ForProfile(EWaterSurface3DProfile profile) noexcept;
};

} // namespace acs
