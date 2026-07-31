// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** メッセージ型ごとの通路を識別する番号。 */
using EventTypeId = u32;

/** 同時に扱えるメッセージ型の上限。 */
inline constexpr EventTypeId kMaxEventTypes = 256;

/**
 * 通路番号が公開上限の範囲内かを返す。
 * @param channel 調べる通路番号。
 */
constexpr bool IsValidEventTypeId(EventTypeId channel) noexcept { return channel < kMaxEventTypes; }

} // namespace acs
