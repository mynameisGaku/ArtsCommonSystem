// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * 画面空間の間接光 (SSGI) の設定。
 *
 * @details
 * SSGI は完成した HDR 色を次のフレームの PBR へ反映する。既定は 0 で切れており、
 * `Intensity` を正の値にすると有効になる。画面外の間接光は扱わない。
 */
struct FScene3DGlobalIllumination {
    /** 間接光の強さ。0 以下で切る。 */
    f32 Intensity = 0.0f;

    /** レイを探す最大の世界距離。0 以下なら既定値を使う。 */
    f32 MaxDistance = 5.0f;
};

} // namespace acs::game
