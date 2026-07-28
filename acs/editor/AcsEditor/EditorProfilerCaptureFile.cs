// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal readonly record struct EditorProfilerCaptureSnapshot(
    EditorProfilerPoint[] Points,
    bool HasLatestSnapshot,
    EditorProfilerSnapshot LatestSnapshot,
    EditorCloudWorkloadQueryStatus CloudWorkloadStatus =
        EditorCloudWorkloadQueryStatus.Unsupported,
    EditorCloudWorkloadSnapshot LatestCloudWorkload = default,
    bool HasNativeRenderDiagnostic = false,
    ViewportNativeRenderDiagnostic NativeRenderDiagnostic = default,
    bool HasDispatcherDiagnostic = false,
    EditorDispatcherWatchdogSnapshot DispatcherDiagnostic = default,
    EditorProfilerRuntimePoint[]? RuntimePoints = null);

internal readonly record struct EditorProfilerRuntimePoint(
    ulong FrameIndex,
    long SampleTimestamp,
    long GpuBackpressureYieldCount,
    long GpuBackpressureInputRetryCount,
    long GpuBackpressureBackgroundFallbackCount,
    long GpuReadyAfterRetryCount,
    long RenderFairnessYieldCount,
    long RenderInputContinuationYieldCount,
    long RenderMaintenanceYieldCount,
    double LastGpuBackpressureEpochMilliseconds,
    double LastRenderContinuationQueueWaitMilliseconds,
    double LastRenderMaintenanceQueueWaitMilliseconds,
    double LastDispatcherGapMilliseconds,
    double DispatcherHeartbeatAgeMilliseconds);

internal sealed record EditorProfilerMetricSummary(
    int SampleCount,
    double? Average,
    double? P95);

internal sealed record EditorProfilerCpuPassSummary(
    EditorProfilerMetricSummary OpaqueMilliseconds,
    EditorProfilerMetricSummary AtmosphereMilliseconds,
    EditorProfilerMetricSummary CloudMilliseconds,
    EditorProfilerMetricSummary FogMilliseconds,
    EditorProfilerMetricSummary PostMilliseconds);

internal sealed record EditorProfilerGpuPassSummary(
    bool Available,
    uint QueryCount,
    uint QueryCapacity,
    uint LatencyFrames,
    double? FrameAverageMilliseconds,
    double? FramePeakMilliseconds,
    double? OpaqueAverageMilliseconds,
    double? OpaquePeakMilliseconds,
    double? AtmosphereAverageMilliseconds,
    double? AtmospherePeakMilliseconds,
    double? CloudAverageMilliseconds,
    double? CloudPeakMilliseconds,
    double? FogAverageMilliseconds,
    double? FogPeakMilliseconds,
    double? PostAverageMilliseconds,
    double? PostPeakMilliseconds);

internal sealed record EditorProfilerRenderStateSummary(
    bool Available,
    uint Flags,
    bool View3D,
    bool CloudsEnabled,
    bool FogEnabled,
    bool AerialPerspectiveEnabled,
    bool GameView,
    bool ScenePresentationSuppressed,
    ulong DrawCalls,
    ulong DispatchCalls,
    ulong Triangles,
    uint ViewportWidth,
    uint ViewportHeight,
    uint CloudWidth,
    uint CloudHeight,
    uint CloudMarchSteps,
    uint CloudLightSteps,
    double? CloudRenderScale);

internal sealed record EditorProfilerCloudWorkloadSummary(
    string Status,
    bool Available,
    bool Attempted,
    bool Submitted,
    bool HistoryWasAvailable,
    bool HistoryReused,
    bool HistoryInvalidated,
    bool TemporalSuperResolution,
    uint SkipReason,
    ulong ProfilerFrameIndex,
    bool ProfilerFrameWithinCapture,
    ulong SubmissionIndex,
    uint TraceWidth,
    uint TraceHeight,
    uint OutputWidth,
    uint OutputHeight,
    uint SteadyDispatches,
    uint OneTimeBakeDispatches,
    uint ShadowCacheDispatches,
    uint TotalComputeDispatches,
    uint CompositeDraws,
    ulong TraceLogicalInvocations,
    ulong TraceLaunchedThreads,
    ulong ResolveLogicalInvocations,
    ulong ResolveLaunchedThreads,
    ulong OneTimeBakeLogicalInvocations,
    ulong OneTimeBakeLaunchedThreads,
    ulong ShadowCacheLogicalInvocations,
    ulong ShadowCacheLaunchedThreads,
    ulong TotalLogicalInvocations,
    ulong TotalLaunchedThreads,
    ulong MaximumViewSamples,
    ulong MaximumLightSamples);

internal sealed record EditorProfilerEditorRuntimeSummary(
    bool NativeAvailable,
    long NativeCallCount,
    long SlowNativeCallCount,
    long GpuBackpressureYieldCount,
    long GpuBackpressureInputRetryCount,
    long GpuBackpressureBackgroundFallbackCount,
    long GpuReadyAfterRetryCount,
    long RenderFairnessYieldCount,
    double? LastGpuBackpressureEpochMilliseconds,
    double? MaximumGpuBackpressureEpochMilliseconds,
    int PeakPresentedRenderBurstFrames,
    double? PeakRenderBurstActiveCpuMilliseconds,
    long RenderInputContinuationYieldCount,
    long RenderMaintenanceYieldCount,
    double? LastRenderContinuationQueueWaitMilliseconds,
    double? MaximumRenderContinuationQueueWaitMilliseconds,
    double? LastRenderMaintenanceQueueWaitMilliseconds,
    double? MaximumRenderMaintenanceQueueWaitMilliseconds,
    double? LastNativeCallMilliseconds,
    double? MaximumNativeCallMilliseconds,
    string LastNativeCallKind,
    bool DispatcherAvailable,
    long DispatcherHeartbeatCount,
    double? DispatcherHeartbeatAgeMilliseconds,
    double? LastDispatcherGapMilliseconds,
    double? MaximumDispatcherGapMilliseconds,
    int DispatcherStallCount,
    bool DispatcherStallActive,
    double? ActiveDispatcherStallMilliseconds,
    double? LongestDispatcherStallMilliseconds,
    string DispatcherPhase);

internal sealed record EditorProfilerRuntimeTimelineSummary(
    int SampleCount,
    EditorProfilerMetricSummary GpuBackpressureEpochMilliseconds,
    EditorProfilerMetricSummary RenderContinuationQueueWaitMilliseconds,
    EditorProfilerMetricSummary RenderMaintenanceQueueWaitMilliseconds,
    EditorProfilerMetricSummary DispatcherGapMilliseconds,
    EditorProfilerMetricSummary DispatcherHeartbeatAgeMilliseconds);

/// <summary>
/// Summary of the exact bounded samples written by an unattended profiler
/// capture. Nullable values mean that no valid measurement was observed; they
/// are never serialized as a zero-cost frame or pass.
/// </summary>
internal sealed record EditorProfilerCaptureSummary(
    int SchemaVersion,
    int SampleCount,
    ulong? FirstFrameIndex,
    ulong? LastFrameIndex,
    bool UsesObservedCadence,
    EditorProfilerMetricSummary NativeReportedFps,
    EditorProfilerMetricSummary EditorFps,
    EditorProfilerMetricSummary ObservedFrameIntervalMilliseconds,
    double? EditorFpsFromP95FrameInterval,
    EditorProfilerMetricSummary CpuFrameMilliseconds,
    EditorProfilerMetricSummary CpuSubmitMilliseconds,
    EditorProfilerMetricSummary NativeRenderActiveCpuMilliseconds,
    EditorProfilerMetricSummary NativePresentCpuMilliseconds,
    double? NativeRenderActiveCpuPeakMilliseconds,
    double? NativePresentCpuPeakMilliseconds,
    ulong? PresentedFrameCountSinceReset,
    ulong? ProfilerResetSerial,
    bool CaptureBoundaryValid,
    EditorProfilerMetricSummary GpuQueryMilliseconds,
    double? GpuThroughputFromAverageMilliseconds,
    double? GpuThroughputFromP95Milliseconds,
    EditorProfilerCpuPassSummary CpuPasses,
    EditorProfilerGpuPassSummary LatestGpuQueryWindow,
    EditorProfilerRenderStateSummary LatestRenderState,
    EditorProfilerCloudWorkloadSummary LatestCloudWorkload,
    EditorProfilerEditorRuntimeSummary LatestEditorRuntime,
    EditorProfilerRuntimeTimelineSummary RuntimeTimeline);

internal static class EditorProfilerCaptureValidation
{
    internal static string[] Expected3DRenderFaults(
        EditorProfilerCaptureSummary summary)
    {
        ArgumentNullException.ThrowIfNull(summary);
        EditorProfilerRenderStateSummary render =
            summary.LatestRenderState;
        var faults = new List<string>(9);
        if (!summary.CaptureBoundaryValid)
            faults.Add("PROFILER_CAPTURE_INVALID_RESET_BOUNDARY");
        if (!render.Available)
        {
            faults.Add("PROFILER_CAPTURE_NO_RENDER_STATE");
            return faults.ToArray();
        }
        if (!render.View3D)
            faults.Add("PROFILER_CAPTURE_EXPECTED_VIEW3D");
        if (render.ScenePresentationSuppressed)
            faults.Add("PROFILER_CAPTURE_SCENE_SUPPRESSED");
        if (render.DrawCalls == 0 && render.DispatchCalls == 0)
            faults.Add("PROFILER_CAPTURE_NO_RENDER_WORK");
        if (!summary.LatestEditorRuntime.NativeAvailable ||
            summary.LatestEditorRuntime.NativeCallCount == 0)
        {
            faults.Add(
                "PROFILER_CAPTURE_NO_EDITOR_RUNTIME_DIAGNOSTICS");
        }
        if (render.CloudsEnabled &&
            (!summary.LatestCloudWorkload.Available ||
             !summary.LatestCloudWorkload.Submitted))
        {
            faults.Add("PROFILER_CAPTURE_NO_CLOUD_WORKLOAD");
        }
        else if (render.CloudsEnabled &&
                 !summary.LatestCloudWorkload
                     .ProfilerFrameWithinCapture)
        {
            faults.Add(
                "PROFILER_CAPTURE_CLOUD_FRAME_OUTSIDE_CAPTURE");
        }
        return faults.ToArray();
    }
}

/// <summary>
/// Builds and atomically publishes unattended profiler captures. Automation
/// destinations are deliberately restricted to an explicit regular CSV below
/// the process TEMP root. Every existing ancestor is checked for reparse
/// points before the sibling-temp write and again before publication.
/// </summary>
internal static class EditorProfilerCaptureFile
{
    internal const int CurrentSchemaVersion = 4;
    internal const int MaximumCaptureBytes = 1024 * 1024;
    internal const int MaximumDestinationCharacters = 1024;

    internal static EditorProfilerCaptureSummary Summarize(
        in EditorProfilerCaptureSnapshot capture)
    {
        EditorProfilerPoint[] points = capture.Points ?? [];
        EditorProfilerMetricSummary nativeReportedFps = Metric(
            points.Select(point => (double)point.Fps),
            positiveOnly: true);
        EditorProfilerMetricSummary nativeFrameIntervals = Metric(
            points
                .Where(point =>
                    float.IsFinite(point.Fps) && point.Fps > 0)
                .Select(point => 1000.0 / point.Fps),
            positiveOnly: true);
        double[] observedIntervals =
            EditorProfilerCadence.FrameIntervalsMilliseconds(points);
        EditorProfilerMetricSummary observedFrameIntervals = Metric(
            observedIntervals,
            positiveOnly: true);
        float observedAverageFps =
            EditorProfilerCadence.FramesPerSecond(points);
        bool usesObservedCadence = observedAverageFps >= 0;
        EditorProfilerMetricSummary observedFps = Metric(
            observedIntervals.Select(interval => 1000.0 / interval),
            positiveOnly: true);
        EditorProfilerMetricSummary editorFps =
            usesObservedCadence
                ? new EditorProfilerMetricSummary(
                    observedFps.SampleCount,
                    observedAverageFps,
                    observedFps.P95)
                : nativeReportedFps;
        EditorProfilerMetricSummary cpuFrame = Metric(
            points.Select(point => (double)point.CpuFrameMs));
        EditorProfilerMetricSummary cpuSubmit = Metric(
            points.Select(point => (double)point.CpuSubmitMs));
        EditorProfilerMetricSummary nativeRenderActive = Metric(
            points.Select(
                point => (double)point.NativeRenderActiveCpuMs));
        EditorProfilerMetricSummary nativePresent = Metric(
            points.Select(
                point => (double)point.NativePresentCpuMs));
        EditorProfilerSnapshot latest = capture.LatestSnapshot;
        bool hasLatestProfiler =
            capture.HasLatestSnapshot &&
            latest.Version == EditorProfilerContract.Version &&
            latest.StructSize >= EditorProfilerContract.SnapshotSize;
        // Native peaks use a rolling 120-frame window. The latest rolling
        // value can legitimately be lower than a peak observed earlier in
        // this capture after that older frame leaves the native window.
        double? nativeRenderActivePeak = Maximum(
            points.Select(
                point =>
                    (double)point.NativeRenderActiveCpuPeakMs));
        double? nativePresentPeak = Maximum(
            points.Select(
                point =>
                    (double)point.NativePresentCpuPeakMs));
        ulong? presentedSinceReset = hasLatestProfiler
            ? latest.PresentedFrameCountSinceReset
            : null;
        ulong? resetSerial = hasLatestProfiler
            ? latest.ProfilerResetSerial
            : null;
        bool captureBoundaryValid =
            CaptureBoundaryIsValid(capture, points);
        EditorProfilerMetricSummary gpuQuery = Metric(
            points.Select(point => (double)point.GpuFrameMs));

        double? editorFpsFromP95 =
            ReciprocalFramesPerSecond(
                usesObservedCadence
                    ? observedFrameIntervals.P95
                    : nativeFrameIntervals.P95);
        double? gpuAverageThroughput =
            ReciprocalFramesPerSecond(gpuQuery.Average);
        double? gpuP95Throughput =
            ReciprocalFramesPerSecond(gpuQuery.P95);

        return new EditorProfilerCaptureSummary(
            CurrentSchemaVersion,
            points.Length,
            points.Length > 0 ? points[0].FrameIndex : null,
            points.Length > 0 ? points[^1].FrameIndex : null,
            usesObservedCadence,
            nativeReportedFps,
            editorFps,
            observedFrameIntervals,
            editorFpsFromP95,
            cpuFrame,
            cpuSubmit,
            nativeRenderActive,
            nativePresent,
            nativeRenderActivePeak,
            nativePresentPeak,
            presentedSinceReset,
            resetSerial,
            captureBoundaryValid,
            gpuQuery,
            gpuAverageThroughput,
            gpuP95Throughput,
            new EditorProfilerCpuPassSummary(
                Metric(points.Select(point => (double)point.OpaqueCpuMs)),
                Metric(points.Select(point => (double)point.AtmosphereCpuMs)),
                Metric(points.Select(point => (double)point.CloudCpuMs)),
                Metric(points.Select(point => (double)point.FogCpuMs)),
                Metric(points.Select(point => (double)point.PostCpuMs))),
            GpuPasses(capture),
            RenderState(capture),
            CloudWorkload(capture),
            EditorRuntime(capture),
            RuntimeTimeline(capture));
    }

    internal static string SerializeCsv(
        in EditorProfilerCaptureSnapshot capture,
        int targetFps,
        EditorProfilerCaptureSummary summary)
    {
        ArgumentNullException.ThrowIfNull(summary);
        var builder = new StringBuilder(4096);
        builder.AppendLine("# ACS Editor unattended profiler capture");
        AppendMetadata(builder, "schema_version", summary.SchemaVersion);
        AppendMetadata(builder, "sample_count", summary.SampleCount);
        AppendMetadata(builder, "first_frame_index", summary.FirstFrameIndex);
        AppendMetadata(builder, "last_frame_index", summary.LastFrameIndex);
        AppendMetadata(
            builder,
            "cadence_source",
            summary.UsesObservedCadence
                ? "stopwatch_frame_index"
                : "native_reported_fallback");
        AppendMetric(
            builder,
            "native_reported_fps",
            summary.NativeReportedFps);
        AppendMetric(builder, "editor_fps", summary.EditorFps);
        AppendMetric(
            builder,
            "observed_frame_interval_ms",
            summary.ObservedFrameIntervalMilliseconds);
        AppendMetadata(
            builder,
            "editor_fps_from_p95_frame_interval",
            summary.EditorFpsFromP95FrameInterval);
        AppendMetric(
            builder,
            "cpu_frame_ms",
            summary.CpuFrameMilliseconds);
        AppendMetric(
            builder,
            "cpu_submit_ms",
            summary.CpuSubmitMilliseconds);
        AppendMetric(
            builder,
            "native_render_active_cpu_ms",
            summary.NativeRenderActiveCpuMilliseconds);
        AppendMetric(
            builder,
            "native_present_cpu_ms",
            summary.NativePresentCpuMilliseconds);
        AppendMetadata(
            builder,
            "native_render_active_cpu_peak_ms",
            summary.NativeRenderActiveCpuPeakMilliseconds);
        AppendMetadata(
            builder,
            "native_present_cpu_peak_ms",
            summary.NativePresentCpuPeakMilliseconds);
        AppendMetadata(
            builder,
            "presented_frame_count_since_reset",
            summary.PresentedFrameCountSinceReset);
        AppendMetadata(
            builder,
            "profiler_reset_serial",
            summary.ProfilerResetSerial);
        AppendMetadata(
            builder,
            "capture_reset_boundary_valid",
            summary.CaptureBoundaryValid);
        AppendMetric(
            builder,
            "gpu_query_ms",
            summary.GpuQueryMilliseconds);
        AppendMetadata(
            builder,
            "gpu_throughput_fps_from_average_ms",
            summary.GpuThroughputFromAverageMilliseconds);
        AppendMetadata(
            builder,
            "gpu_throughput_fps_from_p95_ms",
            summary.GpuThroughputFromP95Milliseconds);
        AppendPassMetric(
            builder,
            "opaque",
            summary.CpuPasses.OpaqueMilliseconds);
        AppendPassMetric(
            builder,
            "atmosphere",
            summary.CpuPasses.AtmosphereMilliseconds);
        AppendPassMetric(
            builder,
            "cloud",
            summary.CpuPasses.CloudMilliseconds);
        AppendPassMetric(
            builder,
            "fog",
            summary.CpuPasses.FogMilliseconds);
        AppendPassMetric(
            builder,
            "post",
            summary.CpuPasses.PostMilliseconds);

        EditorProfilerGpuPassSummary gpu =
            summary.LatestGpuQueryWindow;
        AppendMetadata(builder, "gpu_window_available", gpu.Available);
        AppendMetadata(builder, "gpu_window_query_count", gpu.QueryCount);
        AppendMetadata(
            builder,
            "gpu_window_query_capacity",
            gpu.QueryCapacity);
        AppendMetadata(
            builder,
            "gpu_window_latency_frames",
            gpu.LatencyFrames);
        AppendMetadata(
            builder,
            "gpu_window_frame_average_ms",
            gpu.FrameAverageMilliseconds);
        AppendMetadata(
            builder,
            "gpu_window_frame_peak_ms",
            gpu.FramePeakMilliseconds);
        AppendGpuPass(builder, "opaque", gpu.OpaqueAverageMilliseconds, gpu.OpaquePeakMilliseconds);
        AppendGpuPass(builder, "atmosphere", gpu.AtmosphereAverageMilliseconds, gpu.AtmospherePeakMilliseconds);
        AppendGpuPass(builder, "cloud", gpu.CloudAverageMilliseconds, gpu.CloudPeakMilliseconds);
        AppendGpuPass(builder, "fog", gpu.FogAverageMilliseconds, gpu.FogPeakMilliseconds);
        AppendGpuPass(builder, "post", gpu.PostAverageMilliseconds, gpu.PostPeakMilliseconds);

        EditorProfilerRenderStateSummary render =
            summary.LatestRenderState;
        AppendMetadata(builder, "render_state_available", render.Available);
        AppendMetadata(builder, "render_flags", render.Flags);
        AppendMetadata(builder, "render_view3d", render.View3D);
        AppendMetadata(
            builder,
            "render_clouds_enabled",
            render.CloudsEnabled);
        AppendMetadata(builder, "render_fog_enabled", render.FogEnabled);
        AppendMetadata(
            builder,
            "render_aerial_perspective_enabled",
            render.AerialPerspectiveEnabled);
        AppendMetadata(builder, "render_game_view", render.GameView);
        AppendMetadata(
            builder,
            "render_scene_presentation_suppressed",
            render.ScenePresentationSuppressed);
        AppendMetadata(builder, "render_draw_calls", render.DrawCalls);
        AppendMetadata(
            builder,
            "render_dispatch_calls",
            render.DispatchCalls);
        AppendMetadata(builder, "render_triangles", render.Triangles);
        AppendMetadata(
            builder,
            "render_viewport_width",
            render.ViewportWidth);
        AppendMetadata(
            builder,
            "render_viewport_height",
            render.ViewportHeight);
        AppendMetadata(builder, "render_cloud_width", render.CloudWidth);
        AppendMetadata(builder, "render_cloud_height", render.CloudHeight);
        AppendMetadata(
            builder,
            "render_cloud_march_steps",
            render.CloudMarchSteps);
        AppendMetadata(
            builder,
            "render_cloud_light_steps",
            render.CloudLightSteps);
        AppendMetadata(
            builder,
            "render_cloud_scale",
            render.CloudRenderScale);

        EditorProfilerCloudWorkloadSummary cloud =
            summary.LatestCloudWorkload;
        AppendMetadata(builder, "cloud_workload_status", cloud.Status);
        AppendMetadata(
            builder,
            "cloud_workload_available",
            cloud.Available);
        AppendMetadata(
            builder,
            "cloud_workload_attempted",
            cloud.Attempted);
        AppendMetadata(
            builder,
            "cloud_workload_submitted",
            cloud.Submitted);
        AppendMetadata(
            builder,
            "cloud_workload_history_available",
            cloud.HistoryWasAvailable);
        AppendMetadata(
            builder,
            "cloud_workload_history_reused",
            cloud.HistoryReused);
        AppendMetadata(
            builder,
            "cloud_workload_history_invalidated",
            cloud.HistoryInvalidated);
        AppendMetadata(
            builder,
            "cloud_workload_tsr",
            cloud.TemporalSuperResolution);
        AppendMetadata(
            builder,
            "cloud_workload_skip_reason",
            cloud.SkipReason);
        AppendMetadata(
            builder,
            "cloud_workload_profiler_frame",
            cloud.ProfilerFrameIndex);
        AppendMetadata(
            builder,
            "cloud_workload_profiler_frame_within_capture",
            cloud.ProfilerFrameWithinCapture);
        AppendMetadata(
            builder,
            "cloud_workload_submission",
            cloud.SubmissionIndex);
        AppendMetadata(
            builder,
            "cloud_workload_trace_width",
            cloud.TraceWidth);
        AppendMetadata(
            builder,
            "cloud_workload_trace_height",
            cloud.TraceHeight);
        AppendMetadata(
            builder,
            "cloud_workload_output_width",
            cloud.OutputWidth);
        AppendMetadata(
            builder,
            "cloud_workload_output_height",
            cloud.OutputHeight);
        AppendMetadata(
            builder,
            "cloud_workload_steady_dispatches",
            cloud.SteadyDispatches);
        AppendMetadata(
            builder,
            "cloud_workload_bake_dispatches",
            cloud.OneTimeBakeDispatches);
        AppendMetadata(
            builder,
            "cloud_workload_shadow_dispatches",
            cloud.ShadowCacheDispatches);
        AppendMetadata(
            builder,
            "cloud_workload_total_dispatches",
            cloud.TotalComputeDispatches);
        AppendMetadata(
            builder,
            "cloud_workload_composite_draws",
            cloud.CompositeDraws);
        AppendMetadata(
            builder,
            "cloud_workload_trace_logical_invocations",
            cloud.TraceLogicalInvocations);
        AppendMetadata(
            builder,
            "cloud_workload_trace_launched_threads",
            cloud.TraceLaunchedThreads);
        AppendMetadata(
            builder,
            "cloud_workload_resolve_logical_invocations",
            cloud.ResolveLogicalInvocations);
        AppendMetadata(
            builder,
            "cloud_workload_resolve_launched_threads",
            cloud.ResolveLaunchedThreads);
        AppendMetadata(
            builder,
            "cloud_workload_bake_logical_invocations",
            cloud.OneTimeBakeLogicalInvocations);
        AppendMetadata(
            builder,
            "cloud_workload_bake_launched_threads",
            cloud.OneTimeBakeLaunchedThreads);
        AppendMetadata(
            builder,
            "cloud_workload_shadow_logical_invocations",
            cloud.ShadowCacheLogicalInvocations);
        AppendMetadata(
            builder,
            "cloud_workload_shadow_launched_threads",
            cloud.ShadowCacheLaunchedThreads);
        AppendMetadata(
            builder,
            "cloud_workload_total_logical_invocations",
            cloud.TotalLogicalInvocations);
        AppendMetadata(
            builder,
            "cloud_workload_total_launched_threads",
            cloud.TotalLaunchedThreads);
        AppendMetadata(
            builder,
            "cloud_workload_maximum_view_samples",
            cloud.MaximumViewSamples);
        AppendMetadata(
            builder,
            "cloud_workload_maximum_light_samples",
            cloud.MaximumLightSamples);

        EditorProfilerEditorRuntimeSummary runtime =
            summary.LatestEditorRuntime;
        AppendMetadata(
            builder,
            "editor_runtime_native_available",
            runtime.NativeAvailable);
        AppendMetadata(
            builder,
            "editor_runtime_native_call_count",
            runtime.NativeCallCount);
        AppendMetadata(
            builder,
            "editor_runtime_slow_native_call_count",
            runtime.SlowNativeCallCount);
        AppendMetadata(
            builder,
            "editor_runtime_gpu_backpressure_yield_count",
            runtime.GpuBackpressureYieldCount);
        AppendMetadata(
            builder,
            "editor_runtime_gpu_backpressure_input_retry_count",
            runtime.GpuBackpressureInputRetryCount);
        AppendMetadata(
            builder,
            "editor_runtime_gpu_backpressure_background_fallback_count",
            runtime.GpuBackpressureBackgroundFallbackCount);
        AppendMetadata(
            builder,
            "editor_runtime_gpu_ready_after_retry_count",
            runtime.GpuReadyAfterRetryCount);
        AppendMetadata(
            builder,
            "editor_runtime_render_fairness_yield_count",
            runtime.RenderFairnessYieldCount);
        AppendMetadata(
            builder,
            "editor_runtime_last_gpu_backpressure_epoch_ms",
            runtime.LastGpuBackpressureEpochMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_maximum_gpu_backpressure_epoch_ms",
            runtime.MaximumGpuBackpressureEpochMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_peak_presented_render_burst_frames",
            runtime.PeakPresentedRenderBurstFrames);
        AppendMetadata(
            builder,
            "editor_runtime_peak_render_burst_active_cpu_ms",
            runtime.PeakRenderBurstActiveCpuMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_render_input_continuation_yield_count",
            runtime.RenderInputContinuationYieldCount);
        AppendMetadata(
            builder,
            "editor_runtime_render_maintenance_yield_count",
            runtime.RenderMaintenanceYieldCount);
        AppendMetadata(
            builder,
            "editor_runtime_last_render_continuation_queue_wait_ms",
            runtime.LastRenderContinuationQueueWaitMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_maximum_render_continuation_queue_wait_ms",
            runtime.MaximumRenderContinuationQueueWaitMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_last_render_maintenance_queue_wait_ms",
            runtime.LastRenderMaintenanceQueueWaitMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_maximum_render_maintenance_queue_wait_ms",
            runtime.MaximumRenderMaintenanceQueueWaitMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_last_native_call_ms",
            runtime.LastNativeCallMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_maximum_native_call_ms",
            runtime.MaximumNativeCallMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_last_native_call_kind",
            runtime.LastNativeCallKind);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_available",
            runtime.DispatcherAvailable);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_heartbeat_count",
            runtime.DispatcherHeartbeatCount);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_heartbeat_age_ms",
            runtime.DispatcherHeartbeatAgeMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_last_gap_ms",
            runtime.LastDispatcherGapMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_maximum_gap_ms",
            runtime.MaximumDispatcherGapMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_stall_count",
            runtime.DispatcherStallCount);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_stall_active",
            runtime.DispatcherStallActive);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_active_stall_ms",
            runtime.ActiveDispatcherStallMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_longest_stall_ms",
            runtime.LongestDispatcherStallMilliseconds);
        AppendMetadata(
            builder,
            "editor_runtime_dispatcher_phase",
            runtime.DispatcherPhase);
        EditorProfilerRuntimeTimelineSummary timeline =
            summary.RuntimeTimeline;
        AppendMetadata(
            builder,
            "runtime_timeline_sample_count",
            timeline.SampleCount);
        AppendMetric(
            builder,
            "runtime_timeline_gpu_backpressure_epoch_ms",
            timeline.GpuBackpressureEpochMilliseconds);
        AppendMetric(
            builder,
            "runtime_timeline_render_continuation_queue_wait_ms",
            timeline.RenderContinuationQueueWaitMilliseconds);
        AppendMetric(
            builder,
            "runtime_timeline_render_maintenance_queue_wait_ms",
            timeline.RenderMaintenanceQueueWaitMilliseconds);
        AppendMetric(
            builder,
            "runtime_timeline_dispatcher_gap_ms",
            timeline.DispatcherGapMilliseconds);
        AppendMetric(
            builder,
            "runtime_timeline_dispatcher_heartbeat_age_ms",
            timeline.DispatcherHeartbeatAgeMilliseconds);
        builder.AppendLine(
            "# sample_contract,unique native frames sampled by the visible " +
            "profiler at 10 Hz; bounded to the latest 120 samples");
        builder.AppendLine(
            "# cadence_contract,editor_fps uses Stopwatch elapsed time and " +
            "native frame-index deltas; native_reported_fps is diagnostic only");
        builder.AppendLine(
            "# gpu_pass_contract,latest native unique-query rolling-window " +
            "average and peak; not capture-sample percentiles");
        builder.Append(
            EditorProfilerCapture.SerializeCsv(
                capture.Points ?? [],
                targetFps));
        AppendRuntimeTimeline(
            builder,
            capture.RuntimePoints ?? []);
        return builder.ToString();
    }

    private static void AppendRuntimeTimeline(
        StringBuilder builder,
        IReadOnlyList<EditorProfilerRuntimePoint> points)
    {
        builder.AppendLine(
            "# runtime_timeline_contract,independent bounded 10 Hz " +
            "scheduler/Dispatcher samples; counters are capture-local");
        builder.AppendLine(
            "# runtime_timeline_header,frame_index,sample_timestamp," +
            "gpu_busy_yields,input_retries,background_fallbacks," +
            "ready_after_retry,fairness_yields,input_continuation_yields," +
            "maintenance_yields,busy_epoch_ms,continuation_queue_wait_ms," +
            "maintenance_queue_wait_ms,dispatcher_gap_ms," +
            "dispatcher_heartbeat_age_ms");
        foreach (EditorProfilerRuntimePoint point in points)
        {
            builder.Append("# runtime_timeline_sample,")
                .Append(point.FrameIndex.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(point.SampleTimestamp.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(point.GpuBackpressureYieldCount.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(point.GpuBackpressureInputRetryCount.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(
                    point.GpuBackpressureBackgroundFallbackCount.ToString(
                        CultureInfo.InvariantCulture))
                .Append(',')
                .Append(point.GpuReadyAfterRetryCount.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(point.RenderFairnessYieldCount.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(point.RenderInputContinuationYieldCount.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(point.RenderMaintenanceYieldCount.ToString(
                    CultureInfo.InvariantCulture))
                .Append(',')
                .Append(FormatTimelineValue(
                    point.LastGpuBackpressureEpochMilliseconds))
                .Append(',')
                .Append(FormatTimelineValue(
                    point.LastRenderContinuationQueueWaitMilliseconds))
                .Append(',')
                .Append(FormatTimelineValue(
                    point.LastRenderMaintenanceQueueWaitMilliseconds))
                .Append(',')
                .Append(FormatTimelineValue(
                    point.LastDispatcherGapMilliseconds))
                .Append(',')
                .AppendLine(FormatTimelineValue(
                    point.DispatcherHeartbeatAgeMilliseconds));
        }
    }

    private static string FormatTimelineValue(double value) =>
        double.IsFinite(value) && value >= 0
            ? value.ToString("0.######", CultureInfo.InvariantCulture)
            : "N/A";

    internal static bool TryNormalizeAutomationDestination(
        string? requestedPath,
        out string destination,
        out string? error)
    {
        destination = "";
        error = null;
        if (string.IsNullOrWhiteSpace(requestedPath))
        {
            error = "Profiler capture requires an explicit CSV path.";
            return false;
        }

        try
        {
            destination = Path.GetFullPath(requestedPath);
            if (destination.Length > MaximumDestinationCharacters)
            {
                error =
                    $"Profiler capture path exceeds " +
                    $"{MaximumDestinationCharacters} characters.";
                return false;
            }
            if (!string.Equals(
                    Path.GetExtension(destination),
                    ".csv",
                    StringComparison.OrdinalIgnoreCase))
            {
                error = "Profiler capture destination must use the .csv extension.";
                return false;
            }

            string tempRoot = Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(Path.GetTempPath()));
            string relative = Path.GetRelativePath(tempRoot, destination);
            if (Path.IsPathRooted(relative) ||
                relative == "." ||
                relative == ".." ||
                relative.StartsWith(
                    ".." + Path.DirectorySeparatorChar,
                    StringComparison.Ordinal) ||
                relative.StartsWith(
                    ".." + Path.AltDirectorySeparatorChar,
                    StringComparison.Ordinal))
            {
                error =
                    "Profiler automation captures are restricted to the process TEMP root.";
                return false;
            }

            string? parent = Path.GetDirectoryName(destination);
            if (string.IsNullOrEmpty(parent) || !Directory.Exists(parent))
            {
                error =
                    "Profiler capture parent directory must already exist.";
                return false;
            }
            if (!TryValidateDirectoryChain(parent, tempRoot, out error))
                return false;

            if (Directory.Exists(destination))
            {
                error =
                    "Profiler capture destination is a directory, not a regular file.";
                return false;
            }
            if (File.Exists(destination) &&
                !IsRegularFile(File.GetAttributes(destination)))
            {
                error =
                    "Profiler capture destination is not a regular file.";
                return false;
            }
            return true;
        }
        catch (Exception exception)
            when (exception is ArgumentException or
                  NotSupportedException or
                  PathTooLongException or
                  IOException or
                  UnauthorizedAccessException)
        {
            destination = "";
            error =
                "Profiler capture path is unavailable: " +
                exception.Message;
            return false;
        }
    }

    internal static bool TryWriteAtomic(
        string requestedPath,
        string csv,
        out string destination,
        out string? error)
    {
        destination = "";
        error = null;
        string? temporary = null;
        try
        {
            if (!TryNormalizeAutomationDestination(
                    requestedPath,
                    out destination,
                    out error))
            {
                return false;
            }

            byte[] bytes = new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false).GetBytes(csv);
            if (bytes.Length > MaximumCaptureBytes)
            {
                error =
                    $"Profiler capture exceeds the " +
                    $"{MaximumCaptureBytes}-byte safety limit.";
                return false;
            }

            string parent = Path.GetDirectoryName(destination)!;
            temporary = Path.Combine(
                parent,
                $".acs-profiler-{Guid.NewGuid():N}.tmp");
            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       64 * 1024,
                       FileOptions.WriteThrough))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }

            // Re-evaluate the complete destination contract immediately
            // before the same-directory rename.
            if (!TryNormalizeAutomationDestination(
                    destination,
                    out destination,
                    out error))
            {
                return false;
            }
            File.Move(temporary, destination, overwrite: true);
            temporary = null;
            return true;
        }
        catch (Exception exception)
            when (exception is ArgumentException or
                  NotSupportedException or
                  PathTooLongException or
                  IOException or
                  UnauthorizedAccessException)
        {
            error =
                "Profiler capture publication failed: " +
                exception.Message;
            return false;
        }
        finally
        {
            if (temporary != null)
            {
                try
                {
                    File.Delete(temporary);
                }
                catch
                {
                    // Never remove or truncate the requested destination when
                    // cleanup of our uniquely named sibling fails.
                }
            }
        }
    }

    internal static bool IsSafeDirectoryAttributes(FileAttributes attributes) =>
        (attributes & FileAttributes.Directory) != 0 &&
        (attributes & FileAttributes.ReparsePoint) == 0;

    internal static bool IsRegularFile(FileAttributes attributes) =>
        (attributes &
            (FileAttributes.Directory |
             FileAttributes.ReparsePoint |
             FileAttributes.Device)) == 0;

    private static bool TryValidateDirectoryChain(
        string parent,
        string tempRoot,
        out string? error)
    {
        error = null;
        var current = new DirectoryInfo(parent);
        while (true)
        {
            if (!IsSafeDirectoryAttributes(current.Attributes))
            {
                error =
                    $"Profiler capture directory is not a regular directory: " +
                    $"{current.FullName}";
                return false;
            }
            if (string.Equals(
                    Path.TrimEndingDirectorySeparator(current.FullName),
                    tempRoot,
                    StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
            current = current.Parent ??
                throw new IOException(
                    "Profiler capture directory escaped the TEMP root.");
        }
    }

    private static EditorProfilerMetricSummary Metric(
        IEnumerable<double> source,
        bool positiveOnly = false)
    {
        double[] samples = source
            .Where(value =>
                double.IsFinite(value) &&
                (positiveOnly ? value > 0 : value >= 0))
            .OrderBy(value => value)
            .ToArray();
        if (samples.Length == 0)
            return new EditorProfilerMetricSummary(0, null, null);

        int p95Index = Math.Clamp(
            (int)Math.Ceiling(samples.Length * 0.95) - 1,
            0,
            samples.Length - 1);
        return new EditorProfilerMetricSummary(
            samples.Length,
            samples.Average(),
            samples[p95Index]);
    }

    private static EditorProfilerGpuPassSummary GpuPasses(
        in EditorProfilerCaptureSnapshot capture)
    {
        EditorProfilerSnapshot latest = capture.LatestSnapshot;
        bool valid =
            capture.HasLatestSnapshot &&
            latest.TimingSource ==
                EditorProfilerContract.TimingGpuTimestamp &&
            (latest.Flags &
                EditorProfilerContract.FlagGpuTimingsValid) != 0 &&
            latest.GpuQueryWindowCount > 0;
        return new EditorProfilerGpuPassSummary(
            valid,
            valid ? latest.GpuQueryWindowCount : 0,
            valid ? latest.GpuQueryWindowCapacity : 0,
            valid ? latest.GpuLatencyFrames : 0,
            Optional(latest.GpuFrameAverageMs, valid),
            Optional(latest.GpuFramePeakMs, valid),
            Optional(latest.OpaqueGpuAverageMs, valid),
            Optional(latest.OpaqueGpuWindowPeakMs, valid),
            Optional(latest.AtmosphereGpuAverageMs, valid),
            Optional(latest.AtmosphereGpuWindowPeakMs, valid),
            Optional(latest.CloudGpuAverageMs, valid),
            Optional(latest.CloudGpuWindowPeakMs, valid),
            Optional(latest.FogGpuAverageMs, valid),
            Optional(latest.FogGpuWindowPeakMs, valid),
            Optional(latest.PostGpuAverageMs, valid),
            Optional(latest.PostGpuWindowPeakMs, valid));
    }

    private static EditorProfilerRenderStateSummary RenderState(
        in EditorProfilerCaptureSnapshot capture)
    {
        EditorProfilerSnapshot latest = capture.LatestSnapshot;
        bool available =
            capture.HasLatestSnapshot &&
            latest.Version == EditorProfilerContract.Version &&
            latest.StructSize >= EditorProfilerContract.SnapshotSize;
        uint flags = available ? latest.Flags : 0;
        bool HasFlag(uint flag) => available && (flags & flag) != 0;
        return new EditorProfilerRenderStateSummary(
            available,
            flags,
            HasFlag(EditorProfilerContract.FlagView3D),
            HasFlag(EditorProfilerContract.FlagClouds),
            HasFlag(EditorProfilerContract.FlagFog),
            HasFlag(EditorProfilerContract.FlagAerialPerspective),
            HasFlag(EditorProfilerContract.FlagGameView),
            HasFlag(
                EditorProfilerContract.FlagScenePresentationSuppressed),
            available ? latest.DrawCalls : 0,
            available ? latest.DispatchCalls : 0,
            available ? latest.Triangles : 0,
            available ? latest.ViewportWidth : 0,
            available ? latest.ViewportHeight : 0,
            available ? latest.CloudWidth : 0,
            available ? latest.CloudHeight : 0,
            available ? latest.CloudMarchSteps : 0,
            available ? latest.CloudLightSteps : 0,
            available
                ? Optional(latest.CloudRenderScale, available)
                : null);
    }

    private static EditorProfilerCloudWorkloadSummary CloudWorkload(
        in EditorProfilerCaptureSnapshot capture)
    {
        EditorCloudWorkloadQueryStatus status =
            capture.CloudWorkloadStatus;
        EditorCloudWorkloadSnapshot workload =
            capture.LatestCloudWorkload;
        bool payloadAvailable =
            (status is EditorCloudWorkloadQueryStatus.Available or
                EditorCloudWorkloadQueryStatus.RuntimeUnavailable) &&
            workload.Version == EditorCloudWorkloadContract.Version &&
            workload.StructSize >= EditorCloudWorkloadContract.SnapshotSize;
        uint flags = payloadAvailable ? workload.Flags : 0;
        bool HasFlag(uint flag) =>
            payloadAvailable && (flags & flag) != 0;
        EditorProfilerPoint[] points = capture.Points ?? [];
        bool profilerFrameWithinCapture =
            payloadAvailable &&
            points.Length > 0 &&
            workload.ProfilerFrameIndex >= points[0].FrameIndex &&
            workload.ProfilerFrameIndex <= points[^1].FrameIndex;
        return new EditorProfilerCloudWorkloadSummary(
            status.ToString(),
            status == EditorCloudWorkloadQueryStatus.Available &&
                payloadAvailable,
            HasFlag(EditorCloudWorkloadContract.FlagAttempted),
            HasFlag(EditorCloudWorkloadContract.FlagSubmitted),
            HasFlag(
                EditorCloudWorkloadContract.FlagHistoryWasAvailable),
            HasFlag(EditorCloudWorkloadContract.FlagHistoryReused),
            HasFlag(EditorCloudWorkloadContract.FlagHistoryInvalidated),
            HasFlag(
                EditorCloudWorkloadContract.FlagTemporalSuperResolution),
            payloadAvailable ? workload.SkipReason : 0,
            payloadAvailable ? workload.ProfilerFrameIndex : 0,
            profilerFrameWithinCapture,
            payloadAvailable ? workload.SubmissionIndex : 0,
            payloadAvailable ? workload.TraceWidth : 0,
            payloadAvailable ? workload.TraceHeight : 0,
            payloadAvailable ? workload.OutputWidth : 0,
            payloadAvailable ? workload.OutputHeight : 0,
            payloadAvailable ? workload.SteadyDispatches : 0,
            payloadAvailable ? workload.OneTimeBakeDispatches : 0,
            payloadAvailable ? workload.ShadowCacheDispatches : 0,
            payloadAvailable ? workload.TotalComputeDispatches : 0,
            payloadAvailable ? workload.CompositeDraws : 0,
            payloadAvailable ? workload.TraceLogicalInvocations : 0,
            payloadAvailable ? workload.TraceLaunchedThreads : 0,
            payloadAvailable ? workload.ResolveLogicalInvocations : 0,
            payloadAvailable ? workload.ResolveLaunchedThreads : 0,
            payloadAvailable
                ? workload.OneTimeBakeLogicalInvocations
                : 0,
            payloadAvailable
                ? workload.OneTimeBakeLaunchedThreads
                : 0,
            payloadAvailable
                ? workload.ShadowCacheLogicalInvocations
                : 0,
            payloadAvailable
                ? workload.ShadowCacheLaunchedThreads
                : 0,
            payloadAvailable ? workload.TotalLogicalInvocations : 0,
            payloadAvailable ? workload.TotalLaunchedThreads : 0,
            payloadAvailable ? workload.MaximumViewSamples : 0,
            payloadAvailable ? workload.MaximumLightSamples : 0);
    }

    private static bool CaptureBoundaryIsValid(
        in EditorProfilerCaptureSnapshot capture,
        IReadOnlyList<EditorProfilerPoint> points)
    {
        if (!capture.HasLatestSnapshot || points.Count == 0)
            return false;

        EditorProfilerSnapshot latest = capture.LatestSnapshot;
        if (latest.Version != EditorProfilerContract.Version ||
            latest.StructSize < EditorProfilerContract.SnapshotSize ||
            latest.ProfilerResetSerial == 0 ||
            latest.PresentedFrameCountSinceReset == 0 ||
            latest.FrameIndex != points[^1].FrameIndex ||
            latest.PresentedFrameCountSinceReset !=
                points[^1].PresentedFrameCountSinceReset ||
            latest.ProfilerResetSerial != points[^1].ProfilerResetSerial)
        {
            return false;
        }

        ulong previousPresentedCount = 0;
        foreach (EditorProfilerPoint point in points)
        {
            if (point.ProfilerResetSerial !=
                    latest.ProfilerResetSerial ||
                point.PresentedFrameCountSinceReset == 0 ||
                point.PresentedFrameCountSinceReset <=
                    previousPresentedCount ||
                !float.IsFinite(
                    point.NativeRenderActiveCpuPeakMs) ||
                point.NativeRenderActiveCpuPeakMs <
                    point.NativeRenderActiveCpuMs ||
                !float.IsFinite(point.NativePresentCpuPeakMs) ||
                point.NativePresentCpuPeakMs <
                    point.NativePresentCpuMs)
            {
                return false;
            }

            previousPresentedCount =
                point.PresentedFrameCountSinceReset;
        }

        return latest.PresentedFrameCountSinceReset >=
               (ulong)points.Count;
    }

    private static double? Maximum(IEnumerable<double> source)
    {
        double[] samples = source
            .Where(value => double.IsFinite(value) && value >= 0)
            .ToArray();
        return samples.Length > 0 ? samples.Max() : null;
    }

    private static EditorProfilerEditorRuntimeSummary EditorRuntime(
        in EditorProfilerCaptureSnapshot capture)
    {
        bool nativeAvailable = capture.HasNativeRenderDiagnostic;
        ViewportNativeRenderDiagnostic native =
            capture.NativeRenderDiagnostic;
        bool dispatcherAvailable = capture.HasDispatcherDiagnostic;
        EditorDispatcherWatchdogSnapshot dispatcher =
            capture.DispatcherDiagnostic;
        return new EditorProfilerEditorRuntimeSummary(
            nativeAvailable,
            nativeAvailable ? Math.Max(0L, native.NativeCallCount) : 0,
            nativeAvailable
                ? Math.Max(0L, native.SlowNativeCallCount)
                : 0,
            nativeAvailable
                ? Math.Max(0L, native.GpuBackpressureYieldCount)
                : 0,
            nativeAvailable
                ? Math.Max(
                    0L,
                    native.GpuBackpressureInputRetryCount)
                : 0,
            nativeAvailable
                ? Math.Max(
                    0L,
                    native.GpuBackpressureBackgroundFallbackCount)
                : 0,
            nativeAvailable
                ? Math.Max(0L, native.GpuReadyAfterRetryCount)
                : 0,
            nativeAvailable
                ? Math.Max(0L, native.RenderFairnessYieldCount)
                : 0,
            Optional(
                native.LastGpuBackpressureEpochMilliseconds,
                nativeAvailable),
            Optional(
                native.MaximumGpuBackpressureEpochMilliseconds,
                nativeAvailable),
            nativeAvailable
                ? Math.Max(0, native.PeakPresentedRenderBurstFrames)
                : 0,
            Optional(
                native.PeakRenderBurstActiveCpuMilliseconds,
                nativeAvailable),
            nativeAvailable
                ? Math.Max(
                    0L,
                    native.RenderInputContinuationYieldCount)
                : 0,
            nativeAvailable
                ? Math.Max(0L, native.RenderMaintenanceYieldCount)
                : 0,
            Optional(
                native.LastRenderContinuationQueueWaitMilliseconds,
                nativeAvailable),
            Optional(
                native.MaximumRenderContinuationQueueWaitMilliseconds,
                nativeAvailable),
            Optional(
                native.LastRenderMaintenanceQueueWaitMilliseconds,
                nativeAvailable),
            Optional(
                native.MaximumRenderMaintenanceQueueWaitMilliseconds,
                nativeAvailable),
            Optional(native.LastNativeCallMilliseconds, nativeAvailable),
            Optional(
                native.MaximumNativeCallMilliseconds,
                nativeAvailable),
            nativeAvailable
                ? native.LastNativeCallKind ?? string.Empty
                : string.Empty,
            dispatcherAvailable,
            dispatcherAvailable
                ? Math.Max(0L, dispatcher.HeartbeatCount)
                : 0,
            Optional(
                dispatcher.HeartbeatAgeMilliseconds,
                dispatcherAvailable),
            Optional(
                dispatcher.LastDispatcherGapMilliseconds,
                dispatcherAvailable),
            Optional(
                dispatcher.MaximumDispatcherGapMilliseconds,
                dispatcherAvailable),
            dispatcherAvailable
                ? Math.Max(0, dispatcher.StallCount)
                : 0,
            dispatcherAvailable && dispatcher.StallActive,
            Optional(
                dispatcher.ActiveStallMilliseconds,
                dispatcherAvailable),
            Optional(
                dispatcher.LongestStallMilliseconds,
                dispatcherAvailable),
            dispatcherAvailable
                ? dispatcher.Phase ?? string.Empty
                : string.Empty);
    }

    private static EditorProfilerRuntimeTimelineSummary RuntimeTimeline(
        in EditorProfilerCaptureSnapshot capture)
    {
        EditorProfilerRuntimePoint[] points =
            capture.RuntimePoints ?? [];
        return new EditorProfilerRuntimeTimelineSummary(
            points.Length,
            Metric(
                points.Select(
                    point =>
                        point.LastGpuBackpressureEpochMilliseconds),
                positiveOnly: true),
            Metric(
                points.Select(
                    point =>
                        point.LastRenderContinuationQueueWaitMilliseconds),
                positiveOnly: true),
            Metric(
                points.Select(
                    point =>
                        point.LastRenderMaintenanceQueueWaitMilliseconds),
                positiveOnly: true),
            Metric(
                points.Select(
                    point => point.LastDispatcherGapMilliseconds),
                positiveOnly: true),
            Metric(
                points.Select(
                    point => point.DispatcherHeartbeatAgeMilliseconds),
                positiveOnly: true));
    }

    private static double? Optional(float value, bool available) =>
        available && float.IsFinite(value) && value >= 0
            ? value
            : null;

    private static double? Optional(double value, bool available) =>
        available && double.IsFinite(value) && value >= 0
            ? value
            : null;

    private static double? ReciprocalFramesPerSecond(
        double? milliseconds) =>
        milliseconds is > 0 && double.IsFinite(milliseconds.Value)
            ? 1000.0 / milliseconds.Value
            : null;

    private static void AppendMetric(
        StringBuilder builder,
        string prefix,
        EditorProfilerMetricSummary metric)
    {
        AppendMetadata(builder, prefix + "_sample_count", metric.SampleCount);
        AppendMetadata(builder, prefix + "_average", metric.Average);
        AppendMetadata(builder, prefix + "_p95", metric.P95);
    }

    private static void AppendPassMetric(
        StringBuilder builder,
        string pass,
        EditorProfilerMetricSummary metric) =>
        AppendMetric(builder, "cpu_pass_" + pass + "_ms", metric);

    private static void AppendGpuPass(
        StringBuilder builder,
        string pass,
        double? average,
        double? peak)
    {
        AppendMetadata(
            builder,
            "gpu_window_" + pass + "_average_ms",
            average);
        AppendMetadata(
            builder,
            "gpu_window_" + pass + "_peak_ms",
            peak);
    }

    private static void AppendMetadata(
        StringBuilder builder,
        string key,
        object? value)
    {
        builder.Append("# ")
            .Append(key)
            .Append(',')
            .Append(Format(value))
            .AppendLine();
    }

    private static string Format(object? value) =>
        value switch
        {
            null => "N/A",
            bool boolean => boolean ? "true" : "false",
            IFormattable formattable => formattable.ToString(
                "0.######",
                CultureInfo.InvariantCulture),
            _ => value.ToString() ?? "N/A",
        };
}
