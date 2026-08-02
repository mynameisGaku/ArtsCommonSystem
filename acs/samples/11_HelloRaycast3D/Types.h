// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — 共通型 + 定数。
//
// シーンに置く物体の形状/色を表す POD と、ライト・カメラの定数を集約する。
// `inline constexpr` で header に置くので、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17 要件)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace helloraycast3d {

enum class EShapeKind : acs::u8 { Sphere, Cube };

struct FRaycastTarget {
    EShapeKind kind;
    acs::FVec3 position;
    acs::f32  radius_or_half;     // 球: 半径、立方体: 半サイズ
    acs::FVec3 base_color;
};

// 配置: 円周上に等間隔で並べる
inline constexpr acs::u32 kNumObjects     = 8;
inline constexpr acs::f32 kObjectArenaR   = 5.0f;
inline constexpr acs::f32 kObjectRadius   = 0.7f;

// カメラ初期値
inline constexpr acs::f32  kCamMoveSpeed = 6.0f;
inline constexpr acs::f32  kCamTurnSpeed = 1.5f;
inline constexpr acs::FVec3 kCamInitialPos{0.0f, 2.0f, -8.0f};

// 環境光 (キー / フィルライトの「届かない」陰の色味)
inline constexpr acs::FVec3 kAmbient{0.08f, 0.10f, 0.14f};

// マテリアル定数: 地面 / レイヒット時のハイライト色
inline constexpr acs::FVec3 kPlaneColor   {0.45f, 0.50f, 0.55f};
inline constexpr acs::FVec3 kSelectedColor{1.0f,  1.0f,  1.0f };

} // namespace helloraycast3d
