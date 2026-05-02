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

class World {
public:
    World() noexcept;
    ~World() noexcept;

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // エンティティ生成（フリースロット再利用、世代をインクリメント）
    EntityId Create() noexcept;

    // エンティティ削除（全コンポーネント削除 + フリースロットに戻す）
    void Destroy(EntityId e) noexcept;

    // ID が現在も有効か（世代チェック）
    bool IsAlive(EntityId e) const noexcept;

    // コンポーネント追加（既に存在すれば上書き）
    template<typename T>
    void Add(EntityId e, T value) noexcept {
        if (!IsAlive(e)) return;
        SparseSet<T>& set = GetOrCreateSet<T>();
        set.Add(e.index, Move(value));
    }

    // コンポーネント取得（未登録なら nullptr）
    template<typename T>
    T* Get(EntityId e) noexcept {
        if (!IsAlive(e)) return nullptr;
        SparseSet<T>* set = TryGetSet<T>();
        return set ? set->Get(e.index) : nullptr;
    }

    template<typename T>
    const T* Get(EntityId e) const noexcept {
        if (!IsAlive(e)) return nullptr;
        const SparseSet<T>* set = const_cast<World*>(this)->TryGetSet<T>();
        return set ? set->Get(e.index) : nullptr;
    }

    // コンポーネント所持判定
    template<typename T>
    bool Has(EntityId e) const noexcept { return Get<T>(e) != nullptr; }

    // コンポーネント削除（無くても安全）
    template<typename T>
    void Remove(EntityId e) noexcept {
        if (!IsAlive(e)) return;
        SparseSet<T>* set = TryGetSet<T>();
        if (set) set->Remove(e.index);
    }

    // クエリオブジェクト取得（複数コンポーネント反復用）
    template<typename... Comps>
    auto Query() noexcept;  // Query.h で定義

    // 内部使用: 型 T の SparseSet を取得（無ければ作る）
    template<typename T>
    SparseSet<T>& GetOrCreateSet() noexcept {
        ComponentTypeId id = GetComponentTypeId<T>();
        if (id >= _sets.Size()) _sets.Resize(id + 1);
        if (!_sets[id]) {
            _sets[id] = static_cast<SparseSetBase*>(::new SparseSet<T>());
        }
        return *static_cast<SparseSet<T>*>(_sets[id]);
    }

    // 内部使用: 型 T の SparseSet を取得（無ければ nullptr）
    template<typename T>
    SparseSet<T>* TryGetSet() noexcept {
        ComponentTypeId id = GetComponentTypeId<T>();
        if (id >= _sets.Size()) return nullptr;
        return static_cast<SparseSet<T>*>(_sets[id]);
    }

    // 全エンティティ数（生存数）
    u32 EntityCount() const noexcept { return _alive_count; }

    // index 単独から EntityId を復元（Query 内部で使う、世代を付加）
    EntityId MakeIdFromIndex(u32 index) const noexcept {
        if (index >= _slots.Size()) return kInvalidEntity;
        return EntityId{ index, _slots[index].generation };
    }

private:
    // 1 エンティティスロット（世代と「生きているか」を保持）
    struct Slot {
        u32  generation = 0;
        bool alive      = false;
    };

    Array<Slot>           _slots;          // index -> Slot
    Array<u32>            _free_indices;   // 解放済みでまだ再利用できるスロット
    Array<SparseSetBase*> _sets;           // ComponentTypeId -> SparseSet*
    u32                   _alive_count = 0;
};

} // namespace acs
