// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 一括パッケージ読み取りの一要素。 */
struct FAssetPackReadRequest {
    /** pak 内の UTF-8 仮想パス。 */
    const char* Name = nullptr;

    /** 読み取り先。 */
    u8* OutBuffer = nullptr;

    /** 読み取り先の容量。 */
    u64 BufferSize = 0u;
};

} // namespace acs::game
