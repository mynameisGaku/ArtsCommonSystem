// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — FGame 派生のアプリケーションクラス。
// ImGui の lifecycle (Init/NewFrame/Render/Shutdown/OnEvent) を FGame の各 hook に
// 配線する薄いラッパ。Scene 側は ImGui::* を呼ぶだけでよい。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace helloac {

class FAnimCurveApp : public acs::game::FGame {
public:
    void OnStart() noexcept override;
    void OnRender() noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::FScene> InitialScene() noexcept override;

private:
    acs::FImGuiCtx m_Imgui;
};

} // namespace helloac
