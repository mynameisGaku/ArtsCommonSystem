// SPDX-License-Identifier: Apache-2.0
// 型消去されたコンポーネント情報レジストリ
//
// ECS が任意のコンポーネント型を扱えるようにするため、サイズ・整列・
// コンストラクタ・デストラクタなどを実行時に問い合わせ可能な形で保存する。
#pragma once

#include "foundation/Types.h"
#include "ecs/ComponentId.h"

namespace acs {

// 型ごとの操作（型消去で実行時に呼び出せるようにする関数ポインタ群）
struct ComponentOps {
    usize size      = 0;
    usize alignment = 0;
    void  (*destroy)(void* p) noexcept     = nullptr;  // ~T()
    void  (*move)(void* dst, void* src) noexcept = nullptr;  // T(move(*src))
    const char* name = "Unknown";
};

class FComponentRegistry {
public:
    // 型 T を登録（初回のみ実体登録、以降は同じ Ops を返す）
    template<typename T>
    static const ComponentOps& Register() noexcept {
        ComponentTypeId id = GetComponentTypeId<T>();
        ComponentOps& slot = Slots()[id];
        if (slot.size == 0) {
            // T を破棄する関数ポインタ
            slot.destroy = [](void* p) noexcept {
                if constexpr (!__is_trivially_destructible(T)) static_cast<T*>(p)->~T();
            };
            // T をムーブ構築する関数ポインタ
            slot.move = [](void* dst, void* src) noexcept {
                ::new (dst) T(static_cast<T&&>(*static_cast<T*>(src)));
            };
            slot.size      = sizeof(T);
            slot.alignment = alignof(T);
            slot.name      = "T";  // typeid を使えないため固定（必要なら明示登録）
        }
        return slot;
    }

    static const ComponentOps& Get(ComponentTypeId id) noexcept {
        return Slots()[id];
    }

private:
    static ComponentOps* Slots() noexcept;
};

} // namespace acs
