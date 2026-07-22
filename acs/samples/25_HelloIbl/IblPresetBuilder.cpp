// SPDX-License-Identifier: Apache-2.0
// HelloIbl — IBL リソース構築 (preset 切替対応) の実装。
//
// preset が切り替わったらフラグ (m_bNeedRecapture / m_bNeedStudioHdr / m_bNeedAtmosphere)
// が立つ。本ヘルパはそれを見て env cubemap を作り直し、必要なら SH9 も再計算する。
// 初回フレームでは BRDF LUT / env / irradiance / prefilter を一括生成する。
#include "HelloIblApp.h"
#include "IblEnvBuilder.h"

#include "render/Atmosphere.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloibl {

void ApplyPresetRebuilds(FHelloIblApp& app) noexcept {
    IRhiDevice*      dev = app.GetRenderer().Device();
    IRhiCommandList* cl  = app.GetRenderer().CommandList();
    if (!dev || !cl) return;

    // FSky preset が変わった場合、env / irradiance / prefilter を作り直す
    if (app.m_bNeedRecapture) {
        dev->WaitIdle();
        app.m_Ibl.ResetEnvCubemap();
        app.m_bNeedRecapture = false;
    }
    // Studio HDR preset: equirect float texture を CPU で合成 → LoadEquirectHdr で
    // env cubemap として焼く。
    if (app.m_bNeedStudioHdr) {
        dev->WaitIdle();
        BuildStudioHdrEquirect(app.m_EquirectRgba);
        auto r = app.m_Ibl.LoadEquirectHdrFromMemory(
            *dev, *cl,
            app.m_EquirectRgba.Data(),
            kEquirectWidth, kEquirectHeight);
        if (r.IsErr()) ACS_LOG_ERROR("HelloIbl: LoadEquirectHdr failed");
        app.m_bNeedStudioHdr = false;
    }
    // Hillaire 風物理大気を CPU で焼く
    if (app.m_bNeedAtmosphere) {
        dev->WaitIdle();
        FAtmosphereParams ap;
        ap.sun_dir       = FVec3{0.3f, 0.5f, 0.5f};
        ap.sun_intensity = FVec3{22.0f, 22.0f, 22.0f};
        ap.ray_steps     = 32;
        ap.sun_steps     = 8;
        auto baked = FAtmosphere::BakeEquirect(kEquirectWidth, kEquirectHeight, ap);
        app.m_EquirectRgba = Move(baked);
        auto r = app.m_Ibl.LoadEquirectHdrFromMemory(
            *dev, *cl,
            app.m_EquirectRgba.Data(),
            kEquirectWidth, kEquirectHeight);
        if (r.IsErr()) ACS_LOG_ERROR("HelloIbl: FAtmosphere bake failed");
        app.m_bNeedAtmosphere = false;
    }

    // ===== IBL build (まだ作ってないものだけ。一度作れば cache される) =====
    // BeginRenderToTexture(hdr) の前にやる: IBL の RT 切替は m_MainSwapchain を
    // 触らないので、HDR pass に影響しない。
    if (!app.m_Ibl.HasBrdfLut())       (void)app.m_Ibl.EnsureBrdfLut(*dev, *cl);
    if (!app.m_Ibl.HasEnvCubemap())    (void)app.m_Ibl.EnsureEnvCubemap(*dev, *cl, app.m_Sky);
    if (!app.m_Ibl.HasIrradianceMap()) (void)app.m_Ibl.EnsureIrradiance(*dev, *cl);
    if (!app.m_Ibl.HasPrefilterMap())  (void)app.m_Ibl.EnsurePrefilter(*dev, *cl);
}

} // namespace helloibl
