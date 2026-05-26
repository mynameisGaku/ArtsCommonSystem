// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — Game 派生のアプリケーションクラス。
// ImGui lifecycle を Game に持たせる薄いラッパ (sample 29/30/31 と同形)。
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
    acs::UniquePtr<acs::game::Scene> InitialScene() noexcept override;

private:
    acs::ImGuiCtx _imgui;
};

} // namespace helloac
