// SPDX-License-Identifier: Apache-2.0
// HelloIbl — IBL リソース構築 (preset 切替対応) の実装。
//
// preset が切り替わったらフラグ (_need_recapture / _need_studio_hdr / _need_atmosphere)
// が立つ。本ヘルパはそれを見て env cubemap を作り直し、必要なら SH9 も再計算する。
// 初回フレームでは BRDF LUT / env / irradiance / prefilter を一括生成する。
#include "HelloIblApp.h"
#include "IblEnvBuilder.h"

#include "render/Atmosphere.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloibl {

void ApplyPresetRebuilds(HelloIblApp& app) noexcept {
    IRhiDevice*      dev = app.GetRenderer().Device();
    IRhiCommandList* cl  = app.GetRenderer().CommandList();
    if (!dev || !cl) return;

    // Sky preset が変わった場合、env / irradiance / prefilter を作り直す
    if (app._need_recapture) {
        dev->WaitIdle();
        app._ibl.ResetEnvCubemap();
        app._need_recapture = false;
    }
    // Studio HDR preset: equirect float texture を CPU で合成 → LoadEquirectHdr で
    // env cubemap として焼く。
    if (app._need_studio_hdr) {
        dev->WaitIdle();
        BuildStudioHdrEquirect(app._equirect_rgba);
        auto r = app._ibl.LoadEquirectHdrFromMemory(
            *dev, *cl,
            app._equirect_rgba.Data(),
            kEquirectWidth, kEquirectHeight);
        if (r.IsErr()) ACS_LOG_ERROR("HelloIbl: LoadEquirectHdr failed");
        app._need_studio_hdr = false;
    }
    // Hillaire 風物理大気を CPU で焼く
    if (app._need_atmosphere) {
        dev->WaitIdle();
        AtmosphereParams ap;
        ap.sun_dir       = FVec3{0.3f, 0.5f, 0.5f};
        ap.sun_intensity = FVec3{22.0f, 22.0f, 22.0f};
        ap.ray_steps     = 32;
        ap.sun_steps     = 8;
        auto baked = Atmosphere::BakeEquirect(kEquirectWidth, kEquirectHeight, ap);
        app._equirect_rgba = Move(baked);
        auto r = app._ibl.LoadEquirectHdrFromMemory(
            *dev, *cl,
            app._equirect_rgba.Data(),
            kEquirectWidth, kEquirectHeight);
        if (r.IsErr()) ACS_LOG_ERROR("HelloIbl: Atmosphere bake failed");
        app._need_atmosphere = false;
    }

    // ===== IBL build (まだ作ってないものだけ。一度作れば cache される) =====
    // BeginRenderToTexture(hdr) の前にやる: IBL の RT 切替は _main_swapchain を
    // 触らないので、HDR pass に影響しない。
    if (!app._ibl.HasBrdfLut())       (void)app._ibl.EnsureBrdfLut(*dev, *cl);
    if (!app._ibl.HasEnvCubemap())    (void)app._ibl.EnsureEnvCubemap(*dev, *cl, app._sky);
    if (!app._ibl.HasIrradianceMap()) (void)app._ibl.EnsureIrradiance(*dev, *cl);
    if (!app._ibl.HasPrefilterMap())  (void)app._ibl.EnsurePrefilter(*dev, *cl);
}

} // namespace helloibl
