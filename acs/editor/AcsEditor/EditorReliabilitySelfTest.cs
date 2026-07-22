// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
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
              sparseHeartbeat.Contains("PROFILER_TICK_DENSITY_LOW"),
            "soak policy rejects long dispatcher/profiler gaps and low heartbeat density");

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
