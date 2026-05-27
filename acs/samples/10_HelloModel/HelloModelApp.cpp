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
    ACS_SAMPLE_INIT(_shader.Init(*dev,
                                 GetRenderer().ColorFormat(),
                                 GetRenderer().DepthFormat()));

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    if (!_scene.Init(*dev, aspect)) { Quit(); return; }

    // === オプションで非同期ロードを試みる（ファイルが無くても OK）===
    // 標準ローダ群は FApplication が自動で登録済み。
    _async_mesh = GetAssets().LoadAsync(L"data/optional_mesh.glb");

    ACS_LOG_INFO("HelloModel initialized");
}

void HelloModelApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    _scene.Update(dt, _async_mesh, _async_loaded);
}

void HelloModelApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !_shader.Pipeline()) return;
    _scene.Render(_shader, *cl);
}

void HelloModelApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    // 非同期ロード進行中なら待ってから解放
    if (_async_mesh.Valid() && !_async_loaded) (void)_async_mesh.Wait();
    _scene.Shutdown();
    _shader.Shutdown();
}

} // namespace hellomodel
