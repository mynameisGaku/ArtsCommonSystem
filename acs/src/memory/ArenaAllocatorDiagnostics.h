// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** Arena の予約回数、batch 化、世代 reset を観測する診断値。 */
struct FArenaAllocatorDiagnostics {
    /** 現在保持している page 数。 */
    u64 retained_pages = 0u;

    /** 最後の Reset 以降に成功した batch 確保回数。 */
    u64 batch_allocations = 0u;

    /** 最後の Reset 以降に batch で返した領域数。 */
    u64 batch_suballocations = 0u;

    /** 直前の Reset が直接参照した page 数。 */
    u64 last_reset_page_visits = 0u;

    /** 最後の Reset 以降に初回利用時まで初期化を遅らせた page 数。 */
    u64 lazy_page_resets = 0u;
};

} // namespace acs
