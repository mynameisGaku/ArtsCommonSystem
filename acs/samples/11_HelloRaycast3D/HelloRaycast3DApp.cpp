// SPDX-License-Identifier: Apache-2.0
#include "HelloRaycast3DApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloraycast3d {

void HelloRaycast3DApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    // aspect は swapchain サイズから算出 → Perspective 行列に反映
    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    if (!m_Scene.Init(*dev, aspect)) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    // フォントは見つからなくても致命的ではない (HUD だけ落ちる) → 結果を捨てる
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f);

    ACS_LOG_INFO("HelloRaycast3D initialized");
}

void HelloRaycast3DApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    m_Scene.Update(dt);
}

void HelloRaycast3DApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !m_Shader.Pipeline()) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();
    m_Scene.Render(m_Shader, *cl, m_Batch, m_Font, sw, sh);
}

void HelloRaycast3DApp::OnShutdown() noexcept {
    // GPU が描画中にリソースを破棄するとクラッシュするので WaitIdle が先
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_Scene.Shutdown();
    m_Shader.Shutdown();
}

} // namespace helloraycast3d
