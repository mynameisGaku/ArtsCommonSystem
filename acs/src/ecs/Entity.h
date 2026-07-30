// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/GenerationHandleLayoutTraits.h"

namespace acs {

/**
 * エンティティ識別子 (世代付きハンドル、POD でコピー・比較可)。
 *
 * @details
 * index は FWorld 内のスロット番号、generation は世代番号。スロットが Destroy →
 * 再利用されるたびに generation が +1 されるため、解放済みスロットを指す古い
 * FEntityId は世代不一致として検出でき、dangling 参照を防げる。
 */
struct FEntityId {
    /** FWorld 内のスロット番号 (0xFFFFFFFF は無効を表す)。 */
    u32 index      = 0xFFFFFFFFu;

    /** 世代番号 (Destroy → 再利用のたびに +1)。 */
    u32 generation = 0;

    /**
     * index が有効値かを返す (世代の生存判定は FWorld::IsAlive で行う)。
     *
     * @return index が番兵 0xFFFFFFFF でなければ true。
     */
    constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }

    /**
     * index と generation の両方が一致するかを返す。
     *
     * @param o 比較対象の FEntityId。
     * @return 完全一致すれば true。
     */
    constexpr bool operator==(FEntityId o) const noexcept {
        return index == o.index && generation == o.generation;
    }

    /**
     * operator== の否定を返す。
     *
     * @param o 比較対象の FEntityId。
     * @return 一致しなければ true。
     */
    constexpr bool operator!=(FEntityId o) const noexcept { return !(*this == o); }
};

/** FEntityId の 32bit identity/generation 物理配置契約。 */
template<>
struct TGenerationHandleLayoutTraits<FEntityId> {
    /** 物理配置 trait が利用可能。 */
    static constexpr bool kAvailable = true;
    /** entity identity の byte offset。 */
    static constexpr usize kIdentityOffset = offsetof(FEntityId, index);
    /** generation の byte offset。 */
    static constexpr usize kGenerationOffset = offsetof(FEntityId, generation);
    /** identity field の byte 幅。 */
    static constexpr usize kIdentityBytes = sizeof(FEntityId::index);
    /** generation field の byte 幅。 */
    static constexpr usize kGenerationBytes = sizeof(FEntityId::generation);
    /** domain prefix を持たない。 */
    static constexpr usize kDomainPrefixBytes = 0u;
    /** 型全体の byte 幅。 */
    static constexpr usize kStorageBytes = sizeof(FEntityId);
    /** 型全体の alignment。 */
    static constexpr usize kStorageAlignment = alignof(FEntityId);
};

/** 無効な FEntityId (戻り値や初期値で使う番兵)。 */
inline constexpr FEntityId kInvalidEntity = FEntityId{0xFFFFFFFFu, 0};

} // namespace acs
