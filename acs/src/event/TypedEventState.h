// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "event/TypedEventSlot.h"
#include "foundation/Types.h"

namespace acs::typed_event_detail {

/** 型付きイベントと所有権付き購読が共有する状態。 */
template<typename... Arguments>
struct TEventState {
    /**
     * イベントの共有状態を作る。
     * @param EventId イベント個体の識別番号。
     */
    explicit TEventState(u64 EventId) noexcept : event_id(EventId) {}

    /** 確保済みの購読枠。 */
    TArray<TEventSlot<Arguments...>> slots;

    /** 再利用できる購読枠の位置。 */
    TArray<u32> free_slots;

    /** イベント個体の識別番号。 */
    u64 event_id = 0;

    /** 最後に登録した購読の順序番号。 */
    u64 latest_activation_sequence = 0;

    /** 有効な購読数。 */
    u32 active_count = 0;

    /** 入れ子になった配信の深さ。 */
    u32 publish_depth = 0;
};

/** 新しい型付きイベントの識別番号を返す。 */
u64 AllocateTypedEventIdentifier() noexcept;

} // namespace acs::typed_event_detail
