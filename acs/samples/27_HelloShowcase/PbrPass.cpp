// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — opaque HDR pass の実装。
#include "PbrPass.h"

#include "math/Math.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"

using namespace acs;

namespace helloshowcase {

namespace {

// material lobe 系のパラメータを 1 mesh 描く前にデフォルト値へ戻す。
// 前回の sphere の sheen / iridescence / subsurface / emissive が引き継がれて
// 意図しない見た目になるのを防ぐ。
void ResetMaterialLobes(FPbrShader& pbr) noexcept {
    pbr.SetExtParams(0.0f, 0.5f, 0.0f, FVec3{1, 0, 0});
    pbr.SetSheen(FVec3{0, 0, 0}, 0.0f, 0.0f);
    pbr.SetIridescence(0.0f, 400.0f, 1.35f);
    pbr.SetSubsurface(FVec3{0, 0, 0}, 0.0f);
}

// orb i (= 0..kOrbCount-1) の公転 transform。floor 平面の円軌道に、
// 0.85x ~ 1.15x の鼓動的 pulse を乗せる (orb ごとに位相をずらす)。
FMat4 OrbTransform(u32 i, f32 phase) noexcept {
    const f32 ang = phase + static_cast<f32>(i) * kPi;
    const FVec3 pos{
        kOrbOrbitRadius * Sin(ang),
        kOrbY,
        kOrbOrbitRadius * Cos(ang),
    };
    const f32 pulse = kOrbScale *
                      (1.0f + 0.15f * Sin(phase * 2.7f
                                          + static_cast<f32>(i) * (kPi * 0.5f)));
    return FMat4::Scale(FVec3{pulse, pulse, pulse}) * FMat4::Translation(pos);
}

} // namespace

void ExecutePbrPass(Assets& a, IRhiCommandList& cl,
                    IRhiTexture& hdr, IRhiTexture& depth,
                    const FCamera& camera,
                    const FMat4& view_proj_jittered,
                    FVec3 cam_pos, f32 orb_phase,
                    bool ssr_warm, bool ssao_warm,
                    FMat4 (&orb_curr_out)[kOrbCount]) noexcept {
    cl.BeginRenderToTexture(hdr, FClearColor{0, 0, 0, 1}, &depth, 1.0f);

    FViewport vp{};
    vp.width  = static_cast<f32>(hdr.Width());
    vp.height = static_cast<f32>(hdr.Height());
    cl.SetViewport(vp);
    FScissorRect svr{};
    svr.right  = static_cast<i32>(hdr.Width());
    svr.bottom = static_cast<i32>(hdr.Height());
    cl.SetScissor(svr);

    a.sky.Render(cl, camera);

    FDirLight sun{};
    sun.direction = a.sky.SunDirection();
    sun.color     = a.sky.SunColor() * 1.2f;
    a.pbr.SetLights(view_proj_jittered, cam_pos, &sun, 1, FVec3{0, 0, 0});
    a.pbr.SetIbl(a.ibl.IrradianceMap(), a.ibl.PrefilterMap(),
                 a.ibl.BrdfLut(), a.ibl.PrefilterMips());

    // SSR / SSAO は 1-frame latency。warm flag が立つまで null を渡し
    // frame 0 のゴミを合成しないようにする。
    if (ssr_warm && a.ssr.OutputTexture()) {
        a.pbr.SetSsr(a.ssr.OutputTexture(), 1.0f);
    } else {
        a.pbr.SetSsr(nullptr, 0.0f);
    }
    if (ssao_warm) {
        a.pbr.SetSsao(a.ssao.OutputTexture(), 0.8f,
                      hdr.Width(), hdr.Height());
    } else {
        a.pbr.SetSsao(nullptr, 0.0f, hdr.Width(), hdr.Height());
    }

    // 床
    const FMat4 floor_model = FMat4::Translation(FVec3{0, kFloorY, 0});
    ResetMaterialLobes(a.pbr);
    a.pbr.DrawMesh(cl, a.gm_floor, floor_model,
                   FVec3{0.18f, 0.19f, 0.21f}, 0.0f, 0.45f, 1.0f);

    // 4 体の sphere (ガラス系は refraction pass で描くため skip)
    for (u32 i = 0; i < kSphereCount; ++i) {
        const MaterialKind k = kSphereKind[i];
        if (k == MaterialKind::ClearGlass || k == MaterialKind::FrostedGlass) {
            continue;
        }
        const FMat4 m = FMat4::Scale(FVec3{kSphereScale, kSphereScale, kSphereScale}) *
                       FMat4::Translation(FVec3{kSphereX[i], kSphereY, kSphereZ});
        ResetMaterialLobes(a.pbr);
        a.pbr.SetEmissive(FVec3{0, 0, 0}, 0.0f);

        if (k == MaterialKind::Gold) {
            // 金: metallic 1.0、low roughness、base color は典型的な金
            a.pbr.DrawMesh(cl, a.gm_sphere, m,
                           FVec3{1.00f, 0.78f, 0.34f}, 1.0f, 0.15f, 1.0f);
        } else /* Ceramic */ {
            // セラミック: 高 roughness、白ベース
            a.pbr.DrawMesh(cl, a.gm_sphere, m,
                           FVec3{0.92f, 0.92f, 0.92f}, 0.0f, 0.75f, 1.0f);
        }
    }

    // 公転する emissive オーブ (motion pass が再利用するため transform を返す)
    for (u32 i = 0; i < kOrbCount; ++i) {
        orb_curr_out[i] = OrbTransform(i, orb_phase);
        ResetMaterialLobes(a.pbr);
        a.pbr.SetEmissive(kOrbColors[i], kOrbEmissiveStrength);
        a.pbr.DrawMesh(cl, a.gm_sphere, orb_curr_out[i],
                       kOrbColors[i], 0.0f, 0.4f, 1.0f);
    }
    a.pbr.SetEmissive(FVec3{0, 0, 0}, 0.0f);

    cl.EndRenderToTexture(hdr);
}

} // namespace helloshowcase
