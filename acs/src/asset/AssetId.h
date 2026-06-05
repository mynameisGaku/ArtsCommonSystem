// SPDX-License-Identifier: Apache-2.0
// アセット識別子（パス文字列のハッシュから生成）
//
// 同じ文字列からは常に同じ FAssetId が得られるので、レジストリのキーとして使える。
#pragma once

#include "foundation/Types.h"
#include "container/Hash.h"
#include "container/StringView.h"

namespace acs {

/**
 * パス文字列のハッシュから生成される 64-bit アセット識別子。
 *
 * @details
 * 同じ文字列からは常に同じ値が得られるので、レジストリのキーとして使える。
 * value == 0 は無効値として予約 (kInvalidAssetId)。
 */
struct FAssetId {
    /** パスハッシュ値 (0 は無効を表す)。 */
    u64 value = 0;

    /**
     * 有効な ID かを返す。
     *
     * @return value が 0 でなければ true。
     */
    constexpr bool IsValid() const noexcept { return value != 0; }

    /**
     * 等価比較する。
     *
     * @param o 比較対象の ID。
     * @return value が一致すれば true。
     */
    constexpr bool operator==(FAssetId o) const noexcept { return value == o.value; }

    /**
     * 非等価比較する。
     *
     * @param o 比較対象の ID。
     * @return value が一致しなければ true。
     */
    constexpr bool operator!=(FAssetId o) const noexcept { return value != o.value; }
};

/** 無効を表すアセット ID (value == 0)。 */
inline constexpr FAssetId kInvalidAssetId = FAssetId{0};

/**
 * パス文字列から FAssetId を生成する (同じパスなら同じ ID)。
 *
 * @details ハッシュが 0 になった場合は 1 に補正する (0 は無効値として予約)。
 * @param path ハッシュ元のパス文字列。
 * @return パスから決定論的に生成した有効な FAssetId。
 */
inline FAssetId MakeAssetId(FStringView path) noexcept {
    FAssetId id;
    id.value = HashBytes(path.Data(), path.Size());
    if (id.value == 0) id.value = 1;  // 0 を「無効」用に予約
    return id;
}

/**
 * THashMap<FAssetId, ...> 用のハッシュ特殊化。
 *
 * @tparam FAssetId キーとなるアセット識別子型。
 */
template<> struct THasher<FAssetId> {
    /**
     * アセット ID をハッシュ値へ変換する。
     *
     * @param id ハッシュ対象のアセット ID。
     * @return value を撹拌した 64-bit ハッシュ値。
     */
    ACS_FORCEINLINE u64 operator()(FAssetId id) const noexcept {
        return HashMix64(id.value);
    }
};

} // namespace acs
