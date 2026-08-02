// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — CHelloGfApp 実装。
#include "HelloGfApp.h"
#include "PlayerProfile.h"
#include "TitleScene.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

void CHelloGfApp::OnStart() noexcept {
    // 全 scene から共有される AppState を 1 個だけ CGame に持たせる。
    EmplaceAppState<FPlayerProfile>();
    // 固定 step は既定値と同じだが、API 露出を見せるため明示しておく。
    SetFixedTimestep(1.0f / 60.0f, /*max_steps_per_frame=*/8);
    // 基底の OnStart は InitialScene() を push する。
    CGame::OnStart();
}

TUniquePtr<AScene> CHelloGfApp::InitialScene() noexcept {
    return MakeUnique<ATitleScene>();
}

} // namespace hellogf
