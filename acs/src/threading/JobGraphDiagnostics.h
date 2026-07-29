// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** JobGraph の構築・再実行経路を観測する決定的な診断値。 */
struct FJobGraphDiagnostics {
    /** 依存 topology を構築した回数。 */
    u64 topology_compilations = 0;

    /** Submit で entry 探索のために全 job を走査した回数。 */
    u64 submit_full_graph_scans = 0;

    /** Reset が依存カウンタ復元のために参照した job 数。 */
    u64 reset_job_visits = 0;

    /** graph 内部の inline slot に置かれた job 数。 */
    u32 inline_job_count = 0;

    /** inline slot を超えて個別確保した job 数。 */
    u32 heap_job_count = 0;

    /** job 内部の inline 領域に置かれた所有 callable 数。 */
    u32 inline_callable_count = 0;

    /** サイズまたは alignment 超過で個別確保した callable 数。 */
    u32 heap_callable_count = 0;
};

} // namespace acs
