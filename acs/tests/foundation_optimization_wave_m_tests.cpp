// SPDX-License-Identifier: Apache-2.0

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "event/MessageBroker.h"
#include "platform/FileSystem.h"
#include "test/Expect.h"
#include "test/Test.h"
#include "threading/Atomic.h"
#include "threading/JobGraph.h"
#include "threading/ThreadPool.h"

using namespace acs;

namespace {

/** JobGraph の全 job 実行回数を記録する状態。 */
struct FWaveMJobState {
    /** 完了した job 数。 */
    TAtomic<u32> executed{0u};
};

/** JobGraph の一 job を実行済みにする。 */
void CountWaveMJob(void* User, u32) noexcept
{
    /** 実行回数の記録先。 */
    auto* const State = static_cast<FWaveMJobState*>(User);
    State->executed.FetchAdd(1u);
}

/** MessageBroker の複合再入検証で配送する値。 */
struct FWaveMEvent {
    /** 発行元が付ける識別値。 */
    u32 value = 0u;
};

/** Subscribe・Unsubscribe・nested Publish を同時に検証する状態。 */
struct FWaveMBrokerState {
    /** 検証対象 broker。 */
    CMessageBroker* broker = nullptr;

    /** 最初の callback の呼び出し回数。 */
    u32 root_calls = 0u;

    /** 即時解除対象 callback の呼び出し回数。 */
    u32 cancelled_calls = 0u;

    /** 最初から残る callback の呼び出し回数。 */
    u32 survivor_calls = 0u;

    /** 発行中に追加した callback の呼び出し回数。 */
    u32 added_calls = 0u;

    /** nested Publish を開始済みか。 */
    bool nested_started = false;

    /** 発行中の解除が成功したか。 */
    bool cancellation_succeeded = false;

    /** 発行中の購読追加がすべて成功したか。 */
    bool additions_succeeded = true;

    /** 即時解除する購読 handle。 */
    FSubscriptionHandle cancelled{};

    /** 発行中に追加した購読 handle 群。 */
    FSubscriptionHandle added[96]{};
};

/** 発行中に追加される callback の実行回数を記録する。 */
void OnWaveMAdded(const FWaveMEvent&, void* User) noexcept
{
    /** Broker 複合再入検証の状態。 */
    auto* const State = static_cast<FWaveMBrokerState*>(User);
    ++State->added_calls;
}

/** 発行開始後に解除され、同じ発行では呼ばれない callback。 */
void OnWaveMCancelled(const FWaveMEvent&, void* User) noexcept
{
    /** Broker 複合再入検証の状態。 */
    auto* const State = static_cast<FWaveMBrokerState*>(User);
    ++State->cancelled_calls;
}

/** outer と nested の両方で残る callback。 */
void OnWaveMSurvivor(const FWaveMEvent&, void* User) noexcept
{
    /** Broker 複合再入検証の状態。 */
    auto* const State = static_cast<FWaveMBrokerState*>(User);
    ++State->survivor_calls;
}

/** 最初の発行で購読追加・即時解除・nested Publish を連続実行する。 */
void OnWaveMRoot(const FWaveMEvent&, void* User) noexcept
{
    /** Broker 複合再入検証の状態。 */
    auto* const State = static_cast<FWaveMBrokerState*>(User);
    ++State->root_calls;
    if (State->nested_started) return;
    State->nested_started = true;

    /** callback 中の再確保を確実に起こす追加購読数。 */
    constexpr u32 kAddedSubscriberCount = 96u;
    for (u32 Index = 0u; Index < kAddedSubscriberCount; ++Index) {
        State->added[Index] = State->broker->SubscribeTyped<FWaveMEvent, &OnWaveMAdded>(State);
        State->additions_succeeded = State->additions_succeeded && State->added[Index].IsValid();
    }
    State->cancellation_succeeded = State->broker->Unsubscribe(State->cancelled);
    State->broker->Publish(FWaveMEvent{2u});
}

/** Wave M の package round-trip に使う一時 path。 */
constexpr const wchar_t* kWaveMPackagePath = L"acs_foundation_wave_m_paths.acpak";

} // namespace

ACS_TEST(FoundationOptimizationWaveM, JobGraphReservesCompletionCountOncePerSubmit)
{
    /** job 数と反復数。 */
    constexpr u32 kJobCount = 127u;
    constexpr u32 kRunCount = 16u;
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(4u).IsOk());

    /** 全 job の実行回数を記録する状態。 */
    FWaveMJobState State{};
    /** 一括完了予約を検証する job graph。 */
    CJobGraph Graph;
    /** 依存関係の構築に使う job handle 群。 */
    FJobHandle Handles[kJobCount]{};
    for (u32 Index = 0u; Index < kJobCount; ++Index) {
        Handles[Index] = Graph.Add(&CountWaveMJob, &State);
        EXPECT_TRUE(Handles[Index].IsValid());
    }
    for (u32 Index = 1u; Index < kJobCount; ++Index) {
        /** 二分木状の依存元 job。 */
        const u32 ParentIndex = (Index - 1u) / 2u;
        Handles[Index].DependOn(Handles[ParentIndex]);
    }

    for (u32 Run = 0u; Run < kRunCount; ++Run) {
        if (Run != 0u) {
            Graph.Reset();
            /** Reset 後は現在予約がないことを示す診断値。 */
            const FJobGraphCompletionDiagnostics ResetDiagnostics = Graph.CompletionDiagnostics();
            EXPECT_EQ(ResetDiagnostics.reservation_batch_count, 0u);
            EXPECT_EQ(ResetDiagnostics.reserved_job_count, 0u);
        }
        EXPECT_TRUE(Graph.Submit().IsOk());
        Graph.Wait();
        EXPECT_EQ(State.executed.Load(EMemoryOrder::Acquire), (Run + 1u) * kJobCount);
        /** 既存 ABI から分離した一括予約診断値。 */
        const FJobGraphCompletionDiagnostics Diagnostics = Graph.CompletionDiagnostics();
        EXPECT_EQ(Diagnostics.reservation_batch_count, 1u);
        EXPECT_EQ(Diagnostics.reserved_job_count, kJobCount);
    }

    Graph.Reset();
    /** Reset 後の構築再開で追加する独立 job。 */
    const FJobHandle AddedAfterReset = Graph.Add(&CountWaveMJob, &State);
    EXPECT_TRUE(AddedAfterReset.IsValid());
    EXPECT_TRUE(Graph.Submit().IsOk());
    Graph.Wait();
    EXPECT_EQ(State.executed.Load(EMemoryOrder::Acquire), kRunCount * kJobCount + kJobCount + 1u);
    /** job 追加後の現在予約だけを表す診断値。 */
    const FJobGraphCompletionDiagnostics ExpandedDiagnostics = Graph.CompletionDiagnostics();
    EXPECT_EQ(ExpandedDiagnostics.reservation_batch_count, 1u);
    EXPECT_EQ(ExpandedDiagnostics.reserved_job_count, kJobCount + 1u);

    CThreadPool::Shutdown();
}

ACS_TEST(FoundationOptimizationWaveM, BrokerSnapshotSurvivesMutationAndNestedPublish)
{
    /** 複合再入配送を検証する broker。 */
    CMessageBroker Broker;
    /** callback 間で共有する検証状態。 */
    FWaveMBrokerState State{};
    State.broker = &Broker;

    /** 発行中に mutation を行う先頭購読。 */
    const FSubscriptionHandle Root = Broker.SubscribeTyped<FWaveMEvent, &OnWaveMRoot>(&State);
    State.cancelled = Broker.SubscribeTyped<FWaveMEvent, &OnWaveMCancelled>(&State);
    /** mutation 後も配送対象に残る購読。 */
    const FSubscriptionHandle Survivor = Broker.SubscribeTyped<FWaveMEvent, &OnWaveMSurvivor>(&State);
    EXPECT_TRUE(Root.IsValid());
    EXPECT_TRUE(State.cancelled.IsValid());
    EXPECT_TRUE(Survivor.IsValid());

    Broker.Publish(FWaveMEvent{1u});
    EXPECT_TRUE(State.additions_succeeded);
    EXPECT_TRUE(State.cancellation_succeeded);
    EXPECT_EQ(State.root_calls, 2u);
    EXPECT_EQ(State.cancelled_calls, 0u);
    EXPECT_EQ(State.survivor_calls, 2u);
    EXPECT_EQ(State.added_calls, 96u);

    Broker.Publish(FWaveMEvent{3u});
    EXPECT_EQ(State.root_calls, 3u);
    EXPECT_EQ(State.cancelled_calls, 0u);
    EXPECT_EQ(State.survivor_calls, 3u);
    EXPECT_EQ(State.added_calls, 192u);
}

ACS_TEST(FoundationOptimizationWaveM, CanonicalPackagePathsKeepExactMatchCompatibility)
{
    /** 大文字小文字が異なる二つの正規形 path。 */
    constexpr const wchar_t* kUpperPath = L"Assets/Texture.bin";
    constexpr const wchar_t* kLowerPath = L"assets/texture.bin";
    /** path の NUL を含まない code unit 数。 */
    constexpr usize kPathLength = 18u;
    EXPECT_TRUE(assetpack::IsCanonicalAcpakVirtualPath(kUpperPath, kPathLength));
    EXPECT_TRUE(assetpack::IsCanonicalAcpakVirtualPath(kLowerPath, kPathLength));
    EXPECT_FALSE(assetpack::IsCanonicalAcpakVirtualPath(L"Assets\\Texture.bin", kPathLength));
    EXPECT_FALSE(assetpack::IsCanonicalAcpakVirtualPath(L"Assets/../Texture.bin", 21u));
    EXPECT_NE(assetpack::HashCanonicalAcpakVirtualPath(kUpperPath, kPathLength), assetpack::HashCanonicalAcpakVirtualPath(kLowerPath, kPathLength));

    /** case-sensitive lookup を確認する各 payload。 */
    constexpr u8 kUpperPayload[] = {1u, 2u, 3u};
    constexpr u8 kLowerPayload[] = {7u, 8u, 9u};
    (void)CFileSystem::Delete(kWaveMPackagePath);
    /** 正規形 path を格納する package writer。 */
    assetpack::CAcpakWriter Writer;
    EXPECT_TRUE(Writer.Open(kWaveMPackagePath, assetpack::AcpakFlagNone).IsOk());
    EXPECT_TRUE(Writer.AddFile(kUpperPath, kUpperPayload, sizeof(kUpperPayload)).IsOk());
    EXPECT_TRUE(Writer.AddFile(kLowerPath, kLowerPayload, sizeof(kLowerPayload)).IsOk());
    EXPECT_TRUE(Writer.AddFile(kUpperPath, kUpperPayload, sizeof(kUpperPayload)).IsErr());
    EXPECT_TRUE(Writer.AddFile(L"Assets\\Texture.bin", kUpperPayload, sizeof(kUpperPayload)).IsErr());
    EXPECT_TRUE(Writer.Finalize().IsOk());
    Writer.Close();

    /** 正規形 path の完全一致検索を検証する reader。 */
    assetpack::CAcpakReader Reader;
    EXPECT_TRUE(Reader.Open(kWaveMPackagePath).IsOk());
    EXPECT_NE(Reader.FindEntry(kUpperPath), nullptr);
    EXPECT_NE(Reader.FindEntry(kLowerPath), nullptr);
    EXPECT_EQ(Reader.FindEntry(L"ASSETS/Texture.bin"), nullptr);
    EXPECT_EQ(Reader.FindEntry(L"Assets\\Texture.bin"), nullptr);

    /** 大文字 path の読み戻し先。 */
    u8 UpperOutput[sizeof(kUpperPayload)]{};
    /** 小文字 path の読み戻し先。 */
    u8 LowerOutput[sizeof(kLowerPayload)]{};
    EXPECT_TRUE(Reader.ReadFile(kUpperPath, UpperOutput, sizeof(UpperOutput)).IsOk());
    EXPECT_TRUE(Reader.ReadFile(kLowerPath, LowerOutput, sizeof(LowerOutput)).IsOk());
    for (usize Index = 0u; Index < sizeof(kUpperPayload); ++Index) {
        EXPECT_EQ(UpperOutput[Index], kUpperPayload[Index]);
        EXPECT_EQ(LowerOutput[Index], kLowerPayload[Index]);
    }
    Reader.Close();
    EXPECT_TRUE(CFileSystem::Delete(kWaveMPackagePath).IsOk());
}

ACS_TEST(FoundationOptimizationWaveM, PublicDiagnosticAndOwnerLayoutsStayBounded)
{
    EXPECT_EQ(sizeof(FJobGraphDiagnostics), 40u);
    EXPECT_EQ(sizeof(FJobGraphCompletionDiagnostics), 16u);
    EXPECT_TRUE(sizeof(CJobGraph) <= 4032u);
    EXPECT_TRUE(sizeof(assetpack::CAcpakReader) <= 336u);
}
