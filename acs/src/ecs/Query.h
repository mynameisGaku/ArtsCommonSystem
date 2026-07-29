// SPDX-License-Identifier: Apache-2.0
#pragma once
// FWorld::Query<FPos, FVel>().Each(...) を提供するクエリヘルパ
//
// 仕組み: 指定された全コンポーネントを持つエンティティを選び出して、
//         それぞれにラムダを適用する。
//
// 内部最適化: 一番要素数の少ない TSparseSet を「主軸」にし、その dense 走査の
//             各エンティティが他の TSparseSet にも含まれているか確認する。
//
// 並列バージョン: EachParallel(fn, grain) は FThreadPool::ParallelFor で
//                 chunk 分割して投入。ラムダはエンティティごとに独立に
//                 呼ばれることを前提にする (= 共有資源を触るならユーザー側で同期)。

#include "foundation/Types.h"
#include "ecs/World.h"
#include "threading/ThreadPool.h"

namespace acs {

/**
 * 指定した全コンポーネントを持つエンティティを走査するクエリビュー。
 *
 * @details
 * 一番要素数の少ない TSparseSet を主軸 (primary) に選び、その dense を走査しつつ
 * 各エンティティが他のコンポーネントも持つかを確認してラムダを適用する。dense は
 * 走査前にローカルへスナップショットするため、ラムダ内での Add/Remove (構造変更) で
 * 反復が無効化されない。FWorld::Query<...>() から生成する。
 * @tparam Comps 同時に要求するコンポーネント型 (1 つ以上)。
 */
template<typename... Comps>
class TQueryView {
public:
    /**
     * 走査対象の FWorld を束ねてビューを構築する。
     *
     * @param w 走査対象の FWorld (参照を保持)。
     */
    explicit TQueryView(FWorld& w) noexcept : m_World(w)
    {
    }

    /**
     * 全 Comps を持つ各エンティティにラムダを呼ぶ (逐次)。
     *
     * @details
     * primary の dense をローカルへスナップショットしてから反復するため、fn 内の
     * Add/Remove/Destroy で **訪問するエンティティ集合** は無効化されない (反復開始時点で固定)。
     * 反復中に追加したエンティティはこの走査では訪問されず、削除したエンティティは
     * 世代付きスナップショットと 1 回の必須コンポーネント解決でスキップされる。
     *
     * ただし fn へ渡す Comps& 参照は Get と同じ無効化規約に従う。fn が **同じコンポーネント型** を
     * Add/Remove して TSparseSet が再確保すると、その参照は dangling になる。fn 内で構造変更した後は
     * 渡された参照を使わず、必要なら w.Get<T>(e) で取り直すこと (構造変更 → 渡し済み参照の再利用は
     * use-after-free)。反復中に Add/Remove/Destroy したい場合は `FEntityCommandBuffer`
     * FEntityCommandBuffer (ecs/EntityCommandBuffer.h) へ記録し、Each 後に Flush() するのが安全で推奨。
     * fn は (FEntityId, Comps&...) を受け取る。
     * @tparam Fn (FEntityId, Comps&...) を受け取る呼び出し可能型。
     * @param fn 各エンティティに適用するラムダ (値で受け取る)。
     */
    template<typename Fn>
    void Each(Fn fn) noexcept
    {
        FSparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        const usize count = primary->Size();
        if (count == 0) return;

        // dense[] をローカルへスナップショットしてから反復する。fn が当該コンポーネントを
        // Add/Remove して TSparseSet が m_Dense を再確保すると、生 dense ポインタが dangling
        // になり use-after-free する。コピーに対して反復することで構造的変更に安全化する。
        TArray<FEntityId> snapshot;
        snapshot.Resize(count);
        const u32* dense = primary->DenseEntities();
        for (usize i = 0; i < count; ++i)
            snapshot[i] = m_World.MakeIdFromIndex(dense[i]);
        FSparseSetBase* required_sets[] = {static_cast<FSparseSetBase*>(m_World.template TryGetSet<Comps>())...};

        for (usize i = 0; i < count; ++i) {
            PrefetchRequiredSets(snapshot, i, count, required_sets, sizeof(required_sets) / sizeof(required_sets[0]));
            InvokeIfPresent(fn, snapshot[i]);
        }
    }

    /**
     * Comps を全て持ち、かつ Excludes を 1 つも持たない各エンティティに fn を呼ぶ (逐次)。
     *
     * @details Each と同じく primary の dense をスナップショットしてから反復するため構造変更に
     * 対して安全。Excludes は 1 つでも持てば除外する (例: 生存かつ «凍結でない» を走査)。fn は
     * (FEntityId, Comps&...) を受け取る (Excludes は参照として渡さない)。反復中の構造変更は
     * Each と同じ制約 (FEntityCommandBuffer 推奨)。
     * @tparam Excludes 除外するコンポーネント型 (0 個以上。空なら Each と同じ)。
     * @tparam Fn (FEntityId, Comps&...) を受け取る呼び出し可能型。
     * @param fn 各エンティティに適用するラムダ (値で受け取る)。
     */
    template<typename... Excludes, typename Fn>
    void EachExcluding(Fn fn) noexcept
    {
        FSparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        const usize count = primary->Size();
        if (count == 0) return;

        TArray<FEntityId> snapshot;
        snapshot.Resize(count);
        const u32* dense = primary->DenseEntities();
        for (usize i = 0; i < count; ++i)
            snapshot[i] = m_World.MakeIdFromIndex(dense[i]);

        for (usize i = 0; i < count; ++i) {
            InvokeIfPresentExcluding<Excludes...>(fn, snapshot[i]);
        }
    }

    /**
     * 必須 Comps を全て持つエンティティを走査し、Optional は null 許容ポインターで渡す。
     *
     * @details 必須・任意の両型パックはコンパイル時に特殊化される。エンティティごとの
     * 型種別分岐はなく、各スパース検索は 1 型 1 回だけ行う。
     */
    template<typename... Optional, typename Fn>
    void EachOptional(Fn fn) noexcept
    {
        FSparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        const usize count = primary->Size();
        if (count == 0u) return;

        TArray<FEntityId> snapshot;
        if (!snapshot.TryResize(count)) return;
        const u32* dense = primary->DenseEntities();
        for (usize i = 0; i < count; ++i)
            snapshot[i] = m_World.MakeIdFromIndex(dense[i]);

        for (usize i = 0; i < count; ++i) {
            InvokeIfPresentOptional<Optional...>(fn, snapshot[i]);
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
     * **重要**: fn は FWorld を構造変更 (Add/Remove/Destroy) してはならない。FWorld/TSparseSet は
     * 並行変更に対してスレッドセーフでなく、他スレッドが使用中の Comps& 参照も dangling する。
     * 構造変更が必要なら `FParallelEntityCommandBuffer` (ecs/ParallelEntityCommandBuffer.h)
     * へロックなしで記録し、EachParallel の完了後に Flush() するのが安全で推奨
     * (per-worker バッファなので fn 内から並行に記録できる)。逐次 Each + FEntityCommandBuffer
     * でも良い。
     * @tparam Fn (FEntityId, Comps&...) を受け取る呼び出し可能型。
     * @param fn 各エンティティに適用するラムダ。
     * @param grain 1 chunk あたりの最小エンティティ数 (小さすぎると分割オーバーヘッドが
     *              相対的に増える。1024 前後が経験的に良いバランス)。
     */
    template<typename Fn>
    void EachParallel(Fn fn, u32 grain = 1024) noexcept
    {
        FSparseSetBase* primary = nullptr;
        if (!ResolvePrimary(primary)) return;
        const u32 count = static_cast<u32>(primary->Size());
        if (count == 0) return;

        // dense[] をスナップショットしてから並列反復する。fn が構造的変更を起こすと生 dense
        // が dangling するため、コピーを指す (snapshot は ParallelFor の block 中ずっと生存)。
        TArray<FEntityId> snapshot;
        snapshot.Resize(count);
        {
            const u32* dense = primary->DenseEntities();
            for (u32 i = 0; i < count; ++i)
                snapshot[i] = m_World.MakeIdFromIndex(dense[i]);
        }

        // FThreadPool::ParallelFor は stateless 関数ポインタしか受け取れないので
        // ctx を user data に乗せ、thunk で TQueryView の処理に戻す。
        struct FCtx {
            TQueryView* self;
            Fn* fn;
            const FEntityId* entities;
        };
        FCtx ctx{this, &fn, snapshot.Data()};

        auto thunk = [](u32 i, u32 /*worker*/, void* user) {
            auto* c = static_cast<FCtx*>(user);
            c->self->InvokeIfPresent(*c->fn, c->entities[i]);
        };

        // ParallelFor は完了まで block する (内部で Wait)。
        (void)FThreadPool::ParallelFor(0, count, grain, +thunk, &ctx);
    }

private:
    /**
     * 大規模走査時だけ一定距離先の sparse 対応表を先読みする。
     *
     * @details 128 件未満では分岐直後に戻り、小規模クエリへ追加の集合検索を入れない。
     */
    void PrefetchRequiredSets(const TArray<FEntityId>& snapshot, usize index, usize count, FSparseSetBase* const* sets, usize set_count) noexcept {
        /** 小規模走査では先読み命令の固定費を避ける。 */
        constexpr usize kMinimumCount = 128u;
        /** 疎配列を処理より先にキャッシュへ要求する距離。 */
        constexpr usize kPrefetchDistance = 16u;
        if (count < kMinimumCount || index + kPrefetchDistance >= count) {
            return;
        }
        const FEntityId future = snapshot[index + kPrefetchDistance]; // 先読み対象のエンティティ。
        for (usize i = 0u; i < set_count; ++i) {
            FSparseSetBase* set = sets[i]; // 今回先読みする疎集合。
            if (set != nullptr) set->PrefetchSparse(future.index);
        }
    }

    /**
     * 全 Comps の TSparseSet を取得し、最小サイズのものを主軸として選ぶ。
     *
     * @details どれか 1 つでも未登録 (= 該当コンポーネントを持つエンティティが皆無) の
     * 場合は走査不要なので false を返す。
     * @param out_primary 選んだ主軸 FSparseSetBase を書き戻す出力先。
     * @return 全コンポーネントが揃って主軸を選べたら true。
     */
    bool ResolvePrimary(FSparseSetBase*& out_primary) noexcept
    {
        FSparseSetBase* sets[sizeof...(Comps)] = {static_cast<FSparseSetBase*>(m_World.template TryGetSet<Comps>())...};
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

    /**
     * 指定エンティティが Excludes のいずれかを持っているかを返す。
     *
     * @tparam Excludes 判定するコンポーネント型 (0 個なら常に false)。
     * @param e 判定するエンティティ。
     * @return いずれか 1 つでも持っていれば true。
     */
    template<typename... Excludes>
    bool AnyPresent(FEntityId e) noexcept
    {
        bool any = false;
        ((any = any || (m_World.template Get<Excludes>(e) != nullptr)), ...);
        return any;
    }

    /**
     * 必須コンポーネントのポインターを各型 1 回だけ解決し、全て有効なら呼び出す。
     * 型パックはコンパイル時に特殊化され、エンティティごとの型 ID ループや二重検索を持たない。
     */
    template<typename Fn>
    void InvokeIfPresent(Fn& fn, FEntityId e) noexcept
    {
        InvokeResolved(fn, e, m_World.template Get<Comps>(e)...);
    }

    template<typename Fn>
    static void InvokeResolved(Fn& fn, FEntityId e, Comps*... components) noexcept
    {
        if (((components != nullptr) && ...)) {
            fn(e, *components...);
        }
    }

    /** 必須ポインターを再検索せず、コンパイル時の除外型パックも同じ振り分けに畳む。 */
    template<typename... Excludes, typename Fn>
    void InvokeIfPresentExcluding(Fn& fn, FEntityId e) noexcept
    {
        InvokeResolvedExcluding<Excludes...>(fn, e, m_World.template Get<Comps>(e)...);
    }

    template<typename... Excludes, typename Fn>
    void InvokeResolvedExcluding(Fn& fn, FEntityId e, Comps*... components) noexcept
    {
        if (((components != nullptr) && ...) && !AnyPresent<Excludes...>(e)) {
            fn(e, *components...);
        }
    }

    template<typename... Optional, typename Fn>
    void InvokeIfPresentOptional(Fn& fn, FEntityId e) noexcept
    {
        InvokeResolvedOptional<Optional...>(fn, e, m_World.template Get<Comps>(e)...);
    }

    template<typename... Optional, typename Fn>
    void InvokeResolvedOptional(Fn& fn, FEntityId e, Comps*... components) noexcept
    {
        if (((components != nullptr) && ...)) {
            fn(e, *components..., m_World.template Get<Optional>(e)...);
        }
    }

    /** 走査対象の FWorld。 */
    FWorld& m_World;
};

/**
 * FWorld::Query<...>() の実装本体 (World.h で宣言、TQueryView を生成して返す)。
 *
 * @tparam Comps 同時に要求するコンポーネント型。
 * @return この FWorld を束ねた TQueryView<Comps...>。
 */
template<typename... Comps>
auto FWorld::Query() noexcept
{
    return TQueryView<Comps...>(*this);
}

} // namespace acs
