// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — HelloLightmapApp の実装。
//
// シーン: Cornell box (床 / 天井 / 奥壁 / 左壁(赤) / 右壁(緑))。
// 焼き: OnStart で BakeLightmaps を呼んで各面の lightmap texture を生成。
// 描画: HDR RT → Bloom + ACES tonemap → LDR backbuffer + HUD。
#include "HelloLightmapApp.h"
#include "LightmapBaker.h"
#include "CornellBox.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "render/RenderAssets.h"
#include "math/Math.h"

#include <cstdio>

using namespace acs;

namespace hellolightmap {

void HelloLightmapApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!sc) { Quit(); return; }

    // HDR PostProcess (Bloom + ACES tonemap)。シーンは HDR RT に描く。
    ACS_SAMPLE_INIT(_post.Init(*dev, sc->Width(), sc->Height(),
                               GetRenderer().ColorFormat()));
    // PbrShader は HDR RT フォーマットに合わせて init する。
    ACS_SAMPLE_INIT(_pbr.Init(*dev, _post.HdrFormat(),
                              GetRenderer().DepthFormat()));

    BuildCornellBox(*dev, _quads);
    BakeLightmaps(*dev, _quads);

    // SpriteBatch は tonemap 後の LDR backbuffer に描く。
    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(sc->Width()) /
                       static_cast<f32>(sc->Height());
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.05f, 100.0f);
    _cam_pos = FVec3{0.0f, 1.0f, -0.9f};

    // PostProcess パラメータ (絵作りで調整可)。
    _post_params.bloom_threshold    = 2.5f;   // 天井 (光源) だけが bloom する閾値
    _post_params.bloom_intensity    = 0.5f;
    _post_params.grain_intensity    = 0.0f;   // GI デモなので film grain は切る
    _post_params.vignette_intensity = 0.15f;
    _post_params.ssr_intensity      = 0.0f;   // SSR 未使用 (fallback mip の誤加算防止)
}

void HelloLightmapApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::L)) _show_lightmap = !_show_lightmap;

    const f32 mv = 2.0f * dt, tr = 1.4f * dt;
    if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= tr;
    if (Input::IsKeyDown(EKey::Right)) _cam_yaw += tr;
    if (Input::IsKeyDown(EKey::Up))    _cam_pitch -= tr * 0.8f;
    if (Input::IsKeyDown(EKey::Down))  _cam_pitch += tr * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (_cam_pitch >  limit) _cam_pitch =  limit;
    if (_cam_pitch < -limit) _cam_pitch = -limit;
    FVec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                 -Sin(_cam_pitch),
                  Cos(_cam_yaw) * Cos(_cam_pitch) };
    FVec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
    if (Input::IsKeyDown(EKey::W)) _cam_pos += forward * mv;
    if (Input::IsKeyDown(EKey::S)) _cam_pos -= forward * mv;
    if (Input::IsKeyDown(EKey::D)) _cam_pos += right   * mv;
    if (Input::IsKeyDown(EKey::A)) _cam_pos -= right   * mv;
    _camera.SetLookAt(_cam_pos, _cam_pos + forward);
}

// OnCustomFrame: HDR RT にシーンを描き、PostProcess (Bloom + ACES tonemap)
// で LDR backbuffer へ。HDR lightmap の高輝度が tonemap で自然にロールオフする。
bool HelloLightmapApp::OnCustomFrame() noexcept {
    IRhiDevice*      dev   = GetRenderer().Device();
    IRhiCommandList* cl    = GetRenderer().CommandList();
    IRhiSwapchain*   sc    = GetRenderer().Swapchain();
    IRhiTexture*     hdr   = _post.HdrRenderTarget();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    if (!dev || !cl || !sc || !hdr) return false;

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    // ===== HDR RT に Cornell box を描画 =====
    cl->BeginRenderToTexture(*hdr, ClearColor{0, 0, 0, 1}, depth, 1.0f);
    Viewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                   vp.height = static_cast<f32>(hdr->Height());
    cl->SetViewport(vp);
    ScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                       svr.bottom = static_cast<i32>(hdr->Height());
    cl->SetScissor(svr);

    // 動的ライトは使わず ごく弱い ambient のみ。間接光は lightmap が担う。
    _pbr.SetLights(_camera.ViewProjection(), _camera.Eye(),
                   nullptr, 0, FVec3{0.02f, 0.02f, 0.02f});
    _pbr.SetPointLights(nullptr, 0);

    cl->SetPipeline(*_pbr.Pipeline());
    cl->SetConstantBuffer(0, *_pbr.PerFrameCB());
    cl->SetConstantBuffer(1, *_pbr.PerObjectCB());
    cl->SetTexture(0, *_pbr.DefaultWhiteTexture());

    for (u32 i = 0; i < kQuadCount; ++i) {
        Quad& q = _quads[i];
        // L キーで OFF にすると flat ambient のみになり、間接光の寄与が消える。
        if (_show_lightmap && q.lightmap) {
            _pbr.SetLightmap(q.lightmap.Get(), 1.0f);
        } else {
            _pbr.SetLightmap(nullptr, 0.0f);
        }
        _pbr.SetObject(q.model, q.albedo, /*metallic=*/0.0f,
                       /*roughness=*/0.9f, /*ao=*/1.0f);
        _pbr.BindIblTextures(*cl);
        cl->SetVertexBuffer(*q.mesh.vertex_buffer, q.mesh.vertex_stride);
        cl->SetIndexBuffer(*q.mesh.index_buffer);
        cl->DrawIndexed(q.mesh.index_count);
    }
    cl->EndRenderToTexture(*hdr);

    // ===== Bloom + ACES tonemap → LDR backbuffer =====
    _post.Render(*cl, *sc, buf_idx, _post_params);

    // ===== HUD (LDR backbuffer) =====
    if (_font.AtlasTexture()) {
        const u32 sw = sc->Width();
        const u32 sh = sc->Height();
        _batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Cornell box - path-traced HDR lightmap (%u rays x %u bounces)  FPS: %.1f",
                      kBakeRays, kBounceDepth, static_cast<double>(FPS()));
        _batch.DrawString(_font, buf, 20, 20, FVec4{1, 1, 1, 1});
        std::snprintf(buf, sizeof(buf), "Lightmap: %s   (L で切替)",
                      _show_lightmap ? "ON" : "OFF");
        _batch.DrawString(_font, buf, 20, 44, FVec4{1.0f, 0.95f, 0.7f, 1});
        _batch.DrawString(_font, "WASD: 移動   矢印: 視点   Esc: 終了",
                          20, 68, FVec4{0.7f, 0.85f, 1.0f, 1});
        _batch.End();
    }

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();
    return true;
}

void HelloLightmapApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    for (u32 i = 0; i < kQuadCount; ++i) {
        _quads[i].lightmap.Reset();
        _quads[i].mesh = GpuMesh{};
    }
    _pbr.Shutdown();
    _post.Shutdown();
}

} // namespace hellolightmap
