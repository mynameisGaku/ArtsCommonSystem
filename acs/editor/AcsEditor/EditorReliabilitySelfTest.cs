// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;

namespace AcsEditor;

internal static class EditorReliabilitySelfTest
{
    internal static async Task<int> RunAsync(TextWriter output)
    {
        int failures = 0;

        void Check(bool condition, string description)
        {
            output.WriteLine((condition ? "PASS: " : "FAIL: ") + description);
            if (!condition) failures++;
        }

        var boundedLogs = new BoundedLogCollection(capacity: 5);
        int collectionResets = 0;
        boundedLogs.CollectionChanged += (_, args) =>
        {
            if (args.Action == NotifyCollectionChangedAction.Reset)
                collectionResets++;
        };
        static LogEntry TestLog(long sequence) =>
            new()
            {
                Seq = sequence,
                Time = DateTime.UnixEpoch,
                Tag = "Engine",
                Level = LogLevel.Info,
                Message = "line " + sequence,
            };
        int firstTrimmed = boundedLogs.AppendBatch(
            new[] { TestLog(1), TestLog(2), TestLog(3) });
        int secondTrimmed = boundedLogs.AppendBatch(
            new[] { TestLog(4), TestLog(5), TestLog(6), TestLog(7) });
        Check(
            firstTrimmed == 0 &&
            secondTrimmed == 2 &&
            collectionResets == 2 &&
            boundedLogs.Select(entry => entry.Seq)
                .SequenceEqual(new long[] { 3, 4, 5, 6, 7 }),
            "engine console appends one WPF reset per batch and retains the newest bounded history");

        int oversizedTrimmed = boundedLogs.AppendBatch(
            new[]
            {
                TestLog(8), TestLog(9), TestLog(10), TestLog(11),
                TestLog(12), TestLog(13), TestLog(14),
            });
        Check(
            oversizedTrimmed == 7 &&
            collectionResets == 3 &&
            boundedLogs.Select(entry => entry.Seq)
                .SequenceEqual(new long[] { 10, 11, 12, 13, 14 }) &&
            MainWindow.EngineLogPumpMaximumBatchEntries <= 64 &&
            MainWindow.EngineLogPumpMaximumDrainMilliseconds <= 2.0 &&
            MainWindow.ConsoleLogRetentionCapacity == 5000,
            "oversized log bursts discard only the oldest lines and the Dispatcher slice stays tightly bounded");

        var startupBurst = new BoundedLogCollection(
            MainWindow.ConsoleLogRetentionCapacity);
        int startupNotifications = 0;
        startupBurst.CollectionChanged += (_, _) => startupNotifications++;
        LogEntry[] twoHundredLines = Enumerable.Range(1, 200)
            .Select(index => TestLog(index))
            .ToArray();
        int startupTrimmed = 0;
        for (int first = 0;
             first < twoHundredLines.Length;
             first += MainWindow.EngineLogPumpMaximumBatchEntries)
        {
            startupTrimmed += startupBurst.AppendBatch(
                twoHundredLines
                    .Skip(first)
                    .Take(MainWindow.EngineLogPumpMaximumBatchEntries)
                    .ToArray());
        }
        Check(
            startupNotifications == 4 &&
            startupTrimmed == 0 &&
            startupBurst.Count == 200,
            "the former 200-notification startup tick is published in four bounded collection updates");

        EditorInteractionHealthAssessment disabled =
            EditorInteractionHealthPolicy.Evaluate(new(
                WindowVisible: true,
                WindowClosing: false,
                WindowEnabled: false,
                WindowHitTestVisible: true,
                NonInteractiveLaunch: false,
                WindowMoveSizeActive: false,
                ProfilerAdvanced: true,
                ThreadModal: false,
                VisibleOwnedWindowCount: 0,
                NativeOwnedPopupVisible: false,
                RecoveryPromptVisible: false,
                ViewportOwnsCapture: false,
                ActivePointerButtonMask: 0,
                PhysicallyDownPointerButtonMask: 0,
                PointerMismatchAgeMilliseconds: 0));
        Check(disabled.IsFault &&
              disabled.Kind ==
                  EditorInteractionHealthKind.DisabledWithoutVisibleOwner &&
              disabled.Code == "OWNER_DISABLED_RENDERER_ACTIVE",
            "profiler progress plus an owner disabled without a visible dialog is classified as the reported freeze");

        EditorInteractionHealthAssessment modal =
            EditorInteractionHealthPolicy.Evaluate(new(
                WindowVisible: true,
                WindowClosing: false,
                WindowEnabled: false,
                WindowHitTestVisible: true,
                NonInteractiveLaunch: false,
                WindowMoveSizeActive: false,
                ProfilerAdvanced: true,
                ThreadModal: true,
                VisibleOwnedWindowCount: 1,
                NativeOwnedPopupVisible: false,
                RecoveryPromptVisible: false,
                ViewportOwnsCapture: false,
                ActivePointerButtonMask: 0,
                PhysicallyDownPointerButtonMask: 0,
                PointerMismatchAgeMilliseconds: 0));
        Check(!modal.IsFault &&
              modal.Kind == EditorInteractionHealthKind.ExpectedVisibleModal,
            "a visible owned modal is reported but is not mistaken for an unexplained freeze");
        EditorInteractionHealthAssessment modelessDisabledOwner =
            EditorInteractionHealthPolicy.Evaluate(new(
                WindowVisible: true,
                WindowClosing: false,
                WindowEnabled: false,
                WindowHitTestVisible: true,
                NonInteractiveLaunch: false,
                WindowMoveSizeActive: false,
                ProfilerAdvanced: true,
                ThreadModal: false,
                VisibleOwnedWindowCount: 1,
                NativeOwnedPopupVisible: true,
                RecoveryPromptVisible: false,
                ViewportOwnsCapture: false,
                ActivePointerButtonMask: 0,
                PhysicallyDownPointerButtonMask: 0,
                PointerMismatchAgeMilliseconds: 0));
        Check(modelessDisabledOwner.IsFault &&
              modelessDisabledOwner.Kind ==
                  EditorInteractionHealthKind.DisabledWithoutVisibleOwner,
            "a visible modeless owned window cannot hide an unexpectedly disabled editor owner");
        Check(!PackageProjectDialog.DisablesOwnerDuringPrompt,
            "Package Project is an owned modeless workflow and cannot immobilize the editor window");

        IReadOnlyList<string> healthyHeartbeat =
            EditorInteractionSoakPolicy.Evaluate(
                TimeSpan.FromSeconds(10),
                dispatcherTicks: 20,
                profilerAdvancedTicks: 20,
                maximumDispatcherGapMilliseconds: 510,
                maximumProfilerGapMilliseconds: 510);
        IReadOnlyList<string> stalledHeartbeat =
            EditorInteractionSoakPolicy.Evaluate(
                TimeSpan.FromSeconds(10),
                dispatcherTicks: 18,
                profilerAdvancedTicks: 18,
                maximumDispatcherGapMilliseconds: 2500,
                maximumProfilerGapMilliseconds: 2500);
        IReadOnlyList<string> sparseHeartbeat =
            EditorInteractionSoakPolicy.Evaluate(
                TimeSpan.FromSeconds(10),
                dispatcherTicks: 3,
                profilerAdvancedTicks: 3,
                maximumDispatcherGapMilliseconds: 500,
                maximumProfilerGapMilliseconds: 500);
        Check(healthyHeartbeat.Count == 0 &&
              stalledHeartbeat.Contains("DISPATCHER_HEARTBEAT_STALL") &&
              stalledHeartbeat.Contains("PROFILER_HEARTBEAT_STALL") &&
              sparseHeartbeat.Contains("DISPATCHER_TICK_DENSITY_LOW") &&
              sparseHeartbeat.Contains("PROFILER_TICK_DENSITY_LOW") &&
              MainWindow.InteractionHeartbeatPriority ==
                  DispatcherPriority.Input,
            "soak policy rejects heartbeat stalls and measures responsiveness at input priority");

        long watchdogClock = 0;
        using (var watchdog = new EditorDispatcherWatchdog(
                   _ => { },
                   stallThreshold: TimeSpan.FromMilliseconds(1500),
                   pollInterval: TimeSpan.FromMilliseconds(500),
                   timestampProvider: () => watchdogClock,
                   timestampFrequency: 1000,
                   startAutomatically: false))
        {
            watchdog.Beat("startup / scene");
            watchdogClock = 1499;
            watchdog.PollForSelfTest();
            EditorDispatcherWatchdogSnapshot beforeStall =
                watchdog.Snapshot();
            watchdogClock = 2000;
            watchdog.PollForSelfTest();
            watchdogClock = 2500;
            watchdog.PollForSelfTest();
            watchdog.SetPhase("later dispatcher work");
            EditorDispatcherWatchdogSnapshot active =
                watchdog.Snapshot();
            watchdog.Beat("ready");
            EditorDispatcherWatchdogSnapshot recovered =
                watchdog.Snapshot();
            watchdog.ResetPeaks();
            EditorDispatcherWatchdogSnapshot reset =
                watchdog.Snapshot();
            Check(
                !beforeStall.StallActive &&
                active.StallActive &&
                active.StallCount == 1 &&
                active.Phase == "startup / scene" &&
                Math.Abs(active.ActiveStallMilliseconds - 2500) < 0.001 &&
                !recovered.StallActive &&
                recovered.StallCount == 1 &&
                Math.Abs(recovered.LastDispatcherGapMilliseconds - 2500) <
                    0.001 &&
                Math.Abs(recovered.MaximumDispatcherGapMilliseconds - 2500) <
                    0.001 &&
                Math.Abs(recovered.LongestStallMilliseconds - 2500) < 0.001 &&
                recovered.Phase == "ready" &&
                reset.MaximumDispatcherGapMilliseconds == 0 &&
                reset.LongestStallMilliseconds == 0 &&
                reset.StallCount == 1,
                "independent dispatcher watchdog records one blocked interval, recovery, phase, and resettable peaks");
        }
        long delayedPollClock = 0;
        var delayedPollTransitions =
            new List<EditorDispatcherWatchdogTransition>();
        using (var delayedPollWatchdog = new EditorDispatcherWatchdog(
                   transition => delayedPollTransitions.Add(transition),
                   stallThreshold: TimeSpan.FromMilliseconds(1500),
                   pollInterval: TimeSpan.FromMilliseconds(500),
                   timestampProvider: () => delayedPollClock,
                   timestampFrequency: 1000,
                   startAutomatically: false))
        {
            delayedPollWatchdog.Beat("before long pause");
            delayedPollClock = 2500;
            delayedPollWatchdog.Beat("recovered without poll");
            bool transitionsDrained =
                delayedPollWatchdog.WaitForTransitionsForSelfTest(
                    TimeSpan.FromSeconds(2));
            EditorDispatcherWatchdogSnapshot recoveredWithoutPoll =
                delayedPollWatchdog.Snapshot();
            Check(
                transitionsDrained &&
                delayedPollTransitions.Count == 2 &&
                delayedPollTransitions[0].Sequence == 1 &&
                !delayedPollTransitions[0].Recovered &&
                delayedPollTransitions[1].Sequence == 2 &&
                delayedPollTransitions[1].Recovered &&
                delayedPollTransitions[0].ObservedUtc <=
                    delayedPollTransitions[1].ObservedUtc &&
                recoveredWithoutPoll.StallCount == 1 &&
                !recoveredWithoutPoll.StallActive &&
                Math.Abs(
                    recoveredWithoutPoll.LongestStallMilliseconds - 2500) <
                    0.001,
                "recovery heartbeat records ordered stall evidence even when the ThreadPool poll was delayed");
        }
        using (var extremeWatchdog = new EditorDispatcherWatchdog(
                   _ => { },
                   stallThreshold: TimeSpan.MaxValue,
                   pollInterval: TimeSpan.FromMilliseconds(1),
                   timestampProvider: () => 0,
                   timestampFrequency: double.MaxValue,
                   startAutomatically: false))
        {
            Check(
                extremeWatchdog.StallThresholdTicksForSelfTest ==
                    long.MaxValue,
                "watchdog threshold conversion saturates instead of wrapping an extreme duration to a one-tick stall");
        }
        Check(
            MainWindow.SanitizeDiagnosticField(
                "startup scene\r\ncode=FORGED\u202E") ==
                "startup_scene__code_FORGED_" &&
            MainWindow.SanitizeDiagnosticField(
                "phase\u200B\u2060\uFEFF\u2028\uD800") ==
                "phase_____" &&
            MainWindow.SanitizeDiagnosticField(null) == "unknown" &&
            MainWindow.SanitizeDiagnosticMessage(
                "owned title\r\nFORGED\u202E\uD800") ==
                "owned title__FORGED__",
            "interaction diagnostics reject line injection, key separators, format controls, and surrogate fragments");

        string validReport = JsonSerializer.Serialize(new
        {
            SchemaVersion = EditorReliabilityReportContract.CurrentSchemaVersion,
            Result = "PASS",
            RequestedSeconds = 10.0,
            ActualSeconds = 10.05,
            DispatcherIntervalMilliseconds =
                EditorInteractionSoakPolicy.TimerIntervalMilliseconds,
            ExpectedDispatcherTicks = 20,
            RequiredDispatcherTicks = 15,
            DispatcherTicks = 20,
            DispatcherTickDensity = 1.0,
            RequiredProfilerAdvancedTicks = 10,
            ProfilerAdvancedTicks = 20,
            ProfilerAdvanceDensity = 1.0,
            MaximumDispatcherGapMilliseconds = 510.0,
            MaximumProfilerGapMilliseconds = 510.0,
            RecoveryPromptRequired = true,
            RecoveryPromptObserved = true,
            StartupState = EditorEngineStartupState.Ready.ToString(),
            FaultCodes = Array.Empty<string>(),
        });
        bool validReportAccepted =
            EditorReliabilitySoakRunner.TryValidatePassReport(
                validReport,
                processExitCode: 0,
                recoveryRequired: true,
                out _);
        var invalidReport = new Dictionary<string, object?>
        {
            ["SchemaVersion"] = 1,
            ["Result"] = "PASS",
        };
        Check(validReportAccepted &&
              !EditorReliabilitySoakRunner.TryValidatePassReport(
                  JsonSerializer.Serialize(invalidReport),
                  processExitCode: 0,
                  recoveryRequired: true,
                  out _),
            "runner accepts only the current complete PASS report schema");

        string powershell = Path.Combine(
            Environment.SystemDirectory,
            "WindowsPowerShell",
            "v1.0",
            "powershell.exe");
        using (var cancellation = new CancellationTokenSource(
                   TimeSpan.FromMilliseconds(250)))
        {
            var elapsed = Stopwatch.StartNew();
            bool cancelled = false;
            try
            {
                await BuildService.RunProcessForReliabilitySelfTestAsync(
                    powershell,
                    "-NoLogo -NoProfile -NonInteractive -Command \"Start-Sleep -Seconds 30\"",
                    cancellation.Token);
            }
            catch (OperationCanceledException)
            {
                cancelled = true;
            }
            elapsed.Stop();
            Check(cancelled && elapsed.Elapsed < TimeSpan.FromSeconds(5),
                "package build cancellation terminates its process tree promptly instead of leaving the dialog stuck");
        }

        EditorInteractionHealthAssessment stale =
            EditorInteractionHealthPolicy.Evaluate(new(
                WindowVisible: true,
                WindowClosing: false,
                WindowEnabled: true,
                WindowHitTestVisible: true,
                NonInteractiveLaunch: false,
                WindowMoveSizeActive: false,
                ProfilerAdvanced: true,
                ThreadModal: false,
                VisibleOwnedWindowCount: 0,
                NativeOwnedPopupVisible: false,
                RecoveryPromptVisible: false,
                ViewportOwnsCapture: true,
                ActivePointerButtonMask: EngineViewport.PointerButtonMiddleMask,
                PhysicallyDownPointerButtonMask: 0,
                PointerMismatchAgeMilliseconds:
                    EditorInteractionHealthPolicy.StaleCaptureDiagnosticMilliseconds));
        Check(stale.IsFault &&
              stale.Kind == EditorInteractionHealthKind.StaleViewportCapture &&
              !EditorInteractionHealthPolicy.Evaluate(new(
                  WindowVisible: true,
                  WindowClosing: false,
                  WindowEnabled: true,
                  WindowHitTestVisible: true,
                  NonInteractiveLaunch: false,
                  WindowMoveSizeActive: true,
                  ProfilerAdvanced: true,
                  ThreadModal: false,
                  VisibleOwnedWindowCount: 0,
                  NativeOwnedPopupVisible: false,
                  RecoveryPromptVisible: false,
                  ViewportOwnsCapture: true,
                  ActivePointerButtonMask:
                      EngineViewport.PointerButtonMiddleMask,
                  PhysicallyDownPointerButtonMask: 0,
                  PointerMismatchAgeMilliseconds: 5000)).IsFault &&
              !EditorInteractionHealthPolicy.Evaluate(new(
                  WindowVisible: true,
                  WindowClosing: false,
                  WindowEnabled: true,
                  WindowHitTestVisible: true,
                  NonInteractiveLaunch: false,
                  WindowMoveSizeActive: false,
                  ProfilerAdvanced: true,
                  ThreadModal: false,
                  VisibleOwnedWindowCount: 0,
                  NativeOwnedPopupVisible: false,
                  RecoveryPromptVisible: false,
                  ViewportOwnsCapture: true,
                  ActivePointerButtonMask:
                      EngineViewport.PointerButtonMiddleMask,
                  PhysicallyDownPointerButtonMask:
                      EngineViewport.PointerButtonMiddleMask,
                  PointerMismatchAgeMilliseconds: 5000)).IsFault,
            "stale capture is diagnosed only after its initiating button is up and never during move/size or a valid drag");

        var owner = new Window
        {
            Title = "ACS reliability self-test owner",
            Width = 320,
            Height = 180,
            Left = -10000,
            Top = -10000,
            ShowActivated = false,
            WindowStartupLocation = WindowStartupLocation.Manual,
        };
        try
        {
            owner.Show();
            await owner.Dispatcher.InvokeAsync(
                () => { },
                DispatcherPriority.Loaded);

            var identity = new SceneAutosaveIdentity(
                "self-test-project",
                "self-test-scene",
                Path.Combine(Path.GetTempPath(), "self-test.acsproject"),
                null,
                SceneDocumentMode.ThreeD);
            var candidate = new SceneRecoveryCandidate(
                identity,
                Path.Combine(Path.GetTempPath(), "self-test.meta.json"),
                Path.Combine(Path.GetTempPath(), "self-test.snapshot"),
                DateTimeOffset.UtcNow,
                new string('a', 64),
                128);

            Task<SceneRecoveryDecision> decisionTask =
                SceneRecoveryDialog.PromptAsync(owner, candidate);
            await owner.Dispatcher.InvokeAsync(
                () => { },
                DispatcherPriority.ApplicationIdle);
            SceneRecoveryDialog? prompt = owner.OwnedWindows
                .OfType<SceneRecoveryDialog>()
                .SingleOrDefault(window => window.IsVisible);
            Check(prompt != null &&
                  owner.IsEnabled &&
                  prompt.Owner == owner &&
                  !prompt.ShowActivated &&
                  !SceneRecoveryDialog.DisablesOwnerDuringPrompt &&
                  !decisionTask.IsCompleted,
                "the real recovery prompt is modeless, owned, visible, and leaves its editor owner movable");

            prompt?.Close();
            SceneRecoveryDecision decision = await decisionTask;
            Check(decision == SceneRecoveryDecision.Cancel,
                "closing the modeless recovery prompt completes its asynchronous decision without a nested modal loop");

            string packageRoot = Path.Combine(
                Path.GetTempPath(),
                "acs-package-window-selftest-" + Guid.NewGuid().ToString("N"));
            try
            {
                Directory.CreateDirectory(Path.Combine(packageRoot, "Assets"));
                Directory.CreateDirectory(Path.Combine(packageRoot, "Source"));
                string projectPath = Path.Combine(
                    packageRoot,
                    "PackageWindowSelfTest.acsproject");
                File.WriteAllText(projectPath, "{}");
                File.WriteAllText(
                    Path.Combine(packageRoot, "Assets", "main.acscene"),
                    "ACS_SCENE 1\n");
                var project = new Project
                {
                    Version = 1,
                    Name = "PackageWindowSelfTest",
                    EngineVersion = "self-test",
                    InitialScene = "Assets/main.acscene",
                    ProjectFilePath = projectPath,
                };
                var package = new PackageProjectDialog(project, _ => { });
                Task<bool> packageTask = package.ShowModelessAsync(owner);
                await owner.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.ApplicationIdle);
                Check(owner.IsEnabled &&
                      package.IsVisible &&
                      package.Owner == owner &&
                      !package.ShowActivated &&
                      !packageTask.IsCompleted,
                    "the real Package Project workflow is modeless and leaves its editor owner movable");
                package.Close();
                Check(!await packageTask,
                    "closing Package Project without packaging completes its modeless session cleanly");
            }
            finally
            {
                try
                {
                    if (Directory.Exists(packageRoot))
                        Directory.Delete(packageRoot, recursive: true);
                }
                catch
                {
                }
            }
        }
        finally
        {
            owner.Close();
        }

        output.WriteLine(
            failures == 0
                ? "Editor reliability self-test passed."
                : $"Editor reliability self-test failed: {failures} check(s).");
        return failures;
    }
}
