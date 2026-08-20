// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** 太陽円盤の見かけの角半径を1-cos形式で表した値。 */
inline constexpr f32 kSkySolarDiscRadiusOneMinusCosine = 1.08e-5f;
/** 昼空で太陽の周囲に残す光彩の外端。 */
inline constexpr f32 kSkyDaySunHaloRadiusOneMinusCosine = 0.0038f;
/** 夕空で太陽の周囲に残す光彩の外端。 */
inline constexpr f32 kSkySunsetSunHaloRadiusOneMinusCosine = 0.0076f;
/** 夜空の月光周囲に残す光彩の外端。 */
inline constexpr f32 kSkyNightMoonHaloRadiusOneMinusCosine = 0.0014f;
/** 太陽色へ近づける光彩の最大割合。 */
inline constexpr f32 kSkySunHaloStrength = 0.28f;
/** 地平線付近へ加える前方散乱光の最大割合。 */
inline constexpr f32 kSkySunHorizonGlowStrength = 0.18f;
/** 地平線から離れた前方散乱光を弱める係数。 */
inline constexpr f32 kSkySunHorizonGlowFalloff = 12.0f;
/** 光彩幅から前方散乱の角度減衰幅を求める割合。 */
inline constexpr f32 kSkySunForwardGlowWidthScale = 0.35f;

/** 太陽方向に対する空の各放射成分の重み。 */
struct FSkySunProfile {
    /** 太陽円盤そのものが覆う割合。 */
    f32 disc_weight = 0.0f;
    /** 円盤外側の光彩が太陽色へ近づける割合。 */
    f32 halo_weight = 0.0f;
    /** 地平線付近へ加える前方散乱光の割合。 */
    f32 horizon_glow_weight = 0.0f;
};

/**
 * 視線と太陽の角度から円盤、光彩、地平線前方散乱の重みを求める。
 *
 * @param one_minus_cosine 視線と太陽方向の内積を1から引いた値。
 * @param view_elevation 視線方向の上下成分。
 * @param disc_radius_one_minus_cosine 円盤の角半径を1-cos形式で表した値。
 * @param halo_radius_one_minus_cosine 光彩外端の角半径を1-cos形式で表した値。
 * @param angular_filter_width 画素が覆う1-cos形式の角度幅。
 */
FSkySunProfile ResolveSkySunProfile(f32 one_minus_cosine, f32 view_elevation, f32 disc_radius_one_minus_cosine, f32 halo_radius_one_minus_cosine, f32 angular_filter_width) noexcept;

} // namespace acs
