// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/InputStateSnapshot.h"

namespace acs::game {

/** 固定入力bufferの未消費入力と初期化状態を所有する保存値。 */
struct FFixedStepInputBufferSnapshot {
    /** 次の固定tickへ渡す保持状態、軸、未消費の押下・解放。 */
    FInputStateSnapshot pending_input{};

    /** pending_inputが受理済み入力を表す場合はtrue。 */
    bool has_input_state = false;
};

} // namespace acs::game
