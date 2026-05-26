// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — FGame 派生のアプリケーションクラス。
// ImGui lifecycle を FGame に持たせる薄いラッパ (sample 29/30/31 と同形)。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace hellobt {

class BtEditorApp : public acs::game::FGame {
public:
    void OnStart() noexcept override;
    void OnRender() noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::FScene> InitialScene() noexcept override;

private:
    acs::FImGuiCtx _imgui;
};

} // namespace hellobt
