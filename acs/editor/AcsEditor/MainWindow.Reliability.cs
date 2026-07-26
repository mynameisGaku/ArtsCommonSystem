// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
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
    private static readonly Queue<InteractionDiagnosticWorkItem>
        InteractionLogQueue = new();
    private static bool _interactionLogPumpRunning;
    private static readonly string InteractionDiagnosticSessionFileName =
        "interaction-health-" +
        DateTimeOffset.UtcNow.ToString(
            "yyyyMMddTHHmmssfffZ",
            CultureInfo.InvariantCulture) +
        "-" +
        Environment.ProcessId.ToString(CultureInfo.InvariantCulture) +
        ".log";
    private static readonly JsonSerializerOptions InteractionReportJson = new()
    {
        WriteIndented = true,
    };
    private const long InteractionDiagnosticLogMaximumBytes =
        2L * 1024L * 1024L;
    internal const DispatcherPriority InteractionHeartbeatPriority =
        DispatcherPriority.Input;

    private DispatcherTimer? _interactionHealthTimer;
    private DispatcherTimer? _dispatcherHeartbeatTimer;
    private EditorDispatcherWatchdog? _dispatcherWatchdog;
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

    private sealed record InteractionDiagnosticWorkItem(
        string Line,
        TaskCompletionSource<bool>? Completion);

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
        _dispatcherWatchdog = new EditorDispatcherWatchdog(
            OnDispatcherWatchdogTransition);
        _dispatcherWatchdog.Beat("constructing main window");
        Loaded += OnInteractionDiagnosticsLoaded;
        Closing += (_, _) => _interactionWindowClosing = true;
        Closed += (_, _) =>
        {
            _interactionWindowClosing = true;
            _interactionHealthTimer?.Stop();
            _interactionHealthTimer = null;
            _dispatcherHeartbeatTimer?.Stop();
            _dispatcherHeartbeatTimer = null;
            _dispatcherWatchdog?.Dispose();
            _dispatcherWatchdog = null;
            Task<bool> closeDiagnostic = QueueInteractionDiagnostic(
                "code=EDITOR_CLOSED",
                awaitWrite: true);
            try
            {
                _ = closeDiagnostic.Wait(TimeSpan.FromMilliseconds(500));
            }
            catch
            {
                // Process shutdown must continue even if diagnostics fail.
            }
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
        _dispatcherWatchdog?.Beat("window loaded");
        _dispatcherHeartbeatTimer = new DispatcherTimer(
            TimeSpan.FromMilliseconds(
                EditorInteractionSoakPolicy.TimerIntervalMilliseconds),
            InteractionHeartbeatPriority,
            OnDispatcherHeartbeatTick,
            Dispatcher);
        _dispatcherHeartbeatTimer.Start();
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
        _dispatcherWatchdog?.SetPhase(
            "interaction health / " + _engineStartupState);
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
            _ = QueueInteractionDiagnostic(
                $"code={assessment.Code} fault={assessment.IsFault} " +
                $"enabled={IsEnabled} nativeEnabled={nativeWindowEnabled} " +
                $"hitTest={IsHitTestVisible} " +
                $"profilerAdvanced={profilerAdvanced} " +
                $"capture={capture.OwnsCapture} " +
                $"activeButtons={capture.ActiveButtonMask} " +
                $"physicalButtons={capture.PhysicallyDownButtonMask} " +
                $"mismatchMs={capture.MismatchAgeMilliseconds.ToString("0.0", CultureInfo.InvariantCulture)} " +
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
                _ = QueueInteractionDiagnostic(
                    "code=SOAK_STARTED seconds=" +
                    soak.Duration.TotalSeconds.ToString(
                        "0.###",
                        CultureInfo.InvariantCulture));
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

    private void OnDispatcherHeartbeatTick(object? sender, EventArgs e) =>
        _dispatcherWatchdog?.Beat(
            "input heartbeat / " + _engineStartupState);

    private void OnDispatcherWatchdogTransition(
        EditorDispatcherWatchdogTransition transition)
    {
        _ = QueueInteractionDiagnostic(
            $"code={(transition.Recovered ? "DISPATCHER_RECOVERED" : "DISPATCHER_STALL")} " +
            $"sequence={transition.Sequence.ToString(CultureInfo.InvariantCulture)} " +
            $"durationMs={transition.DurationMilliseconds.ToString("0.0", CultureInfo.InvariantCulture)} " +
            $"phase={SanitizeDiagnosticField(transition.Phase)} " +
            $"stallCount={transition.StallCount.ToString(CultureInfo.InvariantCulture)} " +
            $"debuggerAttached={Debugger.IsAttached}",
            observedUtc: transition.ObservedUtc);
    }

    private EditorDispatcherWatchdogSnapshot GetDispatcherWatchdogSnapshot() =>
        _dispatcherWatchdog?.Snapshot() ?? default;

    private void ResetDispatcherWatchdogPeaks() =>
        _dispatcherWatchdog?.ResetPeaks();

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

        Task<bool> diagnosticWrite = QueueInteractionDiagnostic(
            $"code=SOAK_COMPLETED result={report.Result} " +
            $"ticks={report.DispatcherTicks} " +
            $"profilerAdvances={report.ProfilerAdvancedTicks} " +
            $"maxDispatcherGapMs={report.MaximumDispatcherGapMilliseconds.ToString("0.0", CultureInfo.InvariantCulture)} " +
            $"maxProfilerGapMs={report.MaximumProfilerGapMilliseconds.ToString("0.0", CultureInfo.InvariantCulture)} " +
            $"recoveryObserved={report.RecoveryPromptObserved}",
            awaitWrite: true);
        _ = CompleteInteractionSoakAfterDiagnosticAsync(
            diagnosticWrite,
            soak,
            exitCode);
    }

    private static async Task CompleteInteractionSoakAfterDiagnosticAsync(
        Task<bool> diagnosticWrite,
        InteractionSoakConfiguration soak,
        int exitCode)
    {
        try
        {
            bool persisted = await diagnosticWrite.WaitAsync(
                TimeSpan.FromSeconds(2));
            if (!persisted)
            {
                Console.Error.WriteLine(
                    "Interaction soak completed, but its final diagnostic could not be persisted.");
            }
        }
        catch (TimeoutException)
        {
            Console.Error.WriteLine(
                "Interaction soak diagnostic drain exceeded two seconds.");
        }
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
            InteractionDiagnosticSessionFileName);

    private static Task<bool> QueueInteractionDiagnostic(
        string message,
        bool awaitWrite = false,
        DateTimeOffset? observedUtc = null)
    {
        string safeMessage = SanitizeDiagnosticMessage(message);
        string line =
            (observedUtc ?? DateTimeOffset.UtcNow).ToString(
                "O",
                CultureInfo.InvariantCulture) +
            " pid=" +
            Environment.ProcessId.ToString(CultureInfo.InvariantCulture) +
            " " +
            safeMessage;
        TaskCompletionSource<bool>? completion = awaitWrite
            ? new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously)
            : null;
        bool startPump;
        lock (InteractionLogLock)
        {
            InteractionLogQueue.Enqueue(
                new InteractionDiagnosticWorkItem(line, completion));
            startPump = !_interactionLogPumpRunning;
            if (startPump)
                _interactionLogPumpRunning = true;
        }
        if (startPump)
            _ = Task.Run(DrainInteractionDiagnosticQueue);
        return completion?.Task ?? Task.FromResult(true);
    }

    private static void DrainInteractionDiagnosticQueue()
    {
        while (true)
        {
            InteractionDiagnosticWorkItem item;
            lock (InteractionLogLock)
            {
                if (InteractionLogQueue.Count == 0)
                {
                    _interactionLogPumpRunning = false;
                    return;
                }
                item = InteractionLogQueue.Dequeue();
            }

            bool persisted = TryWriteInteractionDiagnostic(item.Line);
            item.Completion?.TrySetResult(persisted);
        }
    }

    private static bool TryWriteInteractionDiagnostic(string line)
    {
        try
        {
            Trace.WriteLine(line);
            Console.Error.WriteLine(line);
            string path = InteractionDiagnosticLogPath();
            string? parent = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(parent))
                Directory.CreateDirectory(parent);
            long currentLength = File.Exists(path)
                ? new FileInfo(path).Length
                : 0;
            long appendedBytes = Encoding.UTF8.GetByteCount(
                line + Environment.NewLine);
            if (appendedBytes > InteractionDiagnosticLogMaximumBytes ||
                currentLength >
                    InteractionDiagnosticLogMaximumBytes - appendedBytes)
            {
                string previous = path + ".previous";
                if (File.Exists(path))
                    File.Move(path, previous, overwrite: true);
            }
            File.AppendAllText(
                path,
                line + Environment.NewLine,
                new UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false));
            return true;
        }
        catch
        {
            // Diagnostics must never become an editor availability risk.
            return false;
        }
    }

    internal static string SanitizeDiagnosticMessage(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return "unknown";

        var sanitized = new StringBuilder(Math.Min(value.Length, 4096));
        foreach (char character in value)
        {
            if (sanitized.Length == 4096)
                break;
            sanitized.Append(
                IsUnsafeDiagnosticCharacter(character)
                    ? '_'
                    : character);
        }
        return sanitized.ToString();
    }

    internal static string SanitizeDiagnosticField(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return "unknown";

        var sanitized = new StringBuilder(Math.Min(value.Length, 80));
        foreach (char character in value)
        {
            if (sanitized.Length == 80)
                break;
            sanitized.Append(
                IsUnsafeDiagnosticCharacter(character) ||
                character is '=' or ' '
                    ? '_'
                    : character);
        }
        return sanitized.ToString();
    }

    private static bool IsUnsafeDiagnosticCharacter(char character)
    {
        UnicodeCategory category = char.GetUnicodeCategory(character);
        return char.IsControl(character) ||
               category is UnicodeCategory.Format or
                   UnicodeCategory.LineSeparator or
                   UnicodeCategory.ParagraphSeparator or
                   UnicodeCategory.Surrogate;
    }
}
