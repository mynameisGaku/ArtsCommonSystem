// SPDX-License-Identifier: Apache-2.0
// HelloFontEditor — CGame 派生のアプリケーションクラス。
// ImGui lifecycle を CGame に持たせる薄いラッパ。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace hellofont {

class CFontEditorApp : public acs::game::CGame {
public:
    void OnStart()    noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::AScene> InitialScene() noexcept override;

private:
    acs::FImGuiCtx m_Imgui;
};

} // namespace hellofont
