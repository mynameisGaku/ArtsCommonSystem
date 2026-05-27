// SPDX-License-Identifier: Apache-2.0
// 連続メモリ領域への非所有ビュー（std::span 代替）。
// 所有権を持たないため、参照先のライフタイムに注意すること。
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"

namespace acs {

template<typename T>
class TSpan {
public:
    constexpr TSpan() noexcept = default;
    constexpr TSpan(T* data, usize size) noexcept : m_Data(data), m_Size(size) {}

    // C 配列からの暗黙変換（テンプレート引数 N で長さを推論）
    template<usize N>
    constexpr TSpan(T (&arr)[N]) noexcept : m_Data(arr), m_Size(N) {}

    constexpr T*       Data()       noexcept { return m_Data; }
    constexpr const T* Data() const noexcept { return m_Data; }
    constexpr usize    Size() const noexcept { return m_Size; }
    constexpr bool     IsEmpty() const noexcept { return m_Size == 0; }

    // 範囲外アクセスはアサート (Debug ビルドのみ検出)
    constexpr T&       operator[](usize i)       noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }
    constexpr const T& operator[](usize i) const noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }

    constexpr T*       begin()       noexcept { return m_Data; }
    constexpr T*       end()         noexcept { return m_Data + m_Size; }
    constexpr const T* begin() const noexcept { return m_Data; }
    constexpr const T* end()   const noexcept { return m_Data + m_Size; }

    // 部分範囲を取り出す (範囲外は ASSERT)
    constexpr TSpan SubSpan(usize offset, usize count) const noexcept {
        ACS_ASSERT(offset + count <= m_Size);
        return TSpan(m_Data + offset, count);
    }

private:
    T*    m_Data = nullptr;
    usize m_Size = 0;
};

} // namespace acs
