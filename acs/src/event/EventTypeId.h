// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** メッセージ型ごとの通路を識別する番号。 */
using FEventTypeId = u32;

/** 旧名を使う既存コード向けの互換別名。 */
using EventTypeId = FEventTypeId;

/** 同時に扱えるメッセージ型の上限。 */
inline constexpr FEventTypeId kMaxEventTypes = 256;

/**
 * 通路番号が公開上限の範囲内かを返す。
 * @param channel 調べる通路番号。
 */
constexpr bool IsValidEventTypeId(FEventTypeId channel) noexcept { return channel < kMaxEventTypes; }

} // namespace acs
