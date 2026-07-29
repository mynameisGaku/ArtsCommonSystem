// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** JobGraph の現在の完了カウンタ予約を既存診断 ABI と分離して返す値。 */
struct FJobGraphCompletionDiagnostics {
    /** 現在 Submit 済みなら 1、未 Submit または Reset 後なら 0。 */
    u64 reservation_batch_count = 0u;

    /** 現在の一括予約へ含めた job 数。 */
    u64 reserved_job_count = 0u;
};

} // namespace acs
