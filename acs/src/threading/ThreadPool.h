// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "foundation/Move.h"
#include "threading/Atomic.h"
#include "threading/ParallelForDiagnostics.h"
#include "threading/ThreadPoolDiagnostics.h"

#include <cstddef>
#include <new>
#include <type_traits>

namespace acs {

/**
 * タスク群の完了を待つためのアトミックカウンタ。
 *
 * @details
 * Submit ごとに Add(1)、完了ごとに Done() され、0 になったら全完了とみなす。
 * Done は 0 で飽和し underflow しない (underflow すると Finished() が永久に
 * false となり Wait() がハングするため)。コピー禁止。
 */
class FCompletionCounter {
public:
    /** カウント 0 で構築する。 */
    FCompletionCounter() noexcept = default;

    /**
     * 初期カウントを指定して構築する。
     *
     * @param initial 初期の保留タスク数。
     */
    explicit FCompletionCounter(u32 initial) noexcept : m_V(initial) {}

    /** コピー禁止 (カウントを共有すると意味が破綻するため)。 */
    FCompletionCounter(const FCompletionCounter&) = delete;

    /** コピー代入も禁止。 */
    FCompletionCounter& operator=(const FCompletionCounter&) = delete;

    /**
     * 保留タスク数をアトミックに加算する (タスク投入時に呼ぶ)。
     *
     * @param n 加算する数 (既定 1)。
     */
    void Add(u32 n = 1) noexcept   { m_V.FetchAdd(n); }

    /**
     * 保留タスク数をアトミックに 1 減らす (タスク完了時に呼ぶ)。
     *
     * @details
     * 0 を下限に CAS ループで飽和させる。Add より多く Done が呼ばれても 0xFFFFFFFF へ
     * underflow しない (underflow すると Finished() が永久に false となり Wait() がハングする)。
     */
    void Done() noexcept {
        u32 cur = m_V.Load(EMemoryOrder::Relaxed);
        while (cur != 0) {
            if (m_V.CompareExchange(cur, cur - 1)) return;   // 失敗時 cur は実値に更新される
        }
    }

    /**
     * 残りの保留タスク数を返す。
     *
     * @return acquire ロードした保留タスク数。
     */
    u32  Pending() const noexcept  { return m_V.Load(EMemoryOrder::Acquire); }

    /**
     * 全タスクが完了したかを返す。
     *
     * @return 保留タスク数が 0 なら true。
     */
    bool Finished() const noexcept { return Pending() == 0; }

private:
    /** 残りの保留タスク数 (アトミック)。 */
    mutable TAtomic<u32> m_V {0};
};

/** タスク本体の関数型 (worker_index は実行ワーカーの ID 0..N-1)。 */
using TaskFn = void (*)(void* user, u32 worker_index);

/** ワーカーへ投入する 1 つのタスク (POD なので deque に値コピーされる)。 */
struct FTask {
    /** 実行する関数。 */
    TaskFn              fn       = nullptr;

    /** fn に渡すユーザーデータ。 */
    void*               user     = nullptr;

    /** 完了通知先のカウンタ (任意、null 可)。 */
    FCompletionCounter*  counter  = nullptr;
};

/**
 * ワークスチール型のスレッドプール (Chase-Lev SPMC deque + help-stealing wait)。
 *
 * @details
 * 全メンバが static のシングルトン。各ワーカーはローカル deque を持ち、空になると
 * 外部投入キューの drain → 他ワーカーからの steal → 短時間 park の順に動く。Wait は
 * 待機中もスティーリングに参加するため、ワーカースレッドからの呼び出しでもデッドロックしない。
 */
class CThreadPool {
public:
    /**
     * プールを初期化し、ワーカースレッドを起動する。
     *
     * @param worker_count ワーカー数 (0 で論理 CPU 数、上限 256 にクランプ)。
     * @return 成功なら空の TResult。二重 Init や確保失敗時はエラー。
     */
    static TResult<void> Init(u32 worker_count = 0) noexcept;

    /**
     * 受付を閉じ、受理済みタスクとその子タスクを完了させてから全ワーカーを停止・Join する。
     *
     * @details
     * 外部スレッドから終了開始後に行われた Submit はエラーとなり、counter 加算も所有権移動も
     * 発生しない。実行中タスクから派生する Submit は依存グラフを完走させるため受理される。
     * タスク自身から呼ぶと自己 Join を避けるため何もせず戻るので、所有者スレッドから改めて
     * 呼ぶこと。未初期化なら何もしない。
     */
    static void         Shutdown() noexcept;

    /**
     * 起動中のワーカー数を返す。
     *
     * @return ワーカー数 (未初期化なら 0)。
     */
    static u32          WorkerCount() noexcept;

    /**
     * 呼び出しスレッドのワーカーインデックスを返す。
     *
     * @return プールワーカなら 0..N-1、それ以外は kNotAWorker。
     */
    static u32          CurrentWorkerIndex() noexcept;

    /** 現在の同期・割り当て診断値をスナップショットとして返す。 */
    static FThreadPoolDiagnostics Diagnostics() noexcept;

    /** ParallelFor の一時 context 格納診断値をスナップショットとして返す。 */
    static FParallelForDiagnostics CaptureParallelForDiagnostics() noexcept;

    /** ThreadPool と ParallelFor の診断カウンタを 0 に戻す。投入済みタスクには影響しない。 */
    static void ResetDiagnostics() noexcept;

    /** ワーカー以外のスレッドを表す番兵インデックス。 */
    static constexpr u32 kNotAWorker = 0xFFFFFFFFu;

    /**
     * タスクを投入する。
     *
     * @details
     * counter が非 null なら受理が確定したタスクだけ自動で Add(1) される。ワーカースレッドからの投入は
     * 自分のローカル deque へ、それ以外はグローバル投入キュー経由で入る。
     * エラー時はタスクを保持しないため、user の所有権は呼び出し側に残る。
     * @param t 投入するタスク (fn が null なら Threading エラー)。
     * @return 成功なら空の TResult。未初期化・null fn・ノード確保失敗時はエラー。
     */
    static TResult<void> Submit(const FTask& t) noexcept;

    /**
     * 所有権付き callable を非同期タスクとして投入する。
     *
     * @details `void(u32 worker_index) noexcept` または `void() noexcept` として呼べ、
     * noexcept 構築・破棄できる型だけを受け付ける。
     * 小さい callable は固定ノード内へ直接構築し、サイズまたは alignment 超過時だけ
     * callable 本体を heap へ置く。投入成功後の寿命と破棄は ThreadPool が管理する。
     * @tparam Callable 呼び出し可能オブジェクト型。
     * @param callable 所有権を ThreadPool へ渡す callable。
     * @param counter 任意の完了カウンタ。
     * @return 投入結果。
     */
    template<typename Callable>
    static TResult<void> SubmitCallable(Callable&& callable, FCompletionCounter* counter = nullptr) noexcept
    {
        /** ノードへ保存する具象型。 */
        using StoredCallable = std::decay_t<Callable>;
        /** worker index 付き形式を選ぶか。 */
        constexpr bool kUsesWorkerIndex = std::is_invocable_r_v<void, StoredCallable&, u32>;
        static_assert(kUsesWorkerIndex || std::is_invocable_r_v<void, StoredCallable&>, "ThreadPool callable は void(u32) または void() として呼べる必要があります");
        static_assert(std::is_nothrow_constructible_v<StoredCallable, Callable&&>, "ThreadPool callable は noexcept 構築できる必要があります");
        static_assert(std::is_nothrow_destructible_v<StoredCallable>, "ThreadPool callable は noexcept 破棄できる必要があります");
        static_assert(kUsesWorkerIndex ? std::is_nothrow_invocable_r_v<void, StoredCallable&, u32> : std::is_nothrow_invocable_r_v<void, StoredCallable&>, "ThreadPool callable の呼び出しは noexcept である必要があります");

        /** callable の所有情報を保持するノード。 */
        FCallableTaskStorage* const storage = AcquireCallableTaskStorage();
        if (!storage) {
            return ACS_ERR(Threading, 9, "CThreadPool callable storage is unavailable");
        }

        if constexpr (sizeof(StoredCallable) <= kInlineCallableBytes && alignof(StoredCallable) <= alignof(std::max_align_t)) {
            /** 固定領域へ構築した callable。 */
            auto* const object = ::new (static_cast<void*>(storage->inline_storage)) StoredCallable(Forward<Callable>(callable));
            storage->object = object;
            storage->destroy = &DestroyInlineCallable<StoredCallable>;
            storage->heap_callable = false;
        } else {
            /** 個別確保した callable。 */
            auto* const object = new (std::nothrow) StoredCallable(Forward<Callable>(callable));
            if (!object) {
                AbandonCallableTaskStorage(storage);
                return ACS_ERR(Memory, 10, "CThreadPool callable allocation failed");
            }
            storage->object = object;
            storage->destroy = &DestroyHeapCallable<StoredCallable>;
            storage->heap_callable = true;
        }
        storage->invoke = &InvokeCallable<StoredCallable>;
        return PublishCallableTaskStorage(storage, counter);
    }

    /**
     * counter が 0 になるまで待機する (待機中もスティーリングに参加)。
     *
     * @param counter 完了を待つ対象のカウンタ。
     */
    static void         Wait(FCompletionCounter& counter) noexcept;

    /**
     * 範囲 [begin, end) を grain サイズに分割して並列実行し、完了まで待つ。
     *
     * @details 32 chunk までは呼び出し stack、超過分は固定 block pool を使い、
     * pool 枯渇時だけ OS heap へ退避する。全 context は Wait 完了まで保持される。
     * @param begin 範囲の開始インデックス (含む)。
     * @param end 範囲の終了インデックス (含まない)。
     * @param grain 1 チャンクが処理する要素数 (0 は 1 に補正)。
     * @param body 各インデックスに対して呼ぶ関数 (i, worker_index, user)。
     * @param user body に渡すユーザーデータ。
     * @return 成功なら空の TResult。未初期化・null body・確保失敗時はエラー。
     */
    static TResult<void> ParallelFor(u32 begin, u32 end, u32 grain, void (*body)(u32 i, u32 worker_index, void* user), void* user) noexcept;

private:
    /** 固定ノード内へ直接置ける callable の最大 byte 数。 */
    static constexpr usize kInlineCallableBytes = 64;

    /** 所有 callable の実行・破棄・確保元情報を保持する内部ノード。 */
    struct FCallableTaskStorage {
        /** inline callable の固定領域。 */
        alignas(std::max_align_t) u8 inline_storage[kInlineCallableBytes]{};

        /** inline 領域または heap callable 本体。 */
        void* object = nullptr;

        /** callable を具象型で呼ぶ thunk。 */
        void (*invoke)(void* object, u32 worker_index) noexcept = nullptr;

        /** callable を具象型で破棄する thunk。 */
        void (*destroy)(void* object) noexcept = nullptr;

        /** 所有する PoolState。 */
        void* owner = nullptr;

        /** callable 本体が heap fallback なら true。 */
        bool heap_callable = false;
    };

    /** callable ノードを確保し、構築完了まで PoolState の寿命を保持する。 */
    static FCallableTaskStorage* AcquireCallableTaskStorage() noexcept;

    /** 構築前に失敗した callable ノードを返却する。 */
    static void AbandonCallableTaskStorage(FCallableTaskStorage* storage) noexcept;

    /** 構築済み callable ノードを通常 Submit 経路へ公開する。 */
    static TResult<void> PublishCallableTaskStorage(FCallableTaskStorage* storage, FCompletionCounter* counter) noexcept;

    /** 実行中の callable ノードを呼び、破棄して確保元へ返す。 */
    static void CallableTaskThunk(void* storage, u32 worker_index) noexcept;

    /** 型付き callable を共通ノード ABI へ接続する。 */
    template<typename Callable>
    static void InvokeCallable(void* object, u32 worker_index) noexcept
    {
        /** 呼び出す具象 callable。 */
        auto& callable = *static_cast<Callable*>(object);
        if constexpr (std::is_invocable_r_v<void, Callable&, u32>)
            callable(worker_index);
        else
            callable();
    }

    /** inline callable を具象型で破棄する。 */
    template<typename Callable>
    static void DestroyInlineCallable(void* object) noexcept
    {
        static_cast<Callable*>(object)->~Callable();
    }

    /** heap callable を具象型で破棄する。 */
    template<typename Callable>
    static void DestroyHeapCallable(void* object) noexcept
    {
        delete static_cast<Callable*>(object);
    }
};

/** 移行期間中に旧名を受け付ける互換別名。 */
using FThreadPool = CThreadPool;

} // namespace acs
