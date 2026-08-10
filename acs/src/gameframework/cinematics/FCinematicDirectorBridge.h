// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/cinematics/ACinematicAsset.h"
#include "container/Array.h"
#include "foundation/Result.h"
#include "gameframework/CinematicsDirector.h"

namespace acs::game {

class CCinematicPlayer;

/** 検証済みアセットをDirector用keyframeへ変換する値操作です。 */
class FCinematicDirectorBridge final {
private:
    friend class CCinematicPlayer;

    /** assetの値と確保を検証してkeyframe列を返し、失敗時は部分列を返しません。 */
    static TResult<TArray<FTimelineKeyframe>> BuildKeyframes(const asset::ACinematicAsset& asset) noexcept;
};

} // namespace acs::game
