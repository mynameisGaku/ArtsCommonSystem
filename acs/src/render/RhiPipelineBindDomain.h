// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** パイプラインを拘束するコマンド領域。 */
enum class ERhiPipelineBindDomain : u8 {
    /** グラフィックス領域。 */
    Graphics,
    /** コンピュート領域。 */
    Compute,
};

} // namespace acs
