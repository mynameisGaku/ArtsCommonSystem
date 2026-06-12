// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — ComponentFactory 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/ComponentFactory.h"
#include "gameframework/Component2D.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectCatalog.h"   // AcsRegisterEngineTypes
#include "memory/Allocator.h"               // DefaultAllocator

namespace acs::game {

TUniquePtr<FComponent2D> CreateComponentByName(const char* name) noexcept {
    if (name == nullptr) return TUniquePtr<FComponent2D>();
    AcsRegisterEngineTypes();   // カタログ登録を確定 (冪等)

    FTypeRegistry& reg = FTypeRegistry::Get();
    const FTypeDesc* d = reg.FindByName(name);
    if (d == nullptr || d->category != ETypeCategory::Component)
        return TUniquePtr<FComponent2D>();                 // 未登録 / 非 Component

    void* obj = reg.Create(name);   // エンジンアロケータで確保 (AcsConstruct)
    if (obj == nullptr) return TUniquePtr<FComponent2D>(); // Abstract / factory なし

    // FComponent2D は単一継承の唯一基底なので base は offset 0 → void*==FComponent2D*。
    // TUniquePtr は DefaultAllocator で破棄する (AcsConstruct と同一アロケータ = 整合)。
    return TUniquePtr<FComponent2D>(static_cast<FComponent2D*>(obj), &DefaultAllocator());
}

} // namespace acs::game
