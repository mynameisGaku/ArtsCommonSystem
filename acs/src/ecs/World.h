// SPDX-License-Identifier: Apache-2.0
// ECS の World（エンティティとコンポーネントを管理する中心）
//
// 使い方:
//   World w;
//   EntityId e = w.Create();
//   w.Add<Position>(e, {0, 0, 0});
//   w.Add<Velocity>(e, {1, 0, 0});
//
//   if (Position* p = w.Get<Position>(e)) p->x += 1.0f;
//
//   w.Query<Position, Velocity>().Each([](EntityId id, Position& p, Velocity& v) {
//       p.x += v.x; p.y += v.y; p.z += v.z;
//   });
//
//   w.Remove<Velocity>(e);
//   w.Destroy(e);
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "container/HashMap.h"
#include "ecs/Entity.h"
#include "ecs/ComponentId.h"
#include "ecs/SparseSet.h"

namespace acs {

/**
 * エンティティとコンポーネントを管理する ECS の中心 (sparse-set ストレージ)。
 *
 * @details
 * エンティティは世代付きスロットで管理し、Destroy したスロットは世代を進めて
 * フリーリストに戻して再利用する (古い EntityId は世代不一致で無効化)。コンポーネント
 * 型ごとに SparseSet を 1 つ持ち、ComponentTypeId を添字に引く。Query で複数
 * コンポーネントを横断走査する。non-copy 型。
 */
class World {
public:
    /** 空の World を構築する。 */
    World() noexcept;

    /** World を破棄する (全 SparseSet を解放)。 */
    ~World() noexcept;

    /** コピー禁止 (SparseSet を所有するため)。 */
    World(const World&) = delete;

    /** コピー代入も禁止。 */
    World& operator=(const World&) = delete;

    /**
     * エンティティを生成する。
     *
     * @details フリースロットがあれば再利用し (その際スロット世代は既に進んでいる)、
     * 無ければ新規スロットを確保する。
     * @return 生成したエンティティの EntityId。
     */
    EntityId Create() noexcept;

    /**
     * エンティティを削除する (全コンポーネントを除去し、スロットを再利用待ちへ戻す)。
     *
     * @details スロットの世代を +1 して、削除前に得た EntityId を以後無効化する。
     * 既に無効な ID なら何もしない。
     * @param e 削除するエンティティ。
     */
    void Destroy(EntityId e) noexcept;

    /**
     * 全エンティティとコンポーネントストレージを解放し、空の World に戻す。
     *
     * @details MemorySystem の終了前に、実行中に選ばれた既定アロケータを使う
     * SparseSet を確実に破棄するためにも使用する。繰り返し呼んでも安全。
     */
    void Clear() noexcept;

    /**
     * エンティティが現在も生存しているかを返す (世代チェック)。
     *
     * @param e 判定するエンティティ。
     * @return スロットが生存中かつ世代が一致すれば true。
     */
    bool IsAlive(EntityId e) const noexcept;

    /**
     * エンティティにコンポーネントを追加する (既に存在すれば上書き)。
     *
     * @details e が生存していなければ何もしない。型 T の SparseSet が無ければ生成する。
     * @tparam T 追加するコンポーネント型。
     * @param e 追加先のエンティティ。
     * @param value 格納する値 (ムーブで取り込む)。
     */
    template<typename T>
    void Add(EntityId e, T value) noexcept {
        if (!IsAlive(e)) return;
        SparseSet<T>& set = GetOrCreateSet<T>();
        set.Add(e.index, Move(value));
    }

    /**
     * エンティティの T コンポーネントへの可変ポインタを返す。
     *
     * @details 返したポインタは当該 SparseSet への Add/Remove (再確保・swap-remove) で
     * 無効化され得るため、構造変更をまたいで保持しない。
     * @tparam T 取得するコンポーネント型。
     * @param e 取得対象のエンティティ。
     * @return T へのポインタ (e が無効・型未登録・未保持なら nullptr)。
     */
    template<typename T>
    T* Get(EntityId e) noexcept {
        if (!IsAlive(e)) return nullptr;
        SparseSet<T>* set = TryGetSet<T>();
        return set ? set->Get(e.index) : nullptr;
    }

    /**
     * エンティティの T コンポーネントへの const ポインタを返す。
     *
     * @tparam T 取得するコンポーネント型。
     * @param e 取得対象のエンティティ。
     * @return T への const ポインタ (e が無効・型未登録・未保持なら nullptr)。
     */
    template<typename T>
    const T* Get(EntityId e) const noexcept {
        if (!IsAlive(e)) return nullptr;
        const SparseSet<T>* set = const_cast<World*>(this)->TryGetSet<T>();
        return set ? set->Get(e.index) : nullptr;
    }

    /**
     * エンティティが T コンポーネントを持つかを返す。
     *
     * @tparam T 判定するコンポーネント型。
     * @param e 判定対象のエンティティ。
     * @return 持っていれば true。
     */
    template<typename T>
    bool Has(EntityId e) const noexcept { return Get<T>(e) != nullptr; }

    /**
     * エンティティから T コンポーネントを除去する (無くても安全)。
     *
     * @tparam T 除去するコンポーネント型。
     * @param e 除去対象のエンティティ。
     */
    template<typename T>
    void Remove(EntityId e) noexcept {
        if (!IsAlive(e)) return;
        SparseSet<T>* set = TryGetSet<T>();
        if (set) set->Remove(e.index);
    }

    /**
     * 複数コンポーネントを横断走査するクエリビューを返す。
     *
     * @details 実装は Query.h (QueryView を生成して返す)。
     * @tparam Comps 同時に要求するコンポーネント型。
     * @return QueryView<Comps...>。
     */
    template<typename... Comps>
    auto Query() noexcept;  // Query.h で定義

    /**
     * 型 T の SparseSet を返す (無ければ生成する)。内部用。
     *
     * @tparam T 取得・生成する SparseSet のコンポーネント型。
     * @return 型 T の SparseSet への参照。
     */
    template<typename T>
    SparseSet<T>& GetOrCreateSet() noexcept {
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (id >= m_Sets.Size()) m_Sets.Resize(id + 1);
        if (!m_Sets[id]) {
            m_Sets[id] = static_cast<SparseSetBase*>(::new SparseSet<T>());
        }
        return *static_cast<SparseSet<T>*>(m_Sets[id]);
    }

    /**
     * 型 T の SparseSet を返す (未生成なら nullptr)。内部用。
     *
     * @tparam T 取得する SparseSet のコンポーネント型。
     * @return 型 T の SparseSet へのポインタ (未生成なら nullptr)。
     */
    template<typename T>
    SparseSet<T>* TryGetSet() noexcept {
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (id >= m_Sets.Size()) return nullptr;
        return static_cast<SparseSet<T>*>(m_Sets[id]);
    }

    /**
     * 生存中のエンティティ数を返す。
     *
     * @return 現在生きているエンティティの数。
     */
    u32 EntityCount() const noexcept { return m_AliveCount; }

    /**
     * スロット index に現在の世代を付けて EntityId を復元する (Query 内部用)。
     *
     * @param index 復元するスロット番号。
     * @return そのスロットの現世代を付けた EntityId (範囲外なら kInvalidEntity)。
     */
    EntityId MakeIdFromIndex(u32 index) const noexcept {
        if (index >= m_Slots.Size()) return kInvalidEntity;
        return EntityId{ index, m_Slots[index].generation };
    }

private:
    /**
     * 1 エンティティスロット (世代と生存フラグを保持)。
     */
    struct Slot {
        /** スロットの現世代 (Destroy のたびに +1)。 */
        u32  generation = 0;

        /** スロットが生存中かのフラグ。 */
        bool alive      = false;
    };

    /** スロット配列 (index → Slot)。 */
    TArray<Slot>           m_Slots;

    /** 解放済みで再利用待ちのスロット番号。 */
    TArray<u32>            m_FreeIndices;

    /** コンポーネント型ごとの SparseSet (ComponentTypeId → SparseSet*、所有権を持つ)。 */
    TArray<SparseSetBase*> m_Sets;

    /** 生存中のエンティティ数。 */
    u32                   m_AliveCount = 0;

    /** Clear 前の EntityId が再生成後に一致しないよう、新規スロットへ与える世代。 */
    u32 m_GenerationSeed = 0;
};

} // namespace acs
