// SPDX-License-Identifier: Apache-2.0
// HelloShadows — HelloShadowsApp 実装。
#include "HelloShadowsApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"

#include <cstdio>

using namespace acs;

namespace helloshadows {

void CHelloShadowsApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    m_Sky.PresetDay();

    ACS_SAMPLE_INIT(m_Shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(m_Shadow.Init(*dev, /*size=*/kShadowMapSize));

    auto cube   = Primitive::MakeCube(1.0f);
    auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
    auto plane  = Primitive::MakePlane(40.0f, 40.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *cube,   m_GmCube));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, m_GmSphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  m_GmPlane));

    m_Scene.Build();

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    m_CamPos = FVec3{0, 4, -10.0f};

    // シャドウマップを主パスのシェーダに渡しておく (主パスでサンプリング)。
    m_Shader.SetShadowMap(m_Shadow.DepthTexture(), m_Shadow.LightViewProjection(), 0.001f);

    ACS_LOG_INFO("HelloShadows initialized");
}

void CHelloShadowsApp::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();
    m_Time += dt;

    if (CInput::IsKeyDown(EKey::Space)) m_SunYaw += dt * 0.6f;

    // カメラ操作 (yaw/pitch + WASD で平行移動、pitch は ±81 度に clamp)
    const f32 mv = 5.0f * dt, tr = 1.5f * dt;
    if (CInput::IsKeyDown(EKey::Left))  m_CamYaw -= tr;
    if (CInput::IsKeyDown(EKey::Right)) m_CamYaw += tr;
    if (CInput::IsKeyDown(EKey::Up))    m_CamPitch -= tr * 0.8f;
    if (CInput::IsKeyDown(EKey::Down))  m_CamPitch += tr * 0.8f;
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

void CHelloShadowsApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    // 太陽方向 (方向 TO 光源、Y 上向き)
    const FVec3 sun_dir{
        Sin(m_SunYaw) * 0.6f,
        0.85f,
        Cos(m_SunYaw) * 0.6f,
    };

    // シャドウパス + 主パス (CSky → 地面 → キャスタ) は scene に委譲。
    m_Scene.Render(m_Sky, m_Shader, m_Shadow, *cl, m_Camera,
                  m_GmPlane, m_GmCube, m_GmSphere, sun_dir);

    // === HUD ===
    if (m_Font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        m_Batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "シャドウマップ %ux%u  Vogel PCSS 16+24  FPS: %.1f",
                      m_Shadow.Size(), m_Shadow.Size(), static_cast<double>(FPS()));
        m_Batch.DrawString(m_Font, buf, 20, 20, FVec4{1, 1, 1, 1});
        m_Batch.DrawString(m_Font, "WASD: 移動  矢印: 視点  Space: 太陽回転  Esc: 終了",
                        20, 44, FVec4{0.8f, 0.85f, 0.95f, 1});
        m_Batch.End();
    }
}

void CHelloShadowsApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_GmPlane  = FGpuMesh{};
    m_GmSphere = FGpuMesh{};
    m_GmCube   = FGpuMesh{};
    m_Shadow.Shutdown();
    m_Shader.Shutdown();
    m_Sky.Shutdown();
}

} // namespace helloshadows
