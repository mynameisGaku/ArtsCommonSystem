// SPDX-License-Identifier: Apache-2.0
// HelloModel — CApplication 実装。
#include "HelloModelApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellomodel {

void CHelloModelApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    // === 標準ライティングシェーダ ===
    ACS_SAMPLE_INIT(m_Shader.Init(*dev,
                                 GetRenderer().ColorFormat(),
                                 GetRenderer().DepthFormat()));

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    if (!m_Scene.Init(*dev, aspect)) { Quit(); return; }

    // === オプションで非同期ロードを試みる（ファイルが無くても OK）===
    // 標準ローダ群は CApplication が自動で登録済み。
    m_AsyncMesh = GetAssets().LoadAsync(L"data/optional_mesh.glb");

    ACS_LOG_INFO("HelloModel initialized");
}

void CHelloModelApp::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();
    m_Scene.Update(dt, m_AsyncMesh, m_bAsyncLoaded);
}

void CHelloModelApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !m_Shader.Pipeline()) return;
    m_Scene.Render(m_Shader, *cl);
}

void CHelloModelApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    // 非同期ロード進行中なら待ってから解放
    if (m_AsyncMesh.Valid() && !m_bAsyncLoaded) (void)m_AsyncMesh.Wait();
    m_Scene.Shutdown();
    m_Shader.Shutdown();
}

} // namespace hellomodel
