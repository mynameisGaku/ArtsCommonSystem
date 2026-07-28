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

        var monitorFixture = new[]
        {
            new EditorMonitorWorkArea(
                @"\\.\DISPLAY2", 1920, 0, 3840, 1040, IsPrimary: false),
            new EditorMonitorWorkArea(
                @"\\.\DISPLAY1", 0, 0, 1920, 1040, IsPrimary: true),
            new EditorMonitorWorkArea(
                @"\\.\DISPLAY3", -1280, 0, 0, 984, IsPrimary: false)
        };
        bool parsedSecondary = EditorStartupMonitorPlacement.TryParse(
            new[] { "--unattended", "--SECONDARY-MONITOR" },
            out EditorStartupMonitorSelector? secondarySelector,
            out _);
        bool resolvedSecondary =
            secondarySelector is { } secondary &&
            EditorStartupMonitorPlacement.TryResolve(
                monitorFixture,
                secondary,
                out EditorMonitorWorkArea secondaryTarget,
                out _) &&
            secondaryTarget.DeviceName == @"\\.\DISPLAY2";
        bool parsedIndexed = EditorStartupMonitorPlacement.TryParse(
            new[] { "--monitor", "2" },
            out EditorStartupMonitorSelector? indexedSelector,
            out _);
        bool resolvedIndexed =
            indexedSelector is { } indexed &&
            EditorStartupMonitorPlacement.TryResolve(
                monitorFixture,
                indexed,
                out EditorMonitorWorkArea indexedTarget,
                out _) &&
            indexedTarget.DeviceName == @"\\.\DISPLAY3";
        bool rejectedConflict =
            !EditorStartupMonitorPlacement.TryParse(
                new[] { "--secondary-monitor", "--monitor", "0" },
                out _,
                out string? conflictError) &&
            conflictError is { Length: > 0 };
        bool noMonitorRequest = EditorStartupMonitorPlacement.TryParse(
            new[] { "--no-activate" },
            out EditorStartupMonitorSelector? noSelector,
            out _) &&
            noSelector == null;
        Check(
            parsedSecondary &&
            resolvedSecondary &&
            parsedIndexed &&
            resolvedIndexed &&
            rejectedConflict &&
            noMonitorRequest,
            "startup monitor selection is deterministic, validates conflicts, and leaves ordinary saved layouts untouched");

        string startupSnapshotRoot = Path.Combine(
            Path.GetTempPath(),
            "acs-editor-startup-snapshot-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(startupSnapshotRoot);
        try
        {
            string validSnapshot = Path.Combine(
                startupSnapshotRoot,
                "valid.json");
            await File.WriteAllTextAsync(
                validSnapshot,
                "\uFEFF{\"version\":1}",
                new System.Text.UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false));
            EditorStartupTextSnapshot valid =
                await EditorStartupFileSnapshot.ReadAsync(
                    validSnapshot,
                    maximumBytes: 64);

            string invalidUtf8 = Path.Combine(
                startupSnapshotRoot,
                "invalid.json");
            await File.WriteAllBytesAsync(
                invalidUtf8,
                new byte[] { 0x7B, 0xC3, 0x28, 0x7D });
            EditorStartupTextSnapshot invalid =
                await EditorStartupFileSnapshot.ReadAsync(
                    invalidUtf8,
                    maximumBytes: 64);

            string oversized = Path.Combine(
                startupSnapshotRoot,
                "oversized.json");
            await File.WriteAllBytesAsync(
                oversized,
                Enumerable.Repeat((byte)'x', 65).ToArray());
            EditorStartupTextSnapshot tooLarge =
                await EditorStartupFileSnapshot.ReadAsync(
                    oversized,
                    maximumBytes: 64);

            Check(
                valid.Source == "{\"version\":1}" &&
                !valid.Missing &&
                valid.Warning == null &&
                invalid.Source == null &&
                invalid.Warning is { Length: > 0 } &&
                tooLarge.Source == null &&
                tooLarge.Warning is { Length: > 0 } &&
                EditorStartupFileSnapshot.IsOrdinaryFile(
                    FileAttributes.Normal) &&
                !EditorStartupFileSnapshot.IsOrdinaryFile(
                    FileAttributes.ReparsePoint) &&
                !EditorStartupFileSnapshot.IsOrdinaryFile(
                    FileAttributes.Directory),
                "startup layout snapshot is bounded, strict UTF-8, BOM-safe, and rejects reparse/non-file inputs");
        }
        finally
        {
            try { Directory.Delete(startupSnapshotRoot, recursive: true); }
            catch { }
        }

        string layoutStoreRoot = Path.Combine(
            Path.GetTempPath(),
            "acs-editor-layout-store-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(layoutStoreRoot);
        try
        {
            string layoutTarget = Path.Combine(
                layoutStoreRoot,
                "EditorLayout.v1.json");
            await File.WriteAllTextAsync(layoutTarget, "old");
            int layoutCallerThread = Environment.CurrentManagedThreadId;
            int layoutWriterThread = layoutCallerThread;
            await Task.Run(
                () =>
                {
                    layoutWriterThread = Environment.CurrentManagedThreadId;
                    EditorLayoutFileStore.WriteAtomically(
                        layoutTarget,
                        "{\"version\":1}");
                });
            byte[] committedLayout =
                await File.ReadAllBytesAsync(layoutTarget);

            bool injectedFailureObserved = false;
            try
            {
                await Task.Run(
                    () => EditorLayoutFileStore.WriteAtomically(
                        layoutTarget,
                        "{\"version\":2}",
                        _ => throw new IOException(
                            "injected pre-commit failure")));
            }
            catch (IOException)
            {
                injectedFailureObserved = true;
            }

            string retainedLayout =
                await File.ReadAllTextAsync(layoutTarget);
            Check(
                layoutWriterThread != layoutCallerThread &&
                committedLayout.Length > 0 &&
                !(committedLayout.Length >= 3 &&
                  committedLayout[0] == 0xEF &&
                  committedLayout[1] == 0xBB &&
                  committedLayout[2] == 0xBF) &&
                injectedFailureObserved &&
                retainedLayout == "{\"version\":1}" &&
                Directory.GetFiles(
                    layoutStoreRoot,
                    "*.tmp",
                    SearchOption.TopDirectoryOnly).Length == 0,
                "close layout persistence runs off the Dispatcher, commits UTF-8 atomically, preserves the prior file on failure, and removes temps");
        }
        finally
        {
            try { Directory.Delete(layoutStoreRoot, recursive: true); }
            catch { }
        }

        int uiThreadId = Environment.CurrentManagedThreadId;
        int bootstrapThreadId = uiThreadId;
        int createCalls = 0;
        int suppressCalls = 0;
        int destroyCalls = 0;
        using var bootstrapStarted = new ManualResetEventSlim();
        using var releaseBootstrap = new ManualResetEventSlim();
        using var bootstrapCancellation = new CancellationTokenSource();
        EditorAbiSnapshot compatibleAbi = new(
            QueryAvailable: true,
            Compatible: true,
            ProviderVersion: EditorAbiContract.RequestedVersion,
            Capabilities: EditorAbiContract.RequiredCapabilities,
            MissingRequired: EditorAbiCapability.None,
            UnknownCapabilityBits: 0,
            ProductVersion: "self-test",
            RenderBackend: "self-test",
            Diagnostic: "");
        Task<EditorNativeBootstrapResult> bootstrap =
            EditorNativeBootstrap.RunAsync(
                () => compatibleAbi,
                () =>
                {
                    bootstrapThreadId = Environment.CurrentManagedThreadId;
                    Interlocked.Increment(ref createCalls);
                    bootstrapStarted.Set();
                    releaseBootstrap.Wait();
                    return (IntPtr)0x1234;
                },
                _ => Interlocked.Increment(ref suppressCalls),
                _ => Interlocked.Increment(ref destroyCalls),
                bootstrapCancellation.Token);
        bool workerEntered = bootstrapStarted.Wait(TimeSpan.FromSeconds(2));
        bool dispatcherWasNotBlocked = workerEntered && !bootstrap.IsCompleted;
        bootstrapCancellation.Cancel();
        releaseBootstrap.Set();
        EditorNativeBootstrapResult cancelledBootstrap = await bootstrap;
        Check(
            dispatcherWasNotBlocked &&
            bootstrapThreadId != uiThreadId &&
            createCalls == 1 &&
            suppressCalls == 1 &&
            destroyCalls == 1 &&
            cancelledBootstrap.Cancelled &&
            cancelledBootstrap.Engine == IntPtr.Zero,
            "native editor host bootstrap runs off the Dispatcher and cancellation destroys its unpublished handle");

        using var firstCreateEntered = new ManualResetEventSlim();
        using var releaseFirstCreate = new ManualResetEventSlim();
        using var secondCreateEntered = new ManualResetEventSlim();
        int activeNativeCreates = 0;
        int overlappingNativeCreates = 0;
        Task<EditorNativeBootstrapResult> firstGeneration =
            EditorNativeBootstrap.RunAsync(
                () => compatibleAbi,
                () =>
                {
                    if (Interlocked.Increment(ref activeNativeCreates) > 1)
                        Interlocked.Exchange(ref overlappingNativeCreates, 1);
                    firstCreateEntered.Set();
                    releaseFirstCreate.Wait();
                    Interlocked.Decrement(ref activeNativeCreates);
                    return (IntPtr)0x2001;
                },
                _ => { },
                _ => { },
                CancellationToken.None);
        bool firstGenerationStarted =
            firstCreateEntered.Wait(TimeSpan.FromSeconds(2));
        Task<EditorNativeBootstrapResult> secondGeneration =
            EditorNativeBootstrap.RunAsync(
                () => compatibleAbi,
                () =>
                {
                    if (Interlocked.Increment(ref activeNativeCreates) > 1)
                        Interlocked.Exchange(ref overlappingNativeCreates, 1);
                    secondCreateEntered.Set();
                    Interlocked.Decrement(ref activeNativeCreates);
                    return (IntPtr)0x2002;
                },
                _ => { },
                _ => { },
                CancellationToken.None);
        bool secondGenerationWaited =
            !secondCreateEntered.Wait(TimeSpan.FromMilliseconds(100));
        releaseFirstCreate.Set();
        EditorNativeBootstrapResult firstResult =
            await firstGeneration;
        bool secondStillWaitedForFirstLifetime =
            !secondCreateEntered.Wait(TimeSpan.FromMilliseconds(100));

        using var waitingGenerationCancellation =
            new CancellationTokenSource();
        int cancelledWaitingCreateCalls = 0;
        Task<EditorNativeBootstrapResult> cancelledWaitingGeneration =
            EditorNativeBootstrap.RunAsync(
                () => compatibleAbi,
                () =>
                {
                    Interlocked.Increment(
                        ref cancelledWaitingCreateCalls);
                    return (IntPtr)0x2003;
                },
                _ => { },
                _ => { },
                waitingGenerationCancellation.Token);
        waitingGenerationCancellation.Cancel();
        bool cancelledWaiterCompletedBeforeLifetimeRelease =
            await Task.WhenAny(
                cancelledWaitingGeneration,
                Task.Delay(TimeSpan.FromSeconds(1))) ==
            cancelledWaitingGeneration;
        EditorNativeBootstrapResult cancelledWaitingResult =
            cancelledWaiterCompletedBeforeLifetimeRelease
                ? await cancelledWaitingGeneration
                : default;

        await EditorNativeBootstrap.DestroyUnpublishedAsync(
            firstResult,
            _ => { });
        EditorNativeBootstrapResult secondResult =
            await secondGeneration;
        await EditorNativeBootstrap.DestroyUnpublishedAsync(
            secondResult,
            _ => { });
        if (!cancelledWaiterCompletedBeforeLifetimeRelease)
            cancelledWaitingResult = await cancelledWaitingGeneration;
        Check(
            firstGenerationStarted &&
            secondGenerationWaited &&
            secondStillWaitedForFirstLifetime &&
            cancelledWaiterCompletedBeforeLifetimeRelease &&
            cancelledWaitingResult.Cancelled &&
            cancelledWaitingResult.Engine == IntPtr.Zero &&
            cancelledWaitingCreateCalls == 0 &&
            overlappingNativeCreates == 0 &&
            firstResult.Engine == (IntPtr)0x2001 &&
            secondResult.Engine == (IntPtr)0x2002,
            "overlapping HwndHost generations serialize the complete process-global native host lifetime while a cancelled waiter exits promptly");

        int incompatibleCreateCalls = 0;
        EditorAbiSnapshot incompatibleAbi = compatibleAbi with
        {
            Compatible = false,
            Diagnostic = "missing capability",
        };
        EditorNativeBootstrapResult incompatibleBootstrap =
            await EditorNativeBootstrap.RunAsync(
                () => incompatibleAbi,
                () =>
                {
                    incompatibleCreateCalls++;
                    return (IntPtr)0x5678;
                },
                _ => { },
                _ => { },
                CancellationToken.None);
        Check(
            incompatibleCreateCalls == 0 &&
            incompatibleBootstrap.Engine == IntPtr.Zero &&
            !incompatibleBootstrap.Cancelled &&
            incompatibleBootstrap.FailureDetail is { Length: > 0 },
            "an incompatible native ABI fails closed before host creation");

        // DestroyWindowCore intentionally leaves the failure/allowance contract
        // on the managed HwndHost. A replacement HWND must remain inert until
        // the owner explicitly retries, while a fresh or successfully retried
        // generation may bootstrap normally.
        bool failedGenerationAttachFailed = true;
        bool failedGenerationSuspended = true;
        bool failedGenerationHiddenAllowance = false;
        Check(
            !EngineViewport.ShouldBeginNativeBootstrapForHostGeneration(
                failedGenerationAttachFailed,
                failedGenerationSuspended) &&
            !failedGenerationHiddenAllowance &&
            EngineViewport.CanExplicitlyRetryAttach(
                destroying: false,
                attached: false,
                attachFailed: failedGenerationAttachFailed,
                startupFailureSuspended: failedGenerationSuspended,
                hwndReady: true) &&
            !EngineViewport.CanExplicitlyRetryAttach(
                destroying: false,
                attached: false,
                attachFailed: failedGenerationAttachFailed,
                startupFailureSuspended: failedGenerationSuspended,
                hwndReady: false) &&
            EngineViewport.ShouldBeginNativeBootstrapForHostGeneration(
                attachFailed: false,
                startupFailureSuspended: false),
            "a failed HwndHost generation stays fail-closed after rebuild and only an explicit ready-HWND retry reopens bootstrap");

        Check(
            MainWindow.ShouldPublishStartupLayout(
                requestedVersion: 4,
                currentVersion: 4,
                cancelled: false,
                dispatcherShuttingDown: false) &&
            !MainWindow.ShouldPublishStartupLayout(
                requestedVersion: 4,
                currentVersion: 5,
                cancelled: false,
                dispatcherShuttingDown: false) &&
            !MainWindow.ShouldPublishStartupLayout(
                requestedVersion: 4,
                currentVersion: 4,
                cancelled: true,
                dispatcherShuttingDown: false) &&
            MainWindow.ShouldPublishLayoutSave(
                requestedGeneration: 9,
                currentGeneration: 9) &&
            !MainWindow.ShouldPublishLayoutSave(
                requestedGeneration: 8,
                currentGeneration: 9),
            "late startup layouts and superseded background saves cannot overwrite newer live layout state");
        Check(
            MainWindow.ShouldDeferWorkspaceInitialization(
                startupRestorePending: true,
                storeAlreadyPublished: false) &&
            !MainWindow.ShouldDeferWorkspaceInitialization(
                startupRestorePending: true,
                storeAlreadyPublished: true) &&
            !MainWindow.ShouldDeferWorkspaceInitialization(
                startupRestorePending: false,
                storeAlreadyPublished: false),
            "workspace commands never perform a duplicate persisted-catalogue read while startup loading is pending");

        var canonicalDocumentId =
            new EditorDocumentId("scene", "canonical-reuse-self-test");
        var canonicalKey = new SceneCanonicalCapturePublicationKey(
            canonicalDocumentId,
            MutationRevision: 7,
            CleanBaselineGeneration: 3,
            Use3D: true);
        Check(
            SceneCanonicalCapturePublicationPolicy.ShouldDeferWorkspaceCapture(
                documentHostInitialized: true,
                documentCaptureRequired: true) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldDeferWorkspaceCapture(
                documentHostInitialized: false,
                documentCaptureRequired: true) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldDeferWorkspaceCapture(
                documentHostInitialized: true,
                documentCaptureRequired: false) &&
            SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey,
                workspaceCaptureRequired: true,
                activeDocumentMatches: true,
                simulationActive: false) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey with
                {
                    DocumentId =
                        new EditorDocumentId("scene", "another-scene"),
                },
                workspaceCaptureRequired: true,
                activeDocumentMatches: true,
                simulationActive: false) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey with { MutationRevision = 8 },
                workspaceCaptureRequired: true,
                activeDocumentMatches: true,
                simulationActive: false) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey with { CleanBaselineGeneration = 4 },
                workspaceCaptureRequired: true,
                activeDocumentMatches: true,
                simulationActive: false) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey with { Use3D = false },
                workspaceCaptureRequired: true,
                activeDocumentMatches: true,
                simulationActive: false) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey,
                workspaceCaptureRequired: false,
                activeDocumentMatches: true,
                simulationActive: false) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey,
                workspaceCaptureRequired: true,
                activeDocumentMatches: false,
                simulationActive: false) &&
            !SceneCanonicalCapturePublicationPolicy.ShouldPublish(
                canonicalKey,
                canonicalKey,
                workspaceCaptureRequired: true,
                activeDocumentMatches: true,
                simulationActive: true),
            "Undo capture reuse fails closed across scene, mutation, baseline, view, active-document, and simulation boundaries");

        int canonicalCaptureCalls = 0;
        EditorDocumentState currentCanonicalState = new(
            SceneWorldDocumentEnvelope.Pack("2D-new", "3D-new"),
            SceneWorldDocumentEnvelope.Pack("2D-new", "3D-new"));
        var canonicalDocument = new EditorDocument(
            canonicalDocumentId,
            "canonical",
            sourcePath: null,
            capture: () =>
            {
                canonicalCaptureCalls++;
                return currentCanonicalState;
            },
            restore: _ => { },
            initialState: new EditorDocumentState(
                SceneWorldDocumentEnvelope.Pack("2D-old", "3D-old"),
                SceneWorldDocumentEnvelope.Pack("2D-old", "3D-old")));
        canonicalDocument.NotifyPotentialChange();
        bool canonicalChanged = canonicalDocument.Synchronize("edit");
        EditorDocumentState observedCanonical = canonicalDocument.ObservedState;
        SceneWorldDocumentEnvelope.Unpack(
            observedCanonical.ContentFingerprint,
            out string observed2D,
            out string observed3D);
        Check(
            canonicalChanged &&
            canonicalCaptureCalls == 1 &&
            observed2D == "2D-new" &&
            observed3D == "3D-new" &&
            SceneWorldDocumentEnvelope.SelectSubsystem(
                observedCanonical.ContentFingerprint,
                use3D: false) == "2D-new" &&
            SceneWorldDocumentEnvelope.SelectSubsystem(
                observedCanonical.ContentFingerprint,
                use3D: true) == "3D-new" &&
            ReferenceEquals(
                observedCanonical,
                canonicalDocument.ObservedState),
            "workspace dirty state can read the immutable Undo capture without invoking a second serializer");

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
                  DispatcherPriority.Input &&
              MainWindow.InteractionHealthPriority ==
                  DispatcherPriority.Input,
            "soak policy rejects heartbeat stalls and runs heartbeat plus health sampling at render-checkpoint FIFO Input priority");

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
                reset.LastDispatcherGapMilliseconds == 0 &&
                reset.HeartbeatAgeMilliseconds == 0 &&
                reset.HeartbeatCount == 0 &&
                reset.StallCount == 0 &&
                !reset.StallActive &&
                reset.ActiveStallMilliseconds == 0,
                "independent dispatcher watchdog records one blocked interval and rebases heartbeat, stall state, counts, and peaks at capture reset");
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
        Check(
            !EditorCloseDiagnosticPolicy.MayAwaitPersistence(
                dispatcherThread: true,
                windowClosed: false) &&
            !EditorCloseDiagnosticPolicy.MayAwaitPersistence(
                dispatcherThread: false,
                windowClosed: true) &&
            EditorCloseDiagnosticPolicy.MayAwaitPersistence(
                dispatcherThread: false,
                windowClosed: false) &&
            !MainWindow.InteractionDiagnosticPumpLifetimeForSelfTest()
                .IsFaulted,
            "closed-window diagnostics are best-effort, never make the Dispatcher wait, and retain a non-faulted worker lifetime");

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
