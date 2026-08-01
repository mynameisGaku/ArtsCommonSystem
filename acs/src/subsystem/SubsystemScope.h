// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** サブシステムを所有する寿命スコープ。 */
enum class ESubsystemScope : u8 {
    /** アプリケーション全体の寿命。 */
    Engine = 0,
    /** 1 回のゲーム実行セッションの寿命。 */
    GameInstance = 1,
    /** 1 つのシーンまたはワールドの寿命。 */
    World = 2,
};

} // namespace acs
