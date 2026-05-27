// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAtlasEditor — FGame 派生のアプリケーションクラス。
// ImGui lifecycle を FGame に持たせる薄いラッパ。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace hellosa {

class SpriteAtlasApp : public acs::game::FGame {
public:
    void OnStart()    noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::Event& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::Scene> InitialScene() noexcept override;

private:
    acs::ImGuiCtx m_Imgui;
};

} // namespace hellosa
