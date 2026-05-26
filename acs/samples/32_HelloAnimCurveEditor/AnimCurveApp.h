// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — Game 派生のアプリケーションクラス。
// ImGui の lifecycle (Init/NewFrame/Render/Shutdown/OnEvent) を Game の各 hook に
// 配線する薄いラッパ。Scene 側は ImGui::* を呼ぶだけでよい。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace helloac {

class AnimCurveApp : public acs::game::Game {
public:
    void OnStart() noexcept override;
    void OnRender() noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::Event& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::Scene> InitialScene() noexcept override;

private:
    acs::ImGuiCtx _imgui;
};

} // namespace helloac
