// SPDX-License-Identifier: Apache-2.0
// ACS Container — FStringView（std::string_view 代替、UTF-8 非所有ビュー）
//
// ポインタ + 長さで文字列を参照する軽量値型。所有権を持たないため、
// 元の文字列のライフタイムに注意。
//
// UTF-8 を前提とする（バイト単位での比較／検索のみ提供、コードポイント
// 操作は未対応）。
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"

namespace acs {

/**
 * UTF-8 文字列への非所有ビュー (std::string_view 代替)。
 *
 * @details
 * ポインタ + バイト長で文字列を参照する軽量値型。所有権を持たないため、元の文字列の
 * ライフタイムは呼び出し側が保証する。UTF-8 を前提とし、バイト単位の比較・検索のみを
 * 提供する (コードポイント操作は未対応)。
 */
class FStringView {
public:
    /** 空のビューを構築する (data=nullptr, size=0)。 */
    constexpr FStringView() noexcept = default;

    /**
     * 先頭ポインタとバイト長からビューを構築する。
     *
     * @param data 参照する文字列の先頭ポインタ。
     * @param size バイト長。
     */
    constexpr FStringView(const char* data, usize size) noexcept : m_Data(data), m_Size(size) {}

    /**
     * C 文字列から暗黙にビューを構築する (NUL 終端まで走査して長さを求める)。
     *
     * @param cstr NUL 終端文字列 (nullptr なら空ビュー)。
     */
    FStringView(const char* cstr) noexcept : m_Data(cstr), m_Size(0) {
        if (cstr) while (cstr[m_Size]) ++m_Size;
    }

    /**
     * 先頭ポインタを返す。
     *
     * @return 文字列の先頭ポインタ。
     */
    constexpr const char* Data() const noexcept { return m_Data; }

    /**
     * バイト長を返す。
     *
     * @return 文字列のバイト数。
     */
    constexpr usize       Size() const noexcept { return m_Size; }

    /**
     * 空かどうかを返す。
     *
     * @return バイト長が 0 なら true。
     */
    constexpr bool        IsEmpty() const noexcept { return m_Size == 0; }

    /**
     * i 番目のバイトを返す。
     *
     * @details 範囲外は ACS_ASSERT で検出する (Debug ビルドのみ)。
     * @param i バイトインデックス。
     * @return i 番目のバイト。
     */
    constexpr char        operator[](usize i) const noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }

    /**
     * 部分文字列ビューを返す。
     *
     * @details 範囲外指定は ACS_ASSERT で検出する (加算ラップしない形で検査)。
     * @param offset 開始バイトオフセット。
     * @param count 取り出すバイト数。
     * @return [offset, offset+count) を指す部分ビュー。
     */
    constexpr FStringView SubView(usize offset, usize count) const noexcept {
        ACS_ASSERT(offset <= m_Size && count <= m_Size - offset);  // 加算ラップしない形で範囲検査
        return FStringView(offset == 0u ? m_Data : m_Data + offset, count);
    }

    /**
     * 先頭イテレータを返す。
     *
     * @return 先頭バイトへのポインタ。
     */
    constexpr const char* begin() const noexcept { return m_Data; }

    /**
     * 終端イテレータを返す。
     *
     * @return 末尾の次を指すポインタ。
     */
    constexpr const char* end()   const noexcept { return m_Size == 0u ? m_Data : m_Data + m_Size; }

    /**
     * バイト単位で完全一致するかを返す。
     *
     * @param other 比較対象。
     * @return 長さも内容も一致すれば true。
     */
    bool Equals(FStringView other) const noexcept {
        if (m_Size != other.m_Size) return false;
        for (usize i = 0; i < m_Size; ++i) if (m_Data[i] != other.m_Data[i]) return false;
        return true;
    }

    /**
     * 指定した接頭辞で始まるかを返す。
     *
     * @param prefix 判定する接頭辞。
     * @return prefix で始まれば true。
     */
    bool StartsWith(FStringView prefix) const noexcept {
        if (prefix.m_Size > m_Size) return false;
        for (usize i = 0; i < prefix.m_Size; ++i) if (m_Data[i] != prefix.m_Data[i]) return false;
        return true;
    }

    /**
     * 指定した接尾辞で終わるかを返す。
     *
     * @param suffix 判定する接尾辞。
     * @return suffix で終われば true。
     */
    bool EndsWith(FStringView suffix) const noexcept {
        if (suffix.m_Size > m_Size) return false;
        const usize off = m_Size - suffix.m_Size;
        for (usize i = 0; i < suffix.m_Size; ++i) if (m_Data[off + i] != suffix.m_Data[i]) return false;
        return true;
    }

private:
    /** 参照する文字列の先頭ポインタ。 */
    const char* m_Data = nullptr;

    /** 文字列のバイト長。 */
    usize       m_Size = 0;
};

/**
 * 2 つのビューがバイト単位で等しいかを返す。
 *
 * @param a 左辺ビュー。
 * @param b 右辺ビュー。
 * @return 一致すれば true。
 */
inline bool operator==(FStringView a, FStringView b) noexcept { return a.Equals(b); }

/**
 * 2 つのビューがバイト単位で異なるかを返す。
 *
 * @param a 左辺ビュー。
 * @param b 右辺ビュー。
 * @return 不一致なら true。
 */
inline bool operator!=(FStringView a, FStringView b) noexcept { return !a.Equals(b); }

} // namespace acs
