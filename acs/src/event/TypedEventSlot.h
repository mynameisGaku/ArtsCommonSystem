// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "event/TypedEventCallback.h"
#include "foundation/Types.h"

namespace acs::typed_event_detail {

/** 型付きイベントの一件分の購読情報。 */
template<typename... Arguments>
struct TEventSlot {
    /** 配信時に呼び出す関数。 */
    TEventCallback<Arguments...> callback = nullptr;

    /** 呼び出す関数へ渡す値。 */
    void* user = nullptr;

    /** 配信中に追加された購読を見分ける順序番号。 */
    u64 activation_sequence = 0;

    /** 呼び出し順を決める優先度。 */
    i32 priority = 0;

    /** 古いハンドルを見分ける世代番号。 */
    u32 generation = 1;

    /** 現在購読中かを示す。 */
    bool active = false;

    /** 一度の配信後に解除するかを示す。 */
    bool once = false;

    /** 再利用待ちかを示す。 */
    bool pending_reuse = false;

    /** 世代番号を使い切り再利用できないかを示す。 */
    bool retired = false;
};

} // namespace acs::typed_event_detail
