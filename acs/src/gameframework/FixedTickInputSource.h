// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

class FInputStateSnapshot;

/** FGame の各固定 tick へ決定論入力を供給する差し替え境界。 */
class IFixedTickInputSource {
public:
    /** 派生した入力ソースを基底ポインターから安全に破棄する。 */
    virtual ~IFixedTickInputSource() noexcept = default;

    /**
     * 指定した固定 tick の入力を所有 snapshot として取得する。
     * @param fixed_tick 0起点の固定tick番号。時計復元後は同じ番号が再要求される。
     * @param output 取得した入力の書き込み先。失敗時は変更しない。
     * @return 入力を取得できた場合はtrue。falseのtickは無入力として実行される。
     */
    virtual bool TryCaptureFixedTickInput(u64 fixed_tick, FInputStateSnapshot& output) noexcept = 0;
};

} // namespace acs::game
