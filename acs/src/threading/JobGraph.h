// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "threading/Atomic.h"
#include "threading/ThreadPool.h"
#include "threading/JobGraphCompletionDiagnostics.h"
#include "threading/JobGraphDiagnostics.h"
#include "foundation/Move.h"

#include <cstddef>
#include <new>
#include <type_traits>

namespace acs {

class FJobGraph;

/**
 * グラフ内の 1 ジョブを指すハンドル (FJobGraph::Add の戻り値 / DependOn のキー)。
 *
 * @details graph ポインタとジョブ index の組。index が 0xFFFFFFFF または graph が null なら無効。
 */
struct FJobHandle {
    /** このハンドルが属するグラフ (null = 無効)。 */
    FJobGraph* graph = nullptr;

    /** グラフ内のジョブインデックス (0xFFFFFFFF = 無効)。 */
    u32       index = 0xFFFFFFFFu;

    /**
     * ハンドルが有効なジョブを指しているかを返す。
     *
     * @return graph が非 null かつ index が有効なら true。
     */
    bool IsValid() const noexcept { return graph != nullptr && index != 0xFFFFFFFFu; }

    /**
     * upstream が完了するまで自身を走らせない依存を追加する。
     *
     * @details upstream と自身が同じグラフに属する場合のみ有効 (それ以外は無視)。
     * @param upstream 先に完了している必要があるジョブ。
     */
    void DependOn(FJobHandle upstream) noexcept;
};

/** ジョブ本体の関数型 (FThreadPool::TaskFn と同形式)。 */
using JobFn = void (*)(void* user, u32 worker_index);

/**
 * 依存関係付きの並列タスクスケジューラ。
 *
 * @details
 * FThreadPool 上で動く DAG スケジューラ。各ジョブは完了時に dependents の
 * deps_remaining をアトミックにデクリメントし、0 になったものを FThreadPool へ
 * 投入する fan-out 方式。グラフは Submit 後は変更不可 (Add/AddDependency は Submit 前のみ)。
 * Reset で依存構造を保ったまま再実行できる。コピー不可。
 */
class FJobGraph {
public:
    /** 空のジョブグラフを構築する。 */
    FJobGraph() noexcept = default;

    /** 実行中なら Wait してから、Add で確保したすべての FJob を解放する。 */
    ~FJobGraph() noexcept;

    /** コピー禁止。 */
    FJobGraph(const FJobGraph&) = delete;

    /** コピー代入も禁止。 */
    FJobGraph& operator=(const FJobGraph&) = delete;

    /**
     * ジョブを追加する (Submit 前のみ呼べる)。
     *
     * @param fn 実行する関数。
     * @param user fn に渡すユーザーデータ。
     * @return 追加したジョブのハンドル。Submit 済みまたは fn が null なら無効ハンドル。
     */
    FJobHandle Add(JobFn fn, void* user) noexcept;

    /**
     * 所有権付き callable を job として追加する。
     *
     * @details `void(u32 worker_index) noexcept` または `void() noexcept` として呼べ、
     * noexcept 構築・破棄できる型だけを受け付ける。
     * 小さい callable は FJob 内へ直接構築し、サイズまたは alignment が上限を超える場合だけ
     * heap へフォールバックする。どちらも graph 破棄時まで寿命が保証される。
     * @tparam Callable 呼び出し可能オブジェクト型。
     * @param callable graph が所有する callable。
     * @return 追加した job のハンドル。確保失敗時は無効。
     */
    template<typename Callable>
    FJobHandle AddCallable(Callable&& callable) noexcept
    {
        /** job が所有する具象 callable 型。 */
        using StoredCallable = std::decay_t<Callable>;
        /** worker index 付き形式を選ぶか。 */
        constexpr bool kUsesWorkerIndex = std::is_invocable_r_v<void, StoredCallable&, u32>;
        static_assert(kUsesWorkerIndex || std::is_invocable_r_v<void, StoredCallable&>, "JobGraph callable は void(u32) または void() として呼べる必要があります");
        static_assert(std::is_nothrow_constructible_v<StoredCallable, Callable&&>, "JobGraph callable は noexcept 構築できる必要があります");
        static_assert(std::is_nothrow_destructible_v<StoredCallable>, "JobGraph callable は noexcept 破棄できる必要があります");
        static_assert(kUsesWorkerIndex ? std::is_nothrow_invocable_r_v<void, StoredCallable&, u32> : std::is_nothrow_invocable_r_v<void, StoredCallable&>, "JobGraph callable の呼び出しは noexcept である必要があります");

        /** callable を所有する新規 job。 */
        FJob* const job = AppendEmptyJob();
        if (!job) return {};

        if constexpr (sizeof(StoredCallable) <= kInlineCallableBytes && alignof(StoredCallable) <= alignof(std::max_align_t)) {
            /** job の固定領域へ構築した callable。 */
            auto* const stored = ::new (static_cast<void*>(job->callable_storage)) StoredCallable(Forward<Callable>(callable));
            job->user = stored;
            job->destroy_callable = &DestroyInlineCallable<StoredCallable>;
            ++m_Diagnostics.inline_callable_count;
        } else {
            /** サイズ超過により個別確保した callable。 */
            auto* const stored = new (std::nothrow) StoredCallable(Forward<Callable>(callable));
            if (!stored) {
                RemoveLastJob(job);
                return {};
            }
            job->user = stored;
            job->destroy_callable = &DestroyHeapCallable<StoredCallable>;
            ++m_Diagnostics.heap_callable_count;
        }
        job->fn = &CallableThunk<StoredCallable>;
        return FJobHandle{this, m_JobCount - 1};
    }

    /**
     * upstream → downstream の依存関係を追加する (Submit 前のみ有効)。
     *
     * @param upstream 先に完了する必要があるジョブ。
     * @param downstream upstream の完了後に走るジョブ。
     */
    void AddDependency(FJobHandle upstream, FJobHandle downstream) noexcept;

    /**
     * 全ジョブを FThreadPool に投入する。依存 0 のジョブが即座に走り始める。
     *
     * @details Kahn 法でサイクル検知し、循環があれば一件も投入しない。FThreadPool への
     * 個別投入が失敗した場合はそのジョブだけを同期実行し、部分投入による二重実行を防ぐ。
     * @return 成功なら空の TResult。二重 Submit・サイクル検出時はエラー。
     */
    TResult<void> Submit() noexcept;

    /** 全ジョブの完了までブロックして待つ (待機中もスティーリングに参加)。 */
    void Wait() noexcept;

    /** 実行中なら Wait し、同じグラフを再実行できるよう依存カウンタを初期値に戻す。 */
    void Reset() noexcept;

    /**
     * グラフ内のジョブ数を返す。
     *
     * @return 登録済みジョブ数。
     */
    u32 JobCount() const noexcept { return m_JobCount; }

    /** 現在の構築・再実行診断値を返す。 */
    FJobGraphDiagnostics Diagnostics() const noexcept { return m_Diagnostics; }

    /** 現在の完了カウンタ一括予約を既存診断 ABI と分離して返す。 */
    FJobGraphCompletionDiagnostics CompletionDiagnostics() const noexcept
    {
        if (!m_bSubmitted || m_JobCount == 0u) return {};
        return FJobGraphCompletionDiagnostics{1u, m_JobCount};
    }

private:
    friend struct FJobHandle;

    /**
     * 1 ジョブを実行し、依存先を起動する FThreadPool 向けの TaskFn thunk。
     *
     * @param user 実行する FJob へのポインタ。
     * @param worker_index 実行中のワーカーインデックス。
     */
    static void JobThunk(void* user, u32 worker_index) noexcept;

    /** グラフ内の 1 ジョブの状態。 */
    struct FJob {
        /** 実行する関数。 */
        JobFn          fn               = nullptr;

        /** fn に渡すユーザーデータ。 */
        void*          user             = nullptr;

        /** 未解決の依存数 (0 になったら実行可能、アトミック)。 */
        TAtomic<u32>    deps_remaining   {0};

        /** 初期依存数 (Reset で deps_remaining を復元するために保存)。 */
        u32            initial_deps     = 0;

        /** このジョブの完了で起動する後続ジョブの index 群。 */
        TArray<u32>     dependents;

        /** 所属するグラフ (JobThunk から参照する)。 */
        FJobGraph*      owner            = nullptr;

        /** inline callable の固定領域。 */
        alignas(std::max_align_t) u8 callable_storage[48]{};

        /** 所有 callable を破棄する関数。raw Add では null。 */
        void (*destroy_callable)(FJob* job) noexcept = nullptr;
    };

    /** job 内へ直接置ける callable の最大 byte 数。 */
    static constexpr usize kInlineCallableBytes = 48;

    /** graph 自体へ直接置ける job 数。 */
    static constexpr u32 kInlineJobCapacity = 32;

    /** 型付き callable を JobFn ABI へ接続するコンパイル時 thunk。 */
    template<typename Callable>
    static void CallableThunk(void* user, u32 worker_index) noexcept
    {
        /** 呼び出す具象 callable。 */
        auto& callable = *static_cast<Callable*>(user);
        if constexpr (std::is_invocable_r_v<void, Callable&, u32>) {
            callable(worker_index);
        } else {
            callable();
        }
    }

    /** inline callable を正しい具象型で破棄する。 */
    template<typename Callable>
    static void DestroyInlineCallable(FJob* job) noexcept
    {
        static_cast<Callable*>(static_cast<void*>(job->callable_storage))->~Callable();
    }

    /** heap fallback callable を正しい具象型で破棄する。 */
    template<typename Callable>
    static void DestroyHeapCallable(FJob* job) noexcept
    {
        delete static_cast<Callable*>(job->user);
    }

    /** 空の job を末尾へ追加し、確保失敗時は null を返す。 */
    FJob* AppendEmptyJob() noexcept;

    /** 直前に追加した job を公開前の失敗時に取り消す。 */
    void RemoveLastJob(FJob* job) noexcept;

    /** index の job を返す。 */
    FJob* JobAt(u32 index) noexcept;

    /** index の job を返す const 版。 */
    const FJob* JobAt(u32 index) const noexcept;

    /** job と所有 callable を正しい確保元へ返す。 */
    void DestroyJob(FJob* job) noexcept;

    /** 依存構造を検証し、entry index 群を初回だけ構築する。 */
    TResult<void> CompileTopology() noexcept;

    /** graph へ直接置く FJob の未初期化領域。 */
    alignas(FJob) u8 m_InlineJobStorage[sizeof(FJob) * kInlineJobCapacity]{};

    /** inline 上限を超えた job pointer。 */
    TArray<FJob*> m_OverflowJobs;

    /** 登録済み job 数。 */
    u32 m_JobCount = 0;

    /** 検証済みの依存 0 job index 群。 */
    TArray<u32> m_EntryJobs;

    /** Kahn 法で再利用する依存数 scratch。 */
    TArray<u32> m_TopologyRemaining;

    /** Kahn 法で再利用する queue scratch。 */
    TArray<u32> m_TopologyQueue;

    /** 現在の graph 形状を検証済みなら true。 */
    bool m_TopologyCompiled = false;

    /** 検証済み graph が循環を含むなら true。 */
    bool m_TopologyHasCycle = false;

    /** Submit で全 job を一括予約し、各 job 完了時に減らす完了カウンタ。 */
    FCompletionCounter  m_Counter;

    /** Submit 済みフラグ (true 以降は Add/AddDependency 不可)。 */
    bool               m_bSubmitted       = false;

    /** 構築・再実行経路の診断値。 */
    FJobGraphDiagnostics m_Diagnostics;
};

} // namespace acs
