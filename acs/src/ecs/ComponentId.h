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

/** コンポーネント型 ID (0..kMaxComponentTypes-1、ストレージ配列の添字に使う)。 */
using ComponentTypeId = u32;

/** 同時に扱えるコンポーネント型の上限 (Slots 配列の長さ)。 */
inline constexpr ComponentTypeId kMaxComponentTypes = 256;

namespace ecs_detail {
/** 全 T 共通の採番カウンタ (次に割り当てる ID を保持)。 */
inline TAtomic<u32> g_next_component_type_id{0};
} // namespace ecs_detail

/**
 * 型 T に固有な ComponentTypeId を返す (初回呼び出しで採番、以降はキャッシュ)。
 *
 * @details
 * 関数静的変数で型ごとに 1 度だけ割り当てる (C++ の magic statics によりスレッド
 * セーフ)。採番自体は TAtomic の FetchAdd なので、複数スレッドから同じ型 T を初めて
 * 呼んでも一意な値に確定する。割り当ては呼び出し順依存のため、決定的な値は保証しない。
 * @tparam T ID を割り当てるコンポーネント型。
 * @return 型 T に対応する ComponentTypeId。
 */
template<typename T>
ComponentTypeId GetComponentTypeId() noexcept {
    static const ComponentTypeId id = ecs_detail::g_next_component_type_id.FetchAdd(1);
    return id;
}

} // namespace acs
