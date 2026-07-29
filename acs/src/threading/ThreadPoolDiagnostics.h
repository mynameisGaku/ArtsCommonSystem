// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** ThreadPool の同期・割り当て hot path を観測する決定的な診断値。 */
struct FThreadPoolDiagnostics {
    /** 外部投入キュー lock の取得回数。 */
    u64 submission_lock_acquisitions = 0;

    /** 外部投入キューを drain するために lock を取得した回数。 */
    u64 submission_drain_lock_acquisitions = 0;

    /** 外部投入キュー lock を最初の TryLock で取れなかった回数。 */
    u64 submission_lock_contentions = 0;

    /** 外部投入キューから一括取得したタスク数。 */
    u64 external_tasks_drained = 0;

    /** park へ入った回数。 */
    u64 worker_parks = 0;

    /** 実際に待機者がいるときに発行した NotifyOne 回数。 */
    u64 wake_one_calls = 0;

    /** 終了処理で発行した NotifyAll 回数。 */
    u64 wake_all_calls = 0;

    /** 固定ノードプール枯渇後の HeapAlloc 回数。 */
    u64 submission_heap_fallbacks = 0;

    /** inline 領域へ格納した所有 callable の投入数。 */
    u64 callable_inline_submissions = 0;

    /** サイズ超過で heap へ格納した所有 callable の投入数。 */
    u64 callable_heap_submissions = 0;

    /** 所有 callable ノードプール枯渇後の HeapAlloc 回数。 */
    u64 callable_node_heap_fallbacks = 0;

    /** まだワーカーに取得されていない公開済みタスク数。 */
    u32 queued_work = 0;
};

} // namespace acs
