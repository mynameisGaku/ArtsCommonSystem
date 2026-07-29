// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/AssetPathInternerDiagnostics.h"
#include "foundation/Types.h"

namespace acs {

/** アセットレジストリのロード共有診断値。 */
struct FAssetRegistryDiagnostics {
    /** 有効な LoadAsync 呼び出し数。 */
    u64 async_request_count = 0u;

    /** 進行中の同一ロードへ合流した回数。 */
    u64 async_coalesced_count = 0u;

    /** 実際に投入した非同期ジョブ数。 */
    u64 async_job_count = 0u;

    /** 実際に開始したファイル読込数。 */
    u64 physical_file_read_count = 0u;

    /** キャッシュから返した同期・非同期要求数。 */
    u64 cache_hit_count = 0u;

    /** パス共有プールの診断値。 */
    FAssetPathInternerDiagnostics path_interner{};
};

} // namespace acs
