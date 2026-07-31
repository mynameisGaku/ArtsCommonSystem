// SPDX-License-Identifier: Apache-2.0
#include "event/TypedEvent.h"
#include "threading/Atomic.h"

/** 新しい型付きイベントの識別番号を返す。 */
acs::u64 acs::typed_event_detail::AllocateTypedEventIdentifier() noexcept {
    /** 次に割り当てる識別番号。 */
    static TAtomic<u64> next_identifier{1};
    /** 今回割り当てを試す識別番号。 */
    u64 Identifier = next_identifier.Load();
    for (;;) {
        if (Identifier == 0) return 0;
        /** 今回の割り当て後に保存する識別番号。 */
        const u64 Next = Identifier == ~u64(0) ? 0 : Identifier + 1;
        if (next_identifier.CompareExchange(Identifier, Next)) return Identifier;
    }
}
