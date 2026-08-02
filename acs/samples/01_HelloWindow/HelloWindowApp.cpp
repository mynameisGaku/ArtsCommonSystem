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

void CHelloWindowApp::OnStart() noexcept {
    ACS_LOG_INFO("HelloWindow started");
}

void CHelloWindowApp::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();

    if (CInput::IsKeyDown(EKey::W)) m_R += dt;
    if (CInput::IsKeyDown(EKey::S)) m_R -= dt;
    if (CInput::IsKeyDown(EKey::A)) m_G += dt;
    if (CInput::IsKeyDown(EKey::D)) m_G -= dt;
    // 0..1 にクランプ (SetClearColor は範囲外の値を受け取ると見た目が破綻する)
    if (m_R < 0) m_R = 0; if (m_R > 1) m_R = 1;
    if (m_G < 0) m_G = 0; if (m_G > 1) m_G = 1;

    SetClearColor(m_R, m_G, m_B);

    // 毎フレーム SetTitle するとちらつくので 30 フレーム間引き
    if (FrameCount() % 30 == 0) {
        wchar_t title[128];
        ::swprintf_s(title, L"HelloWindow  |  FPS: %.1f  |  RGB: (%.2f, %.2f, %.2f)",
                     FPS(), m_R, m_G, m_B);
        GetWindow().SetTitle(title);
    }
}

void CHelloWindowApp::OnRender() noexcept {
    // 描画コマンドを追加するならここ。HelloWindow では背景クリアのみ。
}

void CHelloWindowApp::OnShutdown() noexcept {
    ACS_LOG_INFO("HelloWindow shutting down");
}

void CHelloWindowApp::OnEvent(const FEvent& e) noexcept {
    if (e.type == EEventType::WindowResize) {
        ACS_LOG_INFO("FWindow resized to %ux%u", e.resize.width, e.resize.height);
    }
}

} // namespace hellowin
