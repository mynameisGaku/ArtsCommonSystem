// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 一次元入力へデッドゾーン、倍率、反転を適用する値。 */
struct FInputAxisOptions {
    /** 無入力として扱う絶対値の上限。0以上1未満を受け付ける。 */
    f32 dead_zone = 0.0f;

    /** デッドゾーン補正後の絶対値へ乗算する非負倍率。 */
    f32 scale = 1.0f;

    /** 入力方向を反転する場合はtrue。 */
    bool inverted = false;

    /**
     * 一次元入力を検査して設定を適用する。
     *
     * @param value 適用前の入力値。
     * @return 有効な入力の補正値。不正な設定または非有限入力なら0。
     */
    f32 Apply(f32 value) const noexcept;
};

} // namespace acs::game
