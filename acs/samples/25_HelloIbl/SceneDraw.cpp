// SPDX-License-Identifier: Apache-2.0
// HelloIbl — シーン本体 (床 + 5x5 球グリッド) の draw。
//
// 球グリッド 5x5: 行ごとに別の material lobe を見せる demo。
//   最上段 (y = kGrid-1)  = cloth / sheen (X 方向に sheen weight を 0→1 掃引)
//   中央段 (y = 2)        = subsurface scattering (X 方向に SSS weight 掃引、暖色)
//   最下段 (y = 0)        = iridescence / 薄膜 (X 方向に膜厚 200→1000nm 掃引)
//   残り 2 段              = 素 PBR (X=metallic, Y=roughness)
//
// 床: 中性灰の plane、glossy (roughness 0.35) にして球の SSR 映り込みを出す。
//     lightmap が有効なときは床にだけ bind する (球には焼いていない)。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

namespace {

constexpr u32 kGrid    = kPbrGridSize;
constexpr f32 kSpacing = 1.4f;

} // anonymous namespace

void DrawFloor(FHelloIblApp& app) noexcept {
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    // clearcoat / anisotropy は無効に上書き、emissive も 0 に reset。
    // roughness 0.35 で glossy にし、SSR の意図 ("磨かれた床に球が映り込む") を物理的に正しく出す。
    app.m_Pbr.SetExtParams(0.0f, 0.08f, 0.0f, FVec3{1, 0, 0});
    app.m_Pbr.SetEmissive(FVec3{0, 0, 0}, 0.0f);

    // 床のみ lightmap を bind (球には焼いていないので OFF)
    if (app.m_bUseLightmap && app.m_Lightmap) {
        app.m_Pbr.SetLightmap(app.m_Lightmap.Get(), /*intensity=*/0.8f);
    } else {
        app.m_Pbr.SetLightmap(nullptr, 0.0f);
    }
    app.m_Pbr.DrawMesh(*cl, app.m_GmPlane,
                      FMat4::Translation(FVec3{0, -0.6f, 3.0f}),
                      FVec3{0.5f, 0.5f, 0.55f}, 0.0f, /*roughness=*/0.35f, 1.0f);
    // 球グリッド draw 前に lightmap を解除 (球には焼いていないので付けない)
    app.m_Pbr.SetLightmap(nullptr, 0.0f);
}

void DrawSphereGrid(FHelloIblApp& app) noexcept {
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    const FVec3 base_color{0.95f, 0.4f, 0.3f};
    // material 拡張 (clear-coat / anisotropic) を有効化
    const f32  cc    = app.m_bUseClearcoat  ? 1.0f : 0.0f;
    const f32  ccr   = 0.08f;                      // 鏡面に近い clear-coat
    const f32  aniso = app.m_bUseAnisotropy ? 0.8f : 0.0f;
    const FVec3 tan_dir{1, 0, 0};                   // 横方向の brushed pattern
    app.m_Pbr.SetExtParams(cc, ccr, aniso, tan_dir);

    for (u32 y = 0; y < kGrid; ++y) {
        // 行ごとに lobe demo 切替
        const bool cloth_row = (y == kGrid - 1);
        const bool sss_row   = (y == 2);
        const bool irid_row  = (y == 0);
        for (u32 x = 0; x < kGrid; ++x) {
            const f32 frac = static_cast<f32>(x) / (kGrid - 1);   // X 掃引 0→1
            app.m_Pbr.SetSheen(FVec3{0.95f, 0.90f, 0.78f}, cloth_row ? frac : 0.0f, 0.4f);
            // iridescence 行は膜厚を X 方向に 200→1000nm 掃引し色変化を見せる。
            app.m_Pbr.SetIridescence(irid_row ? 1.0f : 0.0f, 200.0f + frac * 800.0f, 1.4f);
            // subsurface 行は暖色 (肌/ロウ風) で weight を X 方向に 0→1 掃引。
            app.m_Pbr.SetSubsurface(FVec3{0.90f, 0.30f, 0.18f}, sss_row ? frac : 0.0f);
            // cloth / subsurface 行は非金属 (布・肌・ロウは dielectric)。
            const f32 metallic  = (cloth_row || sss_row) ? 0.0f : frac;
            const f32 roughness = 0.05f + static_cast<f32>(y) / (kGrid - 1) * 0.95f;
            const f32 px = (static_cast<f32>(x) - (kGrid - 1) * 0.5f) * kSpacing;
            const f32 py = (static_cast<f32>(y) - (kGrid - 1) * 0.5f) * kSpacing + 2.5f;
            app.m_Pbr.DrawMesh(*cl, app.m_GmSphere,
                              FMat4::Translation(FVec3{px, py, 3.0f}),
                              base_color, metallic, roughness, 1.0f);
        }
    }
    // 後続描画 (オーブ等) のため reset
    app.m_Pbr.SetSheen(FVec3{0, 0, 0}, 0.0f, 0.3f);
    app.m_Pbr.SetIridescence(0.0f, 400.0f, 1.4f);
    app.m_Pbr.SetSubsurface(FVec3{0, 0, 0}, 0.0f);
}

} // namespace helloibl
