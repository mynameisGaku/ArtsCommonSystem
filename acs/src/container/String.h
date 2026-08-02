// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"             // DefaultAllocator()
#include "container/StringView.h"

namespace acs {

/**
 * UTF-8 可変長文字列 (SSO 対応、std::string 代替)。
 *
 * @details
 * 22 バイトまでは inline 領域に格納し (SSO)、それを超えるとヒープへ遷移する。
 * 24 バイトの union 内に SSO バッファとヒープ記述子を重ね、m_Sso.remaining の MSB を
 * ヒープフラグとして共有する。常に NUL 終端を維持し、View() で FStringView を得る。
 */
class FString {
public:
    /** SSO のインライン格納上限 (NUL を含めないバイト数)。 */
    static constexpr usize kSsoCapacity = 22;

    /** 既定アロケータで空文字列を構築する。 */
    FString() noexcept;

    /**
     * 指定アロケータで空文字列を構築する。
     *
     * @param a ヒープ確保に使うアロケータ。
     */
    explicit FString(IAllocator& a) noexcept;

    /**
     * C 文字列から構築する。
     *
     * @param cstr NUL 終端文字列 (nullptr なら空文字列)。
     * @param a ヒープ確保に使うアロケータ (既定は DefaultAllocator)。
     */
    FString(const char* cstr, IAllocator& a = DefaultAllocator()) noexcept;

    /**
     * FStringView から構築する。
     *
     * @param v コピー元のビュー。
     * @param a ヒープ確保に使うアロケータ (既定は DefaultAllocator)。
     */
    FString(FStringView v,    IAllocator& a = DefaultAllocator()) noexcept;

    /**
     * コピー構築する (内容を複製する)。
     *
     * @param o コピー元。
     */
    FString(const FString& o) noexcept;

    /**
     * ムーブ構築する (ヒープなら所有権移譲、SSO なら memcpy)。
     *
     * @param o ムーブ元 (空文字列にリセットされる)。
     */
    FString(FString&& o)      noexcept;

    /**
     * コピー代入する (既存内容をクリアして複製する)。
     *
     * @param o コピー元。
     * @return *this。
     */
    FString& operator=(const FString& o) noexcept;

    /**
     * ムーブ代入する (既存内容を解放して所有権を奪う)。
     *
     * @param o ムーブ元 (空文字列にリセットされる)。
     * @return *this。
     */
    FString& operator=(FString&& o)      noexcept;

    /** ヒープを使っていれば解放する。 */
    ~FString() noexcept;

    /**
     * 先頭ポインタを const で返す (SSO/ヒープを透過)。
     *
     * @return NUL 終端された文字列バッファの先頭。
     */
    const char* Data()  const noexcept { return IsHeap() ? m_Heap.data : m_Sso.data; }

    /**
     * 先頭ポインタを返す (SSO/ヒープを透過)。
     *
     * @return NUL 終端された文字列バッファの先頭。
     */
    char*       Data()        noexcept { return IsHeap() ? m_Heap.data : m_Sso.data; }

    /**
     * バイト長を返す (SSO/ヒープを透過)。
     *
     * @return NUL を含まない文字列のバイト数。
     */
    usize       Size()  const noexcept { return IsHeap() ? m_Heap.size : (kSsoCapacity - m_Sso.remaining); }

    /**
     * 確保済み容量を返す (SSO/ヒープを透過)。
     *
     * @details
     * ヒープフラグ bit (m_Sso.remaining の MSB = m_Heap.capacity の bit 63) は容量と
     * union で重なるため、必ずマスクして読む。マスクしないと巨大値を返し Append の
     * 拡大判定が常に偽となってヒープバッファを溢れる。
     * @return NUL を含まない格納可能バイト数。
     */
    usize       Capacity() const noexcept { return IsHeap() ? (m_Heap.capacity & ~kHeapFlagBit) : kSsoCapacity; }

    /**
     * 空かどうかを返す。
     *
     * @return バイト長が 0 なら true。
     */
    bool        IsEmpty() const noexcept { return Size() == 0; }

    /**
     * 内容を指す FStringView を返す。
     *
     * @return 文字列全体を指す非所有ビュー。
     */
    FStringView  View()  const noexcept { return FStringView(Data(), Size()); }

    /**
     * FStringView への暗黙変換。
     *
     * @return 文字列全体を指す非所有ビュー。
     */
    operator FStringView() const noexcept { return View(); }

    /** Find / FindLast が「見つからない」を表す番兵値 (FStringView::kNpos と同値)。 */
    static constexpr usize kNpos = static_cast<usize>(-1);

    /**
     * 部分文字列 needle が最初に現れるバイトオフセットを返す (View への転送)。
     *
     * @param needle 探す部分文字列。
     * @param from 探索を開始するバイトオフセット。
     * @return 最初に一致した開始オフセット (無ければ kNpos)。
     */
    usize Find(FStringView needle, usize from = 0) const noexcept { return View().Find(needle, from); }

    /**
     * 文字 c が最初に現れるバイトオフセットを返す (View への転送)。
     *
     * @param c 探す文字。
     * @param from 探索を開始するバイトオフセット。
     * @return 最初に一致したオフセット (無ければ kNpos)。
     */
    usize Find(char c, usize from = 0) const noexcept { return View().Find(c, from); }

    /**
     * 文字 c が最後に現れるバイトオフセットを返す (View への転送)。
     *
     * @param c 探す文字。
     * @return 最後に一致したオフセット (無ければ kNpos)。
     */
    usize FindLast(char c) const noexcept { return View().FindLast(c); }

    /**
     * 部分文字列 needle を含むかを返す (View への転送)。
     *
     * @param needle 探す部分文字列。
     * @return 含めば true。
     */
    bool Contains(FStringView needle) const noexcept { return View().Contains(needle); }

    /**
     * 指定した接頭辞で始まるかを返す (View への転送)。
     *
     * @param prefix 判定する接頭辞。
     * @return prefix で始まれば true。
     */
    bool StartsWith(FStringView prefix) const noexcept { return View().StartsWith(prefix); }

    /**
     * 指定した接尾辞で終わるかを返す (View への転送)。
     *
     * @param suffix 判定する接尾辞。
     * @return suffix で終われば true。
     */
    bool EndsWith(FStringView suffix) const noexcept { return View().EndsWith(suffix); }

    /**
     * i 番目のバイトへの参照を返す。
     *
     * @details 範囲外は ACS_ASSERT で検出する (Debug ビルドのみ)。
     * @param i バイトインデックス。
     * @return i 番目のバイトへの参照。
     */
    char&       operator[](usize i)       noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }

    /**
     * i 番目のバイトへの const 参照を返す。
     *
     * @details 範囲外は ACS_ASSERT で検出する (Debug ビルドのみ)。
     * @param i バイトインデックス。
     * @return i 番目のバイトへの const 参照。
     */
    const char& operator[](usize i) const noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }

    /** 空文字列にリセットする (容量は保持)。 */
    void Clear() noexcept;

    /**
     * 空文字列にし、ヒープ容量も確保元へ返す。
     *
     * @details 設定済みアロケータは維持するため、次回の Append も同じ確保元を使う。
     */
    void ReleaseStorage() noexcept;

    /**
     * 容量を予約する (必要なら SSO からヒープへ遷移)。
     *
     * @param new_capacity 確保する最小容量。
     */
    void Reserve(usize new_capacity) noexcept;

    /**
     * 容量予約を試み、確保に失敗したら文字列を変えず false を返す。
     *
     * @param new_capacity 確保する最小容量。
     * @return 予約済みまたは予約成功なら true、OOM なら false。
     */
    bool TryReserve(usize new_capacity) noexcept;

    /**
     * 文字列を追記する (容量不足なら約 1.5 倍ずつ拡大)。
     *
     * @details 追記元が自分のバッファを指す self-aliasing も安全に扱う。
     * @param v 追記するビュー。
     */
    void Append(FStringView v) noexcept;

    /**
     * 1 文字を追記する。
     *
     * @param c 追記する文字。
     */
    void Append(char c)        noexcept;

    /**
     * 追記を試み、拡張確保に失敗したら文字列を変えず false を返す。
     *
     * @param v 追記するビュー。
     * @return 成功なら true、OOM なら false。
     */
    bool TryAppend(FStringView v) noexcept;

    /**
     * 1 文字の追記を試み、拡張確保に失敗したら false を返す。
     *
     * @param c 追記する文字。
     * @return 成功なら true、OOM なら false。
     */
    bool TryAppend(char c) noexcept;

    /**
     * 1 文字を末尾に追加する (Append(char) のエイリアス)。
     *
     * @param c 追加する文字。
     */
    void PushBack(char c)      noexcept { Append(c); }

    /**
     * printf 風フォーマットで追記する。
     *
     * @details vsnprintf で必要長を求めてから Reserve して書き込む。
     * @param fmt printf 形式の書式文字列。
     * @return 追記後の文字列サイズ (失敗時は 0)。
     */
    usize AppendFormat(const char* fmt, ...) noexcept;

    /**
     * この文字列が使うアロケータを返す。
     *
     * @return ヒープ確保に使っている IAllocator へのポインタ。
     */
    IAllocator* GetAllocator() const noexcept { return m_Alloc; }

private:
    /** ヒープフラグ bit (m_Sso.remaining の MSB = LE x64 で m_Heap.capacity の bit 63)。 */
    static constexpr usize kHeapFlagBit = usize{1} << 63;

    /**
     * ヒープモードかどうかを返す (remaining バイトの MSB がフラグ)。
     *
     * @return ヒープを使っていれば true。
     */
    bool IsHeap() const noexcept { return (m_Sso.remaining & 0x80) != 0; }

    /** ヒープフラグを立てる (remaining の MSB をセット)。 */
    void SetHeap() noexcept { m_Sso.remaining |= 0x80; }

    /**
     * SSO のインライン長を設定する (remaining = 残り容量として格納)。
     *
     * @details bit7 はヒープフラグ専用なので、len が上限を超えない不変条件を ACS_ASSERT で守る。
     * @param len 設定する文字列長。
     */
    void SetInlineLen(u8 len) noexcept {
        ACS_ASSERT(len <= kSsoCapacity);  // bit7 は heap フラグ専用。remaining が MSB を侵さない不変条件を保証
        m_Sso.remaining = static_cast<u8>(kSsoCapacity - len);
    }

    /**
     * 容量を拡大する (必要なら SSO からヒープへ遷移し、内容をコピーする)。
     *
     * @param new_capacity 確保する最小容量。
     */
    void Grow(usize new_capacity) noexcept;

    /**
     * 容量拡大を試み、確保に失敗したら文字列を変えず false を返す。
     *
     * @details バッファ確保が成功するまで既存の記述子を変えないため、OOM 時も文字列は有効なまま。
     * @param new_capacity 確保する最小容量。
     * @return 成功なら true、OOM なら false。
     */
    bool TryGrow(usize new_capacity) noexcept;

    /** SSO 領域とヒープ記述子を重ねる 24 バイト固定 union。 */
    union {
        /** SSO モードのインライン格納構造。 */
        struct {
            /** インライン文字列バッファ (+1 は NUL 終端用)。 */
            char  data[kSsoCapacity + 1];

            /** SSO の残り容量。MSB がヒープフラグを兼ねる。 */
            u8    remaining;
        } m_Sso;

        /** ヒープモードの記述子構造。 */
        struct {
            /** ヒープ確保したバッファの先頭ポインタ。 */
            char* data;

            /** 現在の文字列バイト長。 */
            usize size;

            /** ヒープ容量。最上位バイトは m_Sso.remaining (ヒープフラグ) と重なる。 */
            usize capacity;
        } m_Heap;
    };

    /** ヒープ確保に使うアロケータ。 */
    IAllocator* m_Alloc = nullptr;
};

/**
 * FString と FStringView がバイト一致するかを返す。
 *
 * @param a 左辺の FString。
 * @param b 右辺のビュー。
 * @return 一致すれば true。
 */
inline bool operator==(const FString& a, FStringView b) noexcept { return a.View() == b; }

/**
 * FStringView と FString がバイト一致するかを返す。
 *
 * @param a 左辺のビュー。
 * @param b 右辺の FString。
 * @return 一致すれば true。
 */
inline bool operator==(FStringView a, const FString& b) noexcept { return a == b.View(); }

/**
 * 2 つの FString がバイト一致するかを返す。
 *
 * @param a 左辺の FString。
 * @param b 右辺の FString。
 * @return 一致すれば true。
 */
inline bool operator==(const FString& a, const FString& b) noexcept { return a.View() == b.View(); }

} // namespace acs
