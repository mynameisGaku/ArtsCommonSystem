// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/MeshAsset.h"
#include "foundation/Types.h"
#include "memory/SharedPtr.h"

namespace acs {

namespace Primitive {

/**
 * 中心が原点の立方体メッシュを生成し、成功時だけ output を置き換える。
 * @param size 立方体の一辺の長さ。有限値だけを受け付ける。
 * @param output 成功時に生成結果を受け取る共有ポインタ。失敗時は変更しない。
 * @return 生成できた場合は true。入力不正または確保失敗なら false。
 */
bool TryMakeCube(f32 size, TSharedPtr<AMeshAsset>& output) noexcept;

/**
 * UV 球メッシュを生成し、成功時だけ output を置き換える。
 * @param radius 球の半径。有限値だけを受け付ける。
 * @param segments 経度方向の分割数。3 未満は 3 に補正する。
 * @param rings 緯度方向の分割数。2 未満は 2 に補正する。
 * @param output 成功時に生成結果を受け取る共有ポインタ。失敗時は変更しない。
 * @return 生成できた場合は true。要素数を usize または u32 で表せない場合、入力不正または確保失敗なら false。
 */
bool TryMakeSphere(f32 radius, u32 segments, u32 rings, TSharedPtr<AMeshAsset>& output) noexcept;

/**
 * XZ 平面メッシュを生成し、成功時だけ output を置き換える。
 * @param width X 方向の幅。有限値だけを受け付ける。
 * @param depth Z 方向の奥行き。有限値だけを受け付ける。
 * @param output 成功時に生成結果を受け取る共有ポインタ。失敗時は変更しない。
 * @return 生成できた場合は true。入力不正または確保失敗なら false。
 */
bool TryMakePlane(f32 width, f32 depth, TSharedPtr<AMeshAsset>& output) noexcept;

/**
 * XY平面のpolygon点列をfan分割し、成功時だけoutputを置き換える。
 * @param points polygon外周のlocal XY点列。有限値だけを受け付ける。
 * @param point_count 点数。3以上でなければならない。
 * @param output 成功時に生成結果を受け取る共有ポインタ。失敗時は変更しない。
 * @return 頂点・index数を表現でき、全確保と生成に成功した場合はtrue。
 */
bool TryMakePolygonXY(const FVec2* points, u32 point_count, TSharedPtr<AMeshAsset>& output) noexcept;

/**
 * 中心が原点の立方体メッシュを生成する互換 API。
 * @param size 立方体の一辺の長さ。
 * @return 生成結果。入力不正または確保失敗時は空。
 */
TSharedPtr<AMeshAsset> MakeCube(f32 size = 1.0f) noexcept;

/**
 * UV 球メッシュを生成する互換 API。
 * @param radius 球の半径。
 * @param segments 経度方向の分割数。
 * @param rings 緯度方向の分割数。
 * @return 生成結果。要素数を usize または u32 で表せない場合、入力不正または確保失敗時は空。
 */
TSharedPtr<AMeshAsset> MakeSphere(f32 radius = 0.5f, u32 segments = 32, u32 rings = 16) noexcept;

/**
 * Y=0、法線 +Y の XZ 平面メッシュを生成する互換 API。
 * @param width X 方向の幅。
 * @param depth Z 方向の奥行き。
 * @return 生成結果。入力不正または確保失敗時は空。
 */
TSharedPtr<AMeshAsset> MakePlane(f32 width = 1.0f, f32 depth = 1.0f) noexcept;

} // namespace Primitive

} // namespace acs
