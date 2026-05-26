// SPDX-License-Identifier: Apache-2.0
// 再生中の音声を識別するハンドル
//
// FAudioEngine が発行する。世代付きで、停止後に同じスロットが再利用されても
// 古いハンドルは無効化される。
#pragma once

#include "foundation/Types.h"

namespace acs {

struct FSoundHandle {
    u32 index      = 0xFFFFFFFFu;  // 内部スロット番号
    u32 generation = 0;             // 世代

    constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }
    constexpr bool operator==(FSoundHandle o) const noexcept {
        return index == o.index && generation == o.generation;
    }
};

inline constexpr FSoundHandle kInvalidSound = FSoundHandle{0xFFFFFFFFu, 0};

} // namespace acs
