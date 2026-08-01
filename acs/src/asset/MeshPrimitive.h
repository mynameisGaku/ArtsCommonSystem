// SPDX-License-Identifier: Apache-2.0
// 手続き生成のメッシュプリミティブ
//
// アセットファイルを用意しなくても、簡単に立方体・球・平面を
// AMeshAsset として作れる便利関数。HelloModel 系のサンプルや
// プロトタイピングに便利。
//
// 使い方:
//   TSharedPtr<AMeshAsset> cube  = Primitive::MakeCube();
//   TSharedPtr<AMeshAsset> sphere = Primitive::MakeSphere(1.0f, 32, 16);
//   TSharedPtr<AMeshAsset> plane  = Primitive::MakePlane(10.0f, 10.0f);
//
//   FGpuMesh gm;
//   UploadMesh(*device, *cube, gm);
#pragma once

#include "foundation/Types.h"
#include "memory/SharedPtr.h"

namespace acs {

class AMeshAsset;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FMeshAsset = AMeshAsset;

namespace Primitive {

/**
 * 中心原点の立方体メッシュを生成する。
 *
 * @details 6 面それぞれ独立した 4 頂点 (計 24 頂点) で面ごとの法線を持ち、各面に UV 0..1 を割り当てる。
 * @param size 立方体の一辺の長さ (既定 1.0)。
 * @return 生成した立方体の AMeshAsset。
 */
TSharedPtr<AMeshAsset> MakeCube(f32 size = 1.0f) noexcept;

/**
 * UV 球メッシュを生成する。
 *
 * @details 緯度 (rings) × 経度 (segments) の格子で頂点を並べ、各四角を 2 三角形に分割する。
 * segments は最小 3、rings は最小 2 に丸められる。
 * @param radius 球の半径 (既定 0.5)。
 * @param segments 経度方向の分割数 (既定 32)。
 * @param rings 緯度方向の分割数 (既定 16)。
 * @return 生成した球の AMeshAsset。
 */
TSharedPtr<AMeshAsset> MakeSphere(f32 radius = 0.5f, u32 segments = 32, u32 rings = 16) noexcept;

/**
 * XZ 平面メッシュを生成する (Y=0、法線 +Y)。
 *
 * @param width X 方向の幅 (既定 1.0)。
 * @param depth Z 方向の奥行き (既定 1.0)。
 * @return 生成した平面の AMeshAsset。
 */
TSharedPtr<AMeshAsset> MakePlane(f32 width = 1.0f, f32 depth = 1.0f) noexcept;

} // namespace Primitive

} // namespace acs
