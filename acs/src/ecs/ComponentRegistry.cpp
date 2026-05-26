// SPDX-License-Identifier: Apache-2.0
// コンポーネントレジストリ実装
#include "ecs/ComponentRegistry.h"

namespace acs {

FComponentOps* FComponentRegistry::Slots() noexcept {
    // 全コンポーネント型ぶんの Ops を保持する固定配列
    static FComponentOps slots[kMaxComponentTypes] {};
    return slots;
}

} // namespace acs
