// 再生中の音声を識別するハンドル
//
// AudioEngine が発行する。世代付きで、停止後に同じスロットが再利用されても
// 古いハンドルは無効化される。
#pragma once

#include "foundation/Types.h"

namespace acs {

struct SoundHandle {
    u32 index      = 0xFFFFFFFFu;  // 内部スロット番号
    u32 generation = 0;             // 世代

    constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }
    constexpr bool operator==(SoundHandle o) const noexcept {
        return index == o.index && generation == o.generation;
    }
};

inline constexpr SoundHandle kInvalidSound = SoundHandle{0xFFFFFFFFu, 0};

} // namespace acs
