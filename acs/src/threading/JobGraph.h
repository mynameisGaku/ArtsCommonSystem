// SPDX-License-Identifier: Apache-2.0
// FJobGraph — 依存関係付き並列タスクスケジューラ
//
// 使い方:
//   FJobGraph g;
//   auto loadA  = g.Add(&LoadAssetA, &ctx);
//   auto loadB  = g.Add(&LoadAssetB, &ctx);
//   auto build  = g.Add(&BuildScene, &ctx);
//   auto upload = g.Add(&UploadGpu,  &ctx);
//
//   build.DependOn(loadA);
//   build.DependOn(loadB);
//   upload.DependOn(build);
//
//   g.Submit();         // 投入 (依存 0 の job から走り出す)
//   g.Wait();           // 全 job 完了まで block
//
// 実装:
//   ・FThreadPool 上で動く。各 job は完了時に「dependents の deps_remaining を
//     atomic decrement して 0 になったら FThreadPool::Submit」する fan-out 方式
//   ・グラフは Submit 後に変更不可 (Add は Submit より前のみ)
//   ・Reset() で再利用可能 (依存関係はそのまま、deps_remaining だけ復元)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "threading/Atomic.h"
#include "threading/ThreadPool.h"

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
    u32 JobCount() const noexcept { return static_cast<u32>(m_Jobs.Size()); }

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
    };

    /** 登録済みジョブ群 (各要素は new で確保され、デストラクタで解放)。 */
    TArray<FJob*>        m_Jobs;

    /** 実行中ジョブ数の完了カウンタ (Submit/完了ごとに増減)。 */
    FCompletionCounter  m_Counter;

    /** Submit 済みフラグ (true 以降は Add/AddDependency 不可)。 */
    bool               m_bSubmitted       = false;
};

} // namespace acs
