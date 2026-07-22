// SPDX-License-Identifier: Apache-2.0
// HelloPbr — HelloPbrApp 実装。
#include "HelloPbrApp.h"

#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "math/Mat.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace hellopbr {

void FHelloPbrApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    auto sphere = Primitive::MakeSphere(0.55f, 48, 24);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, m_GmSphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  m_GmPlane));

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    m_CamPos = FVec3{0, 2.0f, -7.5f};
}

void FHelloPbrApp::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();
    m_Time += dt;

    const f32 mv = 5.0f * dt, tr = 1.5f * dt;
    if (FInput::IsKeyDown(EKey::Left))  m_CamYaw -= tr;
    if (FInput::IsKeyDown(EKey::Right)) m_CamYaw += tr;
    if (FInput::IsKeyDown(EKey::Up))    m_CamPitch -= tr * 0.8f;
    if (FInput::IsKeyDown(EKey::Down))  m_CamPitch += tr * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (m_CamPitch >  limit) m_CamPitch =  limit;
    if (m_CamPitch < -limit) m_CamPitch = -limit;
    FVec3 forward{ Sin(m_CamYaw) * Cos(m_CamPitch),
                 -Sin(m_CamPitch),
                  Cos(m_CamYaw) * Cos(m_CamPitch) };
    FVec3 right{ Cos(m_CamYaw), 0, -Sin(m_CamYaw) };
    if (FInput::IsKeyDown(EKey::W)) m_CamPos += forward * mv;
    if (FInput::IsKeyDown(EKey::S)) m_CamPos -= forward * mv;
    if (FInput::IsKeyDown(EKey::D)) m_CamPos += right   * mv;
    if (FInput::IsKeyDown(EKey::A)) m_CamPos -= right   * mv;
    m_Camera.SetLookAt(m_CamPos, m_CamPos + forward);
}

void FHelloPbrApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    FDirLight lights[1];
    // direction = surface から light source へ向くベクトル (y+ = 上空の太陽)。
    // HelloLights / HelloRaycast3D の慣習に合わせて y を正にする。
    lights[0].direction = FVec3{0.4f, 0.8f, -0.5f};
    // LDR backbuffer (B8G8R8A8_UNorm) なので強度 1.x 程度に抑えて clip 回避。
    lights[0].color     = FVec3{1.4f, 1.33f, 1.26f};

    // 旋回する点光源で highlight をハッキリ見せる
    FPointLight pts[1];
    pts[0].position = FVec3{Sin(m_Time * 0.6f) * 4.0f, 1.5f,
                          Cos(m_Time * 0.6f) * 4.0f - 2.0f};
    pts[0].color    = FVec3{0.7f, 0.6f, 1.0f};
    pts[0].range    = 8.0f;

    FVec3 ambient{0.05f, 0.06f, 0.08f};
    m_Shader.SetLights(m_Camera.ViewProjection(), m_Camera.Eye(),
                     lights, 1, ambient);
    m_Shader.SetPointLights(pts, 1);

    cl->SetPipeline(*m_Shader.Pipeline());
    cl->SetConstantBuffer(0, *m_Shader.PerFrameCB());
    cl->SetConstantBuffer(1, *m_Shader.PerObjectCB());
    cl->SetTexture(0, *m_Shader.DefaultWhiteTexture());
    // PBR pipeline は IBL 用 texture slot 1-3 も持つので fallback を bind
    // しておく (HelloPbr では IBL を使わないので shader 側で uniform-branch
    // して flat ambient に落ちる)
    m_Shader.BindIblTextures(*cl);

    // 地面 (PBR で metallic=0, roughness=0.8 の灰色)
    m_Shader.SetObject(FMat4::Translation(FVec3{0, -0.6f, 0}),
                      FVec3{0.5f, 0.5f, 0.5f}, 0.0f, 0.8f, 1.0f);
    cl->SetConstantBuffer(1, *m_Shader.PerObjectCB());
    cl->SetVertexBuffer(*m_GmPlane.vertex_buffer, m_GmPlane.vertex_stride);
    cl->SetIndexBuffer(*m_GmPlane.index_buffer);
    cl->DrawIndexed(m_GmPlane.index_count);

    // material grid: X = metallic 0..1, Y = roughness 0..1
    cl->SetVertexBuffer(*m_GmSphere.vertex_buffer, m_GmSphere.vertex_stride);
    cl->SetIndexBuffer(*m_GmSphere.index_buffer);
    const FVec3 base{0.85f, 0.4f, 0.3f};      // 銅っぽい色
    for (u32 y = 0; y < kGridSize; ++y) {
        for (u32 x = 0; x < kGridSize; ++x) {
            const f32 metallic  = static_cast<f32>(x) / (kGridSize - 1);
            const f32 roughness = 0.05f + static_cast<f32>(y) / (kGridSize - 1) * 0.95f;
            const f32 px = (static_cast<f32>(x) - (kGridSize - 1) * 0.5f) * kSpacing;
            // +3.0 でグリッド全体を床より上に。床 y=-0.6 + sphere radius 0.55 を考慮し
            // row 0 (y=0) の sphere bottom (py - 0.55) が床より上になるように +3.0 を選択。
            const f32 py = (static_cast<f32>(y) - (kGridSize - 1) * 0.5f) * kSpacing + 3.0f;
            m_Shader.SetObject(FMat4::Translation(FVec3{px, py, 2.0f}),
                              base, metallic, roughness, 1.0f);
            cl->SetConstantBuffer(1, *m_Shader.PerObjectCB());
            cl->DrawIndexed(m_GmSphere.index_count);
        }
    }

    if (m_Font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        m_Batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "PBR: %ux%u sphere (X→metallic, Y→roughness)  FPS: %.1f",
                      kGridSize, kGridSize, static_cast<double>(FPS()));
        m_Batch.DrawString(m_Font, buf, 20, 20, FVec4{1, 1, 1, 1});
        m_Batch.DrawString(m_Font,
                          "WASD: 移動  矢印: 視点  Esc: 終了",
                          20, 44, FVec4{0.7f, 0.85f, 1.0f, 1});
        m_Batch.End();
    }
}

void FHelloPbrApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_GmPlane  = FGpuMesh{};
    m_GmSphere = FGpuMesh{};
    m_Shader.Shutdown();
}

} // namespace hellopbr
