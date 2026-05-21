// SPDX-License-Identifier: Apache-2.0
// 手続き生成のメッシュプリミティブ
//
// アセットファイルを用意しなくても、簡単に立方体・球・平面を
// MeshAsset として作れる便利関数。HelloModel 系のサンプルや
// プロトタイピングに便利。
//
// 使い方:
//   Rc<MeshAsset> cube  = Primitive::MakeCube();
//   Rc<MeshAsset> sphere = Primitive::MakeSphere(1.0f, 32, 16);
//   Rc<MeshAsset> plane  = Primitive::MakePlane(10.0f, 10.0f);
//
//   GpuMesh gm;
//   UploadMesh(*device, *cube, gm);
#pragma once

#include "foundation/Types.h"
#include "memory/Rc.h"

namespace acs {

class MeshAsset;

namespace Primitive {

// 1×1×1 の立方体（中心原点）。各面 4 頂点で法線を持ち、UV 0..1 が割当てられる。
Rc<MeshAsset> MakeCube(f32 size = 1.0f) noexcept;

// UV 球（半径、横分割、縦分割）
Rc<MeshAsset> MakeSphere(f32 radius = 0.5f, u32 segments = 32, u32 rings = 16) noexcept;

// XZ 平面（width x depth、Y=0、法線 +Y）
Rc<MeshAsset> MakePlane(f32 width = 1.0f, f32 depth = 1.0f) noexcept;

} // namespace Primitive

} // namespace acs
