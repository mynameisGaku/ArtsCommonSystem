// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * メッセージパイプの同期方式。
 *
 * @details Mpmc は複数 producer / consumer とブロッキング Pop を提供する。
 * Spsc は producer と consumer が各 1 スレッドに固定される場合の固定容量リングである。
 */
enum class EMessagePipePolicy : u8 {
    /** 複数 producer と複数 consumer を mutex で同期する。 */
    Mpmc,

    /** producer と consumer を各 1 スレッドへ固定する。 */
    Spsc,
};

/** SPSC 固定容量として利用できる値かをコンパイル時に返す。 */
template<usize Capacity>
inline constexpr bool kIsValidMessagePipeCapacity = Capacity >= 2 && (Capacity & (Capacity - 1)) == 0;

} // namespace acs
