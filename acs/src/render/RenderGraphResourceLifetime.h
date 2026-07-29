// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * Render Graph 上の一時リソース寿命。
 *
 * @details first_pass と last_pass は両端を含む。同じ pass で読み書きされる
 * リソース同士は重複扱いとなり、同一 alias slot へ割り当てられない。
 */
struct FRenderGraphResourceLifetime {
    /** 呼び出し側が管理する一意なリソース識別子。 */
    u32 resource_id = 0;

    /** heap 種別や用途を含む alias 互換クラス。 */
    u64 compatibility_key = 0;

    /** リソースが必要とする配置バイト数。 */
    u64 size_bytes = 0;

    /** 最初に参照する pass 番号。 */
    u32 first_pass = 0;

    /** 最後に参照する pass 番号。 */
    u32 last_pass = 0;

    /** フレームを越えて保持しないリソースか。 */
    bool transient = true;

    /** バックエンドが alias 配置を許可するか。 */
    bool alias_allowed = true;
};

} // namespace acs
