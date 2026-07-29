// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** 形式が保持する画像成分。 */
enum class EFormatAspect : u8 {
    /** 画像成分を持たない。 */
    None = 0u,
    /** 色成分を持つ。 */
    Color = 1u,
    /** 深度成分を持つ。 */
    Depth = 2u,
    /** ステンシル成分を持つ。 */
    Stencil = 4u,
};

/** 二つの画像成分を結合する。 */
constexpr EFormatAspect operator|(EFormatAspect left, EFormatAspect right) noexcept {
    return static_cast<EFormatAspect>(static_cast<u8>(left) | static_cast<u8>(right));
}

} // namespace acs
