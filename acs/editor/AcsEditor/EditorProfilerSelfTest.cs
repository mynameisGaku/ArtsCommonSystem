// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace AcsEditor;

internal static class EditorProfilerSelfTest
{
    internal static int Run(TextWriter output)
    {
        int failures = 0;

        void Check(bool condition, string description)
        {
            if (condition)
                output.WriteLine("PASS: " + description);
            else
            {
                output.WriteLine("FAIL: " + description);
                failures++;
            }
        }

        bool RejectsCloudSnapshot(
            EditorCloudWorkloadSnapshot snapshot,
            int nativeResult = 1) =>
            EditorCloudWorkloadContract.ClassifyNativeResult(
                nativeResult,
                snapshot) ==
            EditorCloudWorkloadQueryStatus.ContractError;

        Check(Marshal.SizeOf<EditorProfilerSnapshot>() == 256,
            "managed profiler snapshot keeps the packed version-5 stage-timing ABI size");
        Check(EditorProfilerContract.Version == 5,
            "profiler contract requests the version-5 stage-timing ABI");
        Check(EditorProfilerContract.SnapshotSize == 256 &&
              EditorProfilerContract.LegacyVersion == 4 &&
              EditorProfilerContract.LegacySnapshotSize == 224,
            "profiler contract reports the packed snapshot size");
        Check(Marshal.SizeOf<EditorCloudWorkloadSnapshot>() == 200 &&
              EditorCloudWorkloadContract.Version == 2 &&
              EditorCloudWorkloadContract.SnapshotSize == 200,
            "cloud workload v2 uses an independent fixed 200-byte ABI");
        Check(EditorProfilerContract.Version == 5 &&
              EditorProfilerContract.SnapshotSize == 256,
            "cloud workload remains independent from profiler v5");

        EditorProfilerSnapshot resetBoundary = Snapshot(42, 300, 2.0f);
        resetBoundary.ProfilerResetSerial = 7;
        resetBoundary.PresentedFrameCountSinceReset = 0;
        bool captureBoundaryArmed =
            EditorProfilerCaptureBoundaryPolicy.TryArm(
                hadPreviousSnapshot: true,
                previousResetSerial: 6,
                hasResetSnapshot: true,
                resetBoundary,
                out ulong requiredResetSerial);
        EditorProfilerSnapshot firstPostReset =
            Snapshot(43, 300, 2.0f);
        firstPostReset.ProfilerResetSerial = 7;
        firstPostReset.PresentedFrameCountSinceReset = 1;
        EditorProfilerSnapshot staleGeneration = firstPostReset;
        staleGeneration.ProfilerResetSerial = 6;
        Check(
            captureBoundaryArmed &&
            requiredResetSerial == 7 &&
            EditorProfilerCaptureBoundaryPolicy.Accepts(
                firstPostReset,
                requiredResetSerial) &&
            !EditorProfilerCaptureBoundaryPolicy.Accepts(
                resetBoundary,
                requiredResetSerial) &&
            !EditorProfilerCaptureBoundaryPolicy.Accepts(
                staleGeneration,
                requiredResetSerial) &&
            !EditorProfilerCaptureBoundaryPolicy.TryArm(
                hadPreviousSnapshot: true,
                previousResetSerial: 7,
                hasResetSnapshot: true,
                resetBoundary,
                out _),
            "capture sampling waits for a presented frame from the exact reset generation");

        var workload = new EditorCloudWorkloadSnapshot
        {
            Version = EditorCloudWorkloadContract.Version,
            StructSize = EditorCloudWorkloadContract.SnapshotSize,
            Flags =
                EditorCloudWorkloadContract.FlagAttempted |
                EditorCloudWorkloadContract.FlagSubmitted |
                EditorCloudWorkloadContract.FlagHistoryWasAvailable |
                EditorCloudWorkloadContract.FlagHistoryReused |
                EditorCloudWorkloadContract.FlagTemporalSuperResolution,
            SkipReason = EditorCloudWorkloadContract.SkipNone,
            ProfilerFrameIndex = 42,
            SubmissionIndex = 7,
            TraceWidth = 40,
            TraceHeight = 24,
            OutputWidth = 160,
            OutputHeight = 96,
            SteadyDispatches = 2,
            OneTimeBakeDispatches = 4,
            ShadowCacheDispatches = 1,
            WorldShadowDispatches = 1,
            TotalComputeDispatches = 8,
            CompositeDraws = 1,
            TraceLogicalInvocations = 960,
            TraceLaunchedThreads = 960,
            ResolveLogicalInvocations = 15_360,
            ResolveLaunchedThreads = 15_360,
            OneTimeBakeLogicalInvocations = 2_637_824,
            OneTimeBakeLaunchedThreads = 2_637_824,
            ShadowCacheLogicalInvocations = 294_912,
            ShadowCacheLaunchedThreads = 294_912,
            WorldShadowLogicalInvocations = 65_536,
            WorldShadowLaunchedThreads = 65_536,
            TotalLogicalInvocations = 3_014_592,
            TotalLaunchedThreads = 3_014_592,
            MaximumViewSamples = 368_640,
            MaximumLightSamples = 2_949_120,
            MaximumWorldShadowSamples = 2_097_152,
        };
        Check(
            EditorCloudWorkloadContract.ClassifyNativeResult(
                1,
                workload) ==
                EditorCloudWorkloadQueryStatus.Available &&
            EditorCloudWorkloadFormatting.State(
                EditorCloudWorkloadQueryStatus.Available,
                workload) ==
                "SUBMITTED - frame 42 - cloud #7" &&
            EditorCloudWorkloadFormatting.Dispatches(workload) ==
                "8 = 2 steady + 4 bake + 1 internal shadow + 1 world shadow; 1 composite draw" &&
            EditorCloudWorkloadFormatting.Invocations(
                workload.TotalLogicalInvocations,
                workload.TotalLaunchedThreads) ==
                "3,014,592 logical / 3,014,592 launched" &&
            EditorCloudWorkloadFormatting.OneTimeBake(workload) ==
                "4 dispatch; 2,637,824 logical / 2,637,824 launched" &&
            EditorCloudWorkloadFormatting.History(workload) ==
                "REUSED - TSR 4x4",
            "cloud profiler presents exact dispatch, invocation, bake, and history accounting");

        var referenceWorkload = workload;
        referenceWorkload.Flags &=
            ~EditorCloudWorkloadContract.FlagTemporalSuperResolution;
        referenceWorkload.TraceWidth = referenceWorkload.OutputWidth;
        referenceWorkload.TraceHeight = referenceWorkload.OutputHeight;
        referenceWorkload.TraceLogicalInvocations = 15_360;
        referenceWorkload.TraceLaunchedThreads = 15_360;
        referenceWorkload.TotalLogicalInvocations = 3_028_992;
        referenceWorkload.TotalLaunchedThreads = 3_028_992;
        referenceWorkload.MaximumViewSamples = 7_864_320;
        referenceWorkload.MaximumLightSamples = 62_914_560;
        var unrecognizedViewCeiling = referenceWorkload;
        unrecognizedViewCeiling.MaximumViewSamples = 3_932_160;
        unrecognizedViewCeiling.MaximumLightSamples = 31_457_280;
        Check(
            EditorCloudWorkloadContract.ClassifyNativeResult(
                1,
                referenceWorkload) ==
                EditorCloudWorkloadQueryStatus.Available &&
            RejectsCloudSnapshot(unrecognizedViewCeiling),
            "cloud workload accepts the full-resolution 512-step reference ceiling and rejects unknown ceilings");

        var skippedWorkload = new EditorCloudWorkloadSnapshot
        {
            Version = EditorCloudWorkloadContract.Version,
            StructSize = EditorCloudWorkloadContract.SnapshotSize,
            Flags = EditorCloudWorkloadContract.FlagAttempted,
            SkipReason =
                EditorCloudWorkloadContract.SkipInvalidCamera,
        };
        var unavailableWorkload = new EditorCloudWorkloadSnapshot
        {
            Version = EditorCloudWorkloadContract.Version,
            StructSize = EditorCloudWorkloadContract.SnapshotSize,
        };
        var resourceUnavailableWorkload =
            new EditorCloudWorkloadSnapshot
            {
                Version = EditorCloudWorkloadContract.Version,
                StructSize =
                    EditorCloudWorkloadContract.SnapshotSize,
                Flags =
                    EditorCloudWorkloadContract.FlagAttempted,
                SkipReason =
                    EditorCloudWorkloadContract
                        .SkipResourcesNotReady,
            };
        var resourceUnavailableWithHistory =
            resourceUnavailableWorkload;
        resourceUnavailableWithHistory.Flags |=
            EditorCloudWorkloadContract.FlagHistoryWasAvailable;
        var invalidCameraWithHistory = skippedWorkload;
        invalidCameraWithHistory.Flags |=
            EditorCloudWorkloadContract.FlagHistoryWasAvailable |
            EditorCloudWorkloadContract.FlagHistoryInvalidated;
        Check(
            EditorCloudWorkloadContract.ClassifyNativeResult(
                1,
                skippedWorkload) ==
                EditorCloudWorkloadQueryStatus.Available &&
            EditorCloudWorkloadFormatting.State(
                EditorCloudWorkloadQueryStatus.Available,
                skippedWorkload) ==
                "SKIPPED - invalid camera" &&
            EditorCloudWorkloadContract.ClassifyNativeResult(
                0,
                unavailableWorkload) ==
                EditorCloudWorkloadQueryStatus.RuntimeUnavailable &&
            EditorCloudWorkloadFormatting.State(
                EditorCloudWorkloadQueryStatus.RuntimeUnavailable,
                unavailableWorkload) ==
                "UNAVAILABLE - renderer/cloud not ready" &&
            EditorCloudWorkloadContract.ClassifyNativeResult(
                0,
                resourceUnavailableWorkload) ==
                EditorCloudWorkloadQueryStatus.RuntimeUnavailable &&
            EditorCloudWorkloadContract.ClassifyNativeResult(
                0,
                resourceUnavailableWithHistory) ==
                EditorCloudWorkloadQueryStatus.RuntimeUnavailable &&
            EditorCloudWorkloadContract.ClassifyNativeResult(
                1,
                invalidCameraWithHistory) ==
                EditorCloudWorkloadQueryStatus.Available &&
            EditorCloudWorkloadFormatting.State(
                EditorCloudWorkloadQueryStatus.RuntimeUnavailable,
                resourceUnavailableWorkload) ==
                "UNAVAILABLE - cloud resources not ready" &&
            EditorCloudWorkloadContract.ClassifyNativeResult(
                -1,
                unavailableWorkload) ==
                EditorCloudWorkloadQueryStatus.ContractError &&
            EditorCloudWorkloadContract.IsSupported(
                EditorAbiCapability.VolumetricCloudWorkloadV2) &&
            !EditorCloudWorkloadContract.IsSupported(
                EditorAbiCapability.VolumetricCloudWorkloadV1) &&
            !EditorCloudWorkloadContract.IsSupported(
                EditorAbiCapability.ProfilerV3),
            "cloud workload negotiation distinguishes optional, unavailable, skipped, and ABI-error states");

        var unknownFlags = workload;
        unknownFlags.Flags |= 1u << 31;
        var nonzeroReserved = workload;
        nonzeroReserved.Reserved0 = 1;
        var nonzeroV2Reserved = workload;
        nonzeroV2Reserved.Reserved1 = 1;
        Check(
            RejectsCloudSnapshot(unknownFlags) &&
            RejectsCloudSnapshot(nonzeroReserved) &&
            RejectsCloudSnapshot(nonzeroV2Reserved),
            "cloud workload v2 rejects unknown flags and nonzero reserved fields");

        var unknownSkipReason = skippedWorkload;
        unknownSkipReason.SkipReason = 99;
        var submittedWithSkipReason = workload;
        submittedWithSkipReason.SkipReason =
            EditorCloudWorkloadContract.SkipInvalidCamera;
        var skippedWithoutReason = skippedWorkload;
        skippedWithoutReason.SkipReason =
            EditorCloudWorkloadContract.SkipNone;
        Check(
            RejectsCloudSnapshot(unknownSkipReason) &&
            RejectsCloudSnapshot(submittedWithSkipReason) &&
            RejectsCloudSnapshot(skippedWithoutReason) &&
            RejectsCloudSnapshot(
                resourceUnavailableWorkload,
                nativeResult: 1),
            "cloud workload v2 rejects unknown or native-result-inconsistent skip states");

        var incorrectDispatchTotal = workload;
        incorrectDispatchTotal.TotalComputeDispatches--;
        var dispatchedComponentWithoutDispatch = workload;
        dispatchedComponentWithoutDispatch.OneTimeBakeDispatches = 0;
        dispatchedComponentWithoutDispatch.TotalComputeDispatches = 4;
        var duplicatedShadowDispatch = workload;
        duplicatedShadowDispatch.ShadowCacheDispatches = 2;
        duplicatedShadowDispatch.TotalComputeDispatches = 9;
        var duplicatedWorldShadowDispatch = workload;
        duplicatedWorldShadowDispatch.WorldShadowDispatches = 2;
        duplicatedWorldShadowDispatch.TotalComputeDispatches = 9;
        var incorrectLogicalTotal = workload;
        incorrectLogicalTotal.TotalLogicalInvocations--;
        var incorrectSampleCeiling = workload;
        incorrectSampleCeiling.MaximumLightSamples--;
        var incorrectWorldSampleCeiling = workload;
        incorrectWorldSampleCeiling.MaximumWorldShadowSamples--;
        Check(
            RejectsCloudSnapshot(incorrectDispatchTotal) &&
            RejectsCloudSnapshot(dispatchedComponentWithoutDispatch) &&
            RejectsCloudSnapshot(duplicatedShadowDispatch) &&
            RejectsCloudSnapshot(duplicatedWorldShadowDispatch) &&
            RejectsCloudSnapshot(incorrectLogicalTotal) &&
            RejectsCloudSnapshot(incorrectSampleCeiling) &&
            RejectsCloudSnapshot(incorrectWorldSampleCeiling),
            "cloud workload v2 rejects repeated shadow dispatch, invocation-total, and sample-ceiling mismatches");

        var launchedBelowLogical = workload;
        launchedBelowLogical.TraceLaunchedThreads =
            launchedBelowLogical.TraceLogicalInvocations - 1;
        launchedBelowLogical.TotalLaunchedThreads--;
        Check(
            RejectsCloudSnapshot(launchedBelowLogical),
            "cloud workload v2 rejects launched-thread counts below logical work");

        var zeroSubmittedDimension = workload;
        zeroSubmittedDimension.TraceWidth = 0;
        Check(
            RejectsCloudSnapshot(zeroSubmittedDimension),
            "cloud workload v2 rejects submitted work with a zero trace or output dimension");

        var reusedWithoutHistory = workload;
        reusedWithoutHistory.Flags &=
            ~EditorCloudWorkloadContract.FlagHistoryWasAvailable;
        var reusedAndInvalidated = workload;
        reusedAndInvalidated.Flags |=
            EditorCloudWorkloadContract.FlagHistoryInvalidated;
        var missingRequiredTsrState = workload;
        missingRequiredTsrState.Flags &=
            ~EditorCloudWorkloadContract.FlagTemporalSuperResolution;
        var skippedWithTsr = skippedWorkload;
        skippedWithTsr.Flags |=
            EditorCloudWorkloadContract.FlagTemporalSuperResolution;
        Check(
            RejectsCloudSnapshot(reusedWithoutHistory) &&
            RejectsCloudSnapshot(reusedAndInvalidated) &&
            RejectsCloudSnapshot(missingRequiredTsrState) &&
            RejectsCloudSnapshot(skippedWithTsr),
            "cloud workload v2 rejects impossible history and temporal-super-resolution states");

        var staleDormantPayload = unavailableWorkload;
        staleDormantPayload.TotalComputeDispatches = 1;
        var malformedResourcesNotReady =
            resourceUnavailableWorkload;
        malformedResourcesNotReady.TraceWidth = 1;
        Check(
            RejectsCloudSnapshot(
                staleDormantPayload,
                nativeResult: 0) &&
            RejectsCloudSnapshot(
                malformedResourcesNotReady,
                nativeResult: 0),
            "cloud workload v2 unavailable states reject stale work while preserving clean dormant snapshots");

        var history = new EditorProfilerHistory(3);
        for (ulong frame = 1; frame <= 4; frame++)
        {
            EditorProfilerSnapshot snapshot = Snapshot(frame, 60 + frame, 10 + frame);
            Check(history.Add(snapshot), $"unique frame {frame} is sampled");
        }
        Check(history.Points.Count == 3 &&
              history.Points[0].FrameIndex == 2 &&
              history.Points[^1].FrameIndex == 4,
            "history is fixed-capacity and evicts the oldest sample");

        EditorProfilerSnapshot duplicate = Snapshot(4, 120, 99);
        Check(!history.Add(duplicate) && history.Points.Count == 3,
            "duplicate native frame is not sampled twice");

        history.IsPaused = true;
        Check(!history.Add(Snapshot(5, 120, 5)) &&
              history.Points[^1].FrameIndex == 4,
            "pause freezes history without mutating renderer state");
        history.IsPaused = false;
        Check(history.Add(Snapshot(5, 90, 20)),
            "resume accepts new samples");

        EditorProfilerAverage average = history.Average(2);
        Check(Math.Abs(average.Fps - 77.0f) < 0.01f &&
              Math.Abs(average.CpuFrameMs - 17.0f) < 0.01f &&
              average.GpuFrameMs < 0,
            "moving average uses the requested recent window and preserves unavailable GPU data");

        var budgetHistory = new EditorProfilerHistory(24);
        bool acceptedBudgetSamples = true;
        for (ulong frame = 1; frame <= 20; frame++)
        {
            EditorProfilerSnapshot sample = Snapshot(
                frame,
                300,
                frame >= 19 ? 5.0f : 2.0f);
            sample.TimingSource =
                EditorProfilerContract.TimingGpuTimestamp;
            sample.Flags |=
                EditorProfilerContract.FlagGpuTimingsValid;
            sample.GpuFrameMs = 2.5f;
            acceptedBudgetSamples &=
                budgetHistory.Add(sample);
        }
        Check(acceptedBudgetSamples,
            "unique budget-analysis samples are accepted");
        EditorProfilerBudgetAnalysis budget =
            EditorProfilerBudget.Analyze(
                budgetHistory.Points,
                targetFps: 300);
        Check(
            Math.Abs(budget.BudgetMilliseconds - (1000.0f / 300.0f)) <
                0.001f &&
            Math.Abs(budget.CpuP95Milliseconds - 5.0f) < 0.001f &&
            Math.Abs(budget.GpuP95Milliseconds - 2.5f) < 0.001f &&
            budget.CpuOverBudgetSamples == 2 &&
            budget.GpuOverBudgetSamples == 0 &&
            budget.State == EditorProfilerBudgetState.CpuBound,
            "300 FPS analysis exposes the 3.33 ms budget, sampled p95 and CPU bottleneck");
        Check(
            EditorProfilerBudget.StateLabel(budget) ==
                "CPU OVER BUDGET" &&
            EditorProfilerBudget.Percentile(
                budget.CpuP95Milliseconds,
                budget.CpuSampleCount) ==
                "5.00 ms · n=20" &&
            EditorProfilerBudget.Violations(2, 20) ==
                "2/20 (10.0%)",
            "budget labels retain sample provenance and exact violation rate");

        var noGpuHistory = new EditorProfilerHistory(3);
        Check(noGpuHistory.Add(Snapshot(1, 60, 2.0f)),
            "CPU-only budget sample is accepted");
        EditorProfilerBudgetAnalysis noGpuBudget =
            EditorProfilerBudget.Analyze(
                noGpuHistory.Points,
                targetFps: 300);
        Check(
            noGpuBudget.State ==
                EditorProfilerBudgetState.WithinBudget &&
            noGpuBudget.GpuSampleCount == 0 &&
            noGpuBudget.GpuP95Milliseconds < 0 &&
            EditorProfilerBudget.Percentile(
                noGpuBudget.GpuP95Milliseconds,
                noGpuBudget.GpuSampleCount) == "N/A",
            "missing GPU timestamps remain unavailable instead of becoming zero-cost samples");

        EditorProfilerSnapshot strayGpu =
            Snapshot(2, 300, 2.0f);
        strayGpu.GpuFrameMs = 0.25f;
        Check(noGpuHistory.Add(strayGpu),
            "CPU snapshot with stray GPU payload is accepted");
        EditorProfilerBudgetAnalysis strayGpuBudget =
            EditorProfilerBudget.Analyze(
                noGpuHistory.Points,
                targetFps: 300);
        Check(
            strayGpuBudget.GpuSampleCount == 0 &&
            strayGpuBudget.GpuP95Milliseconds < 0,
            "GPU budget ignores payloads that lack the timestamp-valid contract flag");

        string capture = EditorProfilerCapture.SerializeCsv(
            budgetHistory.Points,
            targetFps: 300);
        Check(
            capture.StartsWith(
                "# ACS Editor profiler capture",
                StringComparison.Ordinal) &&
            capture.Contains(
                "# target_fps,300",
                StringComparison.Ordinal) &&
            capture.Contains(
                "# frame_budget_ms,3.333333",
                StringComparison.Ordinal) &&
            capture.Contains(
                "frame_index,sample_timestamp,native_reported_fps," +
                "cpu_frame_ms,cpu_submit_ms," +
                "native_render_active_cpu_ms,native_present_cpu_ms," +
                "native_render_active_cpu_peak_ms," +
                "native_present_cpu_peak_ms," +
                "presented_frame_count_since_reset," +
                "profiler_reset_serial," +
                "gpu_query_ms,gpu_window_average_ms",
                StringComparison.Ordinal) &&
            capture.Contains(
                ",300,5,1,4,1,4,1,20,1,2.5",
                StringComparison.Ordinal) &&
            !capture.Contains(
                "5,0;1",
                StringComparison.Ordinal),
            "CSV capture is deterministic, invariant-culture and includes raw GPU queries");

        EditorProfilerSnapshot automationLatest =
            Snapshot(20, 300, 5.0f);
        automationLatest.TimingSource =
            EditorProfilerContract.TimingGpuTimestamp;
        automationLatest.Flags |=
            EditorProfilerContract.FlagGpuTimingsValid |
            EditorProfilerContract.FlagView3D |
            EditorProfilerContract.FlagClouds |
            EditorProfilerContract.FlagFog |
            EditorProfilerContract.FlagAerialPerspective;
        automationLatest.DrawCalls = 42;
        automationLatest.DispatchCalls = 8;
        automationLatest.Triangles = 12345;
        automationLatest.ViewportWidth = 1920;
        automationLatest.ViewportHeight = 1080;
        automationLatest.CloudWidth = 1280;
        automationLatest.CloudHeight = 720;
        automationLatest.CloudMarchSteps = 384;
        automationLatest.CloudLightSteps = 8;
        automationLatest.CloudRenderScale = 2.0f / 3.0f;
        automationLatest.GpuFrameMs = 2.5f;
        automationLatest.GpuFrameAverageMs = 2.4f;
        automationLatest.GpuFramePeakMs = 3.1f;
        automationLatest.GpuQueryWindowCount = 20;
        automationLatest.GpuQueryWindowCapacity = 120;
        automationLatest.GpuLatencyFrames = 2;
        automationLatest.OpaqueGpuAverageMs = 0.7f;
        automationLatest.OpaqueGpuWindowPeakMs = 0.9f;
        automationLatest.AtmosphereGpuAverageMs = 0.3f;
        automationLatest.AtmosphereGpuWindowPeakMs = 0.4f;
        automationLatest.CloudGpuAverageMs = 0.8f;
        automationLatest.CloudGpuWindowPeakMs = 1.1f;
        automationLatest.FogGpuAverageMs = 0.2f;
        automationLatest.FogGpuWindowPeakMs = 0.3f;
        automationLatest.PostGpuAverageMs = 0.4f;
        automationLatest.PostGpuWindowPeakMs = 0.6f;
        automationLatest.NativeRenderActiveCpuMs = 4.0f;
        automationLatest.NativePresentCpuMs = 1.0f;
        automationLatest.NativeRenderActiveCpuPeakMs = 4.0f;
        automationLatest.NativePresentCpuPeakMs = 1.0f;
        automationLatest.PresentedFrameCountSinceReset = 20;
        automationLatest.ProfilerResetSerial = 1;
        EditorProfilerPoint[] automationPoints =
            budgetHistory.Points.ToArray();
        automationPoints[0] =
            automationPoints[0] with
            {
                NativeRenderActiveCpuPeakMs = 6.25f,
                NativePresentCpuPeakMs = 2.75f,
            };
        EditorCloudWorkloadSnapshot automationWorkload = workload;
        automationWorkload.ProfilerFrameIndex = 20;
        var nativeDiagnostic =
            new ViewportNativeRenderDiagnostic(
                NativeCallCount: 900,
                SlowNativeCallCount: 2,
                GpuBackpressureYieldCount: 640,
                LastNativeCallMilliseconds: 0.21,
                MaximumNativeCallMilliseconds: 7.5,
                LastNativeCallKind: "render",
                GpuBackpressureInputRetryCount: 620,
                GpuBackpressureBackgroundFallbackCount: 7,
                GpuReadyAfterRetryCount: 600,
                RenderFairnessYieldCount: 120,
                LastGpuBackpressureEpochMilliseconds: 3.9,
                MaximumGpuBackpressureEpochMilliseconds: 8.2,
                PeakPresentedRenderBurstFrames: 8,
                PeakRenderBurstActiveCpuMilliseconds: 7.8,
                RenderInputContinuationYieldCount: 110,
                RenderMaintenanceYieldCount: 10,
                LastRenderContinuationQueueWaitMilliseconds: 0.3,
                MaximumRenderContinuationQueueWaitMilliseconds: 1.7,
                LastRenderMaintenanceQueueWaitMilliseconds: 0.8,
                MaximumRenderMaintenanceQueueWaitMilliseconds: 4.5);
        var dispatcherDiagnostic =
            new EditorDispatcherWatchdogSnapshot(
                HeartbeatCount: 40,
                HeartbeatAgeMilliseconds: 12.5,
                LastDispatcherGapMilliseconds: 500.1,
                MaximumDispatcherGapMilliseconds: 667.8,
                StallCount: 0,
                StallActive: false,
                ActiveStallMilliseconds: 0,
                LongestStallMilliseconds: 0,
                Phase: "Ready");
        var automationCapture =
            new EditorProfilerCaptureSnapshot(
                automationPoints,
                HasLatestSnapshot: true,
                automationLatest,
                EditorCloudWorkloadQueryStatus.Available,
                automationWorkload,
                HasNativeRenderDiagnostic: true,
                NativeRenderDiagnostic: nativeDiagnostic,
                HasDispatcherDiagnostic: true,
                DispatcherDiagnostic: dispatcherDiagnostic,
                RuntimePoints:
                [
                    new EditorProfilerRuntimePoint(
                        19,
                        1000,
                        600,
                        580,
                        6,
                        560,
                        110,
                        100,
                        10,
                        2.4,
                        0.2,
                        0.7,
                        12.0,
                        2.0),
                    new EditorProfilerRuntimePoint(
                        20,
                        2000,
                        640,
                        620,
                        7,
                        600,
                        120,
                        110,
                        10,
                        3.9,
                        0.3,
                        0.8,
                        500.1,
                        12.5),
                ]);
        EditorProfilerCaptureSummary automationSummary =
            EditorProfilerCaptureFile.Summarize(automationCapture);
        string automationCsv =
            EditorProfilerCaptureFile.SerializeCsv(
                automationCapture,
                targetFps: 300,
                automationSummary);
        Check(
            automationSummary.SampleCount == 20 &&
            automationSummary.EditorFps.SampleCount == 20 &&
            Math.Abs(
                automationSummary.EditorFps.Average!.Value -
                300.0) < 0.001 &&
            Math.Abs(
                automationSummary.EditorFpsFromP95FrameInterval!.Value -
                300.0) < 0.001 &&
            automationSummary.CpuFrameMilliseconds.SampleCount == 20 &&
            automationSummary.NativeRenderActiveCpuMilliseconds
                .SampleCount == 20 &&
            automationSummary.NativeRenderActiveCpuMilliseconds
                .P95 == 4.0 &&
            automationSummary.NativePresentCpuMilliseconds.P95 == 1.0 &&
            automationSummary.NativeRenderActiveCpuPeakMilliseconds is
                { } activePeak &&
            Math.Abs(activePeak - 6.25) < 0.001 &&
            automationSummary.NativePresentCpuPeakMilliseconds is
                { } presentPeak &&
            Math.Abs(presentPeak - 2.75) < 0.001 &&
            automationSummary.PresentedFrameCountSinceReset == 20 &&
            automationSummary.ProfilerResetSerial == 1 &&
            automationSummary.CaptureBoundaryValid &&
            Math.Abs(
                automationSummary.CpuFrameMilliseconds.P95!.Value -
                5.0) < 0.001 &&
            automationSummary.GpuQueryMilliseconds.SampleCount == 20 &&
            Math.Abs(
                automationSummary.GpuQueryMilliseconds.P95!.Value -
                2.5) < 0.001 &&
            automationSummary.LatestGpuQueryWindow.Available &&
            automationSummary.LatestGpuQueryWindow.QueryCount == 20 &&
            Math.Abs(
                automationSummary.LatestGpuQueryWindow
                    .CloudAverageMilliseconds!.Value -
                0.8) < 0.001 &&
            automationSummary.LatestRenderState.Available &&
            automationSummary.LatestRenderState.View3D &&
            automationSummary.LatestRenderState.CloudsEnabled &&
            automationSummary.LatestRenderState.DrawCalls == 42 &&
            automationSummary.LatestRenderState.DispatchCalls == 8 &&
            automationSummary.LatestRenderState.Triangles == 12345 &&
            automationSummary.LatestRenderState.CloudMarchSteps == 384 &&
            automationSummary.LatestCloudWorkload.Available &&
            automationSummary.LatestCloudWorkload.Submitted &&
            automationSummary.LatestCloudWorkload
                .ProfilerFrameWithinCapture &&
            automationLatest.NativeRenderActiveCpuPeakMs <
                activePeak &&
            automationLatest.NativePresentCpuPeakMs <
                presentPeak &&
            automationSummary.LatestCloudWorkload.HistoryReused &&
            automationSummary.LatestCloudWorkload.TotalComputeDispatches == 8 &&
            automationSummary.LatestCloudWorkload.WorldShadowDispatches == 1 &&
            automationSummary.LatestCloudWorkload
                .WorldShadowLogicalInvocations == 65_536 &&
            automationSummary.LatestCloudWorkload
                .MaximumWorldShadowSamples == 2_097_152 &&
            automationSummary.LatestEditorRuntime.NativeAvailable &&
            automationSummary.LatestEditorRuntime.NativeCallCount == 900 &&
            automationSummary.LatestEditorRuntime
                .GpuBackpressureYieldCount == 640 &&
            automationSummary.LatestEditorRuntime
                .GpuBackpressureInputRetryCount == 620 &&
            automationSummary.LatestEditorRuntime
                .GpuBackpressureBackgroundFallbackCount == 7 &&
            automationSummary.LatestEditorRuntime
                .GpuReadyAfterRetryCount == 600 &&
            automationSummary.LatestEditorRuntime
                .RenderFairnessYieldCount == 120 &&
            automationSummary.LatestEditorRuntime
                .LastGpuBackpressureEpochMilliseconds == 3.9 &&
            automationSummary.LatestEditorRuntime
                .MaximumGpuBackpressureEpochMilliseconds == 8.2 &&
            automationSummary.LatestEditorRuntime
                .PeakPresentedRenderBurstFrames == 8 &&
            automationSummary.LatestEditorRuntime
                .PeakRenderBurstActiveCpuMilliseconds == 7.8 &&
            automationSummary.LatestEditorRuntime
                .RenderInputContinuationYieldCount == 110 &&
            automationSummary.LatestEditorRuntime
                .RenderMaintenanceYieldCount == 10 &&
            automationSummary.LatestEditorRuntime
                .MaximumRenderContinuationQueueWaitMilliseconds == 1.7 &&
            automationSummary.LatestEditorRuntime
                .MaximumRenderMaintenanceQueueWaitMilliseconds == 4.5 &&
            automationSummary.LatestEditorRuntime.DispatcherAvailable &&
            automationSummary.LatestEditorRuntime
                .MaximumDispatcherGapMilliseconds == 667.8 &&
            automationSummary.RuntimeTimeline.SampleCount == 2 &&
            automationSummary.RuntimeTimeline
                .RenderContinuationQueueWaitMilliseconds.P95 == 0.3 &&
            automationSummary.RuntimeTimeline
                .RenderMaintenanceQueueWaitMilliseconds.P95 == 0.8,
            "automation summary reports exact samples, GPU work, backpressure, and dispatcher evidence");
        Check(
            EditorProfilerCaptureValidation
                .Expected3DRenderFaults(automationSummary)
                .Length == 0,
            "3D automation validation accepts an active non-suppressed render with submitted work");
        EditorCloudWorkloadSnapshot outsideCaptureWorkload =
            automationWorkload;
        outsideCaptureWorkload.ProfilerFrameIndex = 21;
        EditorProfilerCaptureSummary outsideCloudSummary =
            EditorProfilerCaptureFile.Summarize(
                automationCapture with
                {
                    LatestCloudWorkload = outsideCaptureWorkload,
                });
        Check(
            !outsideCloudSummary.LatestCloudWorkload
                .ProfilerFrameWithinCapture &&
            EditorProfilerCaptureValidation
                .Expected3DRenderFaults(outsideCloudSummary)
                .Contains(
                    "PROFILER_CAPTURE_CLOUD_FRAME_OUTSIDE_CAPTURE",
                    StringComparer.Ordinal),
            "3D automation validation rejects a cloud workload published outside the reset-bounded capture");
        EditorProfilerPoint[] mixedGenerationPoints =
            automationPoints.ToArray();
        mixedGenerationPoints[0] =
            mixedGenerationPoints[0] with
            {
                ProfilerResetSerial = 2,
            };
        EditorProfilerCaptureSummary mixedGenerationSummary =
            EditorProfilerCaptureFile.Summarize(
                automationCapture with
                {
                    Points = mixedGenerationPoints,
                });
        Check(
            !mixedGenerationSummary.CaptureBoundaryValid &&
            EditorProfilerCaptureValidation
                .Expected3DRenderFaults(mixedGenerationSummary)
                .Contains(
                    "PROFILER_CAPTURE_INVALID_RESET_BOUNDARY",
                    StringComparer.Ordinal),
            "automation validation rejects samples from a different reset generation");
        EditorProfilerPoint[] invalidRollingPeakPoints =
            automationPoints.ToArray();
        invalidRollingPeakPoints[5] =
            invalidRollingPeakPoints[5] with
            {
                NativeRenderActiveCpuPeakMs =
                    invalidRollingPeakPoints[5]
                        .NativeRenderActiveCpuMs - 0.01f,
            };
        EditorProfilerCaptureSummary invalidRollingPeakSummary =
            EditorProfilerCaptureFile.Summarize(
                automationCapture with
                {
                    Points = invalidRollingPeakPoints,
                });
        Check(
            !invalidRollingPeakSummary.CaptureBoundaryValid &&
            EditorProfilerCaptureValidation
                .Expected3DRenderFaults(invalidRollingPeakSummary)
                .Contains(
                    "PROFILER_CAPTURE_INVALID_RESET_BOUNDARY",
                    StringComparer.Ordinal),
            "automation validation checks each rolling peak against its own current sample");
        EditorProfilerCaptureSummary missingCloudProofSummary =
            EditorProfilerCaptureFile.Summarize(
                new EditorProfilerCaptureSnapshot(
                    automationPoints,
                    HasLatestSnapshot: true,
                    automationLatest));
        string[] missingProofFaults =
            EditorProfilerCaptureValidation.Expected3DRenderFaults(
                missingCloudProofSummary);
        Check(
            missingProofFaults.Contains(
                    "PROFILER_CAPTURE_NO_CLOUD_WORKLOAD",
                    StringComparer.Ordinal) &&
            missingProofFaults.Contains(
                "PROFILER_CAPTURE_NO_EDITOR_RUNTIME_DIAGNOSTICS",
                StringComparer.Ordinal),
            "3D automation validation rejects timings without exact cloud-work and editor-runtime evidence");
        EditorProfilerSnapshot rejected3DLatest = automationLatest;
        rejected3DLatest.Flags &=
            ~(EditorProfilerContract.FlagView3D |
              EditorProfilerContract.FlagClouds);
        rejected3DLatest.DrawCalls = 0;
        rejected3DLatest.DispatchCalls = 0;
        EditorProfilerCaptureSummary rejected3DSummary =
            EditorProfilerCaptureFile.Summarize(
                new EditorProfilerCaptureSnapshot(
                    automationPoints,
                    HasLatestSnapshot: true,
                    rejected3DLatest));
        string[] rejected3DFaults =
            EditorProfilerCaptureValidation.Expected3DRenderFaults(
                rejected3DSummary);
        Check(
            rejected3DFaults.Contains(
                "PROFILER_CAPTURE_EXPECTED_VIEW3D",
                StringComparer.Ordinal) &&
            rejected3DFaults.Contains(
                "PROFILER_CAPTURE_NO_RENDER_WORK",
                StringComparer.Ordinal),
            "3D automation validation rejects a mislabeled empty 2D capture");
        Check(
            automationCsv.Contains(
                "# sample_count,20",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_fps_average,300",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_fps_from_p95_frame_interval,300",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# gpu_query_ms_sample_count,20",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# native_render_active_cpu_ms_p95,4",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# native_present_cpu_ms_p95,1",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# native_render_active_cpu_peak_ms,6.25",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# native_present_cpu_peak_ms,2.75",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# presented_frame_count_since_reset,20",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# profiler_reset_serial,1",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# capture_reset_boundary_valid,true",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# gpu_window_available,true",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# gpu_window_cloud_average_ms,0.8",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# render_view3d,true",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# render_draw_calls,42",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# render_cloud_march_steps,384",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# cloud_workload_submitted,true",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# cloud_workload_total_dispatches,8",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# cloud_workload_world_shadow_dispatches,1",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# cloud_workload_maximum_world_shadow_samples,2097152",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# cloud_workload_profiler_frame_within_capture,true",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_gpu_backpressure_yield_count,640",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_gpu_backpressure_input_retry_count,620",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_gpu_backpressure_background_fallback_count,7",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_gpu_ready_after_retry_count,600",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_render_fairness_yield_count,120",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_maximum_gpu_backpressure_epoch_ms,8.2",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_peak_presented_render_burst_frames,8",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_peak_render_burst_active_cpu_ms,7.8",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_render_input_continuation_yield_count,110",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_maximum_render_maintenance_queue_wait_ms,4.5",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# runtime_timeline_sample_count,2",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# runtime_timeline_sample,20,2000,640,620,7,600,120,110,10,3.9,0.3,0.8,500.1,12.5",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# editor_runtime_dispatcher_maximum_gap_ms,667.8",
                StringComparison.Ordinal) &&
            automationCsv.Contains(
                "# gpu_pass_contract,latest native unique-query",
                StringComparison.Ordinal),
            "automation CSV separates cadence, GPU queries, renderer backpressure, and dispatcher evidence");

        long cadenceStep =
            Math.Max(1, System.Diagnostics.Stopwatch.Frequency / 10);
        var cadenceHistory = new EditorProfilerHistory(8);
        for (int index = 0; index < 4; index++)
        {
            EditorProfilerSnapshot cadenceSnapshot =
                Snapshot(
                    100u + (ulong)(index * 30),
                    fps: 1200,
                    cpuFrameMs: 2.0f);
            Check(
                cadenceHistory.Add(
                    cadenceSnapshot,
                    sampleTimestamp: 1L + index * cadenceStep),
                $"observed-cadence sample {index + 1} is accepted");
        }
        EditorProfilerAverage cadenceAverage =
            cadenceHistory.Average(sampleCount: 4);
        var cadenceCapture = new EditorProfilerCaptureSnapshot(
            cadenceHistory.Points.ToArray(),
            HasLatestSnapshot: true,
            automationLatest);
        EditorProfilerCaptureSummary cadenceSummary =
            EditorProfilerCaptureFile.Summarize(cadenceCapture);
        Check(
            Math.Abs(cadenceAverage.Fps - 300.0f) < 0.1f &&
            cadenceSummary.UsesObservedCadence &&
            cadenceSummary.NativeReportedFps.Average is { } nativeFps &&
            Math.Abs(nativeFps - 1200.0) < 0.001 &&
            cadenceSummary.EditorFps.Average is { } editorFps &&
            Math.Abs(editorFps - 300.0) < 0.1 &&
            cadenceSummary.ObservedFrameIntervalMilliseconds.P95 is
                { } observedInterval &&
            Math.Abs(observedInterval - (1000.0 / 300.0)) < 0.01,
            "wall-clock/frame-index cadence replaces native burst FPS after a stable observation span");

        string automationRoot = Path.Combine(
            Path.GetTempPath(),
            $"acs-profiler-capture-selftest-{Guid.NewGuid():N}");
        Directory.CreateDirectory(automationRoot);
        try
        {
            string destination = Path.Combine(
                automationRoot,
                "capture.csv");
            Check(
                EditorProfilerCaptureFile
                    .TryNormalizeAutomationDestination(
                        destination,
                        out string normalized,
                        out string? normalizeError) &&
                string.Equals(
                    normalized,
                    Path.GetFullPath(destination),
                    StringComparison.OrdinalIgnoreCase) &&
                normalizeError == null,
                "automation capture accepts only an explicit regular CSV below the process TEMP root");

            File.WriteAllText(destination, "preserve-before-atomic-move");
            Check(
                EditorProfilerCaptureFile.TryWriteAtomic(
                    destination,
                    automationCsv,
                    out string published,
                    out string? publishError) &&
                string.Equals(
                    published,
                    Path.GetFullPath(destination),
                    StringComparison.OrdinalIgnoreCase) &&
                publishError == null &&
                File.ReadAllText(destination) == automationCsv &&
                Directory.GetFiles(
                    automationRoot,
                    ".acs-profiler-*.tmp").Length == 0,
                "automation capture atomically replaces a regular destination and removes only its unique sibling temporary");

            File.WriteAllText(destination, "preserve-on-limit-failure");
            string oversized =
                new('x',
                    EditorProfilerCaptureFile.MaximumCaptureBytes + 1);
            Check(
                !EditorProfilerCaptureFile.TryWriteAtomic(
                    destination,
                    oversized,
                    out _,
                    out string? oversizedError) &&
                oversizedError != null &&
                File.ReadAllText(destination) ==
                    "preserve-on-limit-failure",
                "automation capture enforces its byte limit without truncating the existing destination");

            string outsideTemp = Path.Combine(
                Path.GetPathRoot(Path.GetTempPath())!,
                "acs-profiler-outside.csv");
            Check(
                !EditorProfilerCaptureFile
                    .TryNormalizeAutomationDestination(
                        outsideTemp,
                        out _,
                        out _) &&
                !EditorProfilerCaptureFile
                    .TryNormalizeAutomationDestination(
                        Path.Combine(automationRoot, "capture.json"),
                        out _,
                        out _) &&
                !EditorProfilerCaptureFile
                    .IsSafeDirectoryAttributes(
                        FileAttributes.Directory |
                        FileAttributes.ReparsePoint) &&
                !EditorProfilerCaptureFile.IsRegularFile(
                    FileAttributes.ReparsePoint) &&
                EditorProfilerCaptureFile.IsRegularFile(
                    FileAttributes.Archive),
                "automation capture rejects paths outside TEMP, non-CSV output, reparse directories, and non-regular files");
        }
        finally
        {
            Directory.Delete(automationRoot, recursive: true);
        }

        EditorProfilerSnapshot cpuSnapshot = Snapshot(9, 60, 16.6f);
        Check(EditorProfilerFormatting.TimingSource(cpuSnapshot) == "CPU record / submit",
            "CPU timings are explicitly labelled and never presented as GPU timings");
        cpuSnapshot.TimingSource = EditorProfilerContract.TimingGpuTimestamp;
        cpuSnapshot.Flags |= EditorProfilerContract.FlagGpuTimingsValid;
        cpuSnapshot.GpuFrameMs = 8.25f;
        cpuSnapshot.GpuLatencyFrames = 2;
        Check(EditorProfilerFormatting.TimingSource(cpuSnapshot) ==
              "GPU timestamp queries (+2f async; warming up)",
            "timestamp-query snapshots expose latency while the rolling window warms");
        cpuSnapshot.GpuQueryWindowCount = 120;
        cpuSnapshot.GpuQueryWindowCapacity = 120;
        cpuSnapshot.GpuFrameAverageMs = 5.25f;
        cpuSnapshot.OpaqueGpuAverageMs = 1.0f;
        cpuSnapshot.AtmosphereGpuAverageMs = 0.75f;
        cpuSnapshot.CloudGpuAverageMs = 2.25f;
        cpuSnapshot.FogGpuAverageMs = 0.25f;
        cpuSnapshot.PostGpuAverageMs = 1.0f;
        cpuSnapshot.OpaqueGpuWindowPeakMs = 1.4f;
        cpuSnapshot.AtmosphereGpuWindowPeakMs = 1.1f;
        cpuSnapshot.CloudGpuWindowPeakMs = 2.9f;
        cpuSnapshot.FogGpuWindowPeakMs = 0.4f;
        cpuSnapshot.PostGpuWindowPeakMs = 1.3f;
        Check(EditorProfilerFormatting.TimingSource(cpuSnapshot) ==
              "GPU timestamp queries (+2f async; 120/120q avg)",
            "timestamp-query label reports latency and the unique valid query count");
        var gpuHistory = new EditorProfilerHistory(3);
        Check(gpuHistory.Add(cpuSnapshot) &&
              Math.Abs(gpuHistory.Average().GpuFrameMs - 5.25f) < 0.001f,
            "headline GPU time uses the native unique-query average instead of the latest query");
        Check(EditorProfilerFormatting.GpuAveragePeak(
                  cpuSnapshot.CloudGpuAverageMs,
                  cpuSnapshot.CloudGpuWindowPeakMs,
                  cpuSnapshot.GpuQueryWindowCount) == " 2.25 / 2.90 ms" &&
              EditorProfilerFormatting.GpuAveragePeak(2.0f, 3.0f, 0) == "   N/A",
            "per-pass GPU formatting shows the native-query average and peak only when valid");
        Check(EditorProfilerFormatting.GpuWindowTooltip(120, 120, 2) ==
              "Average / peak across 120/120 unique valid GPU queries. Latest completed query is +2 frames asynchronous.",
            "per-pass tooltip preserves query validity and asynchronous latency context");
        Check(EditorProfilerFormatting.Peak(21.5f, 120) ==
              "peak 21.50 · 120f" &&
              EditorProfilerFormatting.Peak(9.25f, 120, 2) ==
              "peak 9.25 · 120f · +2f" &&
              EditorProfilerFormatting.Peak(-1, 120) == "peak N/A",
            "rolling peaks format their native window, latency and unavailable state");
        Check(Math.Abs(EditorProfilerFormatting.GpuThroughputFps(5.0f) - 200.0f) < 0.001f &&
              EditorProfilerFormatting.GpuThroughputFps(0.0f) < 0 &&
              EditorProfilerFormatting.GpuThroughput(-1.0f) == "throughput N/A",
            "GPU throughput FPS is derived from valid timestamp averages only");
        Check(EditorProfilerFormatting.CompactSummary(
                  new EditorProfilerAverage(84.0f, 0.72f, 0.32f, 4.91f)) ==
              "Editor 84 FPS  |  GPU 204 FPS / 4.91 ms  |  CPU 0.72 ms",
            "compact summary distinguishes editor cadence from GPU throughput");
        var cullingSnapshot = new EditorProfilerSnapshot
        {
            Flags =
                EditorProfilerContract.FlagFrustumCullingEnabled |
                EditorProfilerContract.FlagGameView |
                EditorProfilerContract.FlagRuntimeSceneCamera,
            FrustumTested = 420,
            FrustumVisible = 125,
            FrustumCulled = 295,
            ActiveCameraNodeId = 17,
        };
        Check(
            EditorProfilerFormatting.CullingState(cullingSnapshot) ==
                "ON · Camera #17" &&
            EditorProfilerFormatting.CullingCounts(cullingSnapshot) ==
                "420 / 125 / 295" &&
            EditorProfilerFormatting.CullingState(default) == "DISABLED",
            "camera culling diagnostics expose state, exact counts and the runtime camera");
        cullingSnapshot.Flags =
            EditorProfilerContract.FlagFrustumCullingEnabled |
            EditorProfilerContract.FlagGameView;
        Check(
            EditorProfilerFormatting.CullingState(cullingSnapshot) ==
                "ON · Game fallback",
            "camera culling diagnostics distinguish deterministic game fallback from editor navigation");
        Check(
            EditorProfilerPresentationPolicy.ShouldPresentDetails(
                panelVisible: true,
                nativeFrameAdvanced: true) &&
            !EditorProfilerPresentationPolicy.ShouldPresentDetails(
                panelVisible: false,
                nativeFrameAdvanced: true) &&
            !EditorProfilerPresentationPolicy.ShouldPresentDetails(
                panelVisible: true,
                nativeFrameAdvanced: false) &&
            EditorProfilerPresentationPolicy.ShouldPresentManagedDiagnostics(
                panelVisible: true) &&
            !EditorProfilerPresentationPolicy.ShouldPresentManagedDiagnostics(
                panelVisible: false),
            "native history waits for frame advance while visible managed diagnostics remain live");
        Check(
            EditorProfilerPresentationPolicy.SampleInterval(
                panelVisible: true) ==
                TimeSpan.FromMilliseconds(100) &&
            EditorProfilerPresentationPolicy.SampleInterval(
                panelVisible: false) ==
                TimeSpan.FromMilliseconds(500),
            "collapsed profiler docks reduce managed sampling load while visible diagnostics stay responsive");

        Check(!App.ShouldForceSoftwareUi(Array.Empty<string>(), false, 2) &&
              App.ShouldForceSoftwareUi(Array.Empty<string>(), true, 2) &&
              !App.ShouldForceSoftwareUi(Array.Empty<string>(), false, 0) &&
              App.ShouldForceSoftwareUi(new[] { "--SOFTWARE-UI" }, false, 2),
            "WPF software composition is limited to remote or explicit launches");
        Check(!App.ShouldAvoidInitialActivation(Array.Empty<string>()) &&
              App.ShouldAvoidInitialActivation(new[] { "--NO-ACTIVATE" }) &&
              App.ShouldAvoidInitialActivation(new[] { "--unattended" }) &&
              !App.ShouldRunUnattended(new[] { "--no-activate" }) &&
              App.ShouldRunUnattended(new[] { "--UNATTENDED" }),
            "initial no-activate remains interactive while unattended alone permanently gates input");
        Check(App.ShouldDeferInteractivePromptsUntilActivation(
                  new[] { "--NO-ACTIVATE" }) &&
              !App.ShouldDeferInteractivePromptsUntilActivation(
                  Array.Empty<string>()) &&
              !App.ShouldDeferInteractivePromptsUntilActivation(
                  new[] { "--unattended", "--no-activate" }) &&
              MainWindow.ShouldDeferInitialRecoveryPrompt(true, false) &&
              !MainWindow.ShouldDeferInitialRecoveryPrompt(true, true) &&
              !MainWindow.ShouldDeferInitialRecoveryPrompt(false, false) &&
              !MainWindow.ShouldDeferInitialRecoveryPrompt(
                  true,
                  false,
                  allowWhileInactive: true),
            "inactive launches defer recovery until activation except for the non-activating reliability fixture");
        Check(!SceneRecoveryDialog.DisablesOwnerDuringPrompt &&
              SceneRecoveryDialog.CanPresentModelessPrompt(true, true, true) &&
              !SceneRecoveryDialog.CanPresentModelessPrompt(false, true, true) &&
              !SceneRecoveryDialog.CanPresentModelessPrompt(true, false, true) &&
              !SceneRecoveryDialog.CanPresentModelessPrompt(true, true, false),
            "scene recovery is an owned modeless prompt and can never disable the editor window");
        Check(App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: false,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: true,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: 0x000F,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0200) &&
              App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0246),
            "initial no-activate guard releases only for an explicit mouse activation and never in unattended mode");
        Check(EditorCommandPaletteWindow.ShouldBeginHeaderDrag(100, 20, 720) &&
              !EditorCommandPaletteWindow.ShouldBeginHeaderDrag(700, 20, 720) &&
              !EditorCommandPaletteWindow.ShouldBeginHeaderDrag(100, 40, 720) &&
              !EditorCommandPaletteWindow.ShouldBeginHeaderDrag(double.NaN, 20, 720),
            "the modal command palette exposes a draggable header without stealing its close button");

        Check(EngineViewport.RenderContinuationPriority ==
                  System.Windows.Threading.DispatcherPriority.Input &&
              ProfilerPanel.SamplePriority ==
                  System.Windows.Threading.DispatcherPriority.Input &&
              EngineViewport.RenderMaintenancePriority ==
                  System.Windows.Threading.DispatcherPriority.Background &&
              EngineViewport.RenderFairnessPriority ==
                  EngineViewport.RenderMaintenancePriority &&
              EngineViewport.DormantWakePriority ==
                  System.Windows.Threading.DispatcherPriority.Background &&
              EngineViewport.ShouldRenderContinuously(
                  false, false, true, true, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  true, false, true, true, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, true, true, true, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, false, true, true, true, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, false, true, false, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, false, true, true, false, 0, 720),
            "native render checkpoints and profiler sampling share FIFO Input priority while bounded Background drains preserve lower-priority progress");
        Check(
            EngineViewport.ShouldRouteEditorViewportInteraction(
                gameView: false) &&
            !EngineViewport.ShouldRouteEditorViewportInteraction(
                gameView: true),
            "Game View routes gameplay input without mutating the independent editor navigation camera");
        Check(
            !EngineViewport.ShouldRouteGameplayInput(
                gameView: false,
                logicPlayActive: true) &&
            !EngineViewport.ShouldRouteGameplayInput(
                gameView: true,
                logicPlayActive: false) &&
            EngineViewport.ShouldRouteGameplayInput(
                gameView: true,
                logicPlayActive: true),
            "gameplay input is isolated to an active Play session shown through Game View");
        EditorViewSwitchPlan sceneWhilePlaying =
            EditorViewSwitchPolicy.Plan(gameView: false, playState: 1);
        EditorViewSwitchPlan gameWhilePlaying =
            EditorViewSwitchPolicy.Plan(gameView: true, playState: 1);
        Check(
            !sceneWhilePlaying.StartPlay &&
            !sceneWhilePlaying.StopPlay &&
            !sceneWhilePlaying.MutateEditorNavigationCamera &&
            !gameWhilePlaying.StartPlay &&
            !gameWhilePlaying.StopPlay &&
            !gameWhilePlaying.MutateEditorNavigationCamera,
            "Scene/Game tab switching keeps Play running and preserves the exact editor camera pose");
        Check(EngineViewport.MaxDirectRenderBurstFrames == 64 &&
              Math.Abs(EngineViewport.MaxDirectRenderBurstMilliseconds - 64.0) < 0.001 &&
              !EngineViewport.ShouldYieldRenderBurst(63, 63.999) &&
              EngineViewport.ShouldYieldRenderBurst(64, 0.0) &&
              EngineViewport.ShouldYieldRenderBurst(1, 64.0) &&
              EngineViewport.ShouldYieldRenderBurst(80, 0.0) &&
              !EngineViewport.RequiresRenderDispatcherCheckpoint(
                  new ViewportRenderBurstState(63, 63.999)) &&
              EngineViewport.RequiresRenderDispatcherCheckpoint(
                  new ViewportRenderBurstState(64, 0.0)) &&
              EngineViewport.RequiresRenderDispatcherCheckpoint(
                  new ViewportRenderBurstState(1, 64.0)),
            "input-aware direct render bursts must enter a Dispatcher checkpoint by 64 frames or the 64-millisecond steady-state budget");
        ViewportRenderBurstState busyBurstSeed = new(3, 2.5);
        ViewportRenderBurstState busyBurstResult =
            EngineViewport.AccountRenderBurstAttempt(
                busyBurstSeed,
                presented: true,
                gpuBackpressure: true,
                activeCpuMilliseconds: 1_000.0);
        ViewportRenderBurstState presentedBurst = default;
        for (int presentedFrame = 0;
             presentedFrame < EngineViewport.MaxDirectRenderBurstFrames;
             presentedFrame++)
        {
            presentedBurst = EngineViewport.AccountRenderBurstAttempt(
                presentedBurst,
                presented: true,
                gpuBackpressure: false,
                activeCpuMilliseconds: 0.125);
        }
        ViewportRenderBurstState cpuBudgetBurst =
            EngineViewport.AccountRenderBurstAttempt(
                default,
                presented: false,
                gpuBackpressure: false,
                activeCpuMilliseconds:
                    EngineViewport.MaxDirectRenderBurstMilliseconds);
        ViewportRenderBurstState failedAccountingBurst =
            EngineViewport.AccountRenderBurstAttempt(
                default,
                presented: false,
                gpuBackpressure: false,
                activeCpuMilliseconds: double.NaN);
        Check(
            busyBurstResult == busyBurstSeed &&
            presentedBurst.PresentedFrames ==
                EngineViewport.MaxDirectRenderBurstFrames &&
            Math.Abs(presentedBurst.ActiveCpuMilliseconds - 8.0) <
                0.000001 &&
            EngineViewport.ShouldYieldRenderBurst(presentedBurst) &&
            EngineViewport.ShouldYieldRenderBurst(cpuBudgetBurst) &&
            !EngineViewport.ShouldYieldRenderBurst(
                default(ViewportRenderBurstState)) &&
            EngineViewport.ShouldYieldRenderBurst(
                new ViewportRenderBurstState(-1, 0.0)) &&
            failedAccountingBurst.PresentedFrames ==
                EngineViewport.MaxDirectRenderBurstFrames &&
            failedAccountingBurst.ActiveCpuMilliseconds ==
                EngineViewport.MaxDirectRenderBurstMilliseconds &&
            EngineViewport.ShouldYieldRenderBurst(failedAccountingBurst),
            "render fairness counts Presented frames and active CPU work, excludes GPU-busy wall time, resets cleanly, and fails closed");
        Check(
            EngineViewport.SlowNativeCallThresholdMilliseconds == 50.0 &&
            EngineViewport.MaximumNativeDeltaSeconds == 0.1 &&
            EngineViewport.ShouldYieldForGpuBackpressure(0) &&
            !EngineViewport.ShouldYieldForGpuBackpressure(1) &&
            !EngineViewport.ShouldYieldForGpuBackpressure(-1),
            "GPU frame-slot backpressure selects a cooperative resume without treating success or invalid handles as saturation");
        Check(
            EngineViewport.GpuBackpressureRetryPriority ==
                System.Windows.Threading.DispatcherPriority.Input &&
            EngineViewport.MaxGpuBackpressureInputRetries == 256 &&
            Math.Abs(
                EngineViewport.MaxGpuBackpressureInputRetryMilliseconds -
                8.0) < 0.001 &&
            EngineViewport.SelectGpuBackpressureResume(0, 0.0) ==
                ViewportGpuBackpressureResumeMode.InputPriorityRetry &&
            EngineViewport.SelectGpuBackpressureResume(255, 7.999) ==
                ViewportGpuBackpressureResumeMode.InputPriorityRetry &&
            EngineViewport.SelectGpuBackpressureResume(256, 0.0) ==
                ViewportGpuBackpressureResumeMode.CooperativeYield &&
            EngineViewport.SelectGpuBackpressureResume(0, 8.000) ==
                ViewportGpuBackpressureResumeMode.CooperativeYield &&
            EngineViewport.SelectGpuBackpressureResume(-1, 0.0) ==
                ViewportGpuBackpressureResumeMode.CooperativeYield &&
            EngineViewport.SelectGpuBackpressureResume(0, double.NaN) ==
                ViewportGpuBackpressureResumeMode.CooperativeYield &&
            EngineViewport.SelectGpuBackpressureResume(
                    0,
                    double.PositiveInfinity) ==
                ViewportGpuBackpressureResumeMode.CooperativeYield,
            "GPU busy retries remain bounded before a mandatory Dispatcher checkpoint and periodic Background drain");
        Check(
            Math.Abs(
                EngineViewport.MaxRenderMaintenanceIntervalMilliseconds -
                500.0) < 0.001 &&
            EngineViewport.SelectRenderYieldMode(
                startupMaintenanceRequired: false,
                millisecondsSinceMaintenance: 0.0) ==
                ViewportRenderYieldMode.InputContinuation &&
            EngineViewport.SelectRenderYieldMode(
                startupMaintenanceRequired: false,
                millisecondsSinceMaintenance: 499.999) ==
                ViewportRenderYieldMode.InputContinuation &&
            EngineViewport.SelectRenderYieldMode(
                startupMaintenanceRequired: false,
                millisecondsSinceMaintenance: 500.0) ==
                ViewportRenderYieldMode.BackgroundMaintenance &&
            EngineViewport.SelectRenderYieldMode(
                startupMaintenanceRequired: true,
                millisecondsSinceMaintenance: 0.0) ==
                ViewportRenderYieldMode.BackgroundMaintenance &&
            EngineViewport.SelectRenderYieldMode(
                startupMaintenanceRequired: false,
                millisecondsSinceMaintenance: double.NaN) ==
                ViewportRenderYieldMode.BackgroundMaintenance &&
            EngineViewport.SelectRenderYieldMode(
                startupMaintenanceRequired: false,
                millisecondsSinceMaintenance: -1.0) ==
                ViewportRenderYieldMode.BackgroundMaintenance,
            "steady rendering enqueues Background maintenance on a bounded cadence while startup, deadlines, and invalid clocks fail closed to maintenance");
        Check(
            !EngineViewport.ShouldYieldToQueuedInput(0u) &&
            !EngineViewport.ShouldYieldToQueuedInput(0x0001u) &&
            !EngineViewport.ShouldYieldToQueuedInput(0x0008u << 16) &&
            EngineViewport.ShouldYieldToQueuedInput(0x0001u << 16) &&
            EngineViewport.ShouldYieldToQueuedInput(0x0004u << 16) &&
            EngineViewport.ShouldYieldToQueuedInput(0x1000u << 16) &&
              EngineViewport.IsRenderPumpContinuationBlocked(
                inputContinuationQueued: false,
                maintenanceQueued: true,
                frameActive: false) &&
            EngineViewport.IsRenderPumpContinuationBlocked(
                inputContinuationQueued: true,
                maintenanceQueued: false,
                frameActive: false) &&
            EngineViewport.IsRenderPumpContinuationBlocked(
                inputContinuationQueued: false,
                maintenanceQueued: false,
                frameActive: true),
            "current keyboard/pointer input can force an early Input checkpoint; both mandatory Input and periodic Background drains block private reposts");
        Check(
            EngineViewport.IsFatalRenderResult(-1) &&
            !EngineViewport.IsFatalRenderResult(0) &&
            !EngineViewport.IsFatalRenderResult(1) &&
            EngineInterop.TryRenderEditorFrame(IntPtr.Zero, 1.0f / 60.0f) ==
                EngineInterop.EditorRenderFatalResult,
            "invalid or unavailable cooperative render contracts fail closed without entering the blocking legacy renderer");
        Check(
            EngineViewport.IsRenderSurfaceVisible(
                nativeWindowVisible: true,
                hiddenStartupRenderingAllowed: false) &&
            EngineViewport.IsRenderSurfaceVisible(
                nativeWindowVisible: false,
                hiddenStartupRenderingAllowed: true) &&
            !EngineViewport.IsRenderSurfaceVisible(
                nativeWindowVisible: false,
                hiddenStartupRenderingAllowed: false),
            "hidden loading UI permits native frames only during explicit renderer warm-up");
        Check(
            !EngineViewport.ShouldContinueRenderingAfterAttachCallback(
                destroying: false,
                renderPumpSuspended: false,
                hiddenStartupRenderingAllowedBeforeCallback: true,
                hiddenStartupRenderingAllowedAfterCallback: false) &&
            EngineViewport.ShouldContinueRenderingAfterAttachCallback(
                destroying: false,
                renderPumpSuspended: false,
                hiddenStartupRenderingAllowedBeforeCallback: true,
                hiddenStartupRenderingAllowedAfterCallback: true) &&
            EngineViewport.ShouldContinueRenderingAfterAttachCallback(
                destroying: false,
                renderPumpSuspended: false,
                hiddenStartupRenderingAllowedBeforeCallback: false,
                hiddenStartupRenderingAllowedAfterCallback: false) &&
            !EngineViewport.ShouldContinueRenderingAfterAttachCallback(
                destroying: true,
                renderPumpSuspended: false,
                hiddenStartupRenderingAllowedBeforeCallback: true,
                hiddenStartupRenderingAllowedAfterCallback: true) &&
            !EngineViewport.ShouldContinueRenderingAfterAttachCallback(
                destroying: false,
                renderPumpSuspended: true,
                hiddenStartupRenderingAllowedBeforeCallback: true,
                hiddenStartupRenderingAllowedAfterCallback: true),
            "an attach callback that pauses hidden startup rendering ends the current frame before native warm-up");
        Check(
            Math.Abs(
                EngineViewport.CommitRenderTimestamp(4.0, 7.0, 1) -
                4.1) < 0.000001 &&
            EngineViewport.CommitRenderTimestamp(4.0, 4.05, 1) == 4.05 &&
            EngineViewport.CommitRenderTimestamp(4.0, 7.0, 0) == 4.0 &&
            EngineViewport.CommitRenderTimestamp(4.0, 7.0, -1) == 4.0,
            "successful frames consume at most 100 ms while preserving stalled time for later frames");
        Check(
            EngineViewport.ShouldDeferFinalResize(
                awaitingStableSize: true,
                requestedWidth: 1600,
                requestedHeight: 900,
                candidateWidth: 0,
                candidateHeight: 0) &&
            EngineViewport.ShouldDeferFinalResize(
                awaitingStableSize: true,
                requestedWidth: 1700,
                requestedHeight: 900,
                candidateWidth: 1600,
                candidateHeight: 900) &&
            !EngineViewport.ShouldDeferFinalResize(
                awaitingStableSize: true,
                requestedWidth: 1700,
                requestedHeight: 900,
                candidateWidth: 1700,
                candidateHeight: 900) &&
            !EngineViewport.ShouldDeferFinalResize(
                awaitingStableSize: false,
                requestedWidth: 1700,
                requestedHeight: 900,
                candidateWidth: 0,
                candidateHeight: 0),
            "window interactions wait for two identical size observations and commit only the final resize");
        ViewportResizeResultPolicy resizeSucceeded =
            EngineViewport.ClassifyResizeResult(1);
        ViewportResizeResultPolicy resizeRetry =
            EngineViewport.ClassifyResizeResult(0);
        Check(
            resizeSucceeded.CommitDimensions &&
            resizeSucceeded.ContinueToRender &&
            !resizeRetry.CommitDimensions &&
            resizeRetry.ContinueToRender,
            "failed resize keeps old dimensions for retry but still renders once so device loss reaches the fatal pump contract");
        int leftGestureMask = EngineViewport.ActivePointerButtonMask(
            gizmoDragging: true,
            gizmo3dDragging: false,
            marqueeDragging: false,
            panning: false,
            panMode: 0);
        int rightPanMask = EngineViewport.ActivePointerButtonMask(
            gizmoDragging: false,
            gizmo3dDragging: false,
            marqueeDragging: false,
            panning: true,
            panMode: 0);
        int middlePanMask = EngineViewport.ActivePointerButtonMask(
            gizmoDragging: false,
            gizmo3dDragging: false,
            marqueeDragging: false,
            panning: true,
            panMode: 1);
        Check(leftGestureMask == EngineViewport.PointerButtonLeftMask &&
              rightPanMask == EngineViewport.PointerButtonRightMask &&
              middlePanMask == EngineViewport.PointerButtonMiddleMask,
            "viewport capture retains the physical button that initiated each gesture");

        var waterReady = new WaterPointerRoutingState(
            EngineReady: true,
            View3D: true,
            PolygonMode: false,
            GizmoDragging: false,
            Gizmo3DDragging: false,
            Panning: false,
            MarqueeDragging: false);
        WaterPointerRoutingDecision waterPress =
            WaterPointerRoutingPolicy.ForPress(
                waterReady,
                gizmoAccepted: false);
        WaterPointerRoutingDecision waterDrag =
            WaterPointerRoutingPolicy.ForMove(
                waterReady,
                leftButtonDown: true);
        WaterPointerRoutingDecision waterHover =
            WaterPointerRoutingPolicy.ForMove(
                waterReady,
                leftButtonDown: false);
        WaterPointerRoutingDecision waterEnd =
            WaterPointerRoutingPolicy.ForEnd(engineReady: true);
        Check(waterPress.Action == WaterPointerAction.Press &&
              waterDrag.Action == WaterPointerAction.Drag &&
              waterHover.Action == WaterPointerAction.Hover &&
              waterEnd.Action == WaterPointerAction.End,
            "interactive water routes press, left-drag, hover, and end to the native ABI kinds");
        Check(!waterPress.CapturePointer &&
              !waterDrag.CapturePointer &&
              !waterHover.CapturePointer &&
              !waterEnd.CapturePointer,
            "interactive water observes viewport gestures without ever requesting mouse capture");

        WaterPointerRoutingState[] waterBlockedStates =
        {
            waterReady with { EngineReady = false },
            waterReady with { View3D = false },
            waterReady with { PolygonMode = true },
            waterReady with { GizmoDragging = true },
            waterReady with { Gizmo3DDragging = true },
            waterReady with { Panning = true },
            waterReady with { MarqueeDragging = true },
        };
        Check(Array.TrueForAll(
                  waterBlockedStates,
                  state =>
                      !WaterPointerRoutingPolicy.ForPress(
                          state,
                          gizmoAccepted: false).ShouldRoute &&
                      !WaterPointerRoutingPolicy.ForMove(
                          state,
                          leftButtonDown: false).ShouldRoute &&
                      !WaterPointerRoutingPolicy.ForMove(
                          state,
                          leftButtonDown: true).ShouldRoute) &&
              !WaterPointerRoutingPolicy.ForPress(
                  waterReady,
                  gizmoAccepted: true).ShouldRoute,
            "interactive water is gated by 3D mode, polygon tools, both gizmos, pan, marquee, and gizmo hit acceptance");
        Check(!WaterPointerRoutingPolicy.ForEnd(
                  engineReady: false).ShouldRoute &&
              WaterPointerRoutingPolicy.ForEnd(
                  engineReady: true).Action == WaterPointerAction.End,
            "interactive water end/reset is emitted whenever a live native engine exists");
        Check(WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmCancelMode) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmLeftButtonUp) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseWheel) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseHWheel) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmCaptureChanged) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseLeave) &&
              !WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  0x0200) &&
              WaterPointerRoutingPolicy.ForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseLeave,
                  engineReady: true).Action == WaterPointerAction.End,
            "cancel, button-up, capture loss, wheel, and mouse leave terminate water pointer tracking");

        Check(EngineViewport.PointerCaptureRecoveryGraceMilliseconds == 100.0 &&
              EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 100.0) &&
              EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: rightPanMask,
                  physicallyDownButtonMask: EngineViewport.PointerButtonLeftMask,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x0202,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: true,
                  finalizingButtonUp: false,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: true,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: EngineViewport.PointerButtonLeftMask,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 99.9) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: false,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: double.NaN) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0,
                  mismatchAgeMilliseconds: 1000.0,
                  destroying: true) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0,
                  mismatchAgeMilliseconds: 1000.0,
                  ownerClosing: true) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0,
                  mismatchAgeMilliseconds: 1000.0,
                  generationMatches: false),
            "stale capture releases by initiating-button mismatch, never during valid input, close, destruction, generation change, or top-level move/size");
        Check(!EngineViewport.IsAnyRenderPumpSuspensionActive(false, false) &&
              EngineViewport.IsAnyRenderPumpSuspensionActive(true, false) &&
              EngineViewport.IsAnyRenderPumpSuspensionActive(false, true) &&
              EngineViewport.IsAnyRenderPumpSuspensionActive(true, true),
            "startup failure and top-level move/size independently suspend the render pump");
        Check(EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, true, false, false) &&
              EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, true, true, true) &&
              !EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  true, true, false, false) &&
              !EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, false, false, false) &&
              !EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, true, true, false),
            "composition is a queue-only bootstrap/watchdog and never duplicates or executes a live frame");
        Check(!EngineViewport.IsQueuedPumpStale(false, 0, 500) &&
              !EngineViewport.IsQueuedPumpStale(true, 1000, 1099) &&
              EngineViewport.IsQueuedPumpStale(true, 1000, 1100) &&
              EngineViewport.IsQueuedPumpStale(true, 1000, 1500),
            "render-pump watchdog recovers an unacknowledged private message without duplicating live messages");
        Check(EngineViewport.ShouldPublishGizmoTransformChange(
                  captureEnded: true,
                  wasDragging: true) &&
              !EngineViewport.ShouldPublishGizmoTransformChange(
                  captureEnded: false,
                  wasDragging: true) &&
              !EngineViewport.ShouldPublishGizmoTransformChange(
                  captureEnded: true,
                  wasDragging: false),
            "3D gizmo transform publication occurs once at drag capture completion, never per pointer move");
        string nodeOneTransform = MainWindow.BuildSceneMergeKey(
            use3D: false,
            nodeId: 1,
            propertyIdentity: "inspector.transform");
        Check(nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: false,
                  nodeId: 2,
                  propertyIdentity: "inspector.transform") &&
              nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: true,
                  nodeId: 1,
                  propertyIdentity: "inspector.transform") &&
              nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: false,
                  nodeId: 1,
                  propertyIdentity: "inspector.appearance") &&
              nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: false,
                  nodeId: 1,
                  propertyIdentity: "inspector.transform",
                  selectionEpoch: 1),
            "interactive merge keys isolate scene kind, node, property, and selection gesture identity");
        Check(
            EngineViewport.ShouldAttemptAttach(false, false) &&
            !EngineViewport.ShouldAttemptAttach(false, true) &&
            !EngineViewport.ShouldAttemptAttach(true, false) &&
            EngineViewport.ShouldBeginNativeBootstrapForHostGeneration(
                attachFailed: false,
                startupFailureSuspended: false) &&
            !EngineViewport.ShouldBeginNativeBootstrapForHostGeneration(
                attachFailed: true,
                startupFailureSuspended: true) &&
            !EngineViewport.ShouldBeginNativeBootstrapForHostGeneration(
                attachFailed: false,
                startupFailureSuspended: true) &&
            EngineViewport.CanExplicitlyRetryAttach(
                destroying: false,
                attached: false,
                attachFailed: true,
                startupFailureSuspended: true,
                hwndReady: true) &&
            EngineViewport.CanExplicitlyRetryAttach(
                destroying: false,
                attached: false,
                attachFailed: false,
                startupFailureSuspended: true,
                hwndReady: true) &&
            !EngineViewport.CanExplicitlyRetryAttach(
                destroying: false,
                attached: false,
                attachFailed: false,
                startupFailureSuspended: false,
                hwndReady: true) &&
            !EngineViewport.CanExplicitlyRetryAttach(
                destroying: true,
                attached: false,
                attachFailed: true,
                startupFailureSuspended: true,
                hwndReady: true),
            "failed attachment stays latched across host generations until an explicit retry clears the failure state");
        Check(!MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.WaitingForAttach, true) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.WarmingRenderer, true) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.FinalizingEditor, true) &&
              MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.Ready, true) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.Ready, false) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.Failed, true),
            "engine commands are gated until editor finalization succeeds and a live handle exists");
        Check(MainWindow.WindowMoveSizeTransition(0x0231) == 1 &&
              MainWindow.WindowMoveSizeTransition(0x0232) == -1 &&
              MainWindow.WindowMoveSizeTransition(0x001F) == 0 &&
              MainWindow.WindowMoveSizeTransition(0x0200) == 0,
            "top-level move/size entry and exit pause and resume the viewport pump deterministically");

        Check(EditorInputGate.ShouldSuppressShortcuts(true, true, true) &&
              EditorInputGate.ShouldSuppressShortcuts(false, false, true) &&
              EditorInputGate.ShouldSuppressShortcuts(false, true, false) &&
              !EditorInputGate.ShouldSuppressShortcuts(false, true, true),
            "shortcut guard requires an interactive active focused editor");
        Check(EditorInputGate.ShouldSuppressNativeMessage(true, 0x0100) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0101) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0102) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0104) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0109) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0119) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x00A1) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0200) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0233) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0240) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x024F) &&
              !EditorInputGate.ShouldSuppressNativeMessage(true, 0x010A) &&
              !EditorInputGate.ShouldSuppressNativeMessage(true, 0x000F) &&
              !EditorInputGate.ShouldSuppressNativeMessage(true, 0x0006) &&
              !EditorInputGate.ShouldSuppressNativeMessage(false, 0x0200),
            "unattended native gate suppresses keyboard, mouse, touch, pointer, gesture and drop input but not painting or activation cleanup");

        history.Reset();
        Check(history.Points.Count == 0, "reset clears profiler history");

        output.WriteLine(
            failures == 0
                ? "Profiler self-test passed."
                : $"Profiler self-test failed: {failures} check(s).");
        return failures;
    }

    private static EditorProfilerSnapshot Snapshot(
        ulong frame,
        float fps,
        float cpuFrameMs) =>
        new()
        {
            Version = EditorProfilerContract.Version,
            StructSize = EditorProfilerContract.SnapshotSize,
            TimingSource = EditorProfilerContract.TimingCpuRecordSubmit,
            FrameIndex = frame,
            Fps = fps,
            CpuFrameMs = cpuFrameMs,
            CpuSubmitMs = cpuFrameMs * 0.2f,
            NativeRenderActiveCpuMs = cpuFrameMs * 0.8f,
            NativePresentCpuMs = cpuFrameMs * 0.2f,
            GpuFrameMs = -1,
            OpaqueGpuMs = -1,
            AtmosphereGpuMs = -1,
            CloudGpuMs = -1,
            FogGpuMs = -1,
            PostGpuMs = -1,
            GpuFramePeakMs = -1,
            PeakWindowFrames = 120,
            GpuQueryWindowCapacity = 120,
            GpuFrameAverageMs = -1,
            OpaqueGpuAverageMs = -1,
            AtmosphereGpuAverageMs = -1,
            CloudGpuAverageMs = -1,
            FogGpuAverageMs = -1,
            PostGpuAverageMs = -1,
            OpaqueGpuWindowPeakMs = -1,
            AtmosphereGpuWindowPeakMs = -1,
            CloudGpuWindowPeakMs = -1,
            FogGpuWindowPeakMs = -1,
            PostGpuWindowPeakMs = -1,
            NativeRenderActiveCpuPeakMs = cpuFrameMs * 0.8f,
            NativePresentCpuPeakMs = cpuFrameMs * 0.2f,
            PresentedFrameCountSinceReset = frame,
            ProfilerResetSerial = 1,
        };
}
