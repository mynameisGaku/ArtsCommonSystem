// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace AcsEditor;

internal enum EditorProfilerBudgetState
{
    NoSamples,
    WithinBudget,
    CpuBound,
    GpuBound,
    CpuAndGpuBound,
}

internal readonly record struct EditorProfilerBudgetAnalysis(
    int TargetFps,
    float BudgetMilliseconds,
    int CpuSampleCount,
    int GpuSampleCount,
    float CpuP95Milliseconds,
    float GpuP95Milliseconds,
    int CpuOverBudgetSamples,
    int GpuOverBudgetSamples)
{
    internal EditorProfilerBudgetState State
    {
        get
        {
            if (CpuSampleCount == 0)
                return EditorProfilerBudgetState.NoSamples;

            bool cpuOver =
                float.IsFinite(CpuP95Milliseconds) &&
                CpuP95Milliseconds > BudgetMilliseconds;
            bool gpuOver =
                GpuSampleCount > 0 &&
                float.IsFinite(GpuP95Milliseconds) &&
                GpuP95Milliseconds > BudgetMilliseconds;
            if (cpuOver && gpuOver)
                return EditorProfilerBudgetState.CpuAndGpuBound;
            if (cpuOver)
                return EditorProfilerBudgetState.CpuBound;
            if (gpuOver)
                return EditorProfilerBudgetState.GpuBound;
            return EditorProfilerBudgetState.WithinBudget;
        }
    }

    internal float CpuOverBudgetPercent =>
        Percentage(CpuOverBudgetSamples, CpuSampleCount);

    internal float GpuOverBudgetPercent =>
        Percentage(GpuOverBudgetSamples, GpuSampleCount);

    private static float Percentage(int count, int total) =>
        total > 0 ? count * 100.0f / total : 0.0f;
}

internal static class EditorProfilerBudget
{
    internal const int DefaultTargetFps = 300;
    internal const int MinimumTargetFps = 1;
    internal const int MaximumTargetFps = 1000;

    internal static EditorProfilerBudgetAnalysis Analyze(
        IReadOnlyList<EditorProfilerPoint> points,
        int targetFps,
        int sampleCount = 120)
    {
        ArgumentNullException.ThrowIfNull(points);
        if (targetFps is < MinimumTargetFps or > MaximumTargetFps)
            throw new ArgumentOutOfRangeException(nameof(targetFps));
        if (sampleCount < 1)
            throw new ArgumentOutOfRangeException(nameof(sampleCount));

        float budget = 1000.0f / targetFps;
        int first = Math.Max(0, points.Count - sampleCount);
        var cpuSamples = new List<float>(points.Count - first);
        var gpuSamples = new List<float>(points.Count - first);
        int cpuOver = 0;
        int gpuOver = 0;

        for (int i = first; i < points.Count; i++)
        {
            EditorProfilerPoint point = points[i];
            if (float.IsFinite(point.CpuFrameMs) &&
                point.CpuFrameMs >= 0)
            {
                cpuSamples.Add(point.CpuFrameMs);
                if (point.CpuFrameMs > budget)
                    cpuOver++;
            }

            // Use the latest completed query rather than the native rolling
            // average. Percentiles over rolling averages conceal individual
            // GPU spikes and would weight the same query window repeatedly.
            if (float.IsFinite(point.GpuFrameMs) &&
                point.GpuFrameMs >= 0)
            {
                gpuSamples.Add(point.GpuFrameMs);
                if (point.GpuFrameMs > budget)
                    gpuOver++;
            }
        }

        return new EditorProfilerBudgetAnalysis(
            targetFps,
            budget,
            cpuSamples.Count,
            gpuSamples.Count,
            Percentile95(cpuSamples),
            Percentile95(gpuSamples),
            cpuOver,
            gpuOver);
    }

    internal static string StateLabel(
        in EditorProfilerBudgetAnalysis analysis) =>
        analysis.State switch
        {
            EditorProfilerBudgetState.NoSamples => "WAITING",
            EditorProfilerBudgetState.WithinBudget => "WITHIN BUDGET",
            EditorProfilerBudgetState.CpuBound => "CPU OVER BUDGET",
            EditorProfilerBudgetState.GpuBound => "GPU OVER BUDGET",
            EditorProfilerBudgetState.CpuAndGpuBound =>
                "CPU + GPU OVER BUDGET",
            _ => "WAITING",
        };

    internal static string Percentile(float milliseconds, int sampleCount) =>
        sampleCount > 0 &&
        float.IsFinite(milliseconds) &&
        milliseconds >= 0
            ? $"{milliseconds:0.00} ms · n={sampleCount}"
            : "N/A";

    internal static string Violations(int overBudget, int sampleCount) =>
        sampleCount > 0
            ? $"{overBudget}/{sampleCount} ({overBudget * 100.0f / sampleCount:0.0}%)"
            : "N/A";

    private static float Percentile95(List<float> samples)
    {
        if (samples.Count == 0)
            return -1.0f;

        samples.Sort();
        int index = Math.Clamp(
            (int)Math.Ceiling(samples.Count * 0.95) - 1,
            0,
            samples.Count - 1);
        return samples[index];
    }
}

internal static class EditorProfilerCapture
{
    internal static string SerializeCsv(
        IReadOnlyList<EditorProfilerPoint> points,
        int targetFps)
    {
        ArgumentNullException.ThrowIfNull(points);
        if (targetFps is <
            EditorProfilerBudget.MinimumTargetFps or >
            EditorProfilerBudget.MaximumTargetFps)
        {
            throw new ArgumentOutOfRangeException(nameof(targetFps));
        }

        var builder = new StringBuilder(
            Math.Max(512, points.Count * 128));
        builder.AppendLine("# ACS Editor profiler capture");
        builder.Append("# target_fps,")
            .Append(targetFps.ToString(CultureInfo.InvariantCulture))
            .AppendLine();
        builder.Append("# frame_budget_ms,")
            .Append((1000.0 / targetFps).ToString(
                "0.000000",
                CultureInfo.InvariantCulture))
            .AppendLine();
        builder.Append("# sample_timestamp_frequency,")
            .Append(System.Diagnostics.Stopwatch.Frequency.ToString(
                CultureInfo.InvariantCulture))
            .AppendLine();
        builder.AppendLine(
            "frame_index,sample_timestamp,native_reported_fps," +
            "cpu_frame_ms,cpu_submit_ms,native_render_active_cpu_ms," +
            "native_present_cpu_ms,native_render_active_cpu_peak_ms," +
            "native_present_cpu_peak_ms," +
            "presented_frame_count_since_reset,profiler_reset_serial," +
            "gpu_query_ms,gpu_window_average_ms,opaque_cpu_ms," +
            "atmosphere_cpu_ms,cloud_cpu_ms,fog_cpu_ms,post_cpu_ms");

        foreach (EditorProfilerPoint point in points)
        {
            Append(builder, point.FrameIndex);
            Append(builder, point.SampleTimestamp);
            Append(builder, point.Fps);
            Append(builder, point.CpuFrameMs);
            Append(builder, point.CpuSubmitMs);
            Append(builder, point.NativeRenderActiveCpuMs);
            Append(builder, point.NativePresentCpuMs);
            Append(builder, point.NativeRenderActiveCpuPeakMs);
            Append(builder, point.NativePresentCpuPeakMs);
            Append(builder, point.PresentedFrameCountSinceReset);
            Append(builder, point.ProfilerResetSerial);
            Append(builder, point.GpuFrameMs);
            Append(builder, point.GpuFrameAverageMs);
            Append(builder, point.OpaqueCpuMs);
            Append(builder, point.AtmosphereCpuMs);
            Append(builder, point.CloudCpuMs);
            Append(builder, point.FogCpuMs);
            Append(builder, point.PostCpuMs, final: true);
        }

        return builder.ToString();
    }

    private static void Append(
        StringBuilder builder,
        ulong value)
    {
        if (builder.Length > 0 &&
            builder[^1] is not '\n' and not '\r')
        {
            builder.Append(',');
        }
        builder.Append(value.ToString(CultureInfo.InvariantCulture));
    }

    private static void Append(
        StringBuilder builder,
        long value)
    {
        if (builder.Length > 0 &&
            builder[^1] is not '\n' and not '\r')
        {
            builder.Append(',');
        }
        builder.Append(value.ToString(CultureInfo.InvariantCulture));
    }

    private static void Append(
        StringBuilder builder,
        float value,
        bool final = false)
    {
        builder.Append(',')
            .Append(value.ToString(
                "0.######",
                CultureInfo.InvariantCulture));
        builder.Append(final ? "\r\n" : "");
    }
}
