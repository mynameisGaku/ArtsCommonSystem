// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — FStringView（std::string_view 代替、UTF-8 非所有ビュー）
// -----------------------------------------------------------------------------
// ポインタ + 長さで文字列を参照する軽量値型。所有権を持たないため、
// 元の文字列のライフタイムに注意。
//
// UTF-8 を前提とする（バイト単位での比較／検索のみ提供、コードポイント
// 操作は未対応）。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"

namespace acs {

class FStringView {
public:
    constexpr FStringView() noexcept = default;
    constexpr FStringView(const char* data, usize size) noexcept : m_Data(data), m_Size(size) {}

    // C 文字列からの暗黙変換（NUL 終端を見つけて長さを計算）
    FStringView(const char* cstr) noexcept : m_Data(cstr), m_Size(0) {
        if (cstr) while (cstr[m_Size]) ++m_Size;
    }

    constexpr const char* Data() const noexcept { return m_Data; }
    constexpr usize       Size() const noexcept { return m_Size; }
    constexpr bool        IsEmpty() const noexcept { return m_Size == 0; }

    constexpr char        operator[](usize i) const noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }

    // 部分文字列ビュー
    constexpr FStringView SubView(usize offset, usize count) const noexcept {
        ACS_ASSERT(offset <= m_Size && count <= m_Size - offset);  // 加算ラップしない形で範囲検査
        return FStringView(m_Data + offset, count);
    }

    constexpr const char* begin() const noexcept { return m_Data; }
    constexpr const char* end()   const noexcept { return m_Data + m_Size; }

    // バイト単位の完全一致比較
    bool Equals(FStringView other) const noexcept {
        if (m_Size != other.m_Size) return false;
        for (usize i = 0; i < m_Size; ++i) if (m_Data[i] != other.m_Data[i]) return false;
        return true;
    }

    // 接頭辞判定
    bool StartsWith(FStringView prefix) const noexcept {
        if (prefix.m_Size > m_Size) return false;
        for (usize i = 0; i < prefix.m_Size; ++i) if (m_Data[i] != prefix.m_Data[i]) return false;
        return true;
    }

    // 接尾辞判定
    bool EndsWith(FStringView suffix) const noexcept {
        if (suffix.m_Size > m_Size) return false;
        usize off = m_Size - suffix.m_Size;
        for (usize i = 0; i < suffix.m_Size; ++i) if (m_Data[off + i] != suffix.m_Data[i]) return false;
        return true;
    }

private:
    const char* m_Data = nullptr;
    usize       m_Size = 0;
};

inline bool operator==(FStringView a, FStringView b) noexcept { return a.Equals(b); }
inline bool operator!=(FStringView a, FStringView b) noexcept { return !a.Equals(b); }

} // namespace acs
