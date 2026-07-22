// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — FPlayerProfile (AppState 例)。
//
// シーン跨ぎで保持したい状態は FGame::EmplaceAppState<T>() で 1 個だけ登録し、
// 各 Scene から GetGame().AppState<T>() で参照する。サンプルでは「ハイスコア」
// と「セッション数」のみ。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

struct FPlayerProfile {
    acs::u32 hi_score = 0;
    acs::u32 sessions = 0;
};

} // namespace hellogf
