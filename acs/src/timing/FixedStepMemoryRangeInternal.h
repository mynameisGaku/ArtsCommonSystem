// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace acs::timing::detail {

/** 生領域の重複と終端越えを検証する半開区間。 */
struct FFixedStepMemoryRangeInternal {
    /** 領域先頭の整数表現。 */
    std::uintptr_t begin = 0u;

    /** 領域終端の整数表現。 */
    std::uintptr_t end = 0u;
};

} // namespace acs::timing::detail
