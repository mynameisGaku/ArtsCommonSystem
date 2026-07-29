// SPDX-License-Identifier: Apache-2.0
// 基盤最適化で扱う公開型の Win64 レイアウト予算テスト
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Array.h"
#include "container/HashMap.h"
#include "container/String.h"
#include "event/MessageBroker.h"
#include "event/Timer.h"
#include "event/TimerDiagnostics.h"
#include "platform/FileSystemDiagnostics.h"
#include "threading/JobGraph.h"
#include "threading/JobGraphDiagnostics.h"
#include "threading/ThreadPool.h"
#include "threading/ThreadPoolDiagnostics.h"

#include <cstddef>
#include <cstdio>

using namespace acs;

static_assert(sizeof(void*) == 8u, "公開レイアウト予算は ACS の Win64 ABI を対象とします");
static_assert(sizeof(TArray<u32>) <= 32u, "TArray 公開レイアウト予算を超えました");
static_assert(sizeof(FString) <= 32u, "FString 公開レイアウト予算を超えました");
static_assert(sizeof(THashMap<u32, u32>) <= 64u, "THashMap 公開レイアウト予算を超えました");
static_assert(sizeof(FCompletionCounter) <= 4u, "完了カウンタの hot state 予算を超えました");
static_assert(sizeof(FTask) <= 24u, "タスク記述子の3ポインタ予算を超えました");
static_assert(sizeof(FJobHandle) <= 16u, "ジョブハンドルの2ワード予算を超えました");
static_assert(sizeof(FJobGraph) <= 4032u, "ジョブグラフのinline storage予算を超えました");
static_assert(sizeof(FSubscriptionHandle) <= 12u, "購読ハンドルの3整数予算を超えました");
static_assert(sizeof(FTimerHandle) <= 8u, "タイマハンドルの2整数予算を超えました");
static_assert(sizeof(FThreadPoolDiagnostics) <= 96u, "thread pool診断型の予算を超えました");
static_assert(sizeof(FJobGraphDiagnostics) <= 40u, "job graph診断型の予算を超えました");
static_assert(sizeof(FTimerDiagnostics) <= 24u, "timer診断型の予算を超えました");
static_assert(sizeof(FFileSystemDiagnostics) <= 24u, "file診断型の予算を超えました");
static_assert(alignof(FTask) <= alignof(void*), "タスク記述子のalignment予算を超えました");
static_assert(alignof(FJobGraph) <= alignof(std::max_align_t), "ジョブグラフのalignment予算を超えました");

ACS_TEST(FoundationOptimizationWaveO, PublicLayoutBudgetsRemainBounded)
{
    EXPECT_TRUE(sizeof(FThreadPoolDiagnostics) <= 96u);
    EXPECT_TRUE(sizeof(FJobGraphDiagnostics) <= 40u);
    EXPECT_TRUE(sizeof(FTimerDiagnostics) <= 24u);
    EXPECT_TRUE(sizeof(FFileSystemDiagnostics) <= 24u);

    std::printf("foundation_layout array=%zu string=%zu hashmap=%zu counter=%zu task=%zu job_handle=%zu job_graph=%zu subscription=%zu timer=%zu thread_diag=%zu job_diag=%zu timer_diag=%zu file_diag=%zu\n", sizeof(TArray<u32>), sizeof(FString), sizeof(THashMap<u32, u32>), sizeof(FCompletionCounter), sizeof(FTask), sizeof(FJobHandle), sizeof(FJobGraph), sizeof(FSubscriptionHandle), sizeof(FTimerHandle), sizeof(FThreadPoolDiagnostics), sizeof(FJobGraphDiagnostics), sizeof(FTimerDiagnostics), sizeof(FFileSystemDiagnostics));
}
