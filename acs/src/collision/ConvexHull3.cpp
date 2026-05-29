// SPDX-License-Identifier: Apache-2.0
// 3D 凸包 (incremental hull) 実装。
#include "collision/ConvexHull3.h"
#include "foundation/Error.h"

namespace acs {

namespace {

constexpr u16 kSubHullDegenerate = 421;
constexpr u16 kSubHullTooFew     = 422;

struct Face {
    u32   a, b, c;
    FVec3 n;        // 外向き法線
    f32   d;        // 平面: dot(n, x) = d
    bool  visible;
};

// a,b,c から centroid の外側を向く法線で Face を作る。
Face MakeFace(const FVec3* p, u32 a, u32 b, u32 c, FVec3 centroid) noexcept {
    FVec3 n = Normalize(Cross(p[b] - p[a], p[c] - p[a]));
    f32   d = Dot(n, p[a]);
    if (Dot(n, centroid) > d) {       // centroid が正側 → 内向き。反転して外向きに。
        n = FVec3{ -n.x, -n.y, -n.z };
        const u32 tmp = b; b = c; c = tmp;   // 巻き順も合わせる
        d = Dot(n, p[a]);
    }
    return Face{ a, b, c, n, d, false };
}

} // namespace

TResult<void> BuildConvexHull3(const FVec3* points, u32 count,
                               TArray<FVec3>& out_verts,
                               TArray<u32>& out_indices) noexcept {
    out_verts.Clear();
    out_indices.Clear();
    if (!points || count < 4) {
        return ACS_ERR(Generic, kSubHullTooFew, "BuildConvexHull3: need >= 4 points");
    }

    // ===== 初期四面体: affinely independent な 4 点を選ぶ =====
    u32 i0 = 0;
    for (u32 i = 1; i < count; ++i) if (points[i].x < points[i0].x) i0 = i;

    u32 i1 = (i0 == 0) ? 1u : 0u;
    f32 best = -1.0f;
    for (u32 i = 0; i < count; ++i) {
        const f32 dl = LengthSq(points[i] - points[i0]);
        if (dl > best) { best = dl; i1 = i; }
    }
    if (best < 1e-10f) return ACS_ERR(Generic, kSubHullDegenerate, "BuildConvexHull3: all points coincident");

    const FVec3 e01 = points[i1] - points[i0];
    u32 i2 = 0; best = -1.0f;
    for (u32 i = 0; i < count; ++i) {
        const FVec3 cr = Cross(e01, points[i] - points[i0]);
        const f32   dl = LengthSq(cr);
        if (dl > best) { best = dl; i2 = i; }
    }
    if (best < 1e-10f) return ACS_ERR(Generic, kSubHullDegenerate, "BuildConvexHull3: points collinear");

    const FVec3 planeN = Normalize(Cross(e01, points[i2] - points[i0]));
    const f32   plane_d = Dot(planeN, points[i0]);
    u32 i3 = 0; best = -1.0f;
    for (u32 i = 0; i < count; ++i) {
        const f32 dist = Abs(Dot(planeN, points[i]) - plane_d);
        if (dist > best) { best = dist; i3 = i; }
    }
    if (best < 1e-6f) return ACS_ERR(Generic, kSubHullDegenerate, "BuildConvexHull3: points coplanar");

    const FVec3 centroid = (points[i0] + points[i1] + points[i2] + points[i3]) * 0.25f;

    TArray<Face> faces;
    faces.PushBack(MakeFace(points, i0, i1, i2, centroid));
    faces.PushBack(MakeFace(points, i0, i1, i3, centroid));
    faces.PushBack(MakeFace(points, i0, i2, i3, centroid));
    faces.PushBack(MakeFace(points, i1, i2, i3, centroid));

    TArray<u8> processed;
    processed.Resize(count);
    for (u32 i = 0; i < count; ++i) processed[i] = 0;
    processed[i0] = processed[i1] = processed[i2] = processed[i3] = 1;

    // スケール依存の可視 eps
    const f32 eps = 1e-5f * (1.0f + Length(points[i1] - points[i0]));

    struct Edge { u32 a, b; };
    TArray<Edge> horizon;

    for (u32 k = 0; k < count; ++k) {
        if (processed[k]) continue;
        const FVec3 P = points[k];

        bool any_visible = false;
        for (u32 f = 0; f < faces.Size(); ++f) {
            faces[f].visible = (Dot(faces[f].n, P) > faces[f].d + eps);
            any_visible = any_visible || faces[f].visible;
        }
        if (!any_visible) continue;   // 内部または面上 → スキップ

        // 地平線エッジ: 可視面の辺のうち、反対側の面が非可視 (or 無し) なもの。
        horizon.Clear();
        for (u32 f = 0; f < faces.Size(); ++f) {
            if (!faces[f].visible) continue;
            const u32 vs[3] = { faces[f].a, faces[f].b, faces[f].c };
            for (int e = 0; e < 3; ++e) {
                const u32 ea = vs[e];
                const u32 eb = vs[(e + 1) % 3];
                // 反対向き辺 (eb,ea) を持つ面を探す
                bool neighbor_visible = false;
                bool found = false;
                for (u32 g = 0; g < faces.Size(); ++g) {
                    if (g == f) continue;
                    const u32 gv[3] = { faces[g].a, faces[g].b, faces[g].c };
                    for (int ge = 0; ge < 3; ++ge) {
                        if (gv[ge] == eb && gv[(ge + 1) % 3] == ea) {
                            found = true; neighbor_visible = faces[g].visible; break;
                        }
                    }
                    if (found) break;
                }
                if (!found || !neighbor_visible) horizon.PushBack(Edge{ ea, eb });
            }
        }

        // 可視面を除去 (非可視を前方に詰める)
        u32 w = 0;
        for (u32 f = 0; f < faces.Size(); ++f) {
            if (!faces[f].visible) { faces[w++] = faces[f]; }
        }
        while (faces.Size() > w) faces.PopBack();

        // 地平線エッジ + P で新面を張る
        for (u32 h = 0; h < horizon.Size(); ++h) {
            faces.PushBack(MakeFace(points, horizon[h].a, horizon[h].b, k, centroid));
        }
        processed[k] = 1;
    }

    // ===== 使用頂点を集めて remap → out_verts / out_indices =====
    TArray<i32> remap;
    remap.Resize(count);
    for (u32 i = 0; i < count; ++i) remap[i] = -1;
    for (u32 f = 0; f < faces.Size(); ++f) {
        const u32 idx[3] = { faces[f].a, faces[f].b, faces[f].c };
        for (int e = 0; e < 3; ++e) {
            const u32 v = idx[e];
            if (remap[v] < 0) {
                remap[v] = static_cast<i32>(out_verts.Size());
                out_verts.PushBack(points[v]);
            }
            out_indices.PushBack(static_cast<u32>(remap[v]));
        }
    }
    return Ok();
}

} // namespace acs
