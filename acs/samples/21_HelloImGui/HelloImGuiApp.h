// SPDX-License-Identifier: Apache-2.0
// HelloImGui — FApplication 派生クラス。
// ImGui を初期化して FPS / 背景色スライダー / ImGui demo window を表示する最小サンプル。
// Imgui モジュールが ACS をどう wrap しているかの reference 実装。
#pragma once

#include "app/Application.h"
#include "imgui/ImGuiContext.h"

namespace helloimgui {

class FHelloImGuiApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

private:
    acs::FImGuiCtx m_Imgui;
    bool          m_bShowDemo = true;
    acs::f32      m_R = 0.1f;
    acs::f32      m_G = 0.12f;
    acs::f32      m_B = 0.16f;
};

} // namespace helloimgui
