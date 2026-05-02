// ACS Threading — Lightweight thread id helper.
#pragma once

#include "foundation/Types.h"

namespace acs {

struct ThreadId {
    u32 raw = 0;
    constexpr bool operator==(ThreadId o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(ThreadId o) const noexcept { return raw != o.raw; }
};

ThreadId CurrentThreadId() noexcept;

} // namespace acs
