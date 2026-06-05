// SPDX-License-Identifier: Apache-2.0
// エンティティ ID（世代付きハンドル）
//
// EntityId は 64 ビットで「インデックス + 世代」を表す。
// 同じインデックスが再利用されても世代が違うので、古い ID を使うと検出できる。
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * エンティティ識別子 (世代付きハンドル、POD でコピー・比較可)。
 *
 * @details
 * index は World 内のスロット番号、generation は世代番号。スロットが Destroy →
 * 再利用されるたびに generation が +1 されるため、解放済みスロットを指す古い
 * EntityId は世代不一致として検出でき、dangling 参照を防げる。
 */
struct EntityId {
    /** World 内のスロット番号 (0xFFFFFFFF は無効を表す)。 */
    u32 index      = 0xFFFFFFFFu;

    /** 世代番号 (Destroy → 再利用のたびに +1)。 */
    u32 generation = 0;

    /**
     * index が有効値かを返す (世代の生存判定は World::IsAlive で行う)。
     *
     * @return index が番兵 0xFFFFFFFF でなければ true。
     */
    constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }

    /**
     * index と generation の両方が一致するかを返す。
     *
     * @param o 比較対象の EntityId。
     * @return 完全一致すれば true。
     */
    constexpr bool operator==(EntityId o) const noexcept {
        return index == o.index && generation == o.generation;
    }

    /**
     * operator== の否定を返す。
     *
     * @param o 比較対象の EntityId。
     * @return 一致しなければ true。
     */
    constexpr bool operator!=(EntityId o) const noexcept { return !(*this == o); }
};

/** 無効な EntityId (戻り値や初期値で使う番兵)。 */
inline constexpr EntityId kInvalidEntity = EntityId{0xFFFFFFFFu, 0};

} // namespace acs
