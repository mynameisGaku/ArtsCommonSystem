// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;

namespace AcsEditor;

internal readonly record struct EditorProfilerPoint(
    ulong FrameIndex,
    float Fps,
    float CpuFrameMs,
    float CpuSubmitMs,
    float GpuFrameMs,
    float GpuFrameAverageMs,
    float OpaqueCpuMs,
    float AtmosphereCpuMs,
    float CloudCpuMs,
    float FogCpuMs,
    float PostCpuMs,
    float NativeRenderActiveCpuMs,
    float NativePresentCpuMs,
    float NativeRenderActiveCpuPeakMs,
    float NativePresentCpuPeakMs,
    ulong PresentedFrameCountSinceReset,
    ulong ProfilerResetSerial,
    long SampleTimestamp);

internal readonly record struct EditorProfilerAverage(
    float Fps,
    float CpuFrameMs,
    float CpuSubmitMs,
    float GpuFrameMs);

internal static class EditorProfilerPresentationPolicy
{
    internal const double VisibleSampleIntervalMilliseconds = 100.0;
    internal const double HiddenSampleIntervalMilliseconds = 500.0;

    /// <summary>
    /// Sampling remains active while the dock is collapsed so the status strip
    /// and history stay current. Expensive WPF detail updates are published
    /// only for a new native frame while the panel is visible.
    /// </summary>
    internal static bool ShouldPresentDetails(
        bool panelVisible,
        bool nativeFrameAdvanced) =>
        panelVisible && nativeFrameAdvanced;

    internal static bool ShouldPresentManagedDiagnostics(bool panelVisible) =>
        panelVisible;

    internal static TimeSpan SampleInterval(bool panelVisible) =>
        TimeSpan.FromMilliseconds(
            panelVisible
                ? VisibleSampleIntervalMilliseconds
                : HiddenSampleIntervalMilliseconds);
}

internal static class EditorProfilerCaptureBoundaryPolicy
{
    internal static bool TryArm(
        bool hadPreviousSnapshot,
        ulong previousResetSerial,
        bool hasResetSnapshot,
        in EditorProfilerSnapshot resetSnapshot,
        out ulong requiredResetSerial)
    {
        requiredResetSerial = 0;
        if (!hasResetSnapshot ||
            resetSnapshot.Version != EditorProfilerContract.Version ||
            resetSnapshot.StructSize <
                EditorProfilerContract.SnapshotSize ||
            resetSnapshot.ProfilerResetSerial == 0 ||
            resetSnapshot.PresentedFrameCountSinceReset != 0 ||
            (hadPreviousSnapshot &&
             resetSnapshot.ProfilerResetSerial == previousResetSerial))
        {
            return false;
        }

        requiredResetSerial = resetSnapshot.ProfilerResetSerial;
        return true;
    }

    internal static bool Accepts(
        in EditorProfilerSnapshot snapshot,
        ulong requiredResetSerial) =>
        requiredResetSerial > 0 &&
        snapshot.Version == EditorProfilerContract.Version &&
        snapshot.StructSize >= EditorProfilerContract.SnapshotSize &&
        snapshot.ProfilerResetSerial == requiredResetSerial &&
        snapshot.PresentedFrameCountSinceReset > 0;
}

internal static class EditorProfilerCadence
{
    internal const double MinimumObservedSpanMilliseconds = 50.0;

    internal static double[] FrameIntervalsMilliseconds(
        IReadOnlyList<EditorProfilerPoint> points,
        int startIndex = 0,
        int count = -1)
    {
        ArgumentNullException.ThrowIfNull(points);
        if (points.Count < 2)
            return [];

        int start = Math.Clamp(startIndex, 0, points.Count);
        int available = points.Count - start;
        int length = count < 0
            ? available
            : Math.Clamp(count, 0, available);
        if (length < 2)
            return [];

        var intervals = new List<double>(length - 1);
        int end = start + length;
        for (int index = start + 1; index < end; index++)
        {
            EditorProfilerPoint previous = points[index - 1];
            EditorProfilerPoint current = points[index];
            if (previous.SampleTimestamp <= 0 ||
                current.SampleTimestamp <= previous.SampleTimestamp ||
                current.FrameIndex <= previous.FrameIndex)
            {
                continue;
            }

            double elapsedMilliseconds =
                (current.SampleTimestamp - previous.SampleTimestamp) *
                1000.0 / Stopwatch.Frequency;
            ulong frameDelta =
                current.FrameIndex - previous.FrameIndex;
            double perFrameMilliseconds =
                elapsedMilliseconds / frameDelta;
            if (double.IsFinite(perFrameMilliseconds) &&
                perFrameMilliseconds > 0)
            {
                intervals.Add(perFrameMilliseconds);
            }
        }
        return intervals.ToArray();
    }

    internal static float FramesPerSecond(
        IReadOnlyList<EditorProfilerPoint> points,
        int startIndex = 0,
        int count = -1)
    {
        ArgumentNullException.ThrowIfNull(points);
        if (points.Count < 2)
            return -1.0f;

        int start = Math.Clamp(startIndex, 0, points.Count);
        int available = points.Count - start;
        int length = count < 0
            ? available
            : Math.Clamp(count, 0, available);
        if (length < 2)
            return -1.0f;

        double elapsedMilliseconds = 0;
        ulong frameCount = 0;
        int end = start + length;
        for (int index = start + 1; index < end; index++)
        {
            EditorProfilerPoint previous = points[index - 1];
            EditorProfilerPoint current = points[index];
            if (previous.SampleTimestamp <= 0 ||
                current.SampleTimestamp <= previous.SampleTimestamp ||
                current.FrameIndex <= previous.FrameIndex)
            {
                continue;
            }
            elapsedMilliseconds +=
                (current.SampleTimestamp - previous.SampleTimestamp) *
                1000.0 / Stopwatch.Frequency;
            frameCount += current.FrameIndex - previous.FrameIndex;
        }
        if (elapsedMilliseconds < MinimumObservedSpanMilliseconds ||
            frameCount == 0)
        {
            return -1.0f;
        }

        double fps = frameCount * 1000.0 / elapsedMilliseconds;
        return double.IsFinite(fps) && fps >= 0 && fps <= float.MaxValue
            ? (float)fps
            : -1.0f;
    }
}

/// <summary>
/// Fixed-capacity sampled history. The native renderer remains per-frame while
/// the WPF panel samples at 10 Hz, keeping UI overhead independent of frame rate.
/// </summary>
internal sealed class EditorProfilerHistory
{
    private readonly int _capacity;
    private readonly List<EditorProfilerPoint> _points;
    private ulong _lastFrameIndex = ulong.MaxValue;

    internal EditorProfilerHistory(int capacity = 120)
    {
        if (capacity < 2)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        _capacity = capacity;
        _points = new List<EditorProfilerPoint>(capacity);
    }

    internal bool IsPaused { get; set; }
    internal IReadOnlyList<EditorProfilerPoint> Points => _points;

    internal bool Add(in EditorProfilerSnapshot snapshot) =>
        Add(snapshot, Stopwatch.GetTimestamp());

    /// <summary>
    /// Deterministic timestamp overload used by cadence regression tests. The
    /// production sampler always calls the monotonic-clock overload above.
    /// </summary>
    internal bool Add(
        in EditorProfilerSnapshot snapshot,
        long sampleTimestamp)
    {
        if (IsPaused || snapshot.FrameIndex == _lastFrameIndex)
            return false;

        _lastFrameIndex = snapshot.FrameIndex;
        if (_points.Count == _capacity)
            _points.RemoveAt(0);
        bool gpuTimestampValid =
            snapshot.TimingSource ==
                EditorProfilerContract.TimingGpuTimestamp &&
            (snapshot.Flags &
                EditorProfilerContract.FlagGpuTimingsValid) != 0;
        _points.Add(new EditorProfilerPoint(
            snapshot.FrameIndex,
            FiniteNonNegative(snapshot.Fps),
            FiniteNonNegative(snapshot.CpuFrameMs),
            FiniteNonNegative(snapshot.CpuSubmitMs),
            gpuTimestampValid
                ? FiniteOrUnavailable(snapshot.GpuFrameMs)
                : -1,
            gpuTimestampValid &&
            snapshot.GpuQueryWindowCount > 0
                ? FiniteOrUnavailable(snapshot.GpuFrameAverageMs)
                : -1,
            FiniteNonNegative(snapshot.OpaqueCpuMs),
            FiniteNonNegative(snapshot.AtmosphereCpuMs),
            FiniteNonNegative(snapshot.CloudCpuMs),
            FiniteNonNegative(snapshot.FogCpuMs),
            FiniteNonNegative(snapshot.PostCpuMs),
            FiniteNonNegative(snapshot.NativeRenderActiveCpuMs),
            FiniteNonNegative(snapshot.NativePresentCpuMs),
            FiniteNonNegative(
                snapshot.NativeRenderActiveCpuPeakMs),
            FiniteNonNegative(snapshot.NativePresentCpuPeakMs),
            snapshot.PresentedFrameCountSinceReset,
            snapshot.ProfilerResetSerial,
            sampleTimestamp));
        return true;
    }

    internal void Reset()
    {
        _points.Clear();
        _lastFrameIndex = ulong.MaxValue;
    }

    internal EditorProfilerAverage Average(int sampleCount = 30)
    {
        if (_points.Count == 0)
            return default;

        int first = Math.Max(0, _points.Count - Math.Max(1, sampleCount));
        int count = _points.Count - first;
        double fps = 0;
        double cpu = 0;
        double submit = 0;
        for (int i = first; i < _points.Count; i++)
        {
            EditorProfilerPoint point = _points[i];
            fps += point.Fps;
            cpu += point.CpuFrameMs;
            submit += point.CpuSubmitMs;
        }

        // Native code already averages every unique completed GPU query. Do
        // not average those rolling averages again at WPF's unrelated 10 Hz
        // cadence; that would reintroduce compositor phase aliasing.
        float gpu = -1;
        for (int i = _points.Count - 1; i >= first; i--)
        {
            if (_points[i].GpuFrameAverageMs < 0)
                continue;
            gpu = _points[i].GpuFrameAverageMs;
            break;
        }

        float observedFps = EditorProfilerCadence.FramesPerSecond(
            _points,
            first,
            count);
        return new EditorProfilerAverage(
            observedFps >= 0
                ? observedFps
                : (float)(fps / count),
            (float)(cpu / count),
            (float)(submit / count),
            gpu);
    }

    private static float FiniteNonNegative(float value) =>
        float.IsFinite(value) && value >= 0 ? value : 0;

    private static float FiniteOrUnavailable(float value) =>
        float.IsFinite(value) && value >= 0 ? value : -1;
}

internal static class EditorProfilerFormatting
{
    internal static string TimingSource(in EditorProfilerSnapshot snapshot)
    {
        bool gpuValid =
            snapshot.TimingSource == EditorProfilerContract.TimingGpuTimestamp &&
            (snapshot.Flags & EditorProfilerContract.FlagGpuTimingsValid) != 0 &&
            float.IsFinite(snapshot.GpuFrameMs) &&
            snapshot.GpuFrameMs >= 0;
        if (!gpuValid)
            return "CPU record / submit";

        uint capacity = snapshot.GpuQueryWindowCapacity;
        return snapshot.GpuQueryWindowCount > 0 && capacity > 0
            ? $"GPU timestamp queries (+{snapshot.GpuLatencyFrames}f async; " +
              $"{snapshot.GpuQueryWindowCount}/{capacity}q avg)"
            : $"GPU timestamp queries (+{snapshot.GpuLatencyFrames}f async; warming up)";
    }

    internal static string Milliseconds(float value) =>
        float.IsFinite(value) && value >= 0 ? $"{value,6:0.00} ms" : "   N/A";

    internal static string GpuAveragePeak(
        float average,
        float peak,
        uint validQueries) =>
        validQueries > 0 &&
        float.IsFinite(average) && average >= 0 &&
        float.IsFinite(peak) && peak >= 0
            ? $"{average,5:0.00} / {peak:0.00} ms"
            : "   N/A";

    internal static string GpuWindowTooltip(
        uint validQueries,
        uint capacity,
        uint latencyFrames) =>
        validQueries > 0
            ? $"Average / peak across {validQueries}/{capacity} unique valid GPU queries. " +
              $"Latest completed query is +{latencyFrames} frames asynchronous."
            : "Waiting for the first completed valid GPU query.";

    internal static string CullingState(in EditorProfilerSnapshot snapshot)
    {
        if ((snapshot.Flags &
             EditorProfilerContract.FlagFrustumCullingEnabled) == 0)
            return "DISABLED";

        if ((snapshot.Flags &
             EditorProfilerContract.FlagRuntimeSceneCamera) != 0)
            return $"ON · Camera #{snapshot.ActiveCameraNodeId}";
        return (snapshot.Flags &
                EditorProfilerContract.FlagGameView) != 0
            ? "ON · Game fallback"
            : "ON · Editor camera";
    }

    internal static string CullingCounts(in EditorProfilerSnapshot snapshot) =>
        $"{snapshot.FrustumTested:N0} / " +
        $"{snapshot.FrustumVisible:N0} / " +
        $"{snapshot.FrustumCulled:N0}";

    internal static string Peak(
        float value,
        uint windowFrames,
        uint? latencyFrames = null)
    {
        if (!float.IsFinite(value) || value < 0)
            return "peak N/A";
        string suffix = latencyFrames.HasValue
            ? $" · +{latencyFrames.Value}f"
            : "";
        return $"peak {value:0.00} · {windowFrames}f{suffix}";
    }

    internal static float GpuThroughputFps(float gpuFrameMilliseconds)
    {
        if (!float.IsFinite(gpuFrameMilliseconds) || gpuFrameMilliseconds <= 0)
            return -1.0f;
        float fps = 1000.0f / gpuFrameMilliseconds;
        return float.IsFinite(fps) ? fps : -1.0f;
    }

    internal static string GpuThroughput(float gpuFrameMilliseconds)
    {
        float fps = GpuThroughputFps(gpuFrameMilliseconds);
        return fps >= 0 ? $"≈ {fps:0.0} FPS throughput" : "throughput N/A";
    }

    internal static string CompactSummary(EditorProfilerAverage average)
    {
        float gpuFps = GpuThroughputFps(average.GpuFrameMs);
        return gpuFps >= 0
            ? $"Editor {average.Fps:0} FPS  |  GPU {gpuFps:0} FPS / {average.GpuFrameMs:0.00} ms  |  CPU {average.CpuFrameMs:0.00} ms"
            : $"Editor {average.Fps:0} FPS  |  Frame {average.CpuFrameMs:0.00} ms  |  CPU record/submit";
    }
}
