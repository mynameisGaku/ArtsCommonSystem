// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/TypeTraits.h"
#include "foundation/Types.h"

namespace acs {

/**
 * 0 から連続する enum の妥当性判定と名前取得を constexpr 配列から行う。
 *
 * @tparam Enum 0 始まりで欠番のない enum 型。
 * @tparam Count 有効値と名前の件数。
 */
template<typename Enum, usize Count>
class TContiguousEnumLookup {
    static_assert(IsEnumV<Enum>, "TContiguousEnumLookup は enum 型だけを受け付けます");
    static_assert(Count > 0u, "TContiguousEnumLookup には 1 件以上の名前が必要です");

public:
    /**
     * enum 値と同じ index 順の名前列から table を生成する。
     *
     * @param Names 0 から Count - 1 に対応する名前列。
     */
    constexpr explicit TContiguousEnumLookup(const char* const (&Names)[Count]) noexcept
    {
        /** 名前表へコピーする enum index。 */
        for (usize Index = 0u; Index < Count; ++Index) m_Names[Index] = Names[Index];
    }

    /**
     * 値が 0 から Count - 1 の範囲なら true を返す。
     *
     * @param Value 検証する enum 値。
     * @return table の有効範囲なら true。
     */
    constexpr bool Contains(Enum Value) const noexcept
    {
        return static_cast<u64>(Value) < static_cast<u64>(Count);
    }

    /**
     * 有効値に対応する名前を返し、範囲外なら Fallback を返す。
     *
     * @param Value 名前を取得する enum 値。
     * @param Fallback 範囲外で返す文字列。
     * @return 名前表内の名前または範囲外文字列。
     */
    constexpr const char* Name(Enum Value, const char* Fallback = "Unknown") const noexcept
    {
        return Contains(Value) ? m_Names[static_cast<usize>(Value)] : Fallback;
    }

    /**
     * table が保持する有効値数を返す。
     *
     * @return 保持する有効値数。
     */
    static constexpr usize Size() noexcept
    {
        return Count;
    }

private:
    /** enum 値の整数表現を index として引く名前列。 */
    const char* m_Names[Count]{};
};

} // namespace acs
