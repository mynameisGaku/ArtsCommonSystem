// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 3D Prefab child nodeで原本より優先できるproperty。 */
enum class EPrefabNodeProperty3D : u8 {
    /** 表示状態。 */
    Visible = 0,

    /** 更新状態。 */
    Enabled = 1,

    /** meshのRGBA色。 */
    Color = 2,
};

/** 指定propertyをPNOVR3D maskの1 bitへ変換する。 */
constexpr u32 PrefabNodeProperty3DBit(EPrefabNodeProperty3D property) noexcept
{
    return u32{1} << static_cast<u32>(property);
}

/** PNOVR3Dで利用できる全property bit。 */
inline constexpr u32 kPrefabNodeProperty3DAllMask =
    PrefabNodeProperty3DBit(EPrefabNodeProperty3D::Visible) |
    PrefabNodeProperty3DBit(EPrefabNodeProperty3D::Enabled) |
    PrefabNodeProperty3DBit(EPrefabNodeProperty3D::Color);

} // namespace acs::game
