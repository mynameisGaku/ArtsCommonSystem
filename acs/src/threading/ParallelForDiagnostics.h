// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** ParallelFor の一時 context 格納経路と同時使用量を観測する診断値。 */
struct FParallelForDiagnostics {
    /** 固定長 stack 領域だけで完了した呼び出し数。 */
    u64 inline_calls = 0u;

    /** 固定 free-list から取得した context block 数。 */
    u64 pool_blocks = 0u;

    /** 現在貸し出している context block 数。 */
    u64 pool_blocks_in_use = 0u;

    /** 同時に貸し出した context block 数の最大値。 */
    u64 pool_blocks_high_water = 0u;

    /** 固定 free-list 枯渇後に OS heap へ退避した context block 数。 */
    u64 heap_blocks = 0u;
};

} // namespace acs
