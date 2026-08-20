// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 一つの名前付きアクションを一入力時点から評価した結果。 */
struct FInputActionState {
    /** 入力時点で押下開始が含まれるか。 */
    bool pressed = false;

    /** 入力時点で操作が保持されているか。 */
    bool held = false;

    /** 入力時点で解放が含まれるか。 */
    bool released = false;

    /** 軸bindingを合成して[-1, 1]へ制限した値。 */
    f32 axis = 0.0f;
};

} // namespace acs::game
