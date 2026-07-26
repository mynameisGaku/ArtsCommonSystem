// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Runtime.InteropServices;

namespace AcsEditor;

internal static class EditorProfilerSelfTest
{
    internal static int Run(TextWriter output)
    {
        int failures = 0;

        void Check(bool condition, string description)
        {
            if (condition)
                output.WriteLine("PASS: " + description);
            else
            {
                output.WriteLine("FAIL: " + description);
                failures++;
            }
        }

        Check(Marshal.SizeOf<EditorProfilerSnapshot>() == 208,
            "managed profiler snapshot keeps the version-3 ABI size");
        Check(EditorProfilerContract.Version == 3,
            "profiler contract requests the version-3 rolling-query ABI");
        Check(EditorProfilerContract.SnapshotSize == 208,
            "profiler contract reports the packed snapshot size");

        var history = new EditorProfilerHistory(3);
        for (ulong frame = 1; frame <= 4; frame++)
        {
            EditorProfilerSnapshot snapshot = Snapshot(frame, 60 + frame, 10 + frame);
            Check(history.Add(snapshot), $"unique frame {frame} is sampled");
        }
        Check(history.Points.Count == 3 &&
              history.Points[0].FrameIndex == 2 &&
              history.Points[^1].FrameIndex == 4,
            "history is fixed-capacity and evicts the oldest sample");

        EditorProfilerSnapshot duplicate = Snapshot(4, 120, 99);
        Check(!history.Add(duplicate) && history.Points.Count == 3,
            "duplicate native frame is not sampled twice");

        history.IsPaused = true;
        Check(!history.Add(Snapshot(5, 120, 5)) &&
              history.Points[^1].FrameIndex == 4,
            "pause freezes history without mutating renderer state");
        history.IsPaused = false;
        Check(history.Add(Snapshot(5, 90, 20)),
            "resume accepts new samples");

        EditorProfilerAverage average = history.Average(2);
        Check(Math.Abs(average.Fps - 77.0f) < 0.01f &&
              Math.Abs(average.CpuFrameMs - 17.0f) < 0.01f &&
              average.GpuFrameMs < 0,
            "moving average uses the requested recent window and preserves unavailable GPU data");

        EditorProfilerSnapshot cpuSnapshot = Snapshot(9, 60, 16.6f);
        Check(EditorProfilerFormatting.TimingSource(cpuSnapshot) == "CPU record / submit",
            "CPU timings are explicitly labelled and never presented as GPU timings");
        cpuSnapshot.TimingSource = EditorProfilerContract.TimingGpuTimestamp;
        cpuSnapshot.Flags |= EditorProfilerContract.FlagGpuTimingsValid;
        cpuSnapshot.GpuFrameMs = 8.25f;
        cpuSnapshot.GpuLatencyFrames = 2;
        Check(EditorProfilerFormatting.TimingSource(cpuSnapshot) ==
              "GPU timestamp queries (+2f async; warming up)",
            "timestamp-query snapshots expose latency while the rolling window warms");
        cpuSnapshot.GpuQueryWindowCount = 120;
        cpuSnapshot.GpuQueryWindowCapacity = 120;
        cpuSnapshot.GpuFrameAverageMs = 5.25f;
        cpuSnapshot.OpaqueGpuAverageMs = 1.0f;
        cpuSnapshot.AtmosphereGpuAverageMs = 0.75f;
        cpuSnapshot.CloudGpuAverageMs = 2.25f;
        cpuSnapshot.FogGpuAverageMs = 0.25f;
        cpuSnapshot.PostGpuAverageMs = 1.0f;
        cpuSnapshot.OpaqueGpuWindowPeakMs = 1.4f;
        cpuSnapshot.AtmosphereGpuWindowPeakMs = 1.1f;
        cpuSnapshot.CloudGpuWindowPeakMs = 2.9f;
        cpuSnapshot.FogGpuWindowPeakMs = 0.4f;
        cpuSnapshot.PostGpuWindowPeakMs = 1.3f;
        Check(EditorProfilerFormatting.TimingSource(cpuSnapshot) ==
              "GPU timestamp queries (+2f async; 120/120q avg)",
            "timestamp-query label reports latency and the unique valid query count");
        var gpuHistory = new EditorProfilerHistory(3);
        Check(gpuHistory.Add(cpuSnapshot) &&
              Math.Abs(gpuHistory.Average().GpuFrameMs - 5.25f) < 0.001f,
            "headline GPU time uses the native unique-query average instead of the latest query");
        Check(EditorProfilerFormatting.GpuAveragePeak(
                  cpuSnapshot.CloudGpuAverageMs,
                  cpuSnapshot.CloudGpuWindowPeakMs,
                  cpuSnapshot.GpuQueryWindowCount) == " 2.25 / 2.90 ms" &&
              EditorProfilerFormatting.GpuAveragePeak(2.0f, 3.0f, 0) == "   N/A",
            "per-pass GPU formatting shows the native-query average and peak only when valid");
        Check(EditorProfilerFormatting.GpuWindowTooltip(120, 120, 2) ==
              "Average / peak across 120/120 unique valid GPU queries. Latest completed query is +2 frames asynchronous.",
            "per-pass tooltip preserves query validity and asynchronous latency context");
        Check(EditorProfilerFormatting.Peak(21.5f, 120) ==
              "peak 21.50 · 120f" &&
              EditorProfilerFormatting.Peak(9.25f, 120, 2) ==
              "peak 9.25 · 120f · +2f" &&
              EditorProfilerFormatting.Peak(-1, 120) == "peak N/A",
            "rolling peaks format their native window, latency and unavailable state");
        Check(Math.Abs(EditorProfilerFormatting.GpuThroughputFps(5.0f) - 200.0f) < 0.001f &&
              EditorProfilerFormatting.GpuThroughputFps(0.0f) < 0 &&
              EditorProfilerFormatting.GpuThroughput(-1.0f) == "throughput N/A",
            "GPU throughput FPS is derived from valid timestamp averages only");
        Check(EditorProfilerFormatting.CompactSummary(
                  new EditorProfilerAverage(84.0f, 0.72f, 0.32f, 4.91f)) ==
              "Editor 84 FPS  |  GPU 204 FPS / 4.91 ms  |  CPU 0.72 ms",
            "compact summary distinguishes editor cadence from GPU throughput");
        Check(
            EditorProfilerPresentationPolicy.ShouldPresentDetails(
                panelVisible: true,
                nativeFrameAdvanced: true) &&
            !EditorProfilerPresentationPolicy.ShouldPresentDetails(
                panelVisible: false,
                nativeFrameAdvanced: true) &&
            !EditorProfilerPresentationPolicy.ShouldPresentDetails(
                panelVisible: true,
                nativeFrameAdvanced: false),
            "collapsed profiler docks keep sampling without invalidating hidden WPF details");

        Check(!App.ShouldForceSoftwareUi(Array.Empty<string>(), false, 2) &&
              App.ShouldForceSoftwareUi(Array.Empty<string>(), true, 2) &&
              !App.ShouldForceSoftwareUi(Array.Empty<string>(), false, 0) &&
              App.ShouldForceSoftwareUi(new[] { "--SOFTWARE-UI" }, false, 2),
            "WPF software composition is limited to remote or explicit launches");
        Check(!App.ShouldAvoidInitialActivation(Array.Empty<string>()) &&
              App.ShouldAvoidInitialActivation(new[] { "--NO-ACTIVATE" }) &&
              App.ShouldAvoidInitialActivation(new[] { "--unattended" }) &&
              !App.ShouldRunUnattended(new[] { "--no-activate" }) &&
              App.ShouldRunUnattended(new[] { "--UNATTENDED" }),
            "initial no-activate remains interactive while unattended alone permanently gates input");
        Check(App.ShouldDeferInteractivePromptsUntilActivation(
                  new[] { "--NO-ACTIVATE" }) &&
              !App.ShouldDeferInteractivePromptsUntilActivation(
                  Array.Empty<string>()) &&
              !App.ShouldDeferInteractivePromptsUntilActivation(
                  new[] { "--unattended", "--no-activate" }) &&
              MainWindow.ShouldDeferInitialRecoveryPrompt(true, false) &&
              !MainWindow.ShouldDeferInitialRecoveryPrompt(true, true) &&
              !MainWindow.ShouldDeferInitialRecoveryPrompt(false, false) &&
              !MainWindow.ShouldDeferInitialRecoveryPrompt(
                  true,
                  false,
                  allowWhileInactive: true),
            "inactive launches defer recovery until activation except for the non-activating reliability fixture");
        Check(!SceneRecoveryDialog.DisablesOwnerDuringPrompt &&
              SceneRecoveryDialog.CanPresentModelessPrompt(true, true, true) &&
              !SceneRecoveryDialog.CanPresentModelessPrompt(false, true, true) &&
              !SceneRecoveryDialog.CanPresentModelessPrompt(true, false, true) &&
              !SceneRecoveryDialog.CanPresentModelessPrompt(true, true, false),
            "scene recovery is an owned modeless prompt and can never disable the editor window");
        Check(App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: false,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: true,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: 0x000F,
                  activationInputMessage: 0x0201) &&
              !App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0200) &&
              App.ShouldReleaseInitialActivationGuard(
                  initialActivationSuppressed: true,
                  nonInteractiveLaunch: false,
                  message: App.WmMouseActivate,
                  activationInputMessage: 0x0246),
            "initial no-activate guard releases only for an explicit mouse activation and never in unattended mode");
        Check(EditorCommandPaletteWindow.ShouldBeginHeaderDrag(100, 20, 720) &&
              !EditorCommandPaletteWindow.ShouldBeginHeaderDrag(700, 20, 720) &&
              !EditorCommandPaletteWindow.ShouldBeginHeaderDrag(100, 40, 720) &&
              !EditorCommandPaletteWindow.ShouldBeginHeaderDrag(double.NaN, 20, 720),
            "the modal command palette exposes a draggable header without stealing its close button");

        Check(EngineViewport.RenderFairnessPriority ==
                  System.Windows.Threading.DispatcherPriority.Background &&
              EngineViewport.DormantWakePriority ==
                  System.Windows.Threading.DispatcherPriority.Background &&
              EngineViewport.ShouldRenderContinuously(
                  false, false, true, true, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  true, false, true, true, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, true, true, true, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, false, true, true, true, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, false, true, false, false, 1280, 720) &&
              !EngineViewport.ShouldRenderContinuously(
                  false, false, true, true, false, 0, 720),
            "native render pump uses Background for mandatory fairness and dormant recovery");
        Check(EngineViewport.MaxDirectRenderBurstFrames == 8 &&
              Math.Abs(EngineViewport.MaxDirectRenderBurstMilliseconds - 8.0) < 0.001 &&
              !EngineViewport.ShouldYieldRenderBurst(7, 7.99) &&
              EngineViewport.ShouldYieldRenderBurst(8, 0.0) &&
              EngineViewport.ShouldYieldRenderBurst(1, 8.0) &&
              EngineViewport.ShouldYieldRenderBurst(80, 0.0),
            "direct render bursts must yield by eight frames or the eight-millisecond UI budget");
        Check(
            EngineViewport.SlowNativeCallThresholdMilliseconds == 50.0 &&
            EngineViewport.MaximumNativeDeltaSeconds == 0.1 &&
            EngineViewport.ShouldYieldForGpuBackpressure(0) &&
            !EngineViewport.ShouldYieldForGpuBackpressure(1) &&
            !EngineViewport.ShouldYieldForGpuBackpressure(-1),
            "GPU frame-slot backpressure yields to WPF without treating success or invalid handles as saturation");
        Check(
            EngineViewport.IsFatalRenderResult(-1) &&
            !EngineViewport.IsFatalRenderResult(0) &&
            !EngineViewport.IsFatalRenderResult(1) &&
            EngineInterop.TryRenderEditorFrame(IntPtr.Zero, 1.0f / 60.0f) ==
                EngineInterop.EditorRenderFatalResult,
            "invalid or unavailable cooperative render contracts fail closed without entering the blocking legacy renderer");
        Check(
            EngineViewport.IsRenderSurfaceVisible(
                nativeWindowVisible: true,
                hiddenStartupRenderingAllowed: false) &&
            EngineViewport.IsRenderSurfaceVisible(
                nativeWindowVisible: false,
                hiddenStartupRenderingAllowed: true) &&
            !EngineViewport.IsRenderSurfaceVisible(
                nativeWindowVisible: false,
                hiddenStartupRenderingAllowed: false),
            "hidden loading UI permits native frames only during explicit renderer warm-up");
        Check(
            Math.Abs(
                EngineViewport.CommitRenderTimestamp(4.0, 7.0, 1) -
                4.1) < 0.000001 &&
            EngineViewport.CommitRenderTimestamp(4.0, 4.05, 1) == 4.05 &&
            EngineViewport.CommitRenderTimestamp(4.0, 7.0, 0) == 4.0 &&
            EngineViewport.CommitRenderTimestamp(4.0, 7.0, -1) == 4.0,
            "successful frames consume at most 100 ms while preserving stalled time for later frames");
        int leftGestureMask = EngineViewport.ActivePointerButtonMask(
            gizmoDragging: true,
            gizmo3dDragging: false,
            marqueeDragging: false,
            panning: false,
            panMode: 0);
        int rightPanMask = EngineViewport.ActivePointerButtonMask(
            gizmoDragging: false,
            gizmo3dDragging: false,
            marqueeDragging: false,
            panning: true,
            panMode: 0);
        int middlePanMask = EngineViewport.ActivePointerButtonMask(
            gizmoDragging: false,
            gizmo3dDragging: false,
            marqueeDragging: false,
            panning: true,
            panMode: 1);
        Check(leftGestureMask == EngineViewport.PointerButtonLeftMask &&
              rightPanMask == EngineViewport.PointerButtonRightMask &&
              middlePanMask == EngineViewport.PointerButtonMiddleMask,
            "viewport capture retains the physical button that initiated each gesture");

        var waterReady = new WaterPointerRoutingState(
            EngineReady: true,
            View3D: true,
            PolygonMode: false,
            GizmoDragging: false,
            Gizmo3DDragging: false,
            Panning: false,
            MarqueeDragging: false);
        WaterPointerRoutingDecision waterPress =
            WaterPointerRoutingPolicy.ForPress(
                waterReady,
                gizmoAccepted: false);
        WaterPointerRoutingDecision waterDrag =
            WaterPointerRoutingPolicy.ForMove(
                waterReady,
                leftButtonDown: true);
        WaterPointerRoutingDecision waterHover =
            WaterPointerRoutingPolicy.ForMove(
                waterReady,
                leftButtonDown: false);
        WaterPointerRoutingDecision waterEnd =
            WaterPointerRoutingPolicy.ForEnd(engineReady: true);
        Check(waterPress.Action == WaterPointerAction.Press &&
              waterDrag.Action == WaterPointerAction.Drag &&
              waterHover.Action == WaterPointerAction.Hover &&
              waterEnd.Action == WaterPointerAction.End,
            "interactive water routes press, left-drag, hover, and end to the native ABI kinds");
        Check(!waterPress.CapturePointer &&
              !waterDrag.CapturePointer &&
              !waterHover.CapturePointer &&
              !waterEnd.CapturePointer,
            "interactive water observes viewport gestures without ever requesting mouse capture");

        WaterPointerRoutingState[] waterBlockedStates =
        {
            waterReady with { EngineReady = false },
            waterReady with { View3D = false },
            waterReady with { PolygonMode = true },
            waterReady with { GizmoDragging = true },
            waterReady with { Gizmo3DDragging = true },
            waterReady with { Panning = true },
            waterReady with { MarqueeDragging = true },
        };
        Check(Array.TrueForAll(
                  waterBlockedStates,
                  state =>
                      !WaterPointerRoutingPolicy.ForPress(
                          state,
                          gizmoAccepted: false).ShouldRoute &&
                      !WaterPointerRoutingPolicy.ForMove(
                          state,
                          leftButtonDown: false).ShouldRoute &&
                      !WaterPointerRoutingPolicy.ForMove(
                          state,
                          leftButtonDown: true).ShouldRoute) &&
              !WaterPointerRoutingPolicy.ForPress(
                  waterReady,
                  gizmoAccepted: true).ShouldRoute,
            "interactive water is gated by 3D mode, polygon tools, both gizmos, pan, marquee, and gizmo hit acceptance");
        Check(!WaterPointerRoutingPolicy.ForEnd(
                  engineReady: false).ShouldRoute &&
              WaterPointerRoutingPolicy.ForEnd(
                  engineReady: true).Action == WaterPointerAction.End,
            "interactive water end/reset is emitted whenever a live native engine exists");
        Check(WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmCancelMode) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmLeftButtonUp) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseWheel) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseHWheel) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmCaptureChanged) &&
              WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseLeave) &&
              !WaterPointerRoutingPolicy.EndsTrackingForWindowMessage(
                  0x0200) &&
              WaterPointerRoutingPolicy.ForWindowMessage(
                  WaterPointerRoutingPolicy.WmMouseLeave,
                  engineReady: true).Action == WaterPointerAction.End,
            "cancel, button-up, capture loss, wheel, and mouse leave terminate water pointer tracking");

        Check(EngineViewport.PointerCaptureRecoveryGraceMilliseconds == 100.0 &&
              EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 100.0) &&
              EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: rightPanMask,
                  physicallyDownButtonMask: EngineViewport.PointerButtonLeftMask,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x0202,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: true,
                  finalizingButtonUp: false,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: true,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: leftGestureMask,
                  physicallyDownButtonMask: EngineViewport.PointerButtonLeftMask,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 99.9) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: false,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: 1000.0) &&
              !EngineViewport.ShouldRecoverStalePointerCapture(
                  viewportOwnsCapture: true,
                  windowInteractionPaused: false,
                  finalizingButtonUp: false,
                  activeButtonMask: middlePanMask,
                  physicallyDownButtonMask: 0,
                  currentMessage: 0x8000 + 0x5A1,
                  mismatchAgeMilliseconds: double.NaN),
            "stale capture releases by initiating-button mismatch, never during valid button-up or top-level move/size");
        Check(!EngineViewport.IsAnyRenderPumpSuspensionActive(false, false) &&
              EngineViewport.IsAnyRenderPumpSuspensionActive(true, false) &&
              EngineViewport.IsAnyRenderPumpSuspensionActive(false, true) &&
              EngineViewport.IsAnyRenderPumpSuspensionActive(true, true),
            "startup failure and top-level move/size independently suspend the render pump");
        Check(EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, true, false, false) &&
              EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, true, true, true) &&
              !EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  true, true, false, false) &&
              !EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, false, false, false) &&
              !EngineViewport.ShouldRecoverRenderPumpFromComposition(
                  false, true, true, false),
            "composition is a queue-only bootstrap/watchdog and never duplicates or executes a live frame");
        Check(!EngineViewport.IsQueuedPumpStale(false, 0, 500) &&
              !EngineViewport.IsQueuedPumpStale(true, 1000, 1099) &&
              EngineViewport.IsQueuedPumpStale(true, 1000, 1100) &&
              EngineViewport.IsQueuedPumpStale(true, 1000, 1500),
            "render-pump watchdog recovers an unacknowledged private message without duplicating live messages");
        Check(EngineViewport.ShouldPublishGizmoTransformChange(
                  captureEnded: true,
                  wasDragging: true) &&
              !EngineViewport.ShouldPublishGizmoTransformChange(
                  captureEnded: false,
                  wasDragging: true) &&
              !EngineViewport.ShouldPublishGizmoTransformChange(
                  captureEnded: true,
                  wasDragging: false),
            "3D gizmo transform publication occurs once at drag capture completion, never per pointer move");
        string nodeOneTransform = MainWindow.BuildSceneMergeKey(
            use3D: false,
            nodeId: 1,
            propertyIdentity: "inspector.transform");
        Check(nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: false,
                  nodeId: 2,
                  propertyIdentity: "inspector.transform") &&
              nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: true,
                  nodeId: 1,
                  propertyIdentity: "inspector.transform") &&
              nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: false,
                  nodeId: 1,
                  propertyIdentity: "inspector.appearance") &&
              nodeOneTransform != MainWindow.BuildSceneMergeKey(
                  use3D: false,
                  nodeId: 1,
                  propertyIdentity: "inspector.transform",
                  selectionEpoch: 1),
            "interactive merge keys isolate scene kind, node, property, and selection gesture identity");
        Check(EngineViewport.ShouldAttemptAttach(false, false) &&
              !EngineViewport.ShouldAttemptAttach(false, true) &&
              !EngineViewport.ShouldAttemptAttach(true, false),
            "failed attachment stays latched until an explicit retry clears the failure state");
        Check(!MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.WaitingForAttach, true) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.WarmingRenderer, true) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.FinalizingEditor, true) &&
              MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.Ready, true) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.Ready, false) &&
              !MainWindow.IsEngineCommandReady(
                  EditorEngineStartupState.Failed, true),
            "engine commands are gated until editor finalization succeeds and a live handle exists");
        Check(MainWindow.WindowMoveSizeTransition(0x0231) == 1 &&
              MainWindow.WindowMoveSizeTransition(0x0232) == -1 &&
              MainWindow.WindowMoveSizeTransition(0x001F) == 0 &&
              MainWindow.WindowMoveSizeTransition(0x0200) == 0,
            "top-level move/size entry and exit pause and resume the viewport pump deterministically");

        Check(EditorInputGate.ShouldSuppressShortcuts(true, true, true) &&
              EditorInputGate.ShouldSuppressShortcuts(false, false, true) &&
              EditorInputGate.ShouldSuppressShortcuts(false, true, false) &&
              !EditorInputGate.ShouldSuppressShortcuts(false, true, true),
            "shortcut guard requires an interactive active focused editor");
        Check(EditorInputGate.ShouldSuppressNativeMessage(true, 0x0100) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0101) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0102) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0104) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0109) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0119) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x00A1) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0200) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0233) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x0240) &&
              EditorInputGate.ShouldSuppressNativeMessage(true, 0x024F) &&
              !EditorInputGate.ShouldSuppressNativeMessage(true, 0x010A) &&
              !EditorInputGate.ShouldSuppressNativeMessage(true, 0x000F) &&
              !EditorInputGate.ShouldSuppressNativeMessage(true, 0x0006) &&
              !EditorInputGate.ShouldSuppressNativeMessage(false, 0x0200),
            "unattended native gate suppresses keyboard, mouse, touch, pointer, gesture and drop input but not painting or activation cleanup");

        history.Reset();
        Check(history.Points.Count == 0, "reset clears profiler history");

        output.WriteLine(
            failures == 0
                ? "Profiler self-test passed."
                : $"Profiler self-test failed: {failures} check(s).");
        return failures;
    }

    private static EditorProfilerSnapshot Snapshot(
        ulong frame,
        float fps,
        float cpuFrameMs) =>
        new()
        {
            Version = EditorProfilerContract.Version,
            StructSize = EditorProfilerContract.SnapshotSize,
            TimingSource = EditorProfilerContract.TimingCpuRecordSubmit,
            FrameIndex = frame,
            Fps = fps,
            CpuFrameMs = cpuFrameMs,
            CpuSubmitMs = cpuFrameMs * 0.2f,
            GpuFrameMs = -1,
            OpaqueGpuMs = -1,
            AtmosphereGpuMs = -1,
            CloudGpuMs = -1,
            FogGpuMs = -1,
            PostGpuMs = -1,
            GpuFramePeakMs = -1,
            PeakWindowFrames = 120,
            GpuQueryWindowCapacity = 120,
            GpuFrameAverageMs = -1,
            OpaqueGpuAverageMs = -1,
            AtmosphereGpuAverageMs = -1,
            CloudGpuAverageMs = -1,
            FogGpuAverageMs = -1,
            PostGpuAverageMs = -1,
            OpaqueGpuWindowPeakMs = -1,
            AtmosphereGpuWindowPeakMs = -1,
            CloudGpuWindowPeakMs = -1,
            FogGpuWindowPeakMs = -1,
            PostGpuWindowPeakMs = -1,
        };
}
