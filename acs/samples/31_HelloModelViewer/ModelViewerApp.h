// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — CGame 派生のアプリケーションクラス。
// ImGui lifecycle を CGame に持たせる薄いラッパ (sample 29/30 と同形)。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace hellomv {

class CModelViewerApp : public acs::game::CGame {
public:
    void OnStart() noexcept override;
    void OnRender() noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::AScene> InitialScene() noexcept override;

private:
    acs::FImGuiCtx m_Imgui;
};

} // namespace hellomv
