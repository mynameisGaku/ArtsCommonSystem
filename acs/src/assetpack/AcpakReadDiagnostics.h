// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::assetpack {

/** 読み取り用ファイルマッピングを作る最小パッケージサイズ。 */
inline constexpr u64 kAcpakMappedReadMinimumBytes = 256u * 1024u;

/** Reader が保持して再利用する一時領域の最大サイズ。 */
inline constexpr usize kAcpakRetainedScratchMaxBytes = 16u * 1024u * 1024u;

/** 一度に受け付けるパッケージ読み取り要求数の上限。 */
inline constexpr u32 kAcpakReadBatchMaxEntries = 1024u;

/** パッケージ読み取り経路の累積診断値。 */
struct FAcpakReadDiagnostics {
    /** 不変マッピングから読んだ回数。 */
    u64 mapped_read_count = 0u;

    /** 不変マッピングから読んだ格納バイト数。 */
    u64 mapped_read_bytes = 0u;

    /** ReadFile を使った回数。 */
    u64 buffered_read_count = 0u;

    /** ReadFile を使って読んだ格納バイト数。 */
    u64 buffered_read_bytes = 0u;

    /** 保持一時領域を再利用した回数。 */
    u64 scratch_reuse_count = 0u;

    /** 競合または上限超過で局所一時領域を使った回数。 */
    u64 scratch_fallback_count = 0u;

    /** 複数要求 API の呼び出し数。 */
    u64 batch_count = 0u;

    /** 複数要求 API で処理した要素数。 */
    u64 batch_entry_count = 0u;

    /** 現在保持している一時領域の容量。 */
    u64 retained_scratch_bytes = 0u;

    /** 現在不変ファイルマッピングを利用できるか。 */
    bool mapped_view_active = false;
};

} // namespace acs::assetpack
