// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// render/VertexScatter.h の検証 (頂点空間サブサーフェススキャタリング):
//   メッシュ隣接グラフ上の RGB 拡散ソルバ。GPU 不要 (純 logic)。
//   検証項目:
//     - Build がグリッド/球メッシュで成功する
//     - 一様な放射照度は拡散で保存される (大域平均 = エネルギー保存)
//     - 1 点の放射照度が «隣接» へ広がり、遠い頂点ほど弱い (局所性)
//     - 赤チャンネルが青より «遠く» まで散る (波長依存プロファイル = 肌の見た目)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "render/VertexScatter.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

using namespace acs;

namespace {

// N×N の格子平面メッシュ (頂点 = 格子点、四角形を 2 三角形に)。
// 行 r 列 c の頂点 index = r*N + c。隣接が規則的なので拡散の局所性を検証しやすい。
void MakeGrid(u32 N, TArray<FVec3>& pos, TArray<u32>& idx) noexcept {
    pos.Reset(); idx.Reset();
    for (u32 r = 0; r < N; ++r)
        for (u32 c = 0; c < N; ++c)
            pos.Add(FVec3{ static_cast<f32>(c), 0.0f, static_cast<f32>(r) });
    for (u32 r = 0; r + 1 < N; ++r)
        for (u32 c = 0; c + 1 < N; ++c) {
            const u32 a = r * N + c, b = r * N + c + 1, d = (r + 1) * N + c, e = (r + 1) * N + c + 1;
            idx.Add(a); idx.Add(b); idx.Add(d);
            idx.Add(b); idx.Add(e); idx.Add(d);
        }
}

f32 SumX(const TArray<FVec3>& v) noexcept { f32 s = 0; for (u32 i = 0; i < v.Num(); ++i) s += v[i].x; return s; }

} // namespace

// --- Build は格子で成功する ------------------------------------------------
ACS_TEST(VertexScatter, BuildGridSucceeds) {
    TArray<FVec3> pos; TArray<u32> idx;
    MakeGrid(5, pos, idx);
    CVertexScatter vs;
    EXPECT_TRUE(vs.Build(pos.GetData(), static_cast<u32>(pos.Num()), idx.GetData(), static_cast<u32>(idx.Num())));
    EXPECT_TRUE(vs.IsBuilt());
    EXPECT_EQ(vs.VertexCount(), 25u);
}

// --- 一様な放射照度は拡散で保存される (エネルギー保存) ----------------------
ACS_TEST(VertexScatter, UniformIsPreserved) {
    TArray<FVec3> pos; TArray<u32> idx;
    MakeGrid(6, pos, idx);
    CVertexScatter vs;
    EXPECT_TRUE(vs.Build(pos.GetData(), static_cast<u32>(pos.Num()), idx.GetData(), static_cast<u32>(idx.Num())));

    const u32 n = vs.VertexCount();
    TArray<FVec3> in, out; in.SetNum(n); out.SetNum(n);
    for (u32 i = 0; i < n; ++i) in[i] = FVec3{ 0.5f, 0.5f, 0.5f };   // 一様
    vs.Scatter(in.GetData(), out.GetData(), FScatterProfile{ 10, 10, 10 });
    // 一様場は拡散の不動点 → 各頂点そのまま
    for (u32 i = 0; i < n; ++i) {
        EXPECT_NEAR(out[i].x, 0.5f, 1e-4f);
        EXPECT_NEAR(out[i].y, 0.5f, 1e-4f);
        EXPECT_NEAR(out[i].z, 0.5f, 1e-4f);
    }
}

// --- 1 点の放射照度が隣接へ広がる (局所性) + 大域和の保存 -------------------
ACS_TEST(VertexScatter, PointSpreadsToNeighbors) {
    const u32 N = 9;
    TArray<FVec3> pos; TArray<u32> idx;
    MakeGrid(N, pos, idx);
    CVertexScatter vs;
    EXPECT_TRUE(vs.Build(pos.GetData(), static_cast<u32>(pos.Num()), idx.GetData(), static_cast<u32>(idx.Num())));

    const u32 n = vs.VertexCount();
    const u32 center = (N / 2) * N + (N / 2);          // 中央頂点
    const u32 nb     = center + 1;                     // その右隣
    const u32 corner = 0;                              // 遠い角

    TArray<FVec3> in, out; in.SetNum(n); out.SetNum(n);
    for (u32 i = 0; i < n; ++i) in[i] = FVec3{ 0, 0, 0 };
    in[center] = FVec3{ 1, 1, 1 };                     // 中央に 1 点

    vs.Scatter(in.GetData(), out.GetData(), FScatterProfile{ 6, 6, 6 });

    // 中央は山として残るが拡散で減る
    EXPECT_TRUE(out[center].x > 0.0f);
    EXPECT_TRUE(out[center].x < 1.0f);
    // 隣接へ漏れる
    EXPECT_TRUE(out[nb].x > 1e-3f);
    // 隣接 < 中央 (山が立っている)
    EXPECT_TRUE(out[nb].x < out[center].x);
    // 遠い角は隣接よりずっと弱い (局所性)
    EXPECT_TRUE(out[corner].x < out[nb].x);
    // 大域和は保存 (拡散は平均化のみ、総量不変)
    EXPECT_NEAR(SumX(out), 1.0f, 1e-3f);
}

// --- 赤が青より遠くまで散る (波長依存プロファイル) --------------------------
ACS_TEST(VertexScatter, RedScattersFartherThanBlue) {
    const u32 N = 11;
    TArray<FVec3> pos; TArray<u32> idx;
    MakeGrid(N, pos, idx);
    CVertexScatter vs;
    EXPECT_TRUE(vs.Build(pos.GetData(), static_cast<u32>(pos.Num()), idx.GetData(), static_cast<u32>(idx.Num())));

    const u32 n = vs.VertexCount();
    const u32 center = (N / 2) * N + (N / 2);
    const u32 far    = (N / 2) * N + (N / 2) + 3;      // 中央から 3 マス右

    TArray<FVec3> in, out; in.SetNum(n); out.SetNum(n);
    for (u32 i = 0; i < n; ++i) in[i] = FVec3{ 0, 0, 0 };
    in[center] = FVec3{ 1, 1, 1 };                     // 白色 1 点

    // 赤の反復多 / 青の反復少 → 赤が遠くまで散る肌プロファイル
    vs.Scatter(in.GetData(), out.GetData(), FScatterProfile{ 14, 6, 2 });

    // 遠点では赤 > 緑 > 青 (赤が最も遠くまで届く)
    EXPECT_TRUE(out[far].x > out[far].z);              // 赤 > 青
    EXPECT_TRUE(out[far].x > out[far].y);              // 赤 > 緑
    EXPECT_TRUE(out[far].y >= out[far].z - 1e-6f);     // 緑 >= 青
    // 中央では逆に青が最も残る (散らないぶん中央に留まる)
    EXPECT_TRUE(out[center].z > out[center].x);        // 中央: 青 > 赤
}

// --- 実プリミティブ (球、UV シーム/極の重複頂点あり) で Build + Scatter -----
ACS_TEST(VertexScatter, SphereBuildAndScatter) {
    auto sphere = Primitive::MakeSphere(1.0f, 24, 12);
    EXPECT_TRUE(sphere.Get() != nullptr);
    CVertexScatter vs;
    EXPECT_TRUE(vs.Build(*sphere));
    EXPECT_TRUE(vs.IsBuilt());

    const u32 n = vs.VertexCount();
    TArray<FVec3> in, out; in.SetNum(n); out.SetNum(n);
    // 片側だけ照らした放射照度 (x>0 の頂点のみ) を入れる
    f32 sum_in = 0.0f;
    const auto& vtx = sphere->Vertices();
    for (u32 i = 0; i < n; ++i) {
        const f32 lit = (vtx[i].position.x > 0.0f) ? 1.0f : 0.0f;
        in[i] = FVec3{ lit, lit, lit };
        sum_in += lit;
    }
    vs.Scatter(in.GetData(), out.GetData(), FScatterProfile{ 10, 4, 2 });
    // 散乱後も有限で非負、暗側 (x<0) にも赤が回り込む (terminator のにじみ)
    f32 dark_red = 0.0f; u32 dark_cnt = 0;
    for (u32 i = 0; i < n; ++i) {
        EXPECT_TRUE(out[i].x >= -1e-5f);
        if (vtx[i].position.x < -0.2f) { dark_red += out[i].x; dark_cnt++; }
    }
    EXPECT_TRUE(dark_cnt > 0);
    EXPECT_TRUE(dark_red > 0.0f);                      // 暗側へ赤が漏れている
}
