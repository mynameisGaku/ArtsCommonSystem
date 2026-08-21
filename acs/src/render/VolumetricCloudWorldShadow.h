// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "math/Vec.h"

namespace acs {

class IRhiTexture;

/**
 * 立体物へ雲影を投影するための、非所有の透過率地図と座標情報。
 *
 * 受光点を太陽方向に沿って reference_height の平面へ戻し、minimum_reference_xz と
 * inverse_extent で正規化する。transmittance の赤成分は 0 が完全遮光、1 が遮光なしを表す。
 */
struct FVolumetricCloudWorldShadowMap {
    /** 雲を通過した太陽光の透過率。所有権は雲描画側が持つ。 */
    IRhiTexture* transmittance = nullptr;

    /** 地図の左下に対応する基準面上のワールド XZ。 */
    FVec2 minimum_reference_xz{};

    /** 地図の一辺の逆数。 */
    f32 inverse_extent = 0.0f;

    /** 高さを投影して戻す基準面のワールド Y。 */
    f32 reference_height = 0.0f;

    /** 受光点から太陽へ向かう正規化済み方向。 */
    FVec3 sun_direction{0.0f, 1.0f, 0.0f};

    /** 曲面雲殻の接平面を置いたワールド原点。 */
    FVec3 world_origin{};

    /** 曲面雲殻上での雲底高度。雲内や雲上へ地表用の影を重ねないために使う。 */
    f32 cloud_base_altitude = 0.0f;

    /** 曲面雲殻の惑星半径。 */
    f32 planet_radius = 0.0f;

    /** 一辺の画素数。境界を滑らかに無効化する幅の算出に使う。 */
    u32 resolution = 0u;

    /** 必要な資源と座標情報が揃っているかを返す。 */
    bool IsValid() const noexcept {
        return transmittance != nullptr && inverse_extent > 0.0f &&
               sun_direction.y > 0.0f && planet_radius >= 100.0f &&
               resolution > 0u;
    }
};

} // namespace acs
