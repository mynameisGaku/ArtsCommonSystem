// SPDX-License-Identifier: Apache-2.0
// HelloWindow — HelloWindowApp 実装。
//
// 動作:
//   ・ウィンドウを開く
//   ・背景を毎フレーム指定色でクリア
//   ・WASD で背景色を変える
//   ・Esc で終了
//   ・タイトルバーに FPS を表示
#include "HelloWindowApp.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <cwchar>

namespace hellowin {

using namespace acs;

void HelloWindowApp::OnStart() noexcept {
    ACS_LOG_INFO("HelloWindow started");
}

void HelloWindowApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();

    if (Input::IsKeyDown(EKey::W)) _r += dt;
    if (Input::IsKeyDown(EKey::S)) _r -= dt;
    if (Input::IsKeyDown(EKey::A)) _g += dt;
    if (Input::IsKeyDown(EKey::D)) _g -= dt;
    // 0..1 にクランプ (SetClearColor は範囲外の値を受け取ると見た目が破綻する)
    if (_r < 0) _r = 0; if (_r > 1) _r = 1;
    if (_g < 0) _g = 0; if (_g > 1) _g = 1;

    SetClearColor(_r, _g, _b);

    // 毎フレーム SetTitle するとちらつくので 30 フレーム間引き
    if (FrameCount() % 30 == 0) {
        wchar_t title[128];
        ::swprintf_s(title, L"HelloWindow  |  FPS: %.1f  |  RGB: (%.2f, %.2f, %.2f)",
                     FPS(), _r, _g, _b);
        GetWindow().SetTitle(title);
    }
}

void HelloWindowApp::OnRender() noexcept {
    // 描画コマンドを追加するならここ。HelloWindow では背景クリアのみ。
}

void HelloWindowApp::OnShutdown() noexcept {
    ACS_LOG_INFO("HelloWindow shutting down");
}

void HelloWindowApp::OnEvent(const Event& e) noexcept {
    if (e.type == EventType::WindowResize) {
        ACS_LOG_INFO("FWindow resized to %ux%u", e.resize.width, e.resize.height);
    }
}

} // namespace hellowin
