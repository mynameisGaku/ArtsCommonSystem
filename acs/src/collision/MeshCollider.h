// SPDX-License-Identifier: Apache-2.0
// 3D メッシュコライダー — 三角形メッシュから BVH を構築し、レイキャスト等の
// 衝突クエリを高速化する。
//
// レンダリング用の FMeshAsset (位置 + 法線 + UV、インデックス三角形列) や生の
// 頂点/インデックス配列からそのまま collider を作れる。中身は三角形リスト +
// median-split AABB BVH。
//
//   acs::FMeshCollider col;
//   col.BuildFromMesh(*Primitive::MakeCube(2.0f));
//   acs::Ray3 ray{ {0, 5, 0}, {0, -1, 0} };
//   acs::RayHit3 h = col.Raycast(ray);   // h.hit / h.t / h.point / h.normal
//
// ACS 規約: STL/<string> 不使用、全 noexcept、TResult、非コピー。視覚に依存
// しない純 CPU ロジックなのでヘッドレスで完全に検証できる。
#pragma once

#include "foundation/Result.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Collision3D.h"

namespace acs {

class FMeshAsset;

class FMeshCollider {
public:
    FMeshCollider() noexcept = default;
    ~FMeshCollider() noexcept = default;
    FMeshCollider(const FMeshCollider&)            = delete;
    FMeshCollider& operator=(const FMeshCollider&) = delete;

    // レンダリング用メッシュからそのまま構築 (位置のみ使用)。
    TResult<void> BuildFromMesh(const FMeshAsset& mesh) noexcept;
    // 生の頂点位置 + 三角形インデックス列から構築。
    TResult<void> BuildFromTriangles(const FVec3* positions, u32 vertex_count,
                                     const u32* indices, u32 index_count) noexcept;
    void Clear() noexcept;

    // 最近接ヒットのレイキャスト (三角形面法線つき)。未ヒットは hit=false。
    RayHit3 Raycast(const Ray3& ray, f32 t_max = 3.4028235e38f) const noexcept;

    // AABB との重なり (broadphase 用、葉の三角形 AABB と判定)。
    bool OverlapsAabb(const Aabb3& box) const noexcept;

    Aabb3 Bounds()        const noexcept { return m_Bounds; }
    u32   TriangleCount() const noexcept { return m_Tris.Size(); }
    u32   NodeCount()     const noexcept { return m_Nodes.Size(); }

private:
    struct Tri {
        FVec3 v0, v1, v2;
        FVec3 centroid;
    };
    // leaf: tri_count>0 で [first_tri, first_tri+tri_count) が m_TriIndex の範囲。
    // internal: tri_count==0、left/left+1 が子ノード index。
    struct BvhNode {
        Aabb3 bounds;
        u32   left      = 0;
        u32   first_tri = 0;
        u32   tri_count = 0;
    };

    void  BuildBvh() noexcept;
    Aabb3 ComputeBounds(u32 first, u32 count) const noexcept;
    void  RaycastNode(u32 node, const Ray3& ray, f32& closest, RayHit3& out) const noexcept;

    TArray<Tri>     m_Tris;
    TArray<u32>     m_TriIndex;   // BVH 用の三角形並べ替えインデックス
    TArray<BvhNode> m_Nodes;
    Aabb3           m_Bounds{};
};

} // namespace acs
