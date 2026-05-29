// SPDX-License-Identifier: Apache-2.0
// 3D 凸包 — 点群 (メッシュ頂点等) から凸包の三角形メッシュを生成する。
// 凹メッシュの「凸コライダー」近似や、軽量な当たり判定形状に使う。
//
//   acs::TArray<acs::FVec3> hv; acs::TArray<acs::u32> hi;
//   acs::BuildConvexHull3(points, count, hv, hi);
//   // hv = 凸包頂点、hi = 三角形インデックス (3 個 1 組)。
//   // そのまま FMeshCollider::BuildFromTriangles(hv.Data(), hv.Size(), hi.Data(), hi.Size())。
//
// アルゴリズムは incremental hull (初期四面体 + 各点を可視面を剥がして追加)。
// 視覚非依存の純 CPU なのでヘッドレスで完全検証できる。
#pragma once

#include "foundation/Result.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs {

// points/count から凸包を構築し、out_verts (凸包頂点) と out_indices (三角形、
// 3 個 1 組) を埋める。点が 4 個未満 / 同一平面など退化時はエラー。
TResult<void> BuildConvexHull3(const FVec3* points, u32 count,
                               TArray<FVec3>& out_verts,
                               TArray<u32>& out_indices) noexcept;

} // namespace acs
