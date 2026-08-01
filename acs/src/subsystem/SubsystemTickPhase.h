// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** サブシステムを呼び出すフレーム更新段階。 */
enum class ESubsystemTickPhase : u8 {
    /** 自動更新を行わない。 */
    None = 0,
    /** 利用側の通常更新より前に呼び出す。 */
    PreUpdate = 1,
    /** 利用側の通常更新より後に呼び出す。 */
    PostUpdate = 2,
};

} // namespace acs
