// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::editor_frame {

enum class EResult : i32 {
    Fatal = -1,
    Busy = 0,
    Presented = 1,
};

constexpr i32 ToAbi(EResult result) noexcept {
    return static_cast<i32>(result);
}

constexpr EResult Classify(i32 abi_result) noexcept {
    return abi_result < 0
        ? EResult::Fatal
        : (abi_result == 0 ? EResult::Busy : EResult::Presented);
}

/**
 * Profiler publication and frame-time consumption are presentation facts.
 * A GPU-busy attempt or a failed submit/present must not advance either.
 */
constexpr bool ShouldPublishProfiler(i32 abi_result) noexcept {
    return Classify(abi_result) == EResult::Presented;
}

} // namespace acs::editor_frame
