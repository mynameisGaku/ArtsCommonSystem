// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — Assets 一括初期化 / 解放の実装。
#include "ShowcaseAssets.h"
#include "ShowcaseTypes.h"

#include "app/Sample.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"

using namespace acs;

namespace helloshowcase {

namespace {

// 1 つの Init 呼び出しに失敗したら IsErr で抜けるための短縮マクロ。
// ACS_SAMPLE_INIT は Application::Quit() を必要とするため、free function 内では
// 使えない。代わりに TResult を伝搬する形にする。
#define HS_TRY(expr)                                                              \
    do {                                                                          \
        auto _r = (expr);                                                         \
        if (_r.IsErr()) {                                                         \
            ACS_LOG_ERROR("HelloShowcase init failed at " #expr ": %s",          \
                          _r.Error().message);                                    \
            return _r;                                                            \
        }                                                                         \
    } while (0)

} // namespace

TResult<void> InitializeAssets(Assets& a, IRhiDevice& dev,
                               u32 sw, u32 sh,
                               EFormat color_fmt, EFormat depth_fmt) noexcept {
    // HDR + Bloom + ACES tonemap
    HS_TRY(a.post.Init(dev, sw, sh, color_fmt));

    // Sky / PBR / mesh は全部 HDR format で初期化
    HS_TRY(a.sky.Init(dev, a.post.HdrFormat(), depth_fmt));
    a.sky.PresetDay();
    HS_TRY(a.pbr.Init(dev, a.post.HdrFormat(), depth_fmt));

    auto sphere = Primitive::MakeSphere(kSphereRadius, 48, 24);
    HS_TRY(UploadMesh(dev, *sphere, a.gm_sphere));
    auto plane = Primitive::MakePlane(kFloorSize, kFloorSize);
    HS_TRY(UploadMesh(dev, *plane, a.gm_floor));

    // SSR は env reflection を PbrShader 側で合成する (1 フレーム遅延)
    HS_TRY(a.ssr.Init(dev, a.post.HdrFormat(), sw, sh));
    // Hi-Z coarse min-depth — SSR の skip-ahead 用 1/8 解像度
    HS_TRY(a.hiz.Init(dev, sw, sh));
    // GTAO — indirect / ambient AO
    HS_TRY(a.ssao.Init(dev, sw, sh));
    // motion vector — TAA / SSR temporal の object motion 用
    HS_TRY(a.motion.Init(dev, sw, sh));
    // 屈折ガラス球
    HS_TRY(a.refr.Init(dev, a.post.HdrFormat(), depth_fmt));
    HS_TRY(a.blit.Init(dev, a.post.HdrFormat()));

    // 屈折 pass の背景複製先。HDR と同じ format / 解像度。
    TextureDesc bg_td{};
    bg_td.width  = sw;
    bg_td.height = sh;
    bg_td.format = a.post.HdrFormat();
    bg_td.is_render_target = true;
    auto bg_r = CreateRhiTexture(dev, bg_td);
    if (bg_r.IsErr()) {
        ACS_LOG_ERROR("HelloShowcase: background RT 作成失敗: %s",
                      bg_r.Error().message);
        return Err(bg_r.Error());
    }
    a.bg_rt = Move(bg_r.Value());

    // LDR tonemap 後のオーバーレイ用
    HS_TRY(a.batch.Init(dev, color_fmt));
    (void)Sample::TryLoadDefaultUIFont(a.font, dev, 18.0f, 1024, true);

    return Ok();
}

void ShutdownAssets(Assets& a) noexcept {
    a.font.Shutdown();
    a.batch.Shutdown();
    a.bg_rt.Reset();
    a.blit.Shutdown();
    a.refr.Shutdown();
    a.motion.Shutdown();
    a.ssao.Shutdown();
    a.ssr.Shutdown();
    a.gm_floor  = GpuMesh{};
    a.gm_sphere = GpuMesh{};
    a.hiz.Shutdown();
    a.pbr.Shutdown();
    a.ibl.Shutdown();
    a.sky.Shutdown();
    a.post.Shutdown();
}

#undef HS_TRY

} // namespace helloshowcase
