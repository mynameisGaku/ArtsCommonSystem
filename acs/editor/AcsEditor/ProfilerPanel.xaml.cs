// SPDX-License-Identifier: Apache-2.0

using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace AcsEditor;

public partial class ProfilerPanel : UserControl
{
    private readonly DispatcherTimer _timer;
    private readonly EditorProfilerHistory _history = new(120);
    private bool _interopUnavailable;

    internal Func<IntPtr>? EngineProvider { get; set; }
    internal event Action<string>? SummaryChanged;

    public ProfilerPanel()
    {
        InitializeComponent();
        _timer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(100),
        };
        _timer.Tick += OnSampleTick;
    }

    internal void Start()
    {
        if (!_timer.IsEnabled)
            _timer.Start();
    }

    internal void Stop() => _timer.Stop();

    internal void ResetHistory()
    {
        _history.Reset();
        HistoryGraph.SetHistory(_history.Points);
        HistoryGraph.SetPeaks(-1, -1);
        AvailabilityText.Text = _interopUnavailable
            ? "Profiler ABI unavailable"
            : "History reset";
    }

    private void OnLoaded(object sender, RoutedEventArgs e) => Start();
    private void OnUnloaded(object sender, RoutedEventArgs e) => Stop();

    private void OnSampleTick(object? sender, EventArgs e)
    {
        if (_history.IsPaused || _interopUnavailable)
            return;

        IntPtr engine = EngineProvider?.Invoke() ?? IntPtr.Zero;
        if (engine == IntPtr.Zero)
        {
            AvailabilityText.Text = "Waiting for renderer…";
            return;
        }

        if (!EngineInterop.TryGetProfilerSnapshot(engine, out EditorProfilerSnapshot snapshot))
        {
            _interopUnavailable = true;
            AvailabilityText.Text = "Profiler ABI unavailable — rebuild acs_editor_abi";
            TimingSourceText.Text = "No timing source";
            return;
        }

        AvailabilityText.Text = "";
        TimingSourceText.Text = EditorProfilerFormatting.TimingSource(snapshot);
        if (_history.Add(snapshot))
        {
            UpdateValues(snapshot);
            HistoryGraph.SetHistory(_history.Points);
        }
    }

    private void UpdateValues(in EditorProfilerSnapshot snapshot)
    {
        EditorProfilerAverage average = _history.Average();
        FpsValue.Text = $"{average.Fps:0.0}";
        CpuFrameValue.Text = EditorProfilerFormatting.Milliseconds(average.CpuFrameMs);
        SubmitValue.Text = EditorProfilerFormatting.Milliseconds(average.CpuSubmitMs);
        GpuFrameValue.Text = EditorProfilerFormatting.Milliseconds(average.GpuFrameMs);
        GpuThroughputValue.Text = EditorProfilerFormatting.GpuThroughput(
            average.GpuFrameMs);
        CpuPeakValue.Text = EditorProfilerFormatting.Peak(
            snapshot.CpuFramePeakMs, snapshot.PeakWindowFrames);
        GpuPeakValue.Text = EditorProfilerFormatting.Peak(
            snapshot.GpuFramePeakMs,
            snapshot.PeakWindowFrames,
            snapshot.GpuLatencyFrames);
        HistoryGraph.SetPeaks(
            snapshot.CpuFramePeakMs,
            snapshot.GpuFramePeakMs);
        DrawCallsValue.Text = snapshot.DrawCalls.ToString("N0");
        DispatchCallsValue.Text = snapshot.DispatchCalls.ToString("N0");
        TrianglesValue.Text = snapshot.Triangles.ToString("N0");

        OpaqueCpuValue.Text = EditorProfilerFormatting.Milliseconds(snapshot.OpaqueCpuMs);
        AtmosphereCpuValue.Text = EditorProfilerFormatting.Milliseconds(snapshot.AtmosphereCpuMs);
        CloudCpuValue.Text = EditorProfilerFormatting.Milliseconds(snapshot.CloudCpuMs);
        FogCpuValue.Text = EditorProfilerFormatting.Milliseconds(snapshot.FogCpuMs);
        PostCpuValue.Text = EditorProfilerFormatting.Milliseconds(snapshot.PostCpuMs);

        uint queryCount = snapshot.GpuQueryWindowCount;
        OpaqueGpuValue.Text = EditorProfilerFormatting.GpuAveragePeak(
            snapshot.OpaqueGpuAverageMs,
            snapshot.OpaqueGpuWindowPeakMs,
            queryCount);
        AtmosphereGpuValue.Text = EditorProfilerFormatting.GpuAveragePeak(
            snapshot.AtmosphereGpuAverageMs,
            snapshot.AtmosphereGpuWindowPeakMs,
            queryCount);
        CloudGpuValue.Text = EditorProfilerFormatting.GpuAveragePeak(
            snapshot.CloudGpuAverageMs,
            snapshot.CloudGpuWindowPeakMs,
            queryCount);
        FogGpuValue.Text = EditorProfilerFormatting.GpuAveragePeak(
            snapshot.FogGpuAverageMs,
            snapshot.FogGpuWindowPeakMs,
            queryCount);
        PostGpuValue.Text = EditorProfilerFormatting.GpuAveragePeak(
            snapshot.PostGpuAverageMs,
            snapshot.PostGpuWindowPeakMs,
            queryCount);
        string gpuWindowTooltip = EditorProfilerFormatting.GpuWindowTooltip(
            queryCount,
            snapshot.GpuQueryWindowCapacity,
            snapshot.GpuLatencyFrames);
        OpaqueGpuValue.ToolTip = gpuWindowTooltip;
        AtmosphereGpuValue.ToolTip = gpuWindowTooltip;
        CloudGpuValue.ToolTip = gpuWindowTooltip;
        FogGpuValue.ToolTip = gpuWindowTooltip;
        PostGpuValue.ToolTip = gpuWindowTooltip;

        bool clouds = (snapshot.Flags & EditorProfilerContract.FlagClouds) != 0;
        CloudStateValue.Text = clouds ? "ACTIVE" : "OFF";
        CloudResolutionValue.Text =
            $"{snapshot.ViewportWidth}×{snapshot.ViewportHeight} / " +
            $"{snapshot.CloudWidth}×{snapshot.CloudHeight}";
        CloudScaleValue.Text = $"{snapshot.CloudRenderScale:0.00}×";
        CloudStepsValue.Text =
            $"{snapshot.CloudMarchSteps} / {snapshot.CloudLightSteps}";

        SummaryChanged?.Invoke(EditorProfilerFormatting.CompactSummary(average));
    }

    private void OnPauseChanged(object sender, RoutedEventArgs e)
    {
        _history.IsPaused = PauseButton.IsChecked == true;
        PauseButton.Content = _history.IsPaused ? "Resume" : "Pause";
        AvailabilityText.Text = _history.IsPaused ? "Sampling paused" : "";
    }

    private void OnReset(object sender, RoutedEventArgs e)
    {
        IntPtr engine = EngineProvider?.Invoke() ?? IntPtr.Zero;
        EngineInterop.ResetProfilerPeaks(engine);
        ResetHistory();
    }

    private void OnDisplayChanged(object sender, RoutedEventArgs e)
    {
        HistoryGraph.ShowCpu = ShowCpuCheck.IsChecked == true;
        HistoryGraph.ShowGpu = ShowGpuCheck.IsChecked == true;
        HistoryGraph.ShowPasses = ShowPassesCheck.IsChecked == true;
        PassMetrics.Visibility = ShowPassesCheck.IsChecked == true
            ? Visibility.Visible : Visibility.Collapsed;
        Visibility counters = ShowCountersCheck.IsChecked == true
            ? Visibility.Visible : Visibility.Collapsed;
        DrawCard.Visibility = counters;
        DispatchCard.Visibility = counters;
        TrianglesCard.Visibility = counters;
        CloudMetrics.Visibility = ShowCloudCheck.IsChecked == true
            ? Visibility.Visible : Visibility.Collapsed;
        HistoryGraph.InvalidateVisual();
    }
}
