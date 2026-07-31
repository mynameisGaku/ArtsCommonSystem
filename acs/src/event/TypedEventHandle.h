// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** 型付きイベントの購読位置を識別するハンドル。 */
struct FTypedEventHandle {
    /** イベント個体の識別番号。 */
    u64 event_id = 0;
    /** 購読枠の位置。 */
    u32 slot_index = 0;
    /** 古いハンドルを見分ける世代番号。 */
    u32 generation = 0;

    /** 有効な購読先を表せる値かを返す。 */
    constexpr bool IsValid() const noexcept {
        return event_id != 0 && generation != 0;
    }

    /**
     * 同じ購読位置を表すかを返す。
     * @param Other 比較するハンドル。
     */
    constexpr bool operator==(FTypedEventHandle Other) const noexcept {
        return event_id == Other.event_id && slot_index == Other.slot_index && generation == Other.generation;
    }

    /**
     * 異なる購読位置を表すかを返す。
     * @param Other 比較するハンドル。
     */
    constexpr bool operator!=(FTypedEventHandle Other) const noexcept {
        return !(*this == Other);
    }
};

} // namespace acs
