// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * ゲームパッド軸へ適用する値変換設定。
 *
 * dead_zone は [0, 1)、scale は 0 以上の有限値だけを受け付ける。
 */
struct FInputAxisOptions {
    f32 dead_zone = 0.0f;
    f32 scale = 1.0f;
    bool inverted = false;

    /** 全設定値が正規範囲なら true。 */
    bool IsValid() const noexcept;

    /** dead-zone、正規化、反転、倍率の順で軸値を変換する。 */
    f32 Apply(f32 value) const noexcept;
};

} // namespace acs::game
