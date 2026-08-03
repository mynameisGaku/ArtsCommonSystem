// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

// 列挙に「名前・個数・並び」を持たせる仕掛け (UE の UENUM 相当)。
//
// UE は UHT がヘッダを走査してコードを生成するが、こちらはコード生成を挟まない。
// コンパイラが持つ関数シグネチャ文字列 (__FUNCSIG__ / __PRETTY_FUNCTION__) から列挙子名を
// constexpr で切り出すため、列挙子を二度書く必要が無く、型を指定するだけで名前が引ける。
// 名前表はコンパイル時に確定し、実行時の確保も RTTI も要らない。
//
// 使い方:
//   ACS_ENUM()
//   enum class EMyColor : u8 { Red, Green, Blue };
//
//   TEnumTraits<EMyColor>::kCount;                  // 3
//   TEnumTraits<EMyColor>::Name(EMyColor::Green);   // "Green" (FEnumName)
//   TEnumTraits<EMyColor>::FromIndex(2);            // EMyColor::Blue
//   TEnumTraits<EMyColor>::TryParse("Blue", out);   // true
//
// 制約:
//   ・列挙子の値は 0 以上 kEnumScanMax 未満であること (既定 64)。範囲外の値を持つ列挙や
//     ビットフラグ列挙は TEnumTraits を明示特殊化して上書きする。
//   ・同じ値に別名を付けた場合、名前は先に宣言した方が引かれる。

namespace acs {

/**
 * 列挙子の名前 (コンパイラのシグネチャ内を指す参照)。
 *
 * @details
 * NUL 終端ではないので、C 文字列として渡す場合は呼び出し側で終端を用意すること。
 * 参照先はコンパイラが埋め込んだ静的な文字列なので、寿命は気にしなくてよい。
 */
struct FEnumName {
    /** 名前の先頭 (NUL 終端ではない)。 */
    const char* Data = nullptr;

    /** 名前のバイト数。 */
    usize Size = 0;

    /** 名前を引けなかった (未定義の値) なら true。 */
    constexpr bool IsEmpty() const noexcept { return Size == 0; }

    /**
     * NUL 終端文字列と一致するかを返す。
     *
     * @param text 比較する NUL 終端文字列。
     * @return 長さも中身も一致すれば true。
     */
    constexpr bool Equals(const char* text) const noexcept {
        if (text == nullptr || Data == nullptr) return false;
        for (usize i = 0; i < Size; ++i) {
            if (text[i] == '\0' || text[i] != Data[i]) return false;
        }
        return text[Size] == '\0';
    }
};

namespace detail {

/** 自動で走査する列挙子の上限 (0 .. kEnumScanMax-1 を見る)。 */
inline constexpr usize kEnumScanMax = 64;

/** コンパイル時に添字を展開するための列 (std::index_sequence 相当)。 */
template<usize... Indices>
struct TIndexSeq {};

/** kEnumScanMax 個の添字列を作るための再帰。 */
template<usize N, usize... Indices>
struct TMakeIndexSeq : TMakeIndexSeq<N - 1, N - 1, Indices...> {};

/** 添字列の再帰終端。 */
template<usize... Indices>
struct TMakeIndexSeq<0, Indices...> { using FType = TIndexSeq<Indices...>; };

/**
 * 列挙子 1 つ分の名前をコンパイラのシグネチャから切り出す。
 *
 * @details
 * MSVC は有効な列挙子を `...Fn<EMyColor::Green>(void)`、未定義の値を
 * `...Fn<(enum EMyColor)0xc8>(void)` と綴る。clang / gcc も `[Value = EMyColor::Green]` と
 * `[Value = (EMyColor)200]` で同じ見分けができるため、括弧で始まるものを未定義として弾く。
 * @tparam Value 名前を引きたい列挙子。
 * @return 切り出した名前。未定義の値なら空。
 */
template<auto Value>
constexpr FEnumName EnumNameFromSignature() noexcept {
#if defined(_MSC_VER)
    /** 対象の列挙子を含む関数シグネチャ。 */
    const char* const signature = __FUNCSIG__;
    /** 名前の終わりを示す文字。 */
    constexpr char kCloser = '>';
    /** 名前の始まりを示す文字。 */
    constexpr char kOpener = '<';
#else
    const char* const signature = __PRETTY_FUNCTION__;
    constexpr char kCloser = ']';
    constexpr char kOpener = '=';
#endif

    /** シグネチャ全体の長さ。 */
    usize length = 0;
    while (signature[length] != '\0') ++length;

    /** 名前の終端位置。 */
    usize close = 0;
    bool closed = false;
    for (usize i = length; i > 0; --i) {
        if (signature[i - 1] != kCloser) continue;
        close = i - 1;
        closed = true;
        break;
    }
    if (!closed) return FEnumName{};

    /** 名前の開始位置 (開き記号の次)。 */
    usize begin = 0;
    bool opened = false;
    for (usize i = 0; i < close; ++i) {
        if (signature[i] != kOpener) continue;
        begin = i + 1;
        opened = true;
        break;
    }
    if (!opened) return FEnumName{};

    // 空白を飛ばす (clang / gcc の "= " 対策)。
    while (begin < close && signature[begin] == ' ') ++begin;
    if (begin >= close) return FEnumName{};

    // キャスト表記で綴られるのは列挙子が割り当たっていない値。
    if (signature[begin] == '(') return FEnumName{};

    // 「型名::列挙子」で来るので、最後の :: の後ろだけを取る。
    for (usize i = close; i > begin + 1; --i) {
        if (signature[i - 1] != ':' || signature[i - 2] != ':') continue;
        begin = i;
        break;
    }
    return FEnumName{ signature + begin, close - begin };
}

/** 走査範囲ぶんの名前表。 */
template<usize N>
struct TEnumNameTable {
    /** 添字 = 列挙子の値。名前を引けなかった位置は空。 */
    FEnumName Items[N];
};

/**
 * 走査範囲の名前をまとめて引く。
 *
 * @tparam TEnum 対象の列挙型。
 * @param indices 走査する添字列。
 * @return 添字ぶんの名前表。
 */
template<typename TEnum, usize... Indices>
constexpr TEnumNameTable<sizeof...(Indices)> MakeEnumNameTable(TIndexSeq<Indices...> indices) noexcept {
    (void)indices;
    return TEnumNameTable<sizeof...(Indices)>{ { EnumNameFromSignature<static_cast<TEnum>(Indices)>()... } };
}

} // namespace detail

/**
 * 列挙型の名前表。
 *
 * @details
 * 型を渡すだけで使える。飛び番やビットフラグなど自動走査に載らない列挙は、本テンプレートを
 * 明示特殊化すれば同じ形のまま差し替えられる。
 * @tparam TEnum 対象の列挙型。
 */
template<typename TEnum>
struct TEnumTraits {
    /** 対象の列挙型。 */
    using FType = TEnum;

    /** 走査範囲ぶんの名前表 (添字 = 列挙子の値)。 */
    static constexpr auto kTable =
        detail::MakeEnumNameTable<TEnum>(typename detail::TMakeIndexSeq<detail::kEnumScanMax>::FType{});

    /** 名前を引けた列挙子の個数。 */
    static constexpr usize kCount = [] {
        usize count = 0;
        for (usize i = 0; i < detail::kEnumScanMax; ++i) {
            if (!kTable.Items[i].IsEmpty()) ++count;
        }
        return count;
    }();

    /**
     * 名前を返す。
     *
     * @param value 対象の列挙子。
     * @return 名前。走査範囲外や未定義の値なら空。
     */
    static constexpr FEnumName Name(TEnum value) noexcept {
        const usize index = static_cast<usize>(value);
        return index < detail::kEnumScanMax ? kTable.Items[index] : FEnumName{};
    }

    /**
     * 添字から列挙子を作る。
     *
     * @details 名前を引けた列挙子だけを宣言順に数えた添字で指す (飛び番があっても詰めて数える)。
     * @param index 0 起点の添字。
     * @return 対応する列挙子。範囲外なら先頭の列挙子。
     */
    static constexpr TEnum FromIndex(i32 index) noexcept {
        if (index < 0) return TEnum{};
        usize remaining = static_cast<usize>(index);
        for (usize i = 0; i < detail::kEnumScanMax; ++i) {
            if (kTable.Items[i].IsEmpty()) continue;
            if (remaining == 0) return static_cast<TEnum>(i);
            --remaining;
        }
        return TEnum{};
    }

    /**
     * 列挙子を添字にする。
     *
     * @param value 対象の列挙子。
     * @return 0 起点の添字。名前を引けない値なら -1。
     */
    static constexpr i32 ToIndex(TEnum value) noexcept {
        const usize target = static_cast<usize>(value);
        if (target >= detail::kEnumScanMax || kTable.Items[target].IsEmpty()) return -1;

        i32 index = 0;
        for (usize i = 0; i < target; ++i) {
            if (!kTable.Items[i].IsEmpty()) ++index;
        }
        return index;
    }

    /**
     * 名前から列挙子へ戻す。
     *
     * @param name 探す名前 (NUL 終端)。
     * @param out_value 見つかった列挙子の書き込み先。
     * @return 見つかれば true。
     */
    static constexpr bool TryParse(const char* name, TEnum& out_value) noexcept {
        for (usize i = 0; i < detail::kEnumScanMax; ++i) {
            if (!kTable.Items[i].Equals(name)) continue;

            out_value = static_cast<TEnum>(i);
            return true;
        }
        return false;
    }
};

/**
 * 列挙子をまとめて持つ表 (範囲 for で回せる)。
 *
 * @tparam TValue 要素の型。
 * @tparam N 要素数。
 */
template<typename TValue, usize N>
struct TEnumTable {
    /** 要素 (宣言順)。 */
    TValue Items[N > 0 ? N : 1];

    /** 要素数を返す。 */
    constexpr usize Size() const noexcept { return N; }

    /** 先頭を指す。 */
    constexpr const TValue* begin() const noexcept { return Items; }

    /** 終端を指す。 */
    constexpr const TValue* end() const noexcept { return Items + N; }

    /**
     * 要素を 1 つ返す。
     *
     * @param index 0 起点の添字 (範囲外は先頭を返す)。
     * @return 該当要素。
     */
    constexpr const TValue& operator[](usize index) const noexcept {
        return Items[index < N ? index : 0];
    }
};


/**
 * 列挙子の名前を返す。
 *
 * @tparam TEnum 対象の列挙型 (呼び出し時は引数から推論される)。
 * @param value 対象の列挙子。
 * @return 名前。未定義の値なら空。
 */
template<typename TEnum>
constexpr FEnumName ToString(TEnum value) noexcept {
    return TEnumTraits<TEnum>::Name(value);
}

/**
 * 名前から列挙子へ戻す。
 *
 * @tparam TEnum 対象の列挙型。
 * @param name 探す名前 (NUL 終端)。
 * @param out_value 見つかった列挙子の書き込み先。
 * @return 見つかれば true。
 */
template<typename TEnum>
constexpr bool ToEnum(const char* name, TEnum& out_value) noexcept {
    return TEnumTraits<TEnum>::TryParse(name, out_value);
}

/**
 * 名前から列挙子へ戻す (見つからなければ既定値)。
 *
 * @tparam TEnum 対象の列挙型。
 * @param name 探す名前 (NUL 終端)。
 * @param fallback 見つからなかったときに返す値。
 * @return 見つかった列挙子、または fallback。
 */
template<typename TEnum>
constexpr TEnum FromString(const char* name, TEnum fallback = TEnum{}) noexcept {
    TEnum value{};
    return TEnumTraits<TEnum>::TryParse(name, value) ? value : fallback;
}

/**
 * 列挙子の個数。
 *
 * @tparam TEnum 対象の列挙型。
 */
template<typename TEnum>
inline constexpr usize EnumCount = TEnumTraits<TEnum>::kCount;

/**
 * 名前を引ける列挙子かを返す。
 *
 * @tparam TEnum 対象の列挙型。
 * @param value 調べる値。
 * @return 宣言された列挙子なら true。
 */
template<typename TEnum>
constexpr bool IsValidEnum(TEnum value) noexcept {
    return !TEnumTraits<TEnum>::Name(value).IsEmpty();
}

/**
 * 添字から列挙子を作る。
 *
 * @tparam TEnum 対象の列挙型。
 * @param index 0 起点の添字。
 * @return 対応する列挙子。範囲外なら先頭。
 */
template<typename TEnum>
constexpr TEnum EnumFromIndex(i32 index) noexcept {
    return TEnumTraits<TEnum>::FromIndex(index);
}

/**
 * 列挙子を添字にする。
 *
 * @tparam TEnum 対象の列挙型。
 * @param value 対象の列挙子。
 * @return 0 起点の添字。名前を引けない値なら -1。
 */
template<typename TEnum>
constexpr i32 EnumToIndex(TEnum value) noexcept {
    return TEnumTraits<TEnum>::ToIndex(value);
}

/**
 * 全ての列挙子を宣言順に並べた表を返す。
 *
 * @details 範囲 for でそのまま回せる。UI の選択肢を作るときなどに使う。
 * @tparam TEnum 対象の列挙型。
 * @return 列挙子の表。
 */
template<typename TEnum>
constexpr TEnumTable<TEnum, TEnumTraits<TEnum>::kCount> EnumValues() noexcept {
    TEnumTable<TEnum, TEnumTraits<TEnum>::kCount> table{};
    usize next = 0;
    for (usize i = 0; i < detail::kEnumScanMax; ++i) {
        if (TEnumTraits<TEnum>::kTable.Items[i].IsEmpty()) continue;

        table.Items[next] = static_cast<TEnum>(i);
        ++next;
    }
    return table;
}

/**
 * 全ての列挙子の名前を宣言順に並べた表を返す。
 *
 * @tparam TEnum 対象の列挙型。
 * @return 名前の表。
 */
template<typename TEnum>
constexpr TEnumTable<FEnumName, TEnumTraits<TEnum>::kCount> EnumNames() noexcept {
    TEnumTable<FEnumName, TEnumTraits<TEnum>::kCount> table{};
    usize next = 0;
    for (usize i = 0; i < detail::kEnumScanMax; ++i) {
        if (TEnumTraits<TEnum>::kTable.Items[i].IsEmpty()) continue;

        table.Items[next] = TEnumTraits<TEnum>::kTable.Items[i];
        ++next;
    }
    return table;
}

/**
 * 宣言順で次 (または前) の列挙子を返す。
 *
 * @details UI の左右送りに使う。飛び番があっても宣言順で 1 つずつ動く。
 * @tparam TEnum 対象の列挙型。
 * @param value 現在の列挙子。
 * @param delta 進める数 (負で戻る)。
 * @param wrap true なら端で反対側へ回り込み、false なら端で止まる。
 * @return 移動後の列挙子。
 */
template<typename TEnum>
constexpr TEnum EnumNext(TEnum value, i32 delta = 1, bool wrap = true) noexcept {
    const i32 count = static_cast<i32>(TEnumTraits<TEnum>::kCount);
    if (count <= 0) return value;

    const i32 current = TEnumTraits<TEnum>::ToIndex(value);
    if (current < 0) return TEnumTraits<TEnum>::FromIndex(0);

    i32 next = current + delta;
    if (wrap) {
        next %= count;
        if (next < 0) next += count;
    } else {
        if (next < 0) next = 0;
        if (next >= count) next = count - 1;
    }
    return TEnumTraits<TEnum>::FromIndex(next);
}

} // namespace acs

/**
 * 列挙をリフレクション対象として印付ける (UE の UENUM 相当)。
 *
 * @details
 * 名前と個数は TEnumTraits がコンパイラのシグネチャから自動で引くため、この印自体は
 * 何も展開しない。列挙の意図をコード上に残し、将来 acsbuild が走査してカタログを
 * 生成できるようにするための目印として置く。指定子を書いても無視される。
 */
#define ACS_ENUM(...)
