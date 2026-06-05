// SPDX-License-Identifier: Apache-2.0
// コンポーネントレジストリ実装
#include "ecs/ComponentRegistry.h"

namespace acs {

/**
 * 全コンポーネント型ぶんの ComponentOps を保持する関数静的配列を返す。
 *
 * @return kMaxComponentTypes 個の ComponentOps 配列先頭。
 */
ComponentOps* FComponentRegistry::Slots() noexcept {
    static ComponentOps slots[kMaxComponentTypes] {};
    return slots;
}

} // namespace acs
