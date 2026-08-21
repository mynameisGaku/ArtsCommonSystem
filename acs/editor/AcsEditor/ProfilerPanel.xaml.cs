// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace AcsEditor;

public partial class ProfilerPanel : UserControl
{
    // The render pump's mandatory checkpoint runs at Input priority. Sampling
    // at the same FIFO priority guarantees that the 10 Hz profiler advances
    // under sustained private-HWND rendering without outranking real input.
    internal const DispatcherPriority SamplePriority =
        DispatcherPriority.Input;

    private readonly DispatcherTimer _timer;
    private readonly EditorProfilerHistory _history = new(120);
    private readonly List<EditorProfilerRuntimePoint> _runtimeHistory =
        new(capacity: 120);
    private bool _interopUnavailable;
    private bool _hasLatestSnapshot;
    private EditorProfilerSnapshot _latestSnapshot;
    private EditorCloudWorkloadQueryStatus _latestCloudWorkloadStatus =
        EditorCloudWorkloadQueryStatus.Unsupported;
    private EditorCloudWorkloadSnapshot _latestCloudWorkload;
    private double _lastPresentationMilliseconds;
    private double _maximumPresentationMilliseconds;
    private bool _exportInProgress;
    private bool _captureBoundaryRequired;
    private bool _captureBoundaryArmed;
    private ulong _captureResetSerial;
    private EditorOptionalServiceUiState _profilerServiceState =
        EditorOptionalServiceUiState.Legacy(
            EditorOptionalService.Profiler,
            hostAvailable: false);
    private EditorOptionalServiceUiState _cloudServiceState =
        EditorOptionalServiceUiState.Legacy(
            EditorOptionalService.VolumetricCloudWorkload,
            hostAvailable: false);

    internal Func<IntPtr>? EngineProvider { get; set; }
    internal Func<EditorAbiCapability>? AbiCapabilitiesProvider {
        get;
        set;
    }
    internal Func<EditorLogPumpSnapshot>? LogPumpProvider { get; set; }
    internal Func<ViewportNativeRenderDiagnostic>? NativeRenderProvider {
        get;
        set;
    }
    internal Func<EditorDispatcherWatchdogSnapshot>?
        DispatcherWatchdogProvider { get; set; }
    internal Func<
        EditorOptionalService,
        EditorOptionalServiceUiState>?
        OptionalServiceUiStateProvider { get; set; }
    internal Action? ResetEditorPeaks { get; set; }
    internal event Action<string>? SummaryChanged;

    public ProfilerPanel()
    {
        InitializeComponent();
        UpdateBudgetPresentation();
        _timer = new DispatcherTimer(SamplePriority)
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

    /// <summary>
    /// Copies the already-sampled bounded history for an unattended capture.
    /// This runs only at an explicit capture boundary; it does not add work to
    /// the renderer or the profiler sampling hot path.
    /// </summary>
    internal EditorProfilerCaptureSnapshot CreateAutomationCaptureSnapshot()
    {
        Dispatcher.VerifyAccess();
        bool hasNativeDiagnostic = NativeRenderProvider != null;
        ViewportNativeRenderDiagnostic nativeDiagnostic =
            NativeRenderProvider?.Invoke() ?? default;
        bool hasDispatcherDiagnostic =
            DispatcherWatchdogProvider != null;
        EditorDispatcherWatchdogSnapshot dispatcherDiagnostic =
            DispatcherWatchdogProvider?.Invoke() ?? default;
        return new EditorProfilerCaptureSnapshot(
            _history.Points.ToArray(),
            _hasLatestSnapshot,
            _latestSnapshot,
            _latestCloudWorkloadStatus,
            _latestCloudWorkload,
            hasNativeDiagnostic,
            nativeDiagnostic,
            hasDispatcherDiagnostic,
            dispatcherDiagnostic,
            _runtimeHistory.ToArray());
    }

    internal int AutomationCaptureTargetFps => TargetFps;

    internal void BeginAutomationCapture()
    {
        Dispatcher.VerifyAccess();
        ResetAtCaptureBoundary();
    }

    internal void ResetHistory()
    {
        _history.Reset();
        _runtimeHistory.Clear();
        _hasLatestSnapshot = false;
        _latestCloudWorkloadStatus =
            EditorCloudWorkloadQueryStatus.Unsupported;
        _latestCloudWorkload = default;
        UpdateCloudValues(default);
        HistoryGraph.SetHistory(_history.Points);
        HistoryGraph.SetPeaks(-1, -1);
        HistoryGraph.ResetRenderDiagnostics();
        _lastPresentationMilliseconds = 0;
        _maximumPresentationMilliseconds = 0;
        UpdateBudgetPresentation();
        AvailabilityText.Text = _interopUnavailable
            ? "Profiler ABI unavailable"
            : "History reset";
    }

    internal void LoadVisualTestHistory()
    {
        _captureBoundaryRequired = false;
        _captureBoundaryArmed = false;
        _captureResetSerial = 0;
        _history.Reset();
        EditorProfilerSnapshot latest = default;
        for (ulong frame = 1; frame <= 60; frame++)
        {
            bool spike = frame > 54;
            latest = new EditorProfilerSnapshot
            {
                Version = EditorProfilerContract.Version,
                StructSize = EditorProfilerContract.SnapshotSize,
                TimingSource =
                    EditorProfilerContract.TimingGpuTimestamp,
                Flags =
                    EditorProfilerContract.FlagView3D |
                    EditorProfilerContract.FlagClouds |
                    EditorProfilerContract.FlagFog |
                    EditorProfilerContract.FlagGpuTimingsValid |
                    EditorProfilerContract.FlagFrustumCullingEnabled |
                    EditorProfilerContract.FlagGameView |
                    EditorProfilerContract.FlagRuntimeSceneCamera,
                FrameIndex = frame,
                DrawCalls = 184,
                DispatchCalls = 11,
                Triangles = 1_284_320,
                Fps = spike ? 205.0f : 292.0f,
                CpuFrameMs = spike ? 4.8f : 2.4f,
                CpuSubmitMs = spike ? 0.92f : 0.48f,
                GpuFrameMs = spike ? 4.2f : 3.0f,
                OpaqueCpuMs = 0.54f,
                AtmosphereCpuMs = 0.18f,
                CloudCpuMs = 0.31f,
                FogCpuMs = 0.07f,
                PostCpuMs = 0.22f,
                ViewportWidth = 1920,
                ViewportHeight = 1080,
                CloudWidth = 960,
                CloudHeight = 540,
                CloudMarchSteps = 96,
                CloudLightSteps = 12,
                CloudRenderScale = 0.5f,
                GpuFrameIndex = frame > 2 ? frame - 2 : 0,
                CpuFramePeakMs = 4.8f,
                GpuFramePeakMs = 4.2f,
                PeakWindowFrames = 120,
                GpuLatencyFrames = 2,
                GpuQueryWindowCount = (uint)frame,
                GpuQueryWindowCapacity = 120,
                GpuFrameAverageMs = 3.12f,
                OpaqueGpuAverageMs = 0.72f,
                AtmosphereGpuAverageMs = 0.34f,
                CloudGpuAverageMs = 1.18f,
                FogGpuAverageMs = 0.16f,
                PostGpuAverageMs = 0.72f,
                OpaqueGpuWindowPeakMs = 0.91f,
                AtmosphereGpuWindowPeakMs = 0.48f,
                CloudGpuWindowPeakMs = 1.62f,
                FogGpuWindowPeakMs = 0.25f,
                PostGpuWindowPeakMs = 0.94f,
                FrustumTested = 420,
                FrustumVisible = 127,
                FrustumCulled = 293,
                ActiveCameraNodeId = 17,
                NativeRenderActiveCpuMs =
                    spike ? 3.72f : 1.86f,
                NativePresentCpuMs =
                    spike ? 0.92f : 0.48f,
                NativeRenderActiveCpuPeakMs = 3.72f,
                NativePresentCpuPeakMs = 0.92f,
                PresentedFrameCountSinceReset = frame,
                ProfilerResetSerial = 3,
            };
            _history.Add(latest);
        }

        _latestSnapshot = latest;
        _hasLatestSnapshot = true;
        _latestCloudWorkloadStatus =
            EditorCloudWorkloadQueryStatus.Unsupported;
        UpdateValues(latest);
        UpdateEditorUiMetrics();
        UpdateProfilerPresentationMetrics();
        HistoryGraph.SetHistory(_history.Points);
        AvailabilityText.Text = "Visual regression fixture";
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
        IntPtr engine = EngineProvider?.Invoke() ?? IntPtr.Zero;
        RefreshOptionalServiceStates(engine);
        if (_history.IsPaused || _interopUnavailable)
            return;

        if (engine == IntPtr.Zero)
        {
            _latestCloudWorkloadStatus =
                EngineInterop.QueryCloudWorkloadSnapshot(
                    IntPtr.Zero,
                    AbiCapabilitiesProvider?.Invoke() ??
                        EditorAbiCapability.None,
                    out _latestCloudWorkload);
            if (IsVisible)
                UpdateCloudValues(
                    _hasLatestSnapshot
                        ? _latestSnapshot
                        : default);
            AvailabilityText.Text = "Waiting for renderer…";
            return;
        }

        if (!_profilerServiceState.CanInvoke)
        {
            AvailabilityText.Text =
                _profilerServiceState.StatusText;
            AvailabilityText.ToolTip =
                _profilerServiceState.ToolTip;
            TimingSourceText.Text = "No timing source";
            RefreshCloudSnapshot(engine, profilerFrameIndex: null);
            return;
        }

        if (!EngineInterop.TryGetProfilerSnapshot(
                engine,
                out EditorProfilerSnapshot snapshot))
        {
            _interopUnavailable = true;
            AvailabilityText.Text = "Profiler ABI unavailable — rebuild acs_editor_abi";
            TimingSourceText.Text = "No timing source";
            return;
        }

        AvailabilityText.Text = "";
        TimingSourceText.Text = EditorProfilerFormatting.TimingSource(snapshot);
        if (_captureBoundaryRequired &&
            (!_captureBoundaryArmed ||
             !EditorProfilerCaptureBoundaryPolicy.Accepts(
                 snapshot,
                 _captureResetSerial)))
        {
            AvailabilityText.Text =
                "Waiting for first post-reset profiler frame…";
            return;
        }

        long sampleTimestamp =
            System.Diagnostics.Stopwatch.GetTimestamp();
        AddRuntimeSample(snapshot.FrameIndex, sampleTimestamp);
        RefreshCloudSnapshot(engine, snapshot.FrameIndex);
        if (_history.Add(snapshot, sampleTimestamp))
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

    private void AddRuntimeSample(
        ulong frameIndex,
        long sampleTimestamp)
    {
        ViewportNativeRenderDiagnostic native =
            NativeRenderProvider?.Invoke() ?? default;
        EditorDispatcherWatchdogSnapshot dispatcher =
            DispatcherWatchdogProvider?.Invoke() ?? default;
        if (_runtimeHistory.Count == 120)
            _runtimeHistory.RemoveAt(0);
        _runtimeHistory.Add(new EditorProfilerRuntimePoint(
            frameIndex,
            sampleTimestamp,
            Math.Max(0L, native.GpuBackpressureYieldCount),
            Math.Max(0L, native.GpuBackpressureInputRetryCount),
            Math.Max(
                0L,
                native.GpuBackpressureBackgroundFallbackCount),
            Math.Max(0L, native.GpuReadyAfterRetryCount),
            Math.Max(0L, native.RenderFairnessYieldCount),
            Math.Max(0L, native.RenderInputContinuationYieldCount),
            Math.Max(0L, native.RenderMaintenanceYieldCount),
            NonNegative(
                native.LastGpuBackpressureEpochMilliseconds),
            NonNegative(
                native.LastRenderContinuationQueueWaitMilliseconds),
            NonNegative(
                native.LastRenderMaintenanceQueueWaitMilliseconds),
            NonNegative(dispatcher.LastDispatcherGapMilliseconds),
            NonNegative(dispatcher.HeartbeatAgeMilliseconds)));
    }

    private static double NonNegative(double value) =>
        double.IsFinite(value) && value >= 0 ? value : 0;

    private void UpdateValues(in EditorProfilerSnapshot snapshot)
    {
        EditorProfilerAverage average = _history.Average();
        FpsValue.Text = $"{average.Fps:0.0}";
        NativeFpsValue.Text = $"native {snapshot.Fps:0.0}";
        CpuFrameValue.Text = EditorProfilerFormatting.Milliseconds(average.CpuFrameMs);
        SubmitValue.Text = EditorProfilerFormatting.Milliseconds(average.CpuSubmitMs);
        NativeStageValue.Text =
            $"{snapshot.NativeRenderActiveCpuMs:0.00} / " +
            $"{snapshot.NativePresentCpuMs:0.00} ms";
        NativeStageValue.ToolTip =
            $"Active / Present peak since capture reset: " +
            $"{snapshot.NativeRenderActiveCpuPeakMs:0.00} / " +
            $"{snapshot.NativePresentCpuPeakMs:0.00} ms; " +
            $"{snapshot.PresentedFrameCountSinceReset:N0} presented; " +
            $"reset #{snapshot.ProfilerResetSerial:N0}.";
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
        CullingStateValue.Text =
            EditorProfilerFormatting.CullingState(snapshot);
        CullingCountsValue.Text =
            EditorProfilerFormatting.CullingCounts(snapshot);

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

        UpdateCloudValues(snapshot);
        UpdateBudgetPresentation();

        SummaryChanged?.Invoke(EditorProfilerFormatting.CompactSummary(average));
    }

    private int TargetFps
    {
        get
        {
            if (TargetFpsCombo?.SelectedItem is ComboBoxItem item &&
                int.TryParse(item.Tag?.ToString(), out int targetFps) &&
                targetFps is >= EditorProfilerBudget.MinimumTargetFps and
                    <= EditorProfilerBudget.MaximumTargetFps)
            {
                return targetFps;
            }

            return EditorProfilerBudget.DefaultTargetFps;
        }
    }

    private void UpdateBudgetPresentation()
    {
        if (FrameBudgetValue == null ||
            CpuP95Value == null ||
            GpuP95Value == null ||
            BudgetStateValue == null)
        {
            return;
        }

        EditorProfilerBudgetAnalysis analysis =
            EditorProfilerBudget.Analyze(_history.Points, TargetFps);
        HistoryGraph.TargetBudgetMilliseconds =
            analysis.BudgetMilliseconds;
        FrameBudgetValue.Text =
            $"{analysis.BudgetMilliseconds:0.00} ms";
        CpuP95Value.Text = EditorProfilerBudget.Percentile(
            analysis.CpuP95Milliseconds,
            analysis.CpuSampleCount);
        GpuP95Value.Text = EditorProfilerBudget.Percentile(
            analysis.GpuP95Milliseconds,
            analysis.GpuSampleCount);
        CpuP95Value.ToolTip =
            $"Over budget: {EditorProfilerBudget.Violations(
                analysis.CpuOverBudgetSamples,
                analysis.CpuSampleCount)}";
        GpuP95Value.ToolTip =
            $"Over budget: {EditorProfilerBudget.Violations(
                analysis.GpuOverBudgetSamples,
                analysis.GpuSampleCount)}";
        BudgetStateValue.Text =
            EditorProfilerBudget.StateLabel(analysis);
        string brushKey = analysis.State switch
        {
            EditorProfilerBudgetState.WithinBudget => "OkFg",
            EditorProfilerBudgetState.NoSamples => "TextDim",
            _ => "WarnFg",
        };
        BudgetStateValue.Foreground =
            (Brush)(TryFindResource(brushKey) ?? Brushes.Gray);
        BudgetStateValue.ToolTip =
            $"Target {analysis.TargetFps} FPS = " +
            $"{analysis.BudgetMilliseconds:0.000} ms. " +
            "P95 is computed from bounded 10 Hz profiler samples, " +
            "not inferred from editor presentation cadence.";
    }

    private void UpdateCloudValues(in EditorProfilerSnapshot profiler)
    {
        EditorCloudWorkloadSnapshot workload = _latestCloudWorkload;
        EditorCloudWorkloadQueryStatus status =
            _latestCloudWorkloadStatus;
        bool available =
            status == EditorCloudWorkloadQueryStatus.Available;
        bool showServiceStatus =
            _cloudServiceState.Source !=
                EditorOptionalServiceUiSource.LegacyCompatibility &&
            (_cloudServiceState.State !=
                EditorOptionalServiceState.Enabled ||
             !_cloudServiceState.CanInvoke);

        CloudStateValue.Text =
            showServiceStatus
                ? _cloudServiceState.StatusText
                : EditorCloudWorkloadFormatting.State(
                    status,
                    workload);
        CloudStateValue.ToolTip = _cloudServiceState.ToolTip;
        CloudResolutionValue.Text = available
            ? $"{workload.OutputWidth}x{workload.OutputHeight} / " +
              $"{workload.TraceWidth}x{workload.TraceHeight}"
            : $"{profiler.ViewportWidth}x{profiler.ViewportHeight} / " +
              $"{profiler.CloudWidth}x{profiler.CloudHeight}";
        CloudScaleValue.Text = $"{profiler.CloudRenderScale:0.00}x";
        CloudStepsValue.Text =
            $"{profiler.CloudMarchSteps} / {profiler.CloudLightSteps}";
        CloudDispatchValue.Text = available
            ? EditorCloudWorkloadFormatting.Dispatches(workload)
            : "N/A";
        CloudTotalInvocationsValue.Text = available
            ? EditorCloudWorkloadFormatting.Invocations(
                workload.TotalLogicalInvocations,
                workload.TotalLaunchedThreads)
            : "N/A";
        CloudTraceInvocationsValue.Text = available
            ? EditorCloudWorkloadFormatting.Invocations(
                workload.TraceLogicalInvocations,
                workload.TraceLaunchedThreads)
            : "N/A";
        CloudResolveInvocationsValue.Text = available
            ? EditorCloudWorkloadFormatting.Invocations(
                workload.ResolveLogicalInvocations,
                workload.ResolveLaunchedThreads)
            : "N/A";
        CloudBakeValue.Text = available
            ? EditorCloudWorkloadFormatting.OneTimeBake(workload)
            : "N/A";
        CloudShadowInvocationsValue.Text = available
            ? $"internal {workload.ShadowCacheDispatches:N0} dispatch; " +
              EditorCloudWorkloadFormatting.Invocations(
                  workload.ShadowCacheLogicalInvocations,
                  workload.ShadowCacheLaunchedThreads) +
              $" | world {workload.WorldShadowDispatches:N0} dispatch; " +
              EditorCloudWorkloadFormatting.Invocations(
                  workload.WorldShadowLogicalInvocations,
                  workload.WorldShadowLaunchedThreads)
            : "N/A";
        CloudHistoryValue.Text = available
            ? EditorCloudWorkloadFormatting.History(workload)
            : "N/A";
        CloudSampleCeilingsValue.Text = available
            ? $"{workload.MaximumViewSamples:N0} view / " +
              $"{workload.MaximumLightSamples:N0} light / " +
              $"{workload.MaximumWorldShadowSamples:N0} world shadow"
            : "N/A";
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
        NativeRetryValue.Text =
            $"{native.GpuBackpressureInputRetryCount:N0} / " +
            $"{native.GpuBackpressureBackgroundFallbackCount:N0}";
        NativeFairnessValue.Text =
            $"{native.GpuReadyAfterRetryCount:N0} / " +
            $"{native.RenderFairnessYieldCount:N0}";
        NativeBusyEpochValue.Text =
            $"{native.LastGpuBackpressureEpochMilliseconds:0.00} / " +
            $"{native.MaximumGpuBackpressureEpochMilliseconds:0.00} ms";
        NativeBurstValue.Text =
            $"{native.PeakPresentedRenderBurstFrames:N0} / " +
            $"{native.PeakRenderBurstActiveCpuMilliseconds:0.00} ms";
        NativeYieldClassValue.Text =
            $"{native.RenderInputContinuationYieldCount:N0} / " +
            $"{native.RenderMaintenanceYieldCount:N0}";
        NativeContinuationWaitValue.Text =
            $"{native.LastRenderContinuationQueueWaitMilliseconds:0.00} / " +
            $"{native.MaximumRenderContinuationQueueWaitMilliseconds:0.00} ms";
        NativeMaintenanceWaitValue.Text =
            $"{native.LastRenderMaintenanceQueueWaitMilliseconds:0.00} / " +
            $"{native.MaximumRenderMaintenanceQueueWaitMilliseconds:0.00} ms";
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
        RefreshOptionalServiceStates(
            EngineProvider?.Invoke() ?? IntPtr.Zero);
        if (!_profilerServiceState.CanInvoke)
        {
            AvailabilityText.Text =
                _profilerServiceState.StatusText;
            AvailabilityText.ToolTip =
                _profilerServiceState.ToolTip;
            return;
        }
        ResetAtCaptureBoundary();
    }

    private void ResetAtCaptureBoundary()
    {
        bool hadPreviousSnapshot = _hasLatestSnapshot;
        ulong previousResetSerial =
            hadPreviousSnapshot
                ? _latestSnapshot.ProfilerResetSerial
                : 0;
        IntPtr engine = EngineProvider?.Invoke() ?? IntPtr.Zero;
        EngineInterop.ResetProfilerPeaks(engine);
        ResetEditorPeaks?.Invoke();
        bool hasResetSnapshot =
            EngineInterop.TryGetProfilerSnapshot(
                engine,
                out EditorProfilerSnapshot resetSnapshot);
        ResetHistory();
        _captureBoundaryRequired = true;
        _captureBoundaryArmed =
            EditorProfilerCaptureBoundaryPolicy.TryArm(
                hadPreviousSnapshot,
                previousResetSerial,
                hasResetSnapshot,
                resetSnapshot,
                out _captureResetSerial);
        AvailabilityText.Text = _captureBoundaryArmed
            ? "Waiting for first post-reset profiler frame…"
            : "Profiler capture reset boundary unavailable";
    }

    private void OnTargetFpsChanged(
        object sender,
        SelectionChangedEventArgs e) =>
        UpdateBudgetPresentation();

    private async void OnExportCsv(
        object sender,
        RoutedEventArgs e)
    {
        if (_exportInProgress)
            return;
        if (_history.Points.Count == 0)
        {
            AvailabilityText.Text =
                "No profiler samples to export";
            return;
        }

        var dialog = new SaveFileDialog
        {
            Title = "Export Profiler Capture",
            Filter = "CSV capture (*.csv)|*.csv|All files (*.*)|*.*",
            DefaultExt = ".csv",
            AddExtension = true,
            OverwritePrompt = true,
            FileName =
                $"acs-profiler-{DateTime.Now:yyyyMMdd-HHmmss}.csv",
        };
        if (dialog.ShowDialog(Window.GetWindow(this)) != true)
            return;

        string csv = EditorProfilerCapture.SerializeCsv(
            _history.Points,
            TargetFps);
        string? temporary = null;
        _exportInProgress = true;
        ExportButton.IsEnabled = false;
        AvailabilityText.Text = "Exporting profiler capture…";
        try
        {
            string destination =
                Path.GetFullPath(dialog.FileName);
            string? directory =
                Path.GetDirectoryName(destination);
            if (string.IsNullOrWhiteSpace(directory))
            {
                throw new IOException(
                    "The selected destination has no parent directory.");
            }

            temporary = Path.Combine(
                directory,
                $".{Path.GetFileName(destination)}." +
                $"{Guid.NewGuid():N}.tmp");
            await File.WriteAllTextAsync(
                temporary,
                csv,
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            File.Move(temporary, destination, overwrite: true);
            AvailabilityText.Text =
                $"Exported {Path.GetFileName(destination)}";
        }
        catch (Exception ex)
        {
            AvailabilityText.Text = "Profiler export failed";
            MessageBox.Show(
                Window.GetWindow(this),
                $"Could not export the profiler capture.\n\n" +
                $"{ex.Message}",
                "Export Profiler Capture",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
        }
        finally
        {
            if (temporary != null)
                TryDeleteTemporaryExport(temporary);
            _exportInProgress = false;
            ExportButton.IsEnabled = true;
        }
    }

    private static void TryDeleteTemporaryExport(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
            // The selected destination is never removed. A failed cleanup of
            // the uniquely named sibling temp file is non-fatal.
        }
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

    private void RefreshOptionalServiceStates(IntPtr engine)
    {
        _profilerServiceState = ResolveOptionalServiceState(
            EditorOptionalService.Profiler,
            engine);
        _cloudServiceState = ResolveOptionalServiceState(
            EditorOptionalService.VolumetricCloudWorkload,
            engine);

        EditorOptionalServiceActionPolicy.ProfilerControlPlan controlPlan =
            EditorOptionalServiceActionPolicy.PlanProfilerControls(
                _profilerServiceState,
                _cloudServiceState);
        ResetButton.IsEnabled = controlPlan.ResetEnabled;
        ResetButton.ToolTip = _profilerServiceState.ToolTip;
        PauseButton.IsEnabled = controlPlan.PauseEnabled;
        AvailabilityText.ToolTip =
            _profilerServiceState.ToolTip;
        ShowCloudCheck.IsEnabled = controlPlan.CloudFilterEnabled;
        ShowCloudCheck.ToolTip = _cloudServiceState.ToolTip;
        CloudMetrics.Opacity =
            _cloudServiceState.CanInvoke ? 1.0 : 0.58;
        CloudMetrics.ToolTip = _cloudServiceState.ToolTip;

        if (!_profilerServiceState.CanInvoke &&
            !_history.IsPaused)
        {
            AvailabilityText.Text =
                _profilerServiceState.StatusText;
            AvailabilityText.ToolTip =
                _profilerServiceState.ToolTip;
        }
    }

    private EditorOptionalServiceUiState ResolveOptionalServiceState(
        EditorOptionalService service,
        IntPtr engine)
    {
        Func<
            EditorOptionalService,
            EditorOptionalServiceUiState>? provider =
            OptionalServiceUiStateProvider;
        return provider != null
            ? provider(service)
            : EditorOptionalServiceUiState.Legacy(
                service,
                engine != IntPtr.Zero);
    }

    private void RefreshCloudSnapshot(
        IntPtr engine,
        ulong? profilerFrameIndex)
    {
        if (!_cloudServiceState.CanInvoke)
        {
            _latestCloudWorkloadStatus =
                EditorCloudWorkloadQueryStatus.RuntimeUnavailable;
            _latestCloudWorkload = default;
            if (IsVisible)
            {
                UpdateCloudValues(
                    _hasLatestSnapshot
                        ? _latestSnapshot
                        : default);
            }
            return;
        }

        _latestCloudWorkloadStatus =
            EngineInterop.QueryCloudWorkloadSnapshot(
                engine,
                AbiCapabilitiesProvider?.Invoke() ??
                    EditorAbiCapability.None,
                out _latestCloudWorkload);
        if (profilerFrameIndex.HasValue &&
            !EditorCloudWorkloadContract.BelongsToProfilerFrame(
                _latestCloudWorkloadStatus,
                _latestCloudWorkload,
                profilerFrameIndex.Value))
        {
            _latestCloudWorkloadStatus =
                EditorCloudWorkloadQueryStatus.ContractError;
        }
        if (IsVisible)
        {
            UpdateCloudValues(
                _hasLatestSnapshot
                    ? _latestSnapshot
                    : default);
        }
    }
}
