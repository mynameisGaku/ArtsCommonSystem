// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Game 派生のアプリケーションクラス。
//
// Phase 2: AppState (PlayerProfile) を Game に持たせ、SetFixedTimestep で
// 固定 step を明示。`InitialScene()` で TitleScene を返す。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class HelloGfApp : public acs::game::Game {
public:
    void OnStart() noexcept override;

protected:
    acs::UniquePtr<acs::game::Scene> InitialScene() noexcept override;
};

} // namespace hellogf
