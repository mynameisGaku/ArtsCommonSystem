// SPDX-License-Identifier: Apache-2.0
// HelloLights — HelloLightsApp 実装。
#include "HelloLightsApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"

#include <cstdio>

using namespace acs;

namespace hellolights {

void CHelloLightsApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    auto cube   = Primitive::MakeCube(1.0f);
    auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *cube,   m_GmCube));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, m_GmSphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  m_GmPlane));

    m_Scene.Build();

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    m_CamPos = FVec3{0, 3.0f, -8.0f};
}

void CHelloLightsApp::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();
    m_Time += dt;

    const f32 mv = 5.0f * dt, tr = 1.5f * dt;
    if (CInput::IsKeyDown(EKey::Left))  m_CamYaw -= tr;
    if (CInput::IsKeyDown(EKey::Right)) m_CamYaw += tr;
    if (CInput::IsKeyDown(EKey::Up))    m_CamPitch -= tr * 0.8f;
    if (CInput::IsKeyDown(EKey::Down))  m_CamPitch += tr * 0.8f;
    // 上下を 0.45π でクランプ: 真上/真下に向くと forward が縮退して
    // LookAt が破綻するため、π/2 のわずか内側で止める。
    const f32 limit = 0.45f * kPi;
    if (m_CamPitch >  limit) m_CamPitch =  limit;
    if (m_CamPitch < -limit) m_CamPitch = -limit;
    FVec3 forward{ Sin(m_CamYaw) * Cos(m_CamPitch),
                 -Sin(m_CamPitch),
                  Cos(m_CamYaw) * Cos(m_CamPitch) };
    FVec3 right{ Cos(m_CamYaw), 0, -Sin(m_CamYaw) };
    if (CInput::IsKeyDown(EKey::W)) m_CamPos += forward * mv;
    if (CInput::IsKeyDown(EKey::S)) m_CamPos -= forward * mv;
    if (CInput::IsKeyDown(EKey::D)) m_CamPos += right   * mv;
    if (CInput::IsKeyDown(EKey::A)) m_CamPos -= right   * mv;
    m_Camera.SetLookAt(m_CamPos, m_CamPos + forward);
}

void CHelloLightsApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    m_Scene.Render(m_Shader, *cl, m_Camera, m_GmPlane, m_GmCube, m_GmSphere, m_Time);

    // HUD: フォントが読めなかった場合 (初回起動時の font asset 欠落など)
    // でも 3D シーンは出すように、ここだけ条件分岐する。
    if (m_Font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        m_Batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "点光源 %u 灯 + 微弱 dir ライト   FPS: %.1f",
                      kPointCount, static_cast<double>(FPS()));
        m_Batch.DrawString(m_Font, buf, 20, 20, FVec4{1,1,1,1});
        m_Batch.DrawString(m_Font, "WASD: 移動  矢印: 視点  Esc: 終了",
                        20, 44, FVec4{0.8f, 0.85f, 0.95f, 1});
        m_Batch.End();
    }
}

void CHelloLightsApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_GmPlane  = FGpuMesh{};
    m_GmSphere = FGpuMesh{};
    m_GmCube   = FGpuMesh{};
    m_Shader.Shutdown();
}

} // namespace hellolights
