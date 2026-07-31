// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** 複数の整数fieldを優先順どおり64bit描画sort keyへ配置するcompile-time layout。 */
template<u32... TFieldBits>
struct TDrawPacketSortKeyLayout final {
    static_assert(sizeof...(TFieldBits) > 0u, "draw sort key requires at least one field");
    static_assert(((TFieldBits > 0u) && ...), "draw sort key fields must not be empty");
    static_assert(((TFieldBits <= 64u) && ...), "draw sort key fields must fit in 64 bits");

    /** layoutが持つfield数を返す。 */
    static constexpr u32 FieldCount() noexcept { return sizeof...(TFieldBits); }

    /** 全fieldが使うbit数を返す。 */
    static constexpr u32 TotalBits() noexcept { return static_cast<u32>((u64{0} + ... + static_cast<u64>(TFieldBits))); }

    static_assert((u64{0} + ... + static_cast<u64>(TFieldBits)) <= 64u, "draw sort key exceeds 64 bits");

    /** 指定fieldのbit幅を返す。 */
    template<u32 TFieldIndex>
    static constexpr u32 FieldBits() noexcept
    {
        static_assert(TFieldIndex < FieldCount(), "draw sort key field index is out of range");
        /** 宣言順のfield幅。 */
        constexpr u32 kWidths[] = {TFieldBits...};
        return kWidths[TFieldIndex];
    }

    /** 指定fieldを格納する下位bit位置を返す。 */
    template<u32 TFieldIndex>
    static constexpr u32 FieldShift() noexcept
    {
        static_assert(TFieldIndex < FieldCount(), "draw sort key field index is out of range");
        /** 宣言順のfield幅。 */
        constexpr u32 kWidths[] = {TFieldBits...};
        /** 後続fieldが使う下位bit数。 */
        u32 shift = 0u;
        for (u32 index = TFieldIndex + 1u; index < FieldCount(); ++index) shift += kWidths[index];
        return shift;
    }

    /** 指定field値をmaskして配置済みbit列を返す。 */
    template<u32 TFieldIndex>
    static constexpr u64 Insert(u64 value) noexcept
    {
        /** 対象fieldのbit幅。 */
        constexpr u32 kBits = FieldBits<TFieldIndex>();
        if constexpr (kBits == 64u) {
            return value;
        } else {
            /** 対象fieldへ残す下位bit mask。 */
            constexpr u64 kMask = (u64{1} << kBits) - 1u;
            return (value & kMask) << FieldShift<TFieldIndex>();
        }
    }
};

} // namespace acs
