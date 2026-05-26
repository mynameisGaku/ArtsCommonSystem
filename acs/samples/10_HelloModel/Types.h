// SPDX-License-Identifier: Apache-2.0
// HelloModel — 共通型 + 定数。
// シーン中で繰り返し参照する数値 (球の自転速度 / カメラ初期位置 /
// キューブ色配列など) を集約。
//
// `inline constexpr` で header に置くので、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace hellomodel {

// 周囲を回るキューブの数
inline constexpr acs::u32 kCubeCount = 4;

// 球の自転速度 (rad / sec)
inline constexpr acs::f32 kSphereSpinSpeed = 0.6f;

// カメラ
inline constexpr acs::f32 kCamMoveSpeed   = 4.0f;   // world units / sec
inline constexpr acs::f32 kCamTurnSpeed   = 1.6f;   // rad / sec
inline constexpr acs::Vec3 kCamInitialPos {0.0f, 1.5f, -5.0f};

// 光源 (方向 + 暖色 + 寒色 ambient)
inline constexpr acs::Vec3 kLightDir     {-0.5f, 0.8f, 0.3f};
inline constexpr acs::Vec3 kLightColor   { 1.0f, 0.95f, 0.9f };
inline constexpr acs::Vec3 kAmbientColor { 0.10f, 0.12f, 0.18f };

// マテリアル色
inline constexpr acs::Vec3 kSphereColor { 1.0f, 0.85f, 0.4f };  // 山吹色
inline constexpr acs::Vec3 kPlaneColor  { 0.4f, 0.45f, 0.5f };  // 灰青
inline constexpr acs::Vec3 kCubeColors[kCubeCount] = {
    {0.9f, 0.3f, 0.3f},   // 赤
    {0.3f, 0.7f, 0.4f},   // 緑
    {0.3f, 0.5f, 0.9f},   // 青
    {0.8f, 0.4f, 0.8f},   // 紫
};

} // namespace hellomodel
