// SPDX-License-Identifier: Apache-2.0
// 3D メッシュコライダー実装 (三角形リスト + median/midpoint-split BVH)。
#include "collision/MeshCollider.h"
#include "asset/MeshAsset.h"
#include "foundation/Error.h"

namespace acs {

namespace {

/** 引数不正 (null/空 positions、範囲外 index 等) を表すサブエラーコード。 */
constexpr u16 kSubColMeshInvalidArg = 401;

/** 三角形が 1 つも作れなかったことを表すサブエラーコード。 */
constexpr u16 kSubColMeshEmpty      = 402;

/** BVH の葉に許す最大三角形数 (これ以下で分割を止める)。 */
constexpr u32 kBvhLeafSize          = 4;

/**
 * ベクトルの指定軸成分を返す。
 *
 * @param v 取り出す元のベクトル。
 * @param axis 軸 (0=x, 1=y, 2=z)。
 * @return 指定軸の成分値。
 */
ACS_FORCEINLINE f32 AxisVal(FVec3 v, int axis) noexcept {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

/**
 * 各成分ごとに最小値を取って min 点を p で広げる。
 *
 * @param mn 更新する min 点 (in/out)。
 * @param p 取り込む点。
 */
void ExpandMin(FVec3& mn, FVec3 p) noexcept {
    if (p.x < mn.x) mn.x = p.x;
    if (p.y < mn.y) mn.y = p.y;
    if (p.z < mn.z) mn.z = p.z;
}

/**
 * 各成分ごとに最大値を取って max 点を p で広げる。
 *
 * @param mx 更新する max 点 (in/out)。
 * @param p 取り込む点。
 */
void ExpandMax(FVec3& mx, FVec3 p) noexcept {
    if (p.x > mx.x) mx.x = p.x;
    if (p.y > mx.y) mx.y = p.y;
    if (p.z > mx.z) mx.z = p.z;
}

} // namespace

/** レンダリング用メッシュから collider を構築する (頂点位置のみ使用)。 */
TResult<void> CMeshCollider::BuildFromMesh(const AMeshAsset& mesh) noexcept {
    const TArray<FMeshVertex>& verts = mesh.Vertices();
    const TArray<u32>&        idx   = mesh.Indices();
    if (verts.Size() == 0) {
        return ACS_ERR(Generic, kSubColMeshEmpty, "CMeshCollider: mesh has no vertices");
    }
    // 位置だけ抜き出して BuildFromTriangles に渡す。
    TArray<FVec3> pos;
    pos.Reserve(verts.Size());
    for (usize i = 0; i < verts.Size(); ++i) pos.PushBack(verts[i].position);
    return BuildFromTriangles(pos.Data(), static_cast<u32>(pos.Size()),
                              idx.Data(), static_cast<u32>(idx.Size()));
}

/** 生の頂点位置 + 三角形インデックス列から collider を構築する。 */
TResult<void> CMeshCollider::BuildFromTriangles(const FVec3* positions, u32 vertex_count,
                                                const u32* indices, u32 index_count) noexcept {
    Clear();
    if (!positions || vertex_count == 0) {
        return ACS_ERR(Generic, kSubColMeshInvalidArg, "CMeshCollider: null/empty positions");
    }
    // index がなければ連番三角形として扱う。
    const bool use_index = (indices != nullptr && index_count >= 3);
    const u32  tri_src   = use_index ? index_count : vertex_count;
    if (tri_src < 3) {
        return ACS_ERR(Generic, kSubColMeshInvalidArg, "CMeshCollider: fewer than 3 vertices");
    }
    const u32 tri_count = tri_src / 3u;
    m_Tris.Reserve(tri_count);

    FVec3 mn{ 3.4e38f, 3.4e38f, 3.4e38f };
    FVec3 mx{ -3.4e38f, -3.4e38f, -3.4e38f };
    for (u32 t = 0; t < tri_count; ++t) {
        u32 i0, i1, i2;
        if (use_index) { i0 = indices[t*3+0]; i1 = indices[t*3+1]; i2 = indices[t*3+2]; }
        else           { i0 = t*3+0;          i1 = t*3+1;          i2 = t*3+2; }
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
            Clear();
            return ACS_ERR(Generic, kSubColMeshInvalidArg, "CMeshCollider: index out of range");
        }
        FTri tr;
        tr.v0 = positions[i0];
        tr.v1 = positions[i1];
        tr.v2 = positions[i2];
        tr.centroid = (tr.v0 + tr.v1 + tr.v2) * (1.0f / 3.0f);
        ExpandMin(mn, tr.v0); ExpandMin(mn, tr.v1); ExpandMin(mn, tr.v2);
        ExpandMax(mx, tr.v0); ExpandMax(mx, tr.v1); ExpandMax(mx, tr.v2);
        m_Tris.PushBack(tr);
    }
    if (m_Tris.Size() == 0) {
        return ACS_ERR(Generic, kSubColMeshEmpty, "CMeshCollider: no triangles built");
    }
    m_Bounds = FAabb3::FromMinMax(mn, mx);
    BuildBvh();
    return Ok();
}

/** 三角形・BVH・境界をすべて破棄して空状態に戻す。 */
void CMeshCollider::Clear() noexcept {
    m_Tris.Clear();
    m_TriIndex.Clear();
    m_Nodes.Clear();
    m_Bounds = FAabb3{};
}

/** m_TriIndex の [first, first+count) 範囲の三角形を包む AABB を求める。 */
FAabb3 CMeshCollider::ComputeBounds(u32 first, u32 count) const noexcept {
    FVec3 mn{ 3.4e38f, 3.4e38f, 3.4e38f };
    FVec3 mx{ -3.4e38f, -3.4e38f, -3.4e38f };
    for (u32 i = 0; i < count; ++i) {
        const FTri& t = m_Tris[m_TriIndex[first + i]];
        ExpandMin(mn, t.v0); ExpandMin(mn, t.v1); ExpandMin(mn, t.v2);
        ExpandMax(mx, t.v0); ExpandMax(mx, t.v1); ExpandMax(mx, t.v2);
    }
    return FAabb3::FromMinMax(mn, mx);
}

/** m_Tris から median-split AABB BVH を構築する。 */
void CMeshCollider::BuildBvh() noexcept {
    m_Nodes.Clear();
    m_TriIndex.Clear();
    if (m_Tris.Size() == 0) return;
    m_TriIndex.Reserve(m_Tris.Size());
    for (u32 i = 0; i < m_Tris.Size(); ++i) m_TriIndex.PushBack(i);

    m_Nodes.PushBack(FBvhNode{});      // ルートはノード 0

    struct FItem { u32 node, first, count; };
    TArray<FItem> stack;
    stack.PushBack(FItem{ 0, 0, static_cast<u32>(m_Tris.Size()) });

    while (stack.Size() > 0) {
        const FItem it = stack[stack.Size() - 1];
        stack.PopBack();

        const FAabb3 b = ComputeBounds(it.first, it.count);
        m_Nodes[it.node].bounds = b;

        if (it.count <= kBvhLeafSize) {
            m_Nodes[it.node].first_tri = it.first;
            m_Nodes[it.node].tri_count = it.count;
            continue;
        }

        // 最長軸の空間中点で partition。
        const FVec3 ext = b.half_size;
        const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0
                       : (ext.y >= ext.z ? 1 : 2);
        const f32 mid = AxisVal(b.center, axis);

        u32 i = it.first;
        const u32 hi = it.first + it.count;
        for (u32 j = it.first; j < hi; ++j) {
            if (AxisVal(m_Tris[m_TriIndex[j]].centroid, axis) < mid) {
                const u32 tmp = m_TriIndex[i]; m_TriIndex[i] = m_TriIndex[j]; m_TriIndex[j] = tmp;
                ++i;
            }
        }
        u32 left_count = i - it.first;
        if (left_count == 0 || left_count == it.count) left_count = it.count / 2;  // 退化時は半分

        const u32 left_idx = static_cast<u32>(m_Nodes.Size());
        m_Nodes.PushBack(FBvhNode{});
        m_Nodes.PushBack(FBvhNode{});
        m_Nodes[it.node].left      = left_idx;
        m_Nodes[it.node].tri_count = 0;     // internal

        stack.PushBack(FItem{ left_idx,     it.first,              left_count });
        stack.PushBack(FItem{ left_idx + 1, it.first + left_count, it.count - left_count });
    }
}

/** レイを撃って最近接ヒットを返す (三角形面法線つき)。 */
FRayHit3 CMeshCollider::Raycast(const FRay3& ray, f32 t_max) const noexcept {
    FRayHit3 best{};
    if (m_Nodes.Size() == 0) return best;
    f32 closest = t_max;

    u32 stack[128];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const u32 ni = stack[--sp];
        const FBvhNode& n = m_Nodes[ni];
        const FRayHit3 bh = RaycastAabb(ray, n.bounds, closest);
        if (!bh.hit) continue;                  // box ミス or closest より遠い

        if (n.tri_count > 0) {                   // leaf
            for (u32 k = 0; k < n.tri_count; ++k) {
                const FTri& tr = m_Tris[m_TriIndex[n.first_tri + k]];
                const FRayHit3 th = RaycastTriangle(ray, tr.v0, tr.v1, tr.v2, closest);
                if (th.hit && th.t < closest) { closest = th.t; best = th; }
            }
        } else if (sp + 2 <= 128) {              // internal
            stack[sp++] = n.left;
            stack[sp++] = n.left + 1;
        }
    }
    return best;
}

/** AABB がメッシュと重なるかを判定する (broadphase 用)。 */
bool CMeshCollider::OverlapsAabb(const FAabb3& box) const noexcept {
    if (m_Nodes.Size() == 0) return false;
    u32 stack[128];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const u32 ni = stack[--sp];
        const FBvhNode& n = m_Nodes[ni];
        if (!Intersect(n.bounds, box)) continue;
        if (n.tri_count > 0) {
            for (u32 k = 0; k < n.tri_count; ++k) {
                const FTri& t = m_Tris[m_TriIndex[n.first_tri + k]];
                FVec3 mn = t.v0, mx = t.v0;
                ExpandMin(mn, t.v1); ExpandMin(mn, t.v2);
                ExpandMax(mx, t.v1); ExpandMax(mx, t.v2);
                if (Intersect(FAabb3::FromMinMax(mn, mx), box)) return true;
            }
        } else if (sp + 2 <= 128) {
            stack[sp++] = n.left;
            stack[sp++] = n.left + 1;
        }
    }
    return false;
}

} // namespace acs
