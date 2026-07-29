// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** アセットパス共有プールの累積診断値。 */
struct FAssetPathInternerDiagnostics {
    /** Intern 呼び出し数。 */
    u64 request_count = 0u;

    /** 既存文字列を共有した回数。 */
    u64 hit_count = 0u;

    /** 新しい文字列を生成した回数。 */
    u64 miss_count = 0u;

    /** 未使用の保持文字列を追い出した回数。 */
    u64 eviction_count = 0u;

    /** 上限到達時に非保持の共有文字列を返した回数。 */
    u64 bypass_count = 0u;

    /** 現在保持している文字列数。 */
    u64 retained_path_count = 0u;

    /** 現在保持している NUL 込み文字数。 */
    u64 retained_code_units = 0u;
};

} // namespace acs
