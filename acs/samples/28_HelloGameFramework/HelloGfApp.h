// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — CGame 派生のアプリケーションクラス。
//
// AppState (FPlayerProfile) の構築と固定 step の明示を行い、InitialScene() で
// ATitleScene を返す。CGame の最小限カスタマイズ例。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class CHelloGfApp : public acs::game::CGame {
public:
    void OnStart() noexcept override;

protected:
    acs::TUniquePtr<acs::game::AScene> InitialScene() noexcept override;
};

} // namespace hellogf
