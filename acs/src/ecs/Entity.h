// エンティティ ID（世代付きハンドル）
//
// EntityId は 64 ビットで「インデックス + 世代」を表す。
// 同じインデックスが再利用されても世代が違うので、古い ID を使うと検出できる。
#pragma once

#include "foundation/Types.h"

namespace acs {

// エンティティ識別子（POD、コピー可、比較可）
struct EntityId {
    u32 index      = 0xFFFFFFFFu;  // World 内のスロット番号
    u32 generation = 0;             // 世代番号（Destroy → 再利用で +1）

    constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }
    constexpr bool operator==(EntityId o) const noexcept {
        return index == o.index && generation == o.generation;
    }
    constexpr bool operator!=(EntityId o) const noexcept { return !(*this == o); }
};

// 無効な EntityId（戻り値や初期値で使う）
inline constexpr EntityId kInvalidEntity = EntityId{0xFFFFFFFFu, 0};

} // namespace acs
