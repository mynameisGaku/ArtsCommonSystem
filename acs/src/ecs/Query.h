// World.Query<Pos, Vel>().Each(...) を提供するクエリヘルパ
//
// 仕組み: 指定された全コンポーネントを持つエンティティを選び出して、
//         それぞれにラムダを適用する。
//
// 内部最適化: 一番要素数の少ない SparseSet を「主軸」にし、その dense 走査の
//             各エンティティが他の SparseSet にも含まれているか確認する。
#pragma once

#include "foundation/Types.h"
#include "ecs/World.h"

namespace acs {

template<typename... Comps>
class QueryView {
public:
    explicit QueryView(World& w) noexcept : _world(w) {}

    // 各エンティティに対してラムダを呼ぶ
    // ラムダは (EntityId, Comps&...) を受け取る
    template<typename Fn>
    void Each(Fn fn) noexcept {
        // 全コンポーネントの SparseSet を取得（どれか欠けていればクエリ結果は空）
        SparseSetBase* sets[sizeof...(Comps)] = { static_cast<SparseSetBase*>(_world.template TryGetSet<Comps>())... };
        for (usize i = 0; i < sizeof...(Comps); ++i) {
            if (!sets[i]) return;
        }

        // 主軸 = 最小サイズの SparseSet
        // 効果: 反復回数を最小限にし、その他のセットへの含有確認だけ追加コスト
        SparseSetBase* primary = sets[0];
        for (usize i = 1; i < sizeof...(Comps); ++i) {
            if (sets[i]->Size() < primary->Size()) primary = sets[i];
        }
        if (primary->Size() == 0) return;

        // 主軸の dense 配列を線形走査
        const u32* dense = primary->DenseEntities();
        usize count = primary->Size();
        for (usize i = 0; i < count; ++i) {
            u32 idx = dense[i];
            // 全コンポーネントが揃っていれば fn を呼ぶ
            // 注: Get<T>(EntityId) は世代チェックを含むため、generation を取り出す
            EntityId e = _world.MakeIdFromIndex(idx);
            if (AllPresent(e)) {
                InvokeWith(fn, e);
            }
        }
    }

private:
    // 指定エンティティが全コンポーネントを持っているか
    bool AllPresent(EntityId e) noexcept {
        bool all = true;
        ((all = all && (_world.template Get<Comps>(e) != nullptr)), ...);
        return all;
    }

    // 全コンポーネントを取り出して fn を呼ぶ（呼び出し前に AllPresent で確認済み）
    template<typename Fn>
    void InvokeWith(Fn fn, EntityId e) noexcept {
        fn(e, *_world.template Get<Comps>(e)...);
    }

    World& _world;
};

// World::Query<...>() の実装本体（World.h で前方宣言）
template<typename... Comps>
auto World::Query() noexcept {
    return QueryView<Comps...>(*this);
}

} // namespace acs
