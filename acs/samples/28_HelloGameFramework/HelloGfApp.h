// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — FGame 派生のアプリケーションクラス。
//
// AppState (PlayerProfile) の構築と固定 step の明示を行い、InitialScene() で
// TitleScene を返す。FGame の最小限カスタマイズ例。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class HelloGfApp : public acs::game::FGame {
public:
    void OnStart() noexcept override;

protected:
    acs::TUniquePtr<acs::game::Scene> InitialScene() noexcept override;
};

} // namespace hellogf
