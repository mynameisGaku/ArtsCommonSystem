// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** タイマ走査とキャンセル経路の決定的な診断値。 */
struct FTimerDiagnostics {
    /** 直近 Tick で実際に参照した active slot 数。 */
    u64 active_slots_visited = 0;

    /** 直近 Tick で読み出した 64-bit active word 数。 */
    u64 active_words_loaded = 0;

    /** Cancel が handle 特定のために参照した slot 数の累計。 */
    u64 cancel_slot_probes = 0;
};

} // namespace acs
