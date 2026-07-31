// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/GenerationHandleLayoutTraits.h"
#include "foundation/Types.h"

#include <cstddef>

namespace acs {

/** ログ購読枠と再利用世代を識別するハンドル。 */
struct FLogSinkHandle {
    /** 固定購読表内の枠番号。 */
    u32 slot = 0xFFFFFFFFu;

    /** 再利用された枠を見分ける世代番号。 */
    u32 generation = 0;

    /** 枠番号と世代番号が有効範囲内かを返す。 */
    constexpr bool IsValid() const noexcept { return slot < 4096u && generation != 0u; }

    /**
     * 同じ購読を表すかを返す。
     * @param other 比較するハンドル。
     * @return 枠番号と世代番号が一致する場合は true。
     */
    constexpr bool operator==(const FLogSinkHandle& other) const noexcept {
        return slot == other.slot && generation == other.generation;
    }
};

/** FLogSinkHandle の世代付き物理配置契約。 */
template<>
struct TGenerationHandleLayoutTraits<FLogSinkHandle> {
    /** 物理配置情報を利用できる。 */
    static constexpr bool kAvailable = true;

    /** 購読枠番号の byte 位置。 */
    static constexpr usize kIdentityOffset = offsetof(FLogSinkHandle, slot);

    /** 世代番号の byte 位置。 */
    static constexpr usize kGenerationOffset = offsetof(FLogSinkHandle, generation);

    /** 購読枠番号の byte 幅。 */
    static constexpr usize kIdentityBytes = sizeof(FLogSinkHandle::slot);

    /** 世代番号の byte 幅。 */
    static constexpr usize kGenerationBytes = sizeof(FLogSinkHandle::generation);

    /** 購読枠番号より前にある領域の byte 幅。 */
    static constexpr usize kDomainPrefixBytes = 0u;

    /** ハンドル全体の byte 幅。 */
    static constexpr usize kStorageBytes = sizeof(FLogSinkHandle);

    /** ハンドル全体の alignment。 */
    static constexpr usize kStorageAlignment = alignof(FLogSinkHandle);
};

/** ログ購読を指さない無効なハンドル。 */
inline constexpr FLogSinkHandle kInvalidLogSink{};

} // namespace acs
