// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 3D Prefab rootで原本より優先する編集プロパティ。 */
enum class EPrefabRootProperty3D : u32 {
    /** rootの表示状態。 */
    Visible = 1u << 0u,

    /** rootの有効状態。 */
    Enabled = 1u << 1u,

    /** rootメッシュのRGBA色。 */
    Color = 1u << 2u,
};

/** 3D Prefab rootプロパティを保存maskへ変換する。 */
[[nodiscard]] constexpr u32 PrefabRootProperty3DBit(EPrefabRootProperty3D property) noexcept
{
    return static_cast<u32>(property);
}

/** 現在のPOVR3D契約が受理する全bit。 */
inline constexpr u32 kPrefabRootProperty3DAllMask =
    PrefabRootProperty3DBit(EPrefabRootProperty3D::Visible) |
    PrefabRootProperty3DBit(EPrefabRootProperty3D::Enabled) |
    PrefabRootProperty3DBit(EPrefabRootProperty3D::Color);

} // namespace acs::game
