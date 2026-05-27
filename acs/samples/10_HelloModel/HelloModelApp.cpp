// SPDX-License-Identifier: Apache-2.0
// HelloModel — FApplication 実装。
#include "HelloModelApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellomodel {

void HelloModelApp::OnStart() noexcept {
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
    // 標準ローダ群は FApplication が自動で登録済み。
    m_AsyncMesh = GetAssets().LoadAsync(L"data/optional_mesh.glb");

    ACS_LOG_INFO("HelloModel initialized");
}

void HelloModelApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    m_Scene.Update(dt, m_AsyncMesh, m_AsyncLoaded);
}

void HelloModelApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !m_Shader.Pipeline()) return;
    m_Scene.Render(m_Shader, *cl);
}

void HelloModelApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    // 非同期ロード進行中なら待ってから解放
    if (m_AsyncMesh.Valid() && !m_AsyncLoaded) (void)m_AsyncMesh.Wait();
    m_Scene.Shutdown();
    m_Shader.Shutdown();
}

} // namespace hellomodel
