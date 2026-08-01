// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — ComponentFactory 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/ComponentFactory.h"
#include "gameframework/AComponent.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectCatalog.h"   // AcsRegisterEngineTypes
#include "memory/Allocator.h"               // DefaultAllocator

namespace acs::game {

TUniquePtr<AComponent> CreateComponentByName(const char* name) noexcept {
    if (name == nullptr) return TUniquePtr<AComponent>();
    AcsRegisterEngineTypes();   // カタログ登録を確定 (冪等)

    CTypeRegistry& reg = CTypeRegistry::Get();
    const FTypeDesc* d = reg.FindByName(name);
    if (d == nullptr || d->category != ETypeCategory::Component)
        return TUniquePtr<AComponent>();                 // 未登録 / 非 Component

    void* obj = reg.Create(name);   // エンジンアロケータで確保 (AcsConstruct)
    if (obj == nullptr) return TUniquePtr<AComponent>(); // Abstract / factory なし

    // AComponent は単一継承の唯一基底なので base は offset 0 → void*==AComponent*。
    // TUniquePtr は DefaultAllocator で破棄する (AcsConstruct と同一アロケータ = 整合)。
    return TUniquePtr<AComponent>(static_cast<AComponent*>(obj), &DefaultAllocator());
}

} // namespace acs::game
