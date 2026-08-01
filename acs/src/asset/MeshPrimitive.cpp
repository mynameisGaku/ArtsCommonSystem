// SPDX-License-Identifier: Apache-2.0
// 手続き生成プリミティブ実装
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "math/Math.h"
#include "foundation/Move.h"

namespace acs::Primitive {

namespace {

/**
 * 1 頂点を MeshVertex として組み立てて配列末尾へ追加する。
 *
 * @param dst 追加先の頂点配列。
 * @param pos 頂点位置。
 * @param nrm 頂点法線。
 * @param u テクスチャ座標 U。
 * @param v テクスチャ座標 V。
 */
void PushVertex(TArray<FMeshVertex>& dst, FVec3 pos, FVec3 nrm, f32 u, f32 v) noexcept {
    FMeshVertex mv;
    mv.position = pos;
    mv.normal   = nrm;
    mv.u        = u;
    mv.v        = v;
    dst.PushBack(mv);
}

} // namespace

/** 中心原点の立方体を生成する (6 面 × 4 頂点、面ごとの法線、UV 0..1)。 */
TSharedPtr<AMeshAsset> MakeCube(f32 size) noexcept {
    auto m = MakeShared<AMeshAsset>();
    if (!m) return TSharedPtr<AMeshAsset>();
    auto& V = m->Vertices();
    auto& I = m->Indices();
    V.Reserve(24);
    I.Reserve(36);

    const f32 s = size * 0.5f;
    // 6 面 × 4 頂点 = 24 頂点
    // 各面は ( base_corner, normal ) で表す。UV は (0,0) (1,0) (1,1) (0,1) を割当。
    struct FFace {
        FVec3 a, b, c, d;
        FVec3 n;
    };
    const FFace faces[6] = {
        // -Z (前面)
        {{-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},{0,0,-1}},
        // +Z
        {{ s,-s, s},{-s,-s, s},{-s, s, s},{ s, s, s},{0,0, 1}},
        // -X
        {{-s,-s, s},{-s,-s,-s},{-s, s,-s},{-s, s, s},{-1,0, 0}},
        // +X
        {{ s,-s,-s},{ s,-s, s},{ s, s, s},{ s, s,-s},{ 1,0, 0}},
        // +Y (上)
        {{-s, s,-s},{ s, s,-s},{ s, s, s},{-s, s, s},{0, 1,0}},
        // -Y (下)
        {{-s,-s, s},{ s,-s, s},{ s,-s,-s},{-s,-s,-s},{0,-1,0}},
    };

    for (u32 f = 0; f < 6; ++f) {
        const u32 base = static_cast<u32>(V.Size());
        PushVertex(V, faces[f].a, faces[f].n, 0, 1);
        PushVertex(V, faces[f].b, faces[f].n, 1, 1);
        PushVertex(V, faces[f].c, faces[f].n, 1, 0);
        PushVertex(V, faces[f].d, faces[f].n, 0, 0);
        // 三角形 2 つ（CW: フロントフェイス）
        I.PushBack(base + 0); I.PushBack(base + 1); I.PushBack(base + 2);
        I.PushBack(base + 0); I.PushBack(base + 2); I.PushBack(base + 3);
    }

    FSubMesh sm; sm.first_index = 0; sm.index_count = static_cast<u32>(I.Size());
    m->SubMeshes().PushBack(sm);
    return m;
}

/** UV 球を生成する (緯度 × 経度の格子、segments>=3 / rings>=2 に丸め)。 */
TSharedPtr<AMeshAsset> MakeSphere(f32 radius, u32 segments, u32 rings) noexcept {
    if (segments < 3) segments = 3;
    if (rings    < 2) rings    = 2;

    auto m = MakeShared<AMeshAsset>();
    if (!m) return TSharedPtr<AMeshAsset>();
    auto& V = m->Vertices();
    auto& I = m->Indices();
    V.Reserve((segments + 1) * (rings + 1));
    I.Reserve(segments * rings * 6);

    for (u32 r = 0; r <= rings; ++r) {
        const f32 v_t = static_cast<f32>(r) / static_cast<f32>(rings);
        const f32 phi = v_t * kPi;                    // 0..π
        const f32 sin_phi = Sin(phi);
        const f32 cos_phi = Cos(phi);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 u_t = static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 theta = u_t * kPi * 2.0f;       // 0..2π
            const f32 sin_theta = Sin(theta);
            const f32 cos_theta = Cos(theta);

            const FVec3 nrm{ sin_phi * cos_theta, cos_phi, sin_phi * sin_theta };
            const FVec3 pos{ nrm.x * radius, nrm.y * radius, nrm.z * radius };
            PushVertex(V, pos, nrm, u_t, v_t);
        }
    }
    // インデックス（ring × segment の四角を 2 三角形へ）
    const u32 stride = segments + 1;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 i0 = r * stride + s;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + stride;
            const u32 i3 = i2 + 1;
            // CW front-face (with our coordinate system)
            I.PushBack(i0); I.PushBack(i2); I.PushBack(i1);
            I.PushBack(i1); I.PushBack(i2); I.PushBack(i3);
        }
    }

    FSubMesh sm; sm.first_index = 0; sm.index_count = static_cast<u32>(I.Size());
    m->SubMeshes().PushBack(sm);
    return m;
}

/** XZ 平面を生成する (4 頂点・2 三角形、Y=0、法線 +Y)。 */
TSharedPtr<AMeshAsset> MakePlane(f32 width, f32 depth) noexcept {
    auto m = MakeShared<AMeshAsset>();
    if (!m) return TSharedPtr<AMeshAsset>();
    auto& V = m->Vertices();
    auto& I = m->Indices();

    const f32 hw = width * 0.5f;
    const f32 hd = depth * 0.5f;
    const FVec3 up{0, 1, 0};
    PushVertex(V, {-hw, 0, -hd}, up, 0, 1);
    PushVertex(V, { hw, 0, -hd}, up, 1, 1);
    PushVertex(V, { hw, 0,  hd}, up, 1, 0);
    PushVertex(V, {-hw, 0,  hd}, up, 0, 0);
    I.PushBack(0); I.PushBack(1); I.PushBack(2);
    I.PushBack(0); I.PushBack(2); I.PushBack(3);

    FSubMesh sm; sm.first_index = 0; sm.index_count = 6;
    m->SubMeshes().PushBack(sm);
    return m;
}

} // namespace acs::Primitive
