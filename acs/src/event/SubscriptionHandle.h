// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "event/EventTypeId.h"
#include "foundation/GenerationHandleLayoutTraits.h"

#include <cstddef>

namespace acs {

/** メッセージ購読を識別するハンドル。 */
struct FSubscriptionHandle {
    /** 購読先の通路番号。 */
    EventTypeId channel = 0xFFFFFFFFu;
    /** 通路内の購読番号。 */
    u32 id = 0;
    /** 再利用された購読枠を見分ける世代番号。 */
    u32 generation = 0;

    /** 通路、番号、世代がすべて設定済みかを返す。 */
    constexpr bool IsValid() const noexcept { return IsValidEventTypeId(channel) && id != 0 && generation != 0; }

    /**
     * 同じ購読を表すかを返す。
     * @param other 比較するハンドル。
     */
    constexpr bool operator==(const FSubscriptionHandle& other) const noexcept {
        return channel == other.channel && id == other.id && generation == other.generation;
    }
};

/** FSubscriptionHandle の通路番号付き物理配置契約。 */
template<>
struct TGenerationHandleLayoutTraits<FSubscriptionHandle> {
    /** 物理配置情報を利用できる。 */
    static constexpr bool kAvailable = true;
    /** 購読番号の byte 位置。 */
    static constexpr usize kIdentityOffset = offsetof(FSubscriptionHandle, id);
    /** 世代番号の byte 位置。 */
    static constexpr usize kGenerationOffset = offsetof(FSubscriptionHandle, generation);
    /** 購読番号の byte 幅。 */
    static constexpr usize kIdentityBytes = sizeof(FSubscriptionHandle::id);
    /** 世代番号の byte 幅。 */
    static constexpr usize kGenerationBytes = sizeof(FSubscriptionHandle::generation);
    /** 購読番号より前にある通路番号の byte 幅。 */
    static constexpr usize kDomainPrefixBytes = offsetof(FSubscriptionHandle, id);
    /** ハンドル全体の byte 幅。 */
    static constexpr usize kStorageBytes = sizeof(FSubscriptionHandle);
    /** ハンドル全体の alignment。 */
    static constexpr usize kStorageAlignment = alignof(FSubscriptionHandle);
};

/** 購読を指さない無効なハンドル。 */
inline constexpr FSubscriptionHandle kInvalidSubscription{};

} // namespace acs
