// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** ファイル I/O hot path の決定的な診断値。 */
struct FFileSystemDiagnostics {
    /** ReadFile の呼び出し回数。 */
    u64 read_syscalls = 0;

    /** WriteFile の呼び出し回数。 */
    u64 write_syscalls = 0;

    /** ReadAllText で中間 byte 配列から再コピーした byte 数。常に 0 が最適経路。 */
    u64 text_intermediate_copy_bytes = 0;
};

} // namespace acs
