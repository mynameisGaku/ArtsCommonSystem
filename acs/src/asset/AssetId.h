// SPDX-License-Identifier: Apache-2.0
// アセット識別子（パス文字列のハッシュから生成）
//
// 同じ文字列からは常に同じ AssetId が得られるので、レジストリのキーとして使える。
#pragma once

#include "foundation/Types.h"
#include "container/Hash.h"
#include "container/StringView.h"

namespace acs {

// 64-bit 識別子（パスハッシュ）
struct AssetId {
    u64 value = 0;

    constexpr bool IsValid() const noexcept { return value != 0; }
    constexpr bool operator==(AssetId o) const noexcept { return value == o.value; }
    constexpr bool operator!=(AssetId o) const noexcept { return value != o.value; }
};

inline constexpr AssetId kInvalidAssetId = AssetId{0};

// パス文字列から AssetId を生成（同じパスなら同じ ID）
inline AssetId MakeAssetId(StringView path) noexcept {
    AssetId id;
    id.value = HashBytes(path.Data(), path.Size());
    if (id.value == 0) id.value = 1;  // 0 を「無効」用に予約
    return id;
}

// HashMap<AssetId, ...> 用ハッシュ特殊化
template<> struct Hasher<AssetId> {
    ACS_FORCEINLINE u64 operator()(AssetId id) const noexcept {
        return HashMix64(id.value);
    }
};

} // namespace acs
