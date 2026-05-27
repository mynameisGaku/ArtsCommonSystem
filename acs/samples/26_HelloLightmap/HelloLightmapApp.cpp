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

    // HDR FPostProcess (Bloom + ACES tonemap)。シーンは HDR RT に描く。
    ACS_SAMPLE_INIT(m_Post.Init(*dev, sc->Width(), sc->Height(),
                               GetRenderer().ColorFormat()));
    // FPbrShader は HDR RT フォーマットに合わせて init する。
    ACS_SAMPLE_INIT(m_Pbr.Init(*dev, m_Post.HdrFormat(),
                              GetRenderer().DepthFormat()));

    BuildCornellBox(*dev, m_Quads);
    BakeLightmaps(*dev, m_Quads);

    // FSpriteBatch は tonemap 後の LDR backbuffer に描く。
    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(sc->Width()) /
                       static_cast<f32>(sc->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.05f, 100.0f);
    m_CamPos = FVec3{0.0f, 1.0f, -0.9f};

    // FPostProcess パラメータ (絵作りで調整可)。
    m_PostParams.bloom_threshold    = 2.5f;   // 天井 (光源) だけが bloom する閾値
    m_PostParams.bloom_intensity    = 0.5f;
    m_PostParams.grain_intensity    = 0.0f;   // GI デモなので film grain は切る
    m_PostParams.vignette_intensity = 0.15f;
    m_PostParams.ssr_intensity      = 0.0f;   // SSR 未使用 (fallback mip の誤加算防止)
}

void HelloLightmapApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::L)) m_bShowLightmap = !m_bShowLightmap;

    const f32 mv = 2.0f * dt, tr = 1.4f * dt;
    if (Input::IsKeyDown(EKey::Left))  m_CamYaw -= tr;
    if (Input::IsKeyDown(EKey::Right)) m_CamYaw += tr;
    if (Input::IsKeyDown(EKey::Up))    m_CamPitch -= tr * 0.8f;
    if (Input::IsKeyDown(EKey::Down))  m_CamPitch += tr * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (m_CamPitch >  limit) m_CamPitch =  limit;
    if (m_CamPitch < -limit) m_CamPitch = -limit;
    FVec3 forward{ Sin(m_CamYaw) * Cos(m_CamPitch),
                 -Sin(m_CamPitch),
                  Cos(m_CamYaw) * Cos(m_CamPitch) };
    FVec3 right{ Cos(m_CamYaw), 0, -Sin(m_CamYaw) };
    if (Input::IsKeyDown(EKey::W)) m_CamPos += forward * mv;
    if (Input::IsKeyDown(EKey::S)) m_CamPos -= forward * mv;
    if (Input::IsKeyDown(EKey::D)) m_CamPos += right   * mv;
    if (Input::IsKeyDown(EKey::A)) m_CamPos -= right   * mv;
    m_Camera.SetLookAt(m_CamPos, m_CamPos + forward);
}

// OnCustomFrame: HDR RT にシーンを描き、FPostProcess (Bloom + ACES tonemap)
// で LDR backbuffer へ。HDR lightmap の高輝度が tonemap で自然にロールオフする。
bool HelloLightmapApp::OnCustomFrame() noexcept {
    IRhiDevice*      dev   = GetRenderer().Device();
    IRhiCommandList* cl    = GetRenderer().CommandList();
    IRhiSwapchain*   sc    = GetRenderer().Swapchain();
    IRhiTexture*     hdr   = m_Post.HdrRenderTarget();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    if (!dev || !cl || !sc || !hdr) return false;

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    // ===== HDR RT に Cornell box を描画 =====
    cl->BeginRenderToTexture(*hdr, ClearColor{0, 0, 0, 1}, depth, 1.0f);
    FViewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                   vp.height = static_cast<f32>(hdr->Height());
    cl->SetViewport(vp);
    FScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                       svr.bottom = static_cast<i32>(hdr->Height());
    cl->SetScissor(svr);

    // 動的ライトは使わず ごく弱い ambient のみ。間接光は lightmap が担う。
    m_Pbr.SetLights(m_Camera.ViewProjection(), m_Camera.Eye(),
                   nullptr, 0, FVec3{0.02f, 0.02f, 0.02f});
    m_Pbr.SetPointLights(nullptr, 0);

    cl->SetPipeline(*m_Pbr.Pipeline());
    cl->SetConstantBuffer(0, *m_Pbr.PerFrameCB());
    cl->SetConstantBuffer(1, *m_Pbr.PerObjectCB());
    cl->SetTexture(0, *m_Pbr.DefaultWhiteTexture());

    for (u32 i = 0; i < kQuadCount; ++i) {
        Quad& q = m_Quads[i];
        // L キーで OFF にすると flat ambient のみになり、間接光の寄与が消える。
        if (m_bShowLightmap && q.lightmap) {
            m_Pbr.SetLightmap(q.lightmap.Get(), 1.0f);
        } else {
            m_Pbr.SetLightmap(nullptr, 0.0f);
        }
        m_Pbr.SetObject(q.model, q.albedo, /*metallic=*/0.0f,
                       /*roughness=*/0.9f, /*ao=*/1.0f);
        m_Pbr.BindIblTextures(*cl);
        cl->SetVertexBuffer(*q.mesh.vertex_buffer, q.mesh.vertex_stride);
        cl->SetIndexBuffer(*q.mesh.index_buffer);
        cl->DrawIndexed(q.mesh.index_count);
    }
    cl->EndRenderToTexture(*hdr);

    // ===== Bloom + ACES tonemap → LDR backbuffer =====
    m_Post.Render(*cl, *sc, buf_idx, m_PostParams);

    // ===== HUD (LDR backbuffer) =====
    if (m_Font.AtlasTexture()) {
        const u32 sw = sc->Width();
        const u32 sh = sc->Height();
        m_Batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Cornell box - path-traced HDR lightmap (%u rays x %u bounces)  FPS: %.1f",
                      kBakeRays, kBounceDepth, static_cast<double>(FPS()));
        m_Batch.DrawString(m_Font, buf, 20, 20, FVec4{1, 1, 1, 1});
        std::snprintf(buf, sizeof(buf), "Lightmap: %s   (L で切替)",
                      m_bShowLightmap ? "ON" : "OFF");
        m_Batch.DrawString(m_Font, buf, 20, 44, FVec4{1.0f, 0.95f, 0.7f, 1});
        m_Batch.DrawString(m_Font, "WASD: 移動   矢印: 視点   Esc: 終了",
                          20, 68, FVec4{0.7f, 0.85f, 1.0f, 1});
        m_Batch.End();
    }

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();
    return true;
}

void HelloLightmapApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    for (u32 i = 0; i < kQuadCount; ++i) {
        m_Quads[i].lightmap.Reset();
        m_Quads[i].mesh = GpuMesh{};
    }
    m_Pbr.Shutdown();
    m_Post.Shutdown();
}

} // namespace hellolightmap
