// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"

namespace acs {

/**
 * 連続メモリ領域への非所有ビュー (std::span 代替)。
 *
 * @details
 * ポインタ + 要素数で連続領域を参照する軽量値型。所有権を持たないため、
 * 参照先のライフタイムは呼び出し側が保証する。範囲外アクセスは ACS_ASSERT で
 * 検出する (Debug ビルドのみ)。
 * @tparam T 要素型 (const を付ければ読み取り専用ビューになる)。
 */
template<typename T>
class TSpan {
public:
    /** 空のビューを構築する (data=nullptr, size=0)。 */
    constexpr TSpan() noexcept = default;

    /**
     * 先頭ポインタと要素数からビューを構築する。
     *
     * @param data 参照する連続領域の先頭ポインタ。
     * @param size 要素数。
     */
    constexpr TSpan(T* data, usize size) noexcept : m_Data(data), m_Size(size) {}

    /**
     * C 配列から暗黙にビューを構築する (長さ N をテンプレート引数で推論)。
     *
     * @tparam N 配列要素数 (コンパイル時に推論される)。
     * @param arr 参照する C 配列。
     */
    template<usize N>
    constexpr TSpan(T (&arr)[N]) noexcept : m_Data(arr), m_Size(N) {}

    /**
     * 先頭ポインタを返す。
     *
     * @return 連続領域の先頭ポインタ。
     */
    constexpr T*       GetData()       noexcept { return m_Data; }

    /**
     * 先頭ポインタを const で返す。
     *
     * @return 連続領域の先頭 const ポインタ。
     */
    constexpr const T* GetData() const noexcept { return m_Data; }

    /**
     * 要素数を返す。
     *
     * @return 要素数。
     */
    constexpr usize    Num() const noexcept { return m_Size; }

    /**
     * 空かどうかを返す。
     *
     * @return 要素数が 0 なら true。
     */
    constexpr bool     IsEmpty() const noexcept { return m_Size == 0; }

    /**
     * i 番目の要素への参照を返す。
     *
     * @details 範囲外アクセスは ACS_ASSERT で検出する (Debug ビルドのみ)。
     * @param i 要素インデックス。
     * @return i 番目の要素への参照。
     */
    constexpr T&       operator[](usize i)       noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }

    /**
     * i 番目の要素への const 参照を返す。
     *
     * @details 範囲外アクセスは ACS_ASSERT で検出する (Debug ビルドのみ)。
     * @param i 要素インデックス。
     * @return i 番目の要素への const 参照。
     */
    constexpr const T& operator[](usize i) const noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }

    /**
     * 先頭イテレータを返す。
     *
     * @return 先頭要素へのポインタ。
     */
    constexpr T*       begin()       noexcept { return m_Data; }

    /**
     * 終端イテレータを返す。
     *
     * @return 末尾の次を指すポインタ。
     */
    constexpr T*       end()         noexcept { return m_Size == 0u ? m_Data : m_Data + m_Size; }

    /**
     * 先頭 const イテレータを返す。
     *
     * @return 先頭要素への const ポインタ。
     */
    constexpr const T* begin() const noexcept { return m_Data; }

    /**
     * 終端 const イテレータを返す。
     *
     * @return 末尾の次を指す const ポインタ。
     */
    constexpr const T* end()   const noexcept { return m_Size == 0u ? m_Data : m_Data + m_Size; }

    /**
     * 部分範囲を切り出したビューを返す。
     *
     * @details 範囲外指定は ACS_ASSERT で検出する (加算ラップしない形で検査)。
     * @param offset 開始オフセット。
     * @param count 取り出す要素数。
     * @return [offset, offset+count) を指す部分ビュー。
     */
    constexpr TSpan SubSpan(usize offset, usize count) const noexcept {
        // 加算ラップしない形で範囲を検査する。
        ACS_ASSERT(offset <= m_Size && count <= m_Size - offset);
        return TSpan(offset == 0u ? m_Data : m_Data + offset, count);
    }

    /**
     * 範囲が妥当な場合だけ部分ビューを返す。
     *
     * @details 外部入力の decode で assert に依存せず、null と加算 overflow を
     * fail-closed に拒否する。失敗時は out を変更しない。
     * @param offset 開始要素位置。
     * @param count 取得要素数。
     * @param out 成功時の部分ビュー格納先。
     * @return 範囲が有効なら true。
     */
    constexpr bool TrySubSpan(usize offset, usize count, TSpan& out) const noexcept {
        if ((m_Data == nullptr && m_Size != 0u) || offset > m_Size || count > m_Size - offset) return false;
        out = TSpan(offset == 0u ? m_Data : m_Data + offset, count);
        return true;
    }

private:
    /** 参照する連続領域の先頭ポインタ。 */
    T*    m_Data = nullptr;

    /** 要素数。 */
    usize m_Size = 0;
};

} // namespace acs
