// SPDX-License-Identifier: Apache-2.0
// HelloSky — HelloSkyApp 実装。
#include "HelloSkyApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"

using namespace acs;

namespace hellosky {

void HelloSkyApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    m_Scene.SetPreset(m_Sky, SkyPreset::Day);

    ACS_SAMPLE_INIT(m_Shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    auto sphere = Primitive::MakeSphere(1.0f, 48, 24);
    auto plane  = Primitive::MakePlane(50.0f, 50.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, m_GmSphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  m_GmPlane));

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
    m_CamPos = FVec3{0, 2.0f, -6.0f};

    ACS_LOG_INFO("HelloSky initialized");
}

void HelloSkyApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::Num1)) m_Scene.SetPreset(m_Sky, SkyPreset::Day);
    if (Input::IsKeyPressed(EKey::Num2)) m_Scene.SetPreset(m_Sky, SkyPreset::Sunset);
    if (Input::IsKeyPressed(EKey::Num3)) m_Scene.SetPreset(m_Sky, SkyPreset::Night);

    m_Angle += dt * 0.5f;

    // ターゲット (原点上 1m) を中心にカメラを yaw 周回させる。
    // Sin/Cos で円軌道を作るのが一番素直で、初学者が読みやすい形。
    if (Input::IsKeyDown(EKey::Left))  m_CamYaw -= dt * 1.0f;
    if (Input::IsKeyDown(EKey::Right)) m_CamYaw += dt * 1.0f;
    const f32 cam_dist = 6.0f;
    m_CamPos = FVec3{ Sin(m_CamYaw) * cam_dist, 2.5f, -Cos(m_CamYaw) * cam_dist };
    m_Camera.SetLookAt(m_CamPos, FVec3{0, 1, 0});
}

void HelloSkyApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    m_Scene.Render(m_Sky, m_Shader, *cl, m_Camera, m_GmPlane, m_GmSphere, m_Angle);

    // HUD は AtlasTexture が用意できているときだけ。フォントロード失敗
    // (アセット不在) でもサンプル自体は動くようにフェイルセーフ。
    if (m_Font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        m_Batch.Begin(*cl, sw, sh);
        const SkyPreset cur = m_Scene.CurrentPreset();
        const char* preset_name = (cur == SkyPreset::Day)    ? "[1] 昼"     :
                                  (cur == SkyPreset::Sunset) ? "[2] 夕焼け" : "[3] 夜";
        m_Batch.DrawString(m_Font, preset_name, 20, 20, FVec4{1,1,1,1});
        m_Batch.DrawString(m_Font, "1/2/3: プリセット切替  ←→: カメラ  Esc: 終了",
                        20, 44, FVec4{0.8f,0.85f,0.95f,1});
        m_Batch.End();
    }
}

void HelloSkyApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_GmPlane  = GpuMesh{};
    m_GmSphere = GpuMesh{};
    m_Shader.Shutdown();
    m_Sky.Shutdown();
}

} // namespace hellosky
