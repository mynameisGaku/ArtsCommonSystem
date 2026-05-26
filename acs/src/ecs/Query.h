// SPDX-License-Identifier: Apache-2.0
// FWorld.Query<Pos, Vel>().Each(...) を提供するクエリヘルパ
//
// 仕組み: 指定された全コンポーネントを持つエンティティを選び出して、
//         それぞれにラムダを適用する。
//
// 内部最適化: 一番要素数の少ない FSparseSet を「主軸」にし、その dense 走査の
//             各エンティティが他の FSparseSet にも含まれているか確認する。
//
// 並列バージョン: EachParallel(fn, grain) は FThreadPool::ParallelFor で
//                 chunk 分割して投入。ラムダはエンティティごとに独立に
//                 呼ばれることを前提にする (= 共有資源を触るならユーザー側で同期)。
#pragma once

#include "foundation/Types.h"
#include "ecs/World.h"
#include "threading/ThreadPool.h"

namespace acs {

template<typename... Comps>
class FQueryView {
public:
    explicit FQueryView(FWorld& w) noexcept : _world(w) {}

    // 各エンティティに対してラムダを呼ぶ
    // ラムダは (FEntityId, Comps&...) を受け取る
    template<typename Fn>
    void Each(Fn fn) noexcept {
        FSparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        if (primary->Size() == 0) return;

        const u32* dense = primary->DenseEntities();
        usize count = primary->Size();
        for (usize i = 0; i < count; ++i) {
            u32 idx = dense[i];
            FEntityId e = _world.MakeIdFromIndex(idx);
            if (AllPresent(e)) {
                InvokeWith(fn, e);
            }
        }
    }

    // 並列イテレーション: FThreadPool::ParallelFor で chunk 分割して fn を呼ぶ。
    // ラムダはエンティティ単位で独立に呼ばれる前提（同じ entity が複数スレッドから
    // 同時に呼ばれることはないが、グローバル資源は別途同期が必要）。
    //
    // grain = 1 chunk あたりの最小エンティティ数。小さすぎるとオーバーヘッドが
    //         相対的に大きくなる。1024 前後が経験的に良いバランス。
    template<typename Fn>
    void EachParallel(Fn fn, u32 grain = 1024) noexcept {
        FSparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        const u32 count = static_cast<u32>(primary->Size());
        if (count == 0) return;

        // FThreadPool::ParallelFor は stateless 関数ポインタしか受け取れないので
        // ctx を user data に乗せ、thunk で FQueryView の処理に戻す。
        struct FCtx {
            FQueryView*  self;
            Fn*         fn;
            const u32*  dense;
        };
        FCtx ctx{ this, &fn, primary->DenseEntities() };

        auto thunk = [](u32 i, u32 /*worker*/, void* user) {
            auto* c = static_cast<FCtx*>(user);
            u32 idx = c->dense[i];
            FEntityId e = c->self->_world.MakeIdFromIndex(idx);
            if (c->self->AllPresent(e)) {
                c->self->InvokeWith(*c->fn, e);
            }
        };

        // ParallelFor は完了まで block する (内部で Wait)。
        FThreadPool::ParallelFor(0, count, grain, +thunk, &ctx);
    }

private:
    // 全コンポーネントの FSparseSet を取得し、主軸 (最小サイズ) を選ぶ。
    // どれか 1 つでも欠けていれば false を返す。
    bool ResolvePrimary(FSparseSetBase*& out_primary) noexcept {
        FSparseSetBase* sets[sizeof...(Comps)] = {
            static_cast<FSparseSetBase*>(_world.template TryGetSet<Comps>())... };
        for (usize i = 0; i < sizeof...(Comps); ++i) {
            if (!sets[i]) return false;
        }
        FSparseSetBase* primary = sets[0];
        for (usize i = 1; i < sizeof...(Comps); ++i) {
            if (sets[i]->Size() < primary->Size()) primary = sets[i];
        }
        out_primary = primary;
        return true;
    }

    // 指定エンティティが全コンポーネントを持っているか
    bool AllPresent(FEntityId e) noexcept {
        bool all = true;
        ((all = all && (_world.template Get<Comps>(e) != nullptr)), ...);
        return all;
    }

    // 全コンポーネントを取り出して fn を呼ぶ（呼び出し前に AllPresent で確認済み）
    template<typename Fn>
    void InvokeWith(Fn fn, FEntityId e) noexcept {
        fn(e, *_world.template Get<Comps>(e)...);
    }

    FWorld& _world;
};

// FWorld::Query<...>() の実装本体（World.h で前方宣言）
template<typename... Comps>
auto FWorld::Query() noexcept {
    return FQueryView<Comps...>(*this);
}

} // namespace acs
