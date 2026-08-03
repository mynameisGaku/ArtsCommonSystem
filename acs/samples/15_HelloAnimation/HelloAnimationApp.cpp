// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — HelloAnimationApp 実装。
#include "HelloAnimationApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"

#include <cstdio>

using namespace acs;

namespace helloanim {

void CHelloAnimationApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    m_Sky.PresetSunset();

    ACS_SAMPLE_INIT(m_Shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(m_StdShader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    // 地面用プレーン
    auto plane = Primitive::MakePlane(40.0f, 40.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane, m_GmPlane));

    // スキンメッシュ生成 + GPU アップロード
    m_Snake = CAnimationScene::BuildSnake();
    if (!m_Snake) { Quit(); return; }
    ACS_SAMPLE_INIT(UploadSkinnedMesh(*dev, *m_Snake, m_GmSnake));
    m_Player.SetMesh(m_Snake.Get());
    m_Player.Play(0, /*loop=*/true);

    // 2D HUD
    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    ACS_LOG_INFO("HelloAnimation initialized: %u verts, %u indices, %u bones",
                 static_cast<u32>(m_Snake->Vertices().Num()),
                 static_cast<u32>(m_Snake->Indices().Num()),
                 static_cast<u32>(m_Snake->Bones().Num()));
}

void CHelloAnimationApp::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();
    if (CInput::IsKeyPressed(EKey::Space)) {
        if (m_Player.IsPlaying()) m_Player.Pause(); else m_Player.Resume();
    }

    if (CInput::IsKeyDown(EKey::Left))  m_CamYaw -= dt * 1.0f;
    if (CInput::IsKeyDown(EKey::Right)) m_CamYaw += dt * 1.0f;
    const f32 cam_dist = 8.0f;
    m_CamPos = FVec3{ Sin(m_CamYaw) * cam_dist, 3.0f, -Cos(m_CamYaw) * cam_dist };
    m_Camera.SetLookAt(m_CamPos, FVec3{0, 2, 0});

    m_Player.Update(dt);
}

void CHelloAnimationApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    // ボーンパレットを書き出してから scene に委譲。
    FMat4 palette[CSkinnedShader::kMaxBones];
    const u32 nb = m_Player.WritePalette(palette, CSkinnedShader::kMaxBones);

    // CSky → 地面 → スキンメッシュ描画は scene に委譲。
    m_Scene.Render(m_Sky, m_StdShader, m_Shader, *cl, m_Camera,
                  m_GmPlane, m_GmSnake, palette, nb);

    // 4. HUD
    if (m_Font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        m_Batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "ボーン: %u  時刻: %.2fs  状態: %s",
                      nb, static_cast<double>(m_Player.Time()),
                      m_Player.IsPlaying() ? "再生中" : "停止");
        m_Batch.DrawString(m_Font, buf, 20, 20, FVec4{1,1,1,1});
        m_Batch.DrawString(m_Font, "Space: 再生/停止  ←→: カメラ  Esc: 終了",
                        20, 44, FVec4{0.8f,0.85f,0.95f,1});
        m_Batch.End();
    }
}

void CHelloAnimationApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_GmSnake = FSkinnedGpuMesh{};
    m_GmPlane = FGpuMesh{};
    m_Snake    = TSharedPtr<ASkinnedMeshAsset>();
    m_StdShader.Shutdown();
    m_Shader.Shutdown();
    m_Sky.Shutdown();
}

} // namespace helloanim
