// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** Render Graph transient alias 候補計画の集計値。 */
struct FRenderGraphAliasPlanSummary {
    /** 解析した論理リソース数。 */
    u32 resource_count = 0;

    /** 候補計画で必要になる物理 slot 数。 */
    u32 slot_count = 0;

    /** alias を行わない場合の合計バイト数。 */
    u64 logical_bytes = 0;

    /** 各候補 slot の最大リソースを合計したバイト数。 */
    u64 candidate_heap_bytes = 0;

    /** placed resource 実装後に削減可能な候補バイト数を返す。 */
    constexpr u64 PotentialSavedBytes() const noexcept {
        return logical_bytes >= candidate_heap_bytes ? logical_bytes - candidate_heap_bytes : 0;
    }
};

} // namespace acs
