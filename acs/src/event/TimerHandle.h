// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/GenerationHandleLayoutTraits.h"
#include "foundation/Types.h"

#include <cstddef>

namespace acs {

/** 登録したタイマーを識別するハンドル。 */
struct FTimerHandle {
    /** タイマーの識別番号。 */
    u32 id = 0;
    /** 再利用された枠を見分ける世代番号。 */
    u32 generation = 0;

    /** 有効なタイマーを表せる値かを返す。 */
    constexpr bool IsValid() const noexcept {
        return id != 0 && generation != 0;
    }

    /** 有効なタイマーを表せる値かを返す。 */
    constexpr bool Valid() const noexcept { return IsValid(); }

    /**
     * 同じタイマーを表すかを返す。
     * @param other 比較するハンドル。
     */
    constexpr bool operator==(const FTimerHandle& other) const noexcept {
        return id == other.id && generation == other.generation;
    }

    /**
     * 異なるタイマーを表すかを返す。
     * @param other 比較するハンドル。
     */
    constexpr bool operator!=(const FTimerHandle& other) const noexcept {
        return !(*this == other);
    }
};

/** FTimerHandle の 32bit 識別番号と世代番号の物理配置契約。 */
template<>
struct TGenerationHandleLayoutTraits<FTimerHandle> {
    /** 物理配置情報を利用できる。 */
    static constexpr bool kAvailable = true;
    /** 識別番号の byte 位置。 */
    static constexpr usize kIdentityOffset = offsetof(FTimerHandle, id);
    /** 世代番号の byte 位置。 */
    static constexpr usize kGenerationOffset = offsetof(FTimerHandle, generation);
    /** 識別番号の byte 幅。 */
    static constexpr usize kIdentityBytes = sizeof(FTimerHandle::id);
    /** 世代番号の byte 幅。 */
    static constexpr usize kGenerationBytes = sizeof(FTimerHandle::generation);
    /** 識別番号より前に固有領域を持たない。 */
    static constexpr usize kDomainPrefixBytes = 0u;
    /** ハンドル全体の byte 幅。 */
    static constexpr usize kStorageBytes = sizeof(FTimerHandle);
    /** ハンドル全体の alignment。 */
    static constexpr usize kStorageAlignment = alignof(FTimerHandle);
};

/** タイマーを指さない無効なハンドル。 */
inline constexpr FTimerHandle kInvalidTimer{};

} // namespace acs
