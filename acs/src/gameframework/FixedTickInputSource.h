// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

class FInputStateSnapshot;

/** CGameの各固定tickへ決定論入力を供給する差し替え境界。 */
class IFixedTickInputSource {
public:
    /** 派生した入力ソースを基底ポインターから安全に破棄する。 */
    virtual ~IFixedTickInputSource() noexcept = default;

    /** 指定した0起点の固定tick入力を取得し、失敗したtickは無入力として扱わせる。 */
    virtual bool TryCaptureFixedTickInput(u64 fixed_tick, FInputStateSnapshot& output) noexcept = 0;
};

} // namespace acs::game
