// SPDX-License-Identifier: Apache-2.0
// 再生中の音声を識別するハンドル
//
// FAudioEngine が発行する。世代付きで、停止後に同じスロットが再利用されても
// 古いハンドルは無効化される。
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * 再生中の音声を識別する世代付きハンドル。
 *
 * @details
 * FAudioEngine が Play で発行する。スロット番号 (index) と世代 (generation) の組で、
 * スロットが停止後に再利用されても世代が進むため、古いハンドルは自動的に無効化される。
 */
struct SoundHandle {
    /** 内部発音スロット番号 (0xFFFFFFFF は無効を表す)。 */
    u32 index      = 0xFFFFFFFFu;

    /** スロットの世代カウンタ (再利用ごとに加算され、古いハンドルを無効化)。 */
    u32 generation = 0;

    /**
     * ハンドルが有効なスロットを指すかを返す。
     *
     * @return index が無効値でなければ true。
     */
    constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }

    /**
     * 2 つのハンドルが同一スロット・同一世代かを比較する。
     *
     * @param o 比較相手のハンドル。
     * @return index と generation がともに一致すれば true。
     */
    constexpr bool operator==(SoundHandle o) const noexcept {
        return index == o.index && generation == o.generation;
    }
};

/** 無効な (どの再生も指さない) SoundHandle の定数値。 */
inline constexpr SoundHandle kInvalidSound = SoundHandle{0xFFFFFFFFu, 0};

} // namespace acs
