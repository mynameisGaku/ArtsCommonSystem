// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — HelloGfApp 実装。
#include "HelloGfApp.h"
#include "GameTypes.h"
#include "TitleScene.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

void HelloGfApp::OnStart() noexcept {
    // Phase 2: AppState を構築 (Title/Gameplay/Pause 全てから見える)。
    EmplaceAppState<PlayerProfile>();
    // Phase 2: 固定 step を明示 (既定値と同じだが API デモ目的で明示)。
    SetFixedTimestep(1.0f / 60.0f, /*max_steps_per_frame=*/8);
    // 基底の OnStart は InitialScene() を push する
    Game::OnStart();
}

UniquePtr<Scene> HelloGfApp::InitialScene() noexcept {
    return MakeUnique<TitleScene>();
}

} // namespace hellogf
