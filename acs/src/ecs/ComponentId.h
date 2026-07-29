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

/** ビルド内で安定したコンパイル時コンポーネント署名。永続化 ID には使用しない。 */
using ComponentSignatureId = u64;

/** 同時に扱えるコンポーネント型の上限 (Slots 配列の長さ)。 */
inline constexpr ComponentTypeId kMaxComponentTypes = 256;

namespace ecs_detail {
/** 全 T 共通の採番カウンタ (次に割り当てる ID を保持)。 */
inline TAtomic<u32> g_next_component_type_id{0};

constexpr ComponentSignatureId HashComponentSignature(
    const char* text) noexcept
{
    ComponentSignatureId hash = 14695981039346656037ull;
    while (*text != '\0') {
        hash ^= static_cast<u8>(*text++);
        hash *= 1099511628211ull;
    }
    return hash;
}

template<typename T>
constexpr ComponentSignatureId StaticComponentSignature() noexcept
{
#if defined(_MSC_VER)
    return HashComponentSignature(__FUNCSIG__);
#else
    return HashComponentSignature(__PRETTY_FUNCTION__);
#endif
}
} // namespace ecs_detail

template<typename T>
ComponentTypeId GetComponentTypeId() noexcept;

/**
 * コンパイル時クエリ・振り分け用の型特性。
 *
 * @details Signature は型パックの比較・特殊化に使い、World ストレージの密な添字は
 * 従来どおり RuntimeId() の動的代替経路を使う。これによりプラグイン型の後付け互換を保つ。
 */
template<typename T>
struct TComponentTypeTraits {
    static constexpr ComponentSignatureId Signature =
        ecs_detail::StaticComponentSignature<T>();

    static ComponentTypeId RuntimeId() noexcept
    {
        return GetComponentTypeId<T>();
    }
};

template<typename T>
constexpr ComponentSignatureId GetComponentSignatureId() noexcept
{
    return TComponentTypeTraits<T>::Signature;
}

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
