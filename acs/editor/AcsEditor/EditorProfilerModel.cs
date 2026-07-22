// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
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
    float PostCpuMs);

internal readonly record struct EditorProfilerAverage(
    float Fps,
    float CpuFrameMs,
    float CpuSubmitMs,
    float GpuFrameMs);

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

    internal bool Add(in EditorProfilerSnapshot snapshot)
    {
        if (IsPaused || snapshot.FrameIndex == _lastFrameIndex)
            return false;

        _lastFrameIndex = snapshot.FrameIndex;
        if (_points.Count == _capacity)
            _points.RemoveAt(0);
        _points.Add(new EditorProfilerPoint(
            snapshot.FrameIndex,
            FiniteNonNegative(snapshot.Fps),
            FiniteNonNegative(snapshot.CpuFrameMs),
            FiniteNonNegative(snapshot.CpuSubmitMs),
            FiniteOrUnavailable(snapshot.GpuFrameMs),
            snapshot.GpuQueryWindowCount > 0
                ? FiniteOrUnavailable(snapshot.GpuFrameAverageMs)
                : -1,
            FiniteNonNegative(snapshot.OpaqueCpuMs),
            FiniteNonNegative(snapshot.AtmosphereCpuMs),
            FiniteNonNegative(snapshot.CloudCpuMs),
            FiniteNonNegative(snapshot.FogCpuMs),
            FiniteNonNegative(snapshot.PostCpuMs)));
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

        return new EditorProfilerAverage(
            (float)(fps / count),
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
