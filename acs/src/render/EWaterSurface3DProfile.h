// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** 水域の大きさと流れに合う初期値を選ぶための水面プロファイル。 */
enum class EWaterSurface3DProfile : u8 {
    /** 数センチ程度の浅い水と細かな波を持つ水たまり。 */
    Puddle,

    /** 透明度が高く、波高を抑えた人工的なプール。 */
    Pool,

    /** 一方向の流れを主とし、大波を抑えた河川。 */
    River,

    /** 中距離の風波と穏やかな反射を持つ湖。 */
    Lake,

    /** 長い波長、強い風波、深い光学距離を持つ海。 */
    Ocean,
};

} // namespace acs
