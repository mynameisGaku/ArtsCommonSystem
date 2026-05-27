// SPDX-License-Identifier: Apache-2.0
// HelloBloom — HelloBloomApp 実装。HDR シーンを Bloom + ACES Tonemap で出す。
#include "HelloBloomApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace hellobloom {

void HelloBloomApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!sc) { Quit(); return; }

    const u32 sw = sc->Width();
    const u32 sh = sc->Height();

    ACS_SAMPLE_INIT(m_Post.Init(*dev, sw, sh, GetRenderer().ColorFormat()));

    // シーン描画は HDR RT へ流すので shader / sky の RT format も HDR に合わせる。
    ACS_SAMPLE_INIT(m_Shader.Init(*dev, m_Post.HdrFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(m_Sky.Init(*dev, m_Post.HdrFormat(), GetRenderer().DepthFormat()));
    m_Sky.PresetNight();

    auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, m_GmSphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  m_GmPlane));

    // HUD は Tonemap 後の LDR backbuffer に書くので backbuffer format で初期化する。
    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f);

    const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    ACS_LOG_INFO("HelloBloom: backend=%s, HDR=%dx%d",
                 dev->BackendName(), sw, sh);
}

void HelloBloomApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::Num1)) m_Params.bloom_intensity = 0.0f;
    if (Input::IsKeyPressed(EKey::Num2)) m_Params.bloom_intensity = 0.6f;
    if (Input::IsKeyPressed(EKey::Num3)) m_Params.bloom_intensity = 1.5f;

    m_Angle += dt * 0.5f;
    const f32 cam_dist = 7.0f;
    m_CamYaw += (Input::IsKeyDown(EKey::Right) ? 1.0f : 0.0f) * dt;
    m_CamYaw -= (Input::IsKeyDown(EKey::Left)  ? 1.0f : 0.0f) * dt;
    FVec3 cam{ Sin(m_CamYaw) * cam_dist, 2.0f, -Cos(m_CamYaw) * cam_dist };
    m_Camera.SetLookAt(cam, FVec3{0, 1, 0});
}

// true を返して FApplication の既定フレームフローを置き換える (HDR RT を挟むため)。
bool HelloBloomApp::OnCustomFrame() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    IRhiTexture*     hdr = m_Post.HdrRenderTarget();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    // false を返すと FApplication が既定フローでフレームを完走してくれる (失敗時の保険)。
    if (!cl || !sc || !hdr) return false;

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    // 1) HDR RT にシーンを描く
    cl->BeginRenderToTexture(*hdr, ClearColor{0,0,0,1}, depth, 1.0f);

    FViewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                   vp.height = static_cast<f32>(hdr->Height());
    cl->SetViewport(vp);
    FScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                      svr.bottom = static_cast<i32>(hdr->Height());
    cl->SetScissor(svr);

    m_Sky.Render(*cl, m_Camera);

    FDirLight dl;
    dl.direction = m_Sky.SunDirection();
    dl.color     = m_Sky.SunColor();
    FVec3 ambient{0.05f, 0.06f, 0.10f};
    m_Shader.SetLights(m_Camera.ViewProjection(), m_Camera.Eye(), &dl, 1, ambient);

    cl->SetPipeline(*m_Shader.Pipeline());
    cl->SetConstantBuffer(0, *m_Shader.PerFrameCB());
    cl->SetConstantBuffer(1, *m_Shader.PerObjectCB());
    cl->SetTexture(0, *m_Shader.DefaultWhiteTexture());
    cl->SetTexture(1, *m_Shader.ShadowTextureOrDefault());

    // 地面: 暗めの色にして Bloom 対象 (球) のコントラストを稼ぐ。
    m_Shader.SetObject(FMat4::Translation(FVec3{0, 0, 0}),
                      FVec3{0.10f, 0.12f, 0.15f}, 0.05f, 8.0f);
    cl->SetVertexBuffer(*m_GmPlane.vertex_buffer, m_GmPlane.vertex_stride);
    cl->SetIndexBuffer(*m_GmPlane.index_buffer);
    cl->DrawIndexed(m_GmPlane.index_count);

    // HDR > 1.0 の色を 4 個並べる: bloom_threshold (既定 1.0) を超える成分にだけ
    // Bloom が乗る、ということを目視で示すためのデモオブジェクト。
    const FVec3 colors[4] = {
        {6.0f, 1.0f, 0.5f},
        {0.5f, 6.0f, 1.5f},
        {1.0f, 1.5f, 8.0f},
        {5.0f, 5.0f, 1.0f},
    };
    for (u32 i = 0; i < 4; ++i) {
        const f32 a = m_Angle + i * (kPi * 0.5f);
        const FVec3 pos{ Cos(a) * 3.0f, 1.5f, Sin(a) * 3.0f };
        FMat4 m = FMat4::Translation(pos);
        m_Shader.SetObject(m, colors[i], 0.0f, 1.0f);
        cl->SetVertexBuffer(*m_GmSphere.vertex_buffer, m_GmSphere.vertex_stride);
        cl->SetIndexBuffer(*m_GmSphere.index_buffer);
        cl->DrawIndexed(m_GmSphere.index_count);
    }

    cl->EndRenderToTexture(*hdr);

    // 2) Bloom + Tonemap を一気にかけて swapchain に合成。
    m_Post.Render(*cl, *sc, buf_idx, m_Params);

    // 3) HUD は Tonemap 後の LDR backbuffer に書く (HDR 値が HUD 色を吹き飛ばさないため)。
    //    FPostProcess::Render が既に swapchain を RT としてバインドしてくれている。
    if (m_Font.AtlasTexture()) {
        const u32 sw = sc->Width();
        const u32 sh = sc->Height();
        m_Batch.Begin(*cl, sw, sh);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Bloom intensity = %.2f   FPS = %.1f",
                      m_Params.bloom_intensity, FPS());
        m_Batch.DrawString(m_Font, buf, 20, 20, FVec4{1,1,1,1});
        m_Batch.DrawString(m_Font,
                        "1: off  2: 0.6  3: 1.5  ←→: camera  Esc: 終了",
                        20, 44, FVec4{0.8f,0.85f,0.95f,1});
        m_Batch.End();
    }

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();
    return true;
}

void HelloBloomApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_GmPlane  = GpuMesh{};
    m_GmSphere = GpuMesh{};
    m_Shader.Shutdown();
    m_Sky.Shutdown();
    m_Post.Shutdown();
}

} // namespace hellobloom
