// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace AcsEditor;

public partial class MainWindow
{
    [DllImport("user32.dll")]
    private static extern nint GetLastActivePopup(nint window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(nint window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowEnabled(nint window);

    private static readonly object InteractionLogLock = new();
    private static readonly JsonSerializerOptions InteractionReportJson = new()
    {
        WriteIndented = true,
    };

    private DispatcherTimer? _interactionHealthTimer;
    private EditorInteractionHealthKind _lastInteractionHealthKind =
        EditorInteractionHealthKind.Healthy;
    private ulong _lastInteractionProfilerFrame;
    private bool _interactionWindowClosing;
    private InteractionSoakConfiguration? _interactionSoak;
    private DateTimeOffset? _interactionSoakStartedUtc;
    private long _interactionSoakStartedTimestamp;
    private long _lastInteractionSoakTickTimestamp;
    private long _lastInteractionProfilerAdvanceTimestamp;
    private int _interactionSoakTicks;
    private int _interactionSoakProfilerAdvances;
    private double _interactionSoakMaxDispatcherGapMilliseconds;
    private double _interactionSoakMaxProfilerGapMilliseconds;
    private bool _interactionSoakRecoveryPromptObserved;
    private readonly HashSet<string> _interactionSoakFaultCodes =
        new(StringComparer.Ordinal);

    private sealed record InteractionSoakConfiguration(
        TimeSpan Duration,
        string ReportPath,
        Action<int> Complete,
        DateTimeOffset RequestedUtc,
        long RequestedTimestamp,
        bool RequireRecoveryPrompt);

    private sealed record InteractionSoakReport(
        int SchemaVersion,
        string Result,
        DateTimeOffset RequestedUtc,
        DateTimeOffset? StartedUtc,
        DateTimeOffset CompletedUtc,
        double RequestedSeconds,
        double ActualSeconds,
        double DispatcherIntervalMilliseconds,
        int ExpectedDispatcherTicks,
        int RequiredDispatcherTicks,
        int DispatcherTicks,
        double DispatcherTickDensity,
        int RequiredProfilerAdvancedTicks,
        int ProfilerAdvancedTicks,
        double ProfilerAdvanceDensity,
        double MaximumDispatcherGapMilliseconds,
        double MaximumProfilerGapMilliseconds,
        bool RecoveryPromptRequired,
        bool RecoveryPromptObserved,
        string StartupState,
        IReadOnlyList<string> FaultCodes,
        string InteractionLogPath);

    private void InitializeInteractionHealthDiagnostics()
    {
        Loaded += OnInteractionDiagnosticsLoaded;
        Closing += (_, _) => _interactionWindowClosing = true;
        Closed += (_, _) =>
        {
            _interactionWindowClosing = true;
            _interactionHealthTimer?.Stop();
            _interactionHealthTimer = null;
        };
    }

    internal void ConfigureInteractionSoak(
        TimeSpan duration,
        string? reportPath,
        Action<int> complete,
        bool requireRecoveryPrompt = false)
    {
        ArgumentNullException.ThrowIfNull(complete);
        if (!double.IsFinite(duration.TotalSeconds) ||
            duration < TimeSpan.FromSeconds(2) ||
            duration > TimeSpan.FromMinutes(10))
        {
            throw new ArgumentOutOfRangeException(
                nameof(duration),
                "Interaction soak duration must be between 2 seconds and 10 minutes.");
        }

        string output = string.IsNullOrWhiteSpace(reportPath)
            ? Path.Combine(
                Path.GetTempPath(),
                $"acs-editor-interaction-soak-{Environment.ProcessId}.json")
            : Path.GetFullPath(reportPath);
        _interactionSoak = new(
            duration,
            output,
            complete,
            DateTimeOffset.UtcNow,
            Stopwatch.GetTimestamp(),
            requireRecoveryPrompt);
    }

    private void OnInteractionDiagnosticsLoaded(object sender, RoutedEventArgs e)
    {
        if (_interactionHealthTimer != null) return;
        _interactionHealthTimer = new DispatcherTimer(
            TimeSpan.FromMilliseconds(
                EditorInteractionSoakPolicy.TimerIntervalMilliseconds),
            DispatcherPriority.Background,
            OnInteractionHealthTick,
            Dispatcher);
        _interactionHealthTimer.Start();
    }

    private void OnInteractionHealthTick(object? sender, EventArgs e)
    {
        bool profilerAdvanced = false;
        IntPtr engine = RawEngine;
        if (engine != IntPtr.Zero &&
            EngineInterop.TryGetProfilerSnapshot(
                engine,
                out EditorProfilerSnapshot profiler))
        {
            profilerAdvanced = _lastInteractionProfilerFrame != 0 &&
                               profiler.FrameIndex >
                                   _lastInteractionProfilerFrame;
            _lastInteractionProfilerFrame = Math.Max(
                _lastInteractionProfilerFrame,
                profiler.FrameIndex);
        }

        Window[] visibleOwned = OwnedWindows
            .Cast<Window>()
            .Where(window => window.IsVisible)
            .ToArray();
        bool recoveryVisible = visibleOwned.Any(
            window => window is SceneRecoveryDialog);
        if (_interactionSoak != null && recoveryVisible)
            _interactionSoakRecoveryPromptObserved = true;

        nint handle = new WindowInteropHelper(this).Handle;
        nint popup = handle == 0 ? 0 : GetLastActivePopup(handle);
        bool nativeWindowEnabled = handle == 0 || IsWindowEnabled(handle);
        bool nativeOwnedPopupVisible = popup != 0 &&
                                       popup != handle &&
                                       IsWindowVisible(popup);

        ViewportPointerCaptureDiagnostic capture =
            _viewport?.GetPointerCaptureDiagnostic() ?? default;
        var sample = new EditorInteractionHealthSample(
            WindowVisible: IsVisible,
            WindowClosing: _interactionWindowClosing,
            WindowEnabled: IsEnabled && nativeWindowEnabled,
            WindowHitTestVisible: IsHitTestVisible,
            NonInteractiveLaunch: App.IsNonInteractiveLaunch,
            WindowMoveSizeActive: _windowMoveSizeActive,
            ProfilerAdvanced: profilerAdvanced,
            ThreadModal: ComponentDispatcher.IsThreadModal,
            VisibleOwnedWindowCount: visibleOwned.Length,
            NativeOwnedPopupVisible: nativeOwnedPopupVisible,
            RecoveryPromptVisible: recoveryVisible,
            ViewportOwnsCapture: capture.OwnsCapture,
            ActivePointerButtonMask: capture.ActiveButtonMask,
            PhysicallyDownPointerButtonMask:
                capture.PhysicallyDownButtonMask,
            PointerMismatchAgeMilliseconds:
                capture.MismatchAgeMilliseconds);
        EditorInteractionHealthAssessment assessment =
            EditorInteractionHealthPolicy.Evaluate(sample);

        if (assessment.Kind != _lastInteractionHealthKind)
        {
            string ownedTitles = visibleOwned.Length == 0
                ? "(none)"
                : string.Join(
                    ", ",
                    visibleOwned.Select(window =>
                        string.IsNullOrWhiteSpace(window.Title)
                            ? window.GetType().Name
                            : window.Title));
            QueueInteractionDiagnostic(
                $"code={assessment.Code} fault={assessment.IsFault} " +
                $"enabled={IsEnabled} nativeEnabled={nativeWindowEnabled} " +
                $"hitTest={IsHitTestVisible} " +
                $"profilerAdvanced={profilerAdvanced} " +
                $"capture={capture.OwnsCapture} " +
                $"activeButtons={capture.ActiveButtonMask} " +
                $"physicalButtons={capture.PhysicallyDownButtonMask} " +
                $"mismatchMs={capture.MismatchAgeMilliseconds:0.0} " +
                $"owned=[{ownedTitles}]");
            _lastInteractionHealthKind = assessment.Kind;
        }

        InteractionSoakConfiguration? soak = _interactionSoak;
        if (soak == null) return;

        DateTimeOffset now = DateTimeOffset.UtcNow;
        long nowTimestamp = Stopwatch.GetTimestamp();
        if (_interactionSoakStartedUtc == null)
        {
            if (_engineStartupState == EditorEngineStartupState.Ready)
            {
                _interactionSoakStartedUtc = now;
                _interactionSoakStartedTimestamp = nowTimestamp;
                _lastInteractionSoakTickTimestamp = nowTimestamp;
                _lastInteractionProfilerAdvanceTimestamp = nowTimestamp;
                QueueInteractionDiagnostic(
                    $"code=SOAK_STARTED seconds={soak.Duration.TotalSeconds:0.###}");
            }
            else if (ElapsedMilliseconds(
                         soak.RequestedTimestamp,
                         nowTimestamp) > 45_000.0)
            {
                _interactionSoakFaultCodes.Add("STARTUP_NOT_READY");
                CompleteInteractionSoak(now);
            }
            return;
        }

        _interactionSoakTicks++;
        _interactionSoakMaxDispatcherGapMilliseconds = Math.Max(
            _interactionSoakMaxDispatcherGapMilliseconds,
            ElapsedMilliseconds(
                _lastInteractionSoakTickTimestamp,
                nowTimestamp));
        _lastInteractionSoakTickTimestamp = nowTimestamp;

        _interactionSoakMaxProfilerGapMilliseconds = Math.Max(
            _interactionSoakMaxProfilerGapMilliseconds,
            ElapsedMilliseconds(
                _lastInteractionProfilerAdvanceTimestamp,
                nowTimestamp));
        if (profilerAdvanced)
        {
            _interactionSoakProfilerAdvances++;
            _lastInteractionProfilerAdvanceTimestamp = nowTimestamp;
        }
        if (assessment.IsFault)
            _interactionSoakFaultCodes.Add(assessment.Code);

        if (ElapsedMilliseconds(
                _interactionSoakStartedTimestamp,
                nowTimestamp) >= soak.Duration.TotalMilliseconds)
            CompleteInteractionSoak(now);
    }

    private void CompleteInteractionSoak(DateTimeOffset completedUtc)
    {
        InteractionSoakConfiguration? soak = _interactionSoak;
        if (soak == null) return;
        _interactionSoak = null;

        if (_interactionSoakStartedUtc != null)
        {
            _interactionSoakFaultCodes.UnionWith(
                EditorInteractionSoakPolicy.Evaluate(
                    soak.Duration,
                    _interactionSoakTicks,
                    _interactionSoakProfilerAdvances,
                    _interactionSoakMaxDispatcherGapMilliseconds,
                    _interactionSoakMaxProfilerGapMilliseconds));
        }
        if (soak.RequireRecoveryPrompt &&
            !_interactionSoakRecoveryPromptObserved)
        {
            _interactionSoakFaultCodes.Add("RECOVERY_PROMPT_NOT_OBSERVED");
        }

        int expectedDispatcherTicks =
            EditorInteractionSoakPolicy.ExpectedDispatcherTicks(soak.Duration);
        int requiredDispatcherTicks = Math.Max(
            2,
            (int)Math.Ceiling(
                expectedDispatcherTicks *
                EditorInteractionSoakPolicy.RequiredDispatcherTickDensity));
        int requiredProfilerTicks = Math.Max(
            1,
            (int)Math.Ceiling(
                expectedDispatcherTicks *
                EditorInteractionSoakPolicy.RequiredProfilerAdvanceDensity));
        double actualSeconds = _interactionSoakStartedTimestamp == 0
            ? 0
            : ElapsedMilliseconds(
                  _interactionSoakStartedTimestamp,
                  Stopwatch.GetTimestamp()) / 1000.0;
        double dispatcherDensity =
            _interactionSoakTicks / (double)expectedDispatcherTicks;
        double profilerDensity =
            _interactionSoakProfilerAdvances /
            (double)expectedDispatcherTicks;

        int exitCode = _interactionSoakFaultCodes.Count == 0 ? 0 : 1;
        var report = new InteractionSoakReport(
            EditorReliabilityReportContract.CurrentSchemaVersion,
            exitCode == 0 ? "PASS" : "FAIL",
            soak.RequestedUtc,
            _interactionSoakStartedUtc,
            completedUtc,
            soak.Duration.TotalSeconds,
            actualSeconds,
            EditorInteractionSoakPolicy.TimerIntervalMilliseconds,
            expectedDispatcherTicks,
            requiredDispatcherTicks,
            _interactionSoakTicks,
            dispatcherDensity,
            requiredProfilerTicks,
            _interactionSoakProfilerAdvances,
            profilerDensity,
            _interactionSoakMaxDispatcherGapMilliseconds,
            _interactionSoakMaxProfilerGapMilliseconds,
            soak.RequireRecoveryPrompt,
            _interactionSoakRecoveryPromptObserved,
            _engineStartupState.ToString(),
            _interactionSoakFaultCodes.OrderBy(code => code).ToArray(),
            InteractionDiagnosticLogPath());

        try
        {
            string? parent = Path.GetDirectoryName(soak.ReportPath);
            if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            File.WriteAllText(
                soak.ReportPath,
                JsonSerializer.Serialize(report, InteractionReportJson),
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            Console.Error.WriteLine(
                $"Interaction soak {report.Result}: {soak.ReportPath}");
        }
        catch (Exception error)
        {
            exitCode = 1;
            Console.Error.WriteLine(
                "Interaction soak report write failed: " + error.Message);
        }

        QueueInteractionDiagnostic(
            $"code=SOAK_COMPLETED result={report.Result} " +
            $"ticks={report.DispatcherTicks} " +
            $"profilerAdvances={report.ProfilerAdvancedTicks} " +
            $"maxDispatcherGapMs={report.MaximumDispatcherGapMilliseconds:0.0} " +
            $"maxProfilerGapMs={report.MaximumProfilerGapMilliseconds:0.0} " +
            $"recoveryObserved={report.RecoveryPromptObserved}");
        soak.Complete(exitCode);
    }

    private static double ElapsedMilliseconds(long start, long end)
    {
        if (start <= 0 || end <= start) return 0;
        return (end - start) * 1000.0 / Stopwatch.Frequency;
    }

    private static string InteractionDiagnosticLogPath() =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "ACS",
            "Editor",
            "Diagnostics",
            "interaction-health.log");

    private static void QueueInteractionDiagnostic(string message)
    {
        string line =
            $"{DateTimeOffset.UtcNow:O} pid={Environment.ProcessId} {message}";
        Trace.WriteLine(line);
        Console.Error.WriteLine(line);
        string path = InteractionDiagnosticLogPath();
        _ = Task.Run(() =>
        {
            try
            {
                lock (InteractionLogLock)
                {
                    string? parent = Path.GetDirectoryName(path);
                    if (!string.IsNullOrEmpty(parent))
                        Directory.CreateDirectory(parent);
                    File.AppendAllText(
                        path,
                        line + Environment.NewLine,
                        new UTF8Encoding(
                            encoderShouldEmitUTF8Identifier: false));
                }
            }
            catch
            {
                // Diagnostics must never become an editor availability risk.
            }
        });
    }
}
