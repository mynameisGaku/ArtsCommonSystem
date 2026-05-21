// SPDX-License-Identifier: Apache-2.0
// コンポーネント型 ID の取得（型ごとに一意な番号を割り当てる）
//
// 仕組み: GetComponentTypeId<T>() を呼ぶと、初回呼び出し時に新しい u32 を割り当てる。
//         以降は同じ値を返すので、コンポーネント T → ストレージ配列のインデックス
//         として使える。
#pragma once

#include "foundation/Types.h"
#include "threading/Atomic.h"

namespace acs {

// コンポーネント型 ID（0..kMaxComponentTypes-1）
using ComponentTypeId = u32;
inline constexpr ComponentTypeId kMaxComponentTypes = 256;

namespace ecs_detail {
// 全 T 共通のカウンタ。型ごとに割り当てた最後の ID +1 を保持。
inline Atomic<u32> g_next_component_type_id{0};
} // namespace ecs_detail

// 型 T に固有な ComponentTypeId を返す（初回呼び出しで割り当て、以降キャッシュ）
template<typename T>
ComponentTypeId GetComponentTypeId() noexcept {
    // 関数静的変数で型ごとに 1 度だけ割り当てる（C++ の magic statics でスレッドセーフ）
    static const ComponentTypeId id = ecs_detail::g_next_component_type_id.FetchAdd(1);
    return id;
}

} // namespace acs
