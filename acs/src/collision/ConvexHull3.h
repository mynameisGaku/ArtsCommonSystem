// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs {

/**
 * 点群から 3D 凸包の三角形メッシュを構築する (incremental hull)。
 *
 * @details
 * 初期四面体を affinely independent な 4 点で張り、残りの点を可視面を剥がしながら
 * 1 点ずつ追加していく。out_verts には凸包の頂点、out_indices には三角形を構成する
 * 頂点インデックスを 3 個 1 組で埋める (out_verts は使用頂点だけに remap される)。
 * そのまま CMeshCollider::BuildFromTriangles に渡せる。点が 4 個未満、全点一致、共線、
 * 同一平面 (coplanar) など退化した入力ではエラーを返す。
 * @param points 入力点群の先頭ポインタ。
 * @param count 入力点の個数 (4 以上が必要)。
 * @param out_verts 凸包頂点を格納する出力配列 (呼び出し時に Clear される)。
 * @param out_indices 三角形インデックス (3 個 1 組) を格納する出力配列 (Clear される)。
 * @return 成功なら空の TResult、点が不足・退化していればエラー。
 */
TResult<void> BuildConvexHull3(const FVec3* points, u32 count,
                               TArray<FVec3>& out_verts,
                               TArray<u32>& out_indices) noexcept;

} // namespace acs
