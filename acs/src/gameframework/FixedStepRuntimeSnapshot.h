// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/FixedStepInputBufferSnapshot.h"
#include "timing/FixedStepClockSnapshot.h"

namespace acs::game {

/** 固定更新時計と未消費入力を同じ復元境界で保持するprocess内保存値。 */
struct FFixedStepRuntimeSnapshot {
    /** 取得元CGameを識別するprocess内token。永続化や別CGameへの移送には使わない。 */
    u64 runtime_owner_token = 0u;

    /** 取得時のactive scene境界。scene遷移後の復元を拒否するために使う。 */
    u64 active_scene_epoch = 0u;

    /** 取得時のframe/tick入力source結線。source切替後の復元を拒否するために使う。 */
    u64 input_source_epoch = 0u;

    /** 固定更新時計の設定と進行状態。 */
    timing::FFixedStepClockSnapshot clock{};

    /** active sceneが保持する未消費の固定入力。 */
    FFixedStepInputBufferSnapshot input{};

    /** 固定タイムステップ更新が有効だったか。 */
    bool fixed_step_enabled = true;
};

} // namespace acs::game
