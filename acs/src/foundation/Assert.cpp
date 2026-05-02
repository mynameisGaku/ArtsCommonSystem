// Assert is header-only; this TU exists so MSBuild has a non-empty .cpp for the
// foundation static library on platforms that need at least one object file
// per archive. Future inline-out assertions can move here.
#include "foundation/Assert.h"

namespace acs::detail {
// Anchor symbol — keeps the TU non-empty.
[[maybe_unused]] inline int kAssertAnchor = 0;
}
