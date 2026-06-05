// SPDX-License-Identifier: Apache-2.0
// 型消去されたコンポーネント情報レジストリ
//
// ECS が任意のコンポーネント型を扱えるようにするため、サイズ・整列・
// コンストラクタ・デストラクタなどを実行時に問い合わせ可能な形で保存する。
#pragma once

#include "foundation/Types.h"
#include "ecs/ComponentId.h"

namespace acs {

/**
 * 型消去されたコンポーネント型ごとの操作 (サイズ・整列・破棄・ムーブ)。
 *
 * @details
 * ECS が任意のコンポーネント型を型を知らずに扱えるよう、破棄・ムーブ構築を
 * 関数ポインタ化して保持する。size==0 のスロットは未登録を表す。
 */
struct ComponentOps {
    /** コンポーネント 1 個分のバイトサイズ (0 なら未登録)。 */
    usize size      = 0;

    /** コンポーネント型の要求アラインメント (バイト)。 */
    usize alignment = 0;

    /** *p を T として破棄する関数ポインタ (~T() 相当、trivial なら何もしない)。 */
    void  (*destroy)(void* p) noexcept     = nullptr;

    /** *src を *dst へムーブ構築する関数ポインタ (T(move(*src)) 相当)。 */
    void  (*move)(void* dst, void* src) noexcept = nullptr;

    /** 型名 (typeid 非依存のため既定は固定文字列)。 */
    const char* name = "Unknown";
};

/**
 * コンポーネント型ごとの ComponentOps を保持・参照する型消去レジストリ。
 *
 * @details
 * ComponentTypeId をキーに、サイズ・整列・破棄・ムーブを実行時に問い合わせ可能な
 * 形で保存する。Slots() の固定配列を共有する純粋な静的ユーティリティ。
 */
class FComponentRegistry {
public:
    /**
     * 型 T を登録し、その ComponentOps を返す (初回のみ実体登録、以降は既存を返す)。
     *
     * @details
     * 初回は破棄・ムーブの関数ポインタ、sizeof/alignof を埋める。trivial 破棄なら
     * destroy は何もしない。typeid を使えないため name は固定文字列のまま。
     * @tparam T 登録するコンポーネント型。
     * @return 型 T に対応する ComponentOps への const 参照。
     */
    template<typename T>
    static const ComponentOps& Register() noexcept {
        const ComponentTypeId id = GetComponentTypeId<T>();
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

    /**
     * 登録済みの ComponentTypeId に対応する ComponentOps を返す。
     *
     * @param id 取得対象のコンポーネント型 ID。
     * @return 対応する ComponentOps への const 参照 (未登録なら size==0 の既定値)。
     */
    static const ComponentOps& Get(ComponentTypeId id) noexcept {
        return Slots()[id];
    }

private:
    /**
     * 全コンポーネント型ぶんの ComponentOps を保持する固定配列を返す。
     *
     * @return kMaxComponentTypes 個の ComponentOps 配列先頭。
     */
    static ComponentOps* Slots() noexcept;
};

} // namespace acs
