// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** Render Graph リソースから alias slot への決定的な割り当て。 */
struct FRenderGraphAliasAssignment {
    /** 入力寿命と同じリソース識別子。 */
    u32 resource_id = 0;

    /** 共有候補となる物理 slot 番号。 */
    u32 slot_index = 0;
};

} // namespace acs
