// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_VOLUMETRIC_CLOUD_WEATHER_H
#define ACS_RENDER_VOLUMETRIC_CLOUD_WEATHER_H

#include "foundation/Types.h"

namespace acs {

/**
 * 手続き生成した天候場を残しながら、雲種と降水成分を目的の天候へ寄せる設定。
 *
 * 各適用率が 0 なら生成済みの天候場をそのまま使い、1 なら対応する目標値へ固定する。
 * 雲量とは独立しているため、雲量を増やしても暗黙に嵐や層雲へ変化しない。
 */
struct FVolumetricCloudWeather {
    /** 目標とする雲種。0 は層雲、0.5 は層積雲、1 は積雲。 */
    f32 CloudType = 0.78f;

    /** 手続き生成した雲種から CloudType へ寄せる割合。 */
    f32 CloudTypeInfluence = 0.0f;

    /** 目標とする降水成分。0 は晴天、1 は強い降水域。 */
    f32 Precipitation = 0.0f;

    /** 手続き生成した降水成分から Precipitation へ寄せる割合。 */
    f32 PrecipitationInfluence = 0.0f;
};

/**
 * 天候設定を GPU が受け取れる有限な 0～1 の範囲へ直す。
 *
 * @param requested 利用側が指定した天候設定。非有限値は各項目の既定値へ戻す。
 * @return 雲種、降水成分、各適用率を 0～1 へ収めた設定。
 */
FVolumetricCloudWeather SanitizeVolumetricCloudWeather(
    const FVolumetricCloudWeather& requested) noexcept;

} // namespace acs

#endif // ACS_RENDER_VOLUMETRIC_CLOUD_WEATHER_H
