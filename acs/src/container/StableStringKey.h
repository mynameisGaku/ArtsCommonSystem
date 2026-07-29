// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/ConstexprHash.h"
#include "container/StringView.h"

namespace acs {

/**
 * 文字列ビューと事前計算済みハッシュを保持する検索キー。
 *
 * THashMap::FindByHash(Key.View, Key.Hash) へ渡し、文字列の再 hash を避ける。
 */
struct FStableStringKey {
    /** 比較対象の文字列ビュー。 */
    FStringView View;

    /** HashBytes と同じ手順で求めたハッシュ値。 */
    u64 Hash = 0u;
};

/**
 * 文字列リテラルから確保不要の検索キーを作る。
 *
 * @tparam N 終端 NUL を含むリテラル配列長。
 * @param Literal 検索に使う文字列リテラル。
 * @return リテラル view と HashLiteral の結果を保持するキー。
 */
template<usize N>
constexpr FStableStringKey MakeStableStringKey(const char (&Literal)[N]) noexcept
{
    static_assert(N > 0u, "文字列リテラルは終端 NUL を必要とする");
    return FStableStringKey{FStringView(Literal, N - 1u), HashLiteral(Literal)};
}

} // namespace acs
