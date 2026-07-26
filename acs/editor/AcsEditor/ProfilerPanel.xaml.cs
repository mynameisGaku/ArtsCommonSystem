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
    private bool _hasLatestSnapshot;
    private EditorProfilerSnapshot _latestSnapshot;
    private double _lastPresentationMilliseconds;
    private double _maximumPresentationMilliseconds;

    internal Func<IntPtr>? EngineProvider { get; set; }
    internal Func<EditorLogPumpSnapshot>? LogPumpProvider { get; set; }
    internal Func<ViewportNativeRenderDiagnostic>? NativeRenderProvider {
        get;
        set;
    }
    internal Func<EditorDispatcherWatchdogSnapshot>?
        DispatcherWatchdogProvider { get; set; }
    internal Action? ResetEditorPeaks { get; set; }
    internal event Action<string>? SummaryChanged;

    public ProfilerPanel()
    {
        InitializeComponent();
        _timer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = EditorProfilerPresentationPolicy.SampleInterval(
                panelVisible: false),
        };
        _timer.Tick += OnSampleTick;
    }

    internal void Start()
    {
        UpdateSampleCadence();
        if (!_timer.IsEnabled)
            _timer.Start();
    }

    internal void Stop() => _timer.Stop();

    internal void ResetHistory()
    {
        _history.Reset();
        _hasLatestSnapshot = false;
        HistoryGraph.SetHistory(_history.Points);
        HistoryGraph.SetPeaks(-1, -1);
        HistoryGraph.ResetRenderDiagnostics();
        _lastPresentationMilliseconds = 0;
        _maximumPresentationMilliseconds = 0;
        AvailabilityText.Text = _interopUnavailable
            ? "Profiler ABI unavailable"
            : "History reset";
    }

    private void OnLoaded(object sender, RoutedEventArgs e) => Start();
    private void OnUnloaded(object sender, RoutedEventArgs e) => Stop();

    private void OnSampleTick(object? sender, EventArgs e)
    {
        long started = System.Diagnostics.Stopwatch.GetTimestamp();
        try
        {
            SampleProfiler();
        }
        finally
        {
            _lastPresentationMilliseconds =
                (System.Diagnostics.Stopwatch.GetTimestamp() - started) *
                1000.0 / System.Diagnostics.Stopwatch.Frequency;
            _maximumPresentationMilliseconds = Math.Max(
                _maximumPresentationMilliseconds,
                _lastPresentationMilliseconds);
            if (IsVisible)
                UpdateProfilerPresentationMetrics();
        }
    }

    private void SampleProfiler()
    {
        // Managed diagnostics remain live even when the renderer stops
        // advancing its native frame index. Only native history and graph
        // invalidation are gated by EditorProfilerHistory.Add.
        if (EditorProfilerPresentationPolicy.ShouldPresentManagedDiagnostics(
                IsVisible))
            UpdateEditorUiMetrics();
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
            _latestSnapshot = snapshot;
            _hasLatestSnapshot = true;
            if (EditorProfilerPresentationPolicy.ShouldPresentDetails(
                    IsVisible,
                    nativeFrameAdvanced: true))
            {
                UpdateValues(snapshot);
                HistoryGraph.SetHistory(_history.Points);
            }
            else
            {
                // Keep the always-visible status strip current without
                // touching dozens of collapsed TextBlocks or invalidating the
                // history graph. Full presentation catches up on reveal.
                SummaryChanged?.Invoke(
                    EditorProfilerFormatting.CompactSummary(
                        _history.Average()));
            }
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
        MeshCacheStateValue.Text =
            (snapshot.Flags &
             EditorProfilerContract.FlagSceneMeshCacheRebuilt) != 0
                ? "REBUILT"
                : "HIT";

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

    private void UpdateEditorUiMetrics()
    {
        EditorLogPumpSnapshot snapshot =
            LogPumpProvider?.Invoke() ?? default;
        LogPumpStateValue.Text = snapshot.LastBatchEntries > 0
            ? $"{snapshot.LastBatchEntries} lines"
            : "IDLE";
        LogPumpStateValue.ToolTip =
            $"{snapshot.EngineEntriesDrained:N0} engine lines in " +
            $"{snapshot.UiBatches:N0} WPF batch(es); " +
            $"largest batch {snapshot.MaximumBatchEntries:N0}.";
        LogDrainValue.Text =
            $"{snapshot.LastDrainMilliseconds:0.00} / " +
            $"{snapshot.MaximumDrainMilliseconds:0.00} ms";
        LogApplyValue.Text =
            $"{snapshot.LastApplyMilliseconds:0.00} / " +
            $"{snapshot.MaximumApplyMilliseconds:0.00} ms";
        LogRetentionValue.Text =
            $"{snapshot.RetainedEntries:N0}/{snapshot.RetentionCapacity:N0} / " +
            $"{snapshot.RetentionTrimmed:N0}";

        ViewportNativeRenderDiagnostic native =
            NativeRenderProvider?.Invoke() ?? default;
        NativeCallValue.Text =
            $"{native.LastNativeCallMilliseconds:0.00} / " +
            $"{native.MaximumNativeCallMilliseconds:0.00} ms";
        NativeBackpressureValue.Text =
            $"{native.GpuBackpressureYieldCount:N0} / " +
            $"{native.SlowNativeCallCount:N0}";
        NativeCallValue.ToolTip =
            $"Last call: {native.LastNativeCallKind}; " +
            $"{native.NativeCallCount:N0} measured native calls.";

        EditorDispatcherWatchdogSnapshot dispatcher =
            DispatcherWatchdogProvider?.Invoke() ?? default;
        DispatcherGapValue.Text =
            $"{dispatcher.LastDispatcherGapMilliseconds:0.0} / " +
            $"{dispatcher.MaximumDispatcherGapMilliseconds:0.0} ms";
        DispatcherGapValue.ToolTip =
            $"Heartbeat age {dispatcher.HeartbeatAgeMilliseconds:0.0} ms; " +
            $"phase: {dispatcher.Phase ?? "unavailable"}; " +
            $"{dispatcher.HeartbeatCount:N0} heartbeats.";
        DispatcherStallValue.Text = dispatcher.StallActive
            ? $"ACTIVE {dispatcher.ActiveStallMilliseconds:0} ms"
            : $"{dispatcher.StallCount:N0} / " +
              $"{dispatcher.LongestStallMilliseconds:0.0} ms";
    }

    private void UpdateProfilerPresentationMetrics()
    {
        ProfilerUiValue.Text =
            $"{_lastPresentationMilliseconds:0.00} / " +
            $"{_maximumPresentationMilliseconds:0.00} ms";
        ProfilerGraphValue.Text =
            $"{HistoryGraph.LastRenderMilliseconds:0.00} / " +
            $"{HistoryGraph.MaximumRenderMilliseconds:0.00} ms";
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
        ResetEditorPeaks?.Invoke();
        ResetHistory();
    }

    private void OnIsVisibleChanged(
        object sender,
        DependencyPropertyChangedEventArgs e)
    {
        UpdateSampleCadence();
        if (IsVisible && _hasLatestSnapshot)
        {
            UpdateValues(_latestSnapshot);
            UpdateEditorUiMetrics();
            UpdateProfilerPresentationMetrics();
            HistoryGraph.SetHistory(_history.Points);
        }
    }

    private void UpdateSampleCadence()
    {
        if (_timer == null)
            return;
        TimeSpan requested =
            EditorProfilerPresentationPolicy.SampleInterval(IsVisible);
        if (_timer.Interval != requested)
            _timer.Interval = requested;
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
