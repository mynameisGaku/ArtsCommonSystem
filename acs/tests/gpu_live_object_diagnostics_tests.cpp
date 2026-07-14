// SPDX-License-Identifier: Apache-2.0
// GPU live-object 診断のメッセージ分類契約テスト。
#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Compiler.h"

#if ACS_BUILD_DEBUG && ACS_PLATFORM_WINDOWS

#    include "render/Dx12/Dx12LiveObjectDiagnosticsInternal.h"

using namespace acs;

ACS_TEST(GpuLiveObjectDiagnostics, SeparatesDiagnosticSelfMessagesFromSevereMessages)
{
    render_internal::GpuDebugMessageSummary summary{};

    constexpr char expected_summary[] =
        "Using ID3D12DebugDevice2::ReportLiveDeviceObjects with D3D12_RLDO_DETAIL will help drill into object "
        "lifetimes.";
    constexpr char expected_device[] = "Live ID3D12Device at 0x0000000000000001, Refcount: 2";
    const bool summary_is_known = render_internal::IsKnownD3D12LiveObjectDiagnosticMessage(
        D3D12_MESSAGE_ID_LIVE_OBJECT_SUMMARY, expected_summary, sizeof(expected_summary));
    const bool device_is_known = render_internal::IsKnownD3D12LiveObjectDiagnosticMessage(D3D12_MESSAGE_ID_LIVE_DEVICE,
                                                                                          expected_device,
                                                                                          sizeof(expected_device));

    EXPECT_TRUE(summary_is_known);
    EXPECT_TRUE(device_is_known);
    render_internal::CountD3D12MessageSeverity(D3D12_MESSAGE_SEVERITY_WARNING, summary_is_known, summary);
    render_internal::CountD3D12MessageSeverity(D3D12_MESSAGE_SEVERITY_WARNING, device_is_known, summary);

    EXPECT_EQ(summary.known_diagnostic_message_count, 2ull);
    EXPECT_EQ(summary.warning_message_count, 0ull);
    EXPECT_EQ(summary.live_object_message_count, 0ull);

    render_internal::CountD3D12MessageSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false, summary);
    EXPECT_EQ(summary.known_diagnostic_message_count, 2ull);
    EXPECT_EQ(summary.warning_message_count, 1ull);
    EXPECT_EQ(summary.live_object_message_count, 1ull);
}

ACS_TEST(GpuLiveObjectDiagnostics, SameIdentifiersWithUnexpectedDescriptionsRemainLeakCandidates)
{
    constexpr char unexpected_summary[] = "Live object summary: one object remains";
    constexpr char unexpected_reference_count[] = "Live ID3D12Device at 0x0000000000000001, Refcount: 3";

    EXPECT_FALSE(render_internal::IsKnownD3D12LiveObjectDiagnosticMessage(D3D12_MESSAGE_ID_LIVE_OBJECT_SUMMARY,
                                                                          unexpected_summary,
                                                                          sizeof(unexpected_summary)));
    EXPECT_FALSE(render_internal::IsKnownD3D12LiveObjectDiagnosticMessage(D3D12_MESSAGE_ID_LIVE_DEVICE,
                                                                          unexpected_reference_count,
                                                                          sizeof(unexpected_reference_count)));
    EXPECT_FALSE(render_internal::IsKnownD3D12LiveObjectDiagnosticMessage(D3D12_MESSAGE_ID_LIVE_DEVICE, nullptr, 0));
}

ACS_TEST(GpuLiveObjectDiagnostics, CompleteInspectionProducesMachineLeakVerdict)
{
    render_internal::GpuDebugMessageSummary summary{};
    summary.queue_available = true;
    summary.filter_override_attempted = true;
    summary.filter_override_succeeded = true;
    summary.filter_restore_succeeded = true;

    EXPECT_TRUE(render_internal::EvaluateGpuLiveObjectVerdict(S_OK, summary) ==
                render_internal::EGpuLiveObjectVerdict::NoLeak);

    summary.live_object_message_count = 1;
    EXPECT_TRUE(render_internal::EvaluateGpuLiveObjectVerdict(S_OK, summary) ==
                render_internal::EGpuLiveObjectVerdict::Leak);

    summary.inspection_truncated = true;
    EXPECT_TRUE(render_internal::EvaluateGpuLiveObjectVerdict(S_OK, summary) ==
                render_internal::EGpuLiveObjectVerdict::Inconclusive);
}

ACS_TEST(GpuLiveObjectDiagnostics, DxgiDiagnosticSequenceIsMonotonicUnderGlobalLock)
{
    u64 first_sequence = 0;
    u64 second_sequence = 0;
    {
        SRWLOCK& lock = render_internal::DxgiLiveObjectDiagnosticsLock();
        render_internal::DxgiLiveObjectDiagnosticsLockGuard guard(lock);
        first_sequence = render_internal::NextDxgiLiveObjectDiagnosticSequence();
    }
    {
        SRWLOCK& lock = render_internal::DxgiLiveObjectDiagnosticsLock();
        render_internal::DxgiLiveObjectDiagnosticsLockGuard guard(lock);
        second_sequence = render_internal::NextDxgiLiveObjectDiagnosticSequence();
    }

    EXPECT_TRUE(first_sequence != 0);
    EXPECT_EQ(second_sequence, first_sequence + 1u);
}

#endif // ACS_BUILD_DEBUG && ACS_PLATFORM_WINDOWS
