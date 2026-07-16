// SPDX-License-Identifier: Apache-2.0
// World.Query<Pos, Vel>().Each(...) を提供するクエリヘルパ
//
// 仕組み: 指定された全コンポーネントを持つエンティティを選び出して、
//         それぞれにラムダを適用する。
//
// 内部最適化: 一番要素数の少ない SparseSet を「主軸」にし、その dense 走査の
//             各エンティティが他の SparseSet にも含まれているか確認する。
//
// 並列バージョン: EachParallel(fn, grain) は FThreadPool::ParallelFor で
//                 chunk 分割して投入。ラムダはエンティティごとに独立に
//                 呼ばれることを前提にする (= 共有資源を触るならユーザー側で同期)。
#pragma once

#include "foundation/Types.h"
#include "ecs/World.h"
#include "threading/ThreadPool.h"

namespace acs {

/**
 * 指定した全コンポーネントを持つエンティティを走査するクエリビュー。
 *
 * @details
 * 一番要素数の少ない SparseSet を主軸 (primary) に選び、その dense を走査しつつ
 * 各エンティティが他のコンポーネントも持つかを確認してラムダを適用する。dense は
 * 走査前にローカルへスナップショットするため、ラムダ内での Add/Remove (構造変更) で
 * 反復が無効化されない。World::Query<...>() から生成する。
 * @tparam Comps 同時に要求するコンポーネント型 (1 つ以上)。
 */
template<typename... Comps>
class QueryView {
public:
    /**
     * 走査対象の World を束ねてビューを構築する。
     *
     * @param w 走査対象の World (参照を保持)。
     */
    explicit QueryView(World& w) noexcept : m_World(w)
    {
    }

    /**
     * 全 Comps を持つ各エンティティにラムダを呼ぶ (逐次)。
     *
     * @details
     * primary の dense をローカルへスナップショットしてから反復するため、fn 内の
     * Add/Remove/Destroy で **訪問するエンティティ集合** は無効化されない (反復開始時点で固定)。
     * 反復中に追加したエンティティはこの走査では訪問されず、削除したエンティティは InvokeWith の
     * 再 Get + AllPresent 再確認でスキップされる。
     *
     * ただし fn へ渡す Comps& 参照は Get と同じ無効化規約に従う。fn が **同じコンポーネント型** を
     * Add/Remove して SparseSet が再確保すると、その参照は dangling になる。fn 内で構造変更した後は
     * 渡された参照を使わず、必要なら w.Get<T>(e) で取り直すこと (構造変更 → 渡し済み参照の再利用は
     * use-after-free)。反復中に Add/Remove/Destroy したい場合は `FEntityCommandBuffer`
     * (ecs/EntityCommandBuffer.h) へ記録し、Each 後に Flush() するのが安全で推奨。
     * fn は (EntityId, Comps&...) を受け取る。
     * @tparam Fn (EntityId, Comps&...) を受け取る呼び出し可能型。
     * @param fn 各エンティティに適用するラムダ (値で受け取る)。
     */
    template<typename Fn>
    void Each(Fn fn) noexcept
    {
        SparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        const usize count = primary->Size();
        if (count == 0) return;

        // dense[] をローカルへスナップショットしてから反復する。fn が当該コンポーネントを
        // Add/Remove して SparseSet が m_Dense を再確保すると、生 dense ポインタが dangling
        // になり use-after-free する。コピーに対して反復することで構造的変更に安全化する。
        TArray<u32> snapshot;
        snapshot.Resize(count);
        const u32* dense = primary->DenseEntities();
        for (usize i = 0; i < count; ++i)
            snapshot[i] = dense[i];

        for (usize i = 0; i < count; ++i) {
            const EntityId e = m_World.MakeIdFromIndex(snapshot[i]);
            if (AllPresent(e)) {
                InvokeWith(fn, e);
            }
        }
    }

    /**
     * 全 Comps を持つ各エンティティに fn を並列で呼ぶ。
     *
     * @details
     * primary の dense をスナップショットし、FThreadPool::ParallelFor で chunk 分割して
     * fn を呼ぶ (完了まで block)。同じエンティティが複数スレッドから同時に呼ばれること
     * はないが、グローバル資源を触る場合はユーザー側で同期が必要。
     *
     * **重要**: fn は World を構造変更 (Add/Remove/Destroy) してはならない。World/SparseSet は
     * 並行変更に対してスレッドセーフでなく、他スレッドが使用中の Comps& 参照も dangling する。
     * 構造変更が必要なら逐次 Each を使うか、変更を後段へ遅延すること。
     * @tparam Fn (EntityId, Comps&...) を受け取る呼び出し可能型。
     * @param fn 各エンティティに適用するラムダ。
     * @param grain 1 chunk あたりの最小エンティティ数 (小さすぎると分割オーバーヘッドが
     *              相対的に増える。1024 前後が経験的に良いバランス)。
     */
    template<typename Fn>
    void EachParallel(Fn fn, u32 grain = 1024) noexcept
    {
        SparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        const u32 count = static_cast<u32>(primary->Size());
        if (count == 0) return;

        // dense[] をスナップショットしてから並列反復する。fn が構造的変更を起こすと生 dense
        // が dangling するため、コピーを指す (snapshot は ParallelFor の block 中ずっと生存)。
        TArray<u32> snapshot;
        snapshot.Resize(count);
        {
            const u32* dense = primary->DenseEntities();
            for (u32 i = 0; i < count; ++i)
                snapshot[i] = dense[i];
        }

        // FThreadPool::ParallelFor は stateless 関数ポインタしか受け取れないので
        // ctx を user data に乗せ、thunk で QueryView の処理に戻す。
        struct Ctx {
            QueryView* self;
            Fn* fn;
            const u32* dense;
        };
        Ctx ctx{this, &fn, snapshot.Data()};

        auto thunk = [](u32 i, u32 /*worker*/, void* user) {
            auto* c = static_cast<Ctx*>(user);
            const u32 idx = c->dense[i];
            const EntityId e = c->self->m_World.MakeIdFromIndex(idx);
            if (c->self->AllPresent(e)) {
                c->self->InvokeWith(*c->fn, e);
            }
        };

        // ParallelFor は完了まで block する (内部で Wait)。
        (void)FThreadPool::ParallelFor(0, count, grain, +thunk, &ctx);
    }

private:
    /**
     * 全 Comps の SparseSet を取得し、最小サイズのものを主軸として選ぶ。
     *
     * @details どれか 1 つでも未登録 (= 該当コンポーネントを持つエンティティが皆無) の
     * 場合は走査不要なので false を返す。
     * @param out_primary 選んだ主軸 SparseSet を書き戻す出力先。
     * @return 全コンポーネントが揃って主軸を選べたら true。
     */
    bool ResolvePrimary(SparseSetBase*& out_primary) noexcept
    {
        SparseSetBase* sets[sizeof...(Comps)] = {static_cast<SparseSetBase*>(m_World.template TryGetSet<Comps>())...};
        for (usize i = 0; i < sizeof...(Comps); ++i) {
            if (!sets[i]) return false;
        }
        SparseSetBase* primary = sets[0];
        for (usize i = 1; i < sizeof...(Comps); ++i) {
            if (sets[i]->Size() < primary->Size()) primary = sets[i];
        }
        out_primary = primary;
        return true;
    }

    /**
     * 指定エンティティが Comps を全て持っているかを返す。
     *
     * @param e 判定するエンティティ。
     * @return 全コンポーネントを持っていれば true。
     */
    bool AllPresent(EntityId e) noexcept
    {
        bool all = true;
        ((all = all && (m_World.template Get<Comps>(e) != nullptr)), ...);
        return all;
    }

    /**
     * 全コンポーネントを取り出して fn を呼ぶ (呼び出し前に AllPresent で確認済み)。
     *
     * @tparam Fn (EntityId, Comps&...) を受け取る呼び出し可能型。
     * @param fn 適用するラムダ。
     * @param e 対象エンティティ。
     */
    template<typename Fn>
    void InvokeWith(Fn fn, EntityId e) noexcept
    {
        fn(e, *m_World.template Get<Comps>(e)...);
    }

    /** 走査対象の World。 */
    World& m_World;
};

/**
 * World::Query<...>() の実装本体 (World.h で宣言、QueryView を生成して返す)。
 *
 * @tparam Comps 同時に要求するコンポーネント型。
 * @return この World を束ねた QueryView<Comps...>。
 */
template<typename... Comps>
auto World::Query() noexcept
{
    return QueryView<Comps...>(*this);
}

} // namespace acs
