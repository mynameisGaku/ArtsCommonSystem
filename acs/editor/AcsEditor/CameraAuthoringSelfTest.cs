// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal static class CameraAuthoringSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS: " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL: " + label);
            }
        }

        static CameraAuthoringState Camera(
            int node,
            string stable,
            bool active = false,
            bool enabled = true,
            int priority = 0) =>
            new(
                node,
                stable,
                active,
                enabled,
                CameraProjectionMode.Perspective,
                60.0f,
                10.0f,
                0.1f,
                1000.0f,
                priority);

        static ResolvedAuthoredCameraPose Resolved(
            CameraAuthoringState camera,
            float positionX = 100.0f,
            float positionY = 50.0f,
            float positionZ = -25.0f,
            float forwardX = 0.0f,
            float forwardY = 0.0f,
            float forwardZ = 1.0f,
            float upX = 0.0f,
            float upY = 1.0f,
            float upZ = 0.0f) =>
            new(
                camera.NodeId,
                camera.Projection,
                positionX,
                positionY,
                positionZ,
                forwardX,
                forwardY,
                forwardZ,
                upX,
                upY,
                upZ,
                camera.FieldOfViewDegrees,
                camera.OrthographicSize,
                camera.NearPlane,
                camera.FarPlane);

        static bool Nearly(float left, float right) =>
            MathF.Abs(left - right) <=
            MathF.Max(1.0f, MathF.Max(MathF.Abs(left), MathF.Abs(right))) *
            1.0e-4f;

        try
        {
            CameraAuthoringState[] ordered = CameraAuthoringContract.Order(
                new[]
                {
                    Camera(40, "cam-4", priority: 100),
                    Camera(30, "cam-3", active: true, priority: -5),
                    Camera(20, "cam-2", active: true, priority: 7),
                    Camera(11, "cam-1", active: true, priority: 7),
                    Camera(10, "cam-1", active: true, priority: 7),
                }).ToArray();
            Check(
                ordered.Select(static camera => camera.NodeId)
                    .SequenceEqual(new[] { 10, 11, 20, 30, 40 }),
                "camera order is active, priority, stable ID, then node ID");

            CameraAuthoringState? designated =
                CameraAuthoringContract.ChooseDesignated(
                    new[]
                    {
                        Camera(1, "disabled", active: true, enabled: false, priority: 999),
                        Camera(4, "camera-d", priority: 4),
                        Camera(3, "camera-c", priority: 5),
                    });
            Check(
                designated is { NodeId: 3 },
                "disabled cameras never become the designated camera");
            Check(
                CameraAuthoringContract.ChooseDesignated(
                    new[] { Camera(1, "disabled", enabled: false) }) == null,
                "a scene with no enabled camera has no designation");

            CameraAuthoringState[] exclusive =
                CameraAuthoringContract.ActivateExclusively(
                    new[]
                    {
                        Camera(7, "cam-100", active: true),
                        Camera(8, "cam-200", active: true),
                        Camera(9, "cam-300"),
                    },
                    stableCameraId: "cam-300",
                    nodeId: 9);
            Check(
                exclusive.Count(static camera => camera.IsActive) == 1 &&
                exclusive.Single(static camera => camera.IsActive).NodeId == 9,
                "Set Active clears every other explicit active designation");

            bool missingRejected = false;
            try
            {
                _ = CameraAuthoringContract.ActivateExclusively(
                    new[] { Camera(1, "cam-1") },
                    stableCameraId: "cam-2",
                    nodeId: 2);
            }
            catch (KeyNotFoundException)
            {
                missingRejected = true;
            }
            Check(
                missingRejected,
                "exclusive activation rejects a stale camera identity");

            CameraAuthoringState hostile = Camera(1, "cam-1") with
            {
                FieldOfViewDegrees = -50.0f,
                OrthographicSize = 0.0f,
                NearPlane = -10.0f,
                FarPlane = 0.0f,
                Priority = int.MaxValue,
            };
            Check(
                CameraAuthoringContract.TryNormalize(
                    hostile,
                    out CameraAuthoringState clamped,
                    out _) &&
                clamped.FieldOfViewDegrees ==
                    CameraAuthoringContract.MinimumFieldOfViewDegrees &&
                clamped.OrthographicSize ==
                    CameraAuthoringContract.MinimumOrthographicSize &&
                clamped.NearPlane ==
                    CameraAuthoringContract.MinimumNearPlane &&
                clamped.FarPlane > clamped.NearPlane &&
                clamped.Priority == CameraAuthoringContract.MaximumPriority,
                "projection values and priority normalize into supported bounds");

            Check(
                !CameraAuthoringContract.TryNormalize(
                    Camera(1, "cam-1") with { FieldOfViewDegrees = float.NaN },
                    out _,
                    out string nonFiniteDetail) &&
                nonFiniteDetail.Contains("finite", StringComparison.Ordinal),
                "non-finite projection values fail closed");
            Check(
                !CameraAuthoringContract.TryNormalize(
                    Camera(-1, "cam-1"),
                    out _,
                    out _) &&
                !CameraAuthoringContract.TryNormalize(
                    Camera(1, ""),
                    out _,
                    out _),
                "invalid node and stable identities fail closed");
            Check(
                !CameraAuthoringContract.TryNormalize(
                    Camera(1, "cam-1") with
                    {
                        Projection = (CameraProjectionMode)99,
                    },
                    out _,
                    out _),
                "unknown projection modes fail closed");

            CameraAuthoringState orthographic = Camera(2, "cam-2") with
            {
                Projection = CameraProjectionMode.Orthographic,
                OrthographicSize = 42.0f,
            };
            Check(
                CameraAuthoringContract.TryNormalize(
                    orthographic,
                    out CameraAuthoringState preserved,
                    out _) &&
                preserved.Projection == CameraProjectionMode.Orthographic &&
                preserved.OrthographicSize == 42.0f,
                "valid orthographic authored values are preserved");
            Check(
                CameraAuthoringContract.IsValidStableCameraId("Camera_01.preview") &&
                !CameraAuthoringContract.IsValidStableCameraId("-camera") &&
                !CameraAuthoringContract.IsValidStableCameraId("camera/one") &&
                !CameraAuthoringContract.IsValidStableCameraId(
                    "c" + new string('x', 64)),
                "stable camera IDs enforce the native ASCII grammar and cap");
            Check(
                CameraAuthoringContract.MaximumCameraCount == 256 &&
                CameraAuthoringContract.MaximumStableCameraIdLength == 64,
                "managed authoring bounds match native camera and scratch-buffer caps");

            var owner = new CameraViewPixelBounds(100, 100, 800, 600);
            var moving = new CameraViewPixelBounds(107, 109, 200, 150);
            CameraViewPixelBounds topLeft =
                CameraViewSnapPolicy.Snap(moving, owner, 12);
            CameraViewPixelBounds topRight =
                CameraViewSnapPolicy.Snap(
                    moving with { Left = 695 },
                    owner,
                    12);
            CameraViewPixelBounds bottomLeft =
                CameraViewSnapPolicy.Snap(
                    moving with { Top = 545 },
                    owner,
                    12);
            CameraViewPixelBounds bottomRight =
                CameraViewSnapPolicy.Snap(
                    moving with { Left = 695, Top = 545 },
                    owner,
                    12);
            Check(
                topLeft.Left == 100 && topLeft.Top == 100 &&
                topRight.Left == 700 && topRight.Top == 100 &&
                bottomLeft.Left == 100 && bottomLeft.Top == 550 &&
                bottomRight.Left == 700 && bottomRight.Top == 550,
                "Camera View snaps to all four owner corners");

            CameraViewPixelBounds left =
                CameraViewSnapPolicy.Snap(
                    moving with { Left = -95, Top = 300 },
                    owner,
                    12);
            CameraViewPixelBounds right =
                CameraViewSnapPolicy.Snap(
                    moving with { Left = 895, Top = 300 },
                    owner,
                    12);
            CameraViewPixelBounds top =
                CameraViewSnapPolicy.Snap(
                    moving with { Left = 400, Top = -45 },
                    owner,
                    12);
            CameraViewPixelBounds bottom =
                CameraViewSnapPolicy.Snap(
                    moving with { Left = 400, Top = 695 },
                    owner,
                    12);
            Check(
                left.Left == -100 &&
                right.Left == 900 &&
                top.Top == -50 &&
                bottom.Top == 700,
                "Camera View snaps outside each owner edge");

            CameraViewPixelBounds outsideThreshold =
                CameraViewSnapPolicy.Snap(
                    moving with { Left = 113, Top = 113 },
                    owner,
                    12);
            Check(
                outsideThreshold.Left == 113 &&
                outsideThreshold.Top == 113 &&
                CameraViewSnapPolicy.ThresholdPixels(96) == 12 &&
                CameraViewSnapPolicy.ThresholdPixels(192) == 24,
                "Camera View snap threshold is strict and DPI-aware");

            var workArea = new CameraViewPixelBounds(0, 0, 1920, 1080);
            CameraViewPixelBounds clampedLow =
                CameraViewSnapPolicy.ClampReachable(
                    new CameraViewPixelBounds(-5000, -5000, 600, 400),
                    workArea,
                    96,
                    48);
            CameraViewPixelBounds clampedHigh =
                CameraViewSnapPolicy.ClampReachable(
                    new CameraViewPixelBounds(5000, 5000, 600, 400),
                    workArea,
                    96,
                    48);
            Check(
                clampedLow.Left == -504 && clampedLow.Top == 0 &&
                clampedHigh.Left == 1824 && clampedHigh.Top == 1032,
                "Camera View keeps a reachable title and edge inside monitor work area");

            CameraViewPixelBounds topologyGap =
                CameraViewPlacementPolicy.ClampRestoredToWorkArea(
                    new CameraViewPixelBounds(2000, 900, 600, 400),
                    new CameraViewPixelBounds(3000, 0, 1920, 1080),
                    96);
            Check(
                topologyGap.Left == 2496 &&
                topologyGap.Top == 900 &&
                topologyGap.Right >= 3000 &&
                topologyGap.Bottom >= 0,
                "Camera View restored in a monitor-topology gap is clamped to the nearest work area");

            CameraViewPlacementState normalizedPlacement =
                CameraViewPlacementStore.Normalize(
                    new CameraViewPlacementState
                    {
                        Left = double.NaN,
                        Top = double.PositiveInfinity,
                        Width = double.NaN,
                        Height = double.NegativeInfinity,
                        StableCameraId = "bad/id",
                    },
                    new System.Windows.Rect(0, 0, 1920, 1080),
                    new System.Windows.Rect(120, 80, 720, 480));
            Check(
                normalizedPlacement.Left == 120 &&
                normalizedPlacement.Top == 80 &&
                normalizedPlacement.Width == 720 &&
                normalizedPlacement.Height == 480 &&
                normalizedPlacement.StableCameraId.Length == 0,
                "Camera View placement rejects non-finite geometry and invalid stable IDs");

            CameraViewPreviewPlan previewPlan =
                CameraViewPreviewPolicy.Plan(
                    cameraAuthoringCapability: true,
                    cameraEnabled: true);
            CameraViewPreviewPlan unavailablePreview =
                CameraViewPreviewPolicy.Plan(
                    cameraAuthoringCapability: false,
                    cameraEnabled: true);
            CameraViewPreviewPlan disabledPreview =
                CameraViewPreviewPolicy.Plan(
                    cameraAuthoringCapability: true,
                    cameraEnabled: false);
            Check(
                previewPlan.CanApply &&
                previewPlan.UsesTransientNativeOverride &&
                !previewPlan.MutatesAuthoredCamera &&
                !previewPlan.RecordsSceneHistory &&
                !unavailablePreview.CanApply &&
                !disabledPreview.CanApply,
                "Camera View uses only the transient preview override and never dirties authored Active state");

            CameraViewBackendPlan requestBackend =
                CameraViewBackendPolicy.Resolve(
                    cameraAuthoringCapability: true,
                    cameraViewRequestsCapability: true);
            CameraViewBackendPlan legacyBackend =
                CameraViewBackendPolicy.Resolve(
                    cameraAuthoringCapability: true,
                    cameraViewRequestsCapability: false);
            CameraViewBackendPlan unavailableBackend =
                CameraViewBackendPolicy.Resolve(
                    cameraAuthoringCapability: false,
                    cameraViewRequestsCapability: true);
            Check(
                requestBackend is
                {
                    CanOpen: true,
                    UsesRequestContract: true,
                    UsesLegacyGlobalOverride: false,
                    MaximumLivePresenters: 1,
                    HasDedicatedOffscreenTargets: false,
                } &&
                legacyBackend is
                {
                    CanOpen: true,
                    UsesRequestContract: false,
                    UsesLegacyGlobalOverride: true,
                    MaximumLivePresenters: 1,
                    HasDedicatedOffscreenTargets: false,
                } &&
                unavailableBackend == default,
                "Camera View request negotiation never claims a second swapchain or synchronous-readback live target");
            Check(
                !CameraViewPresenterPublicationPolicy.CanBindPresenter(
                    liveSurfaceAttached: false) &&
                CameraViewPresenterPublicationPolicy.CanBindPresenter(
                    liveSurfaceAttached: true),
                "Camera View cannot redirect the main Game View before its window owns the one live surface");

            var logicalSlots =
                new List<CameraViewSlotIdentity>(
                    CameraViewSlotPolicy.MaximumLogicalSlots);
            bool acceptedEightLogicalSlots = true;
            for (int index = 0;
                 index < CameraViewSlotPolicy.MaximumLogicalSlots;
                 ++index)
            {
                string stableId = $"slot-camera-{index}";
                CameraViewSlotAddPlan plan =
                    CameraViewSlotPolicy.PlanAdd(
                        logicalSlots,
                        cameraNodeId: 100 + index,
                        stableId,
                        usesRequestContract: true);
                acceptedEightLogicalSlots &=
                    plan.Action == CameraViewSlotAddAction.Add &&
                    plan.Capacity ==
                        CameraViewSlotPolicy.MaximumLogicalSlots;
                logicalSlots.Add(new CameraViewSlotIdentity(
                    checked((ulong)index + 1),
                    stableId,
                    IsCameraAvailable: true));
            }
            CameraViewSlotAddPlan duplicateSlot =
                CameraViewSlotPolicy.PlanAdd(
                    logicalSlots,
                    cameraNodeId: 900,
                    stableCameraId: "slot-camera-3",
                    usesRequestContract: true);
            CameraViewSlotAddPlan ninthSlot =
                CameraViewSlotPolicy.PlanAdd(
                    logicalSlots,
                    cameraNodeId: 901,
                    stableCameraId: "slot-camera-8",
                    usesRequestContract: true);
            CameraViewSlotAddPlan firstLegacySlot =
                CameraViewSlotPolicy.PlanAdd(
                    Array.Empty<CameraViewSlotIdentity>(),
                    cameraNodeId: 1,
                    stableCameraId: "legacy-camera",
                    usesRequestContract: false);
            CameraViewSlotAddPlan secondLegacySlot =
                CameraViewSlotPolicy.PlanAdd(
                    new[]
                    {
                        new CameraViewSlotIdentity(
                            1,
                            "legacy-camera",
                            IsCameraAvailable: true),
                    },
                    cameraNodeId: 2,
                    stableCameraId: "legacy-camera-2",
                    usesRequestContract: false);
            Check(
                acceptedEightLogicalSlots &&
                CameraViewSlotPolicy.MaximumLogicalSlots == 8 &&
                CameraViewSlotPolicy.MaximumLivePresenters == 1 &&
                duplicateSlot.Action ==
                    CameraViewSlotAddAction.SelectExisting &&
                duplicateSlot.ExistingSlotId == 4 &&
                ninthSlot.Action == CameraViewSlotAddAction.Reject &&
                firstLegacySlot.Action == CameraViewSlotAddAction.Add &&
                firstLegacySlot.Capacity == 1 &&
                secondLegacySlot.Action == CameraViewSlotAddAction.Reject,
                "Camera View exposes eight logical V1 slots while legacy and physical presentation remain single-owner");

            CameraViewPresenterTransferPlan switchPresenter =
                CameraViewSlotPolicy.PlanTransfer(
                    currentPresenterSlotId: 10,
                    targetSlotId: 20);
            CameraViewPresenterTransferPlan keepPresenter =
                CameraViewSlotPolicy.PlanTransfer(
                    currentPresenterSlotId: 20,
                    targetSlotId: 20);
            CameraViewPresenterTransferPlan invalidPresenter =
                CameraViewSlotPolicy.PlanTransfer(
                    currentPresenterSlotId: 20,
                    targetSlotId: 0);
            Check(
                switchPresenter is
                {
                    CanApply: true,
                    UnbindCurrentPresenter: true,
                    BindTargetPresenter: true,
                    MutatesAuthoredCamera: false,
                    RecordsSceneHistory: false,
                } &&
                keepPresenter.CanApply &&
                !keepPresenter.UnbindCurrentPresenter &&
                !keepPresenter.BindTargetPresenter &&
                invalidPresenter == default,
                "Camera View tab switching is an explicit one-presenter transfer with no authored or Undo mutation");

            ulong[] orderedSlots = { 10, 20, 30 };
            CameraViewSlotClosePlan closeMiddle =
                CameraViewSlotPolicy.PlanClose(
                    orderedSlots,
                    selectedSlotId: 20,
                    closingSlotId: 20);
            CameraViewSlotClosePlan closeLast =
                CameraViewSlotPolicy.PlanClose(
                    orderedSlots,
                    selectedSlotId: 30,
                    closingSlotId: 30);
            CameraViewSlotClosePlan closeBackground =
                CameraViewSlotPolicy.PlanClose(
                    orderedSlots,
                    selectedSlotId: 20,
                    closingSlotId: 10);
            CameraViewSlotClosePlan closeOnly =
                CameraViewSlotPolicy.PlanClose(
                    new ulong[] { 77 },
                    selectedSlotId: 77,
                    closingSlotId: 77);
            Check(
                closeMiddle.CanClose &&
                closeMiddle.ClosesSelectedSlot &&
                closeMiddle.NextSelectedSlotId == 30 &&
                closeLast.NextSelectedSlotId == 20 &&
                closeBackground.NextSelectedSlotId == 20 &&
                !closeBackground.ClosesSelectedSlot &&
                closeOnly.CloseWindow &&
                closeOnly.NextSelectedSlotId == 0,
                "Camera View slot close chooses a deterministic adjacent tab and closes the window only for the final slot");

            CameraViewExtentUpdatePlan extentUpdate =
                CameraViewSlotPolicy.PlanExtentUpdate(
                    selectedSlotId: 20,
                    width: 1920,
                    height: 1080);
            Check(
                extentUpdate is
                {
                    CanApply: true,
                    TargetSlotId: 20,
                    MutatesOnlyTargetRequest: true,
                    ResetsOnlyTargetHistory: true,
                } &&
                CameraViewSlotPolicy.PlanExtentUpdate(
                    selectedSlotId: 0,
                    width: 1920,
                    height: 1080) == default &&
                CameraViewSlotPolicy.PlanExtentUpdate(
                    selectedSlotId: 20,
                    width: 0,
                    height: 1080) == default,
                "Camera View resize targets only the selected request and its own history generation");

            CameraViewChoice[] replacementCameras =
            {
                new(
                    501,
                    "Replacement Camera",
                    "stable-follow",
                    true,
                    false,
                    false,
                    10),
                new(
                    601,
                    "Duplicate A",
                    "duplicate-follow",
                    true,
                    false,
                    false,
                    0),
                new(
                    602,
                    "Duplicate B",
                    "duplicate-follow",
                    true,
                    false,
                    false,
                    0),
            };
            CameraViewChoice? followedReplacement =
                CameraViewSlotPolicy.ResolveStableCamera(
                    replacementCameras,
                    "stable-follow",
                    previousNodeId: 41);
            CameraViewChoice? exactDuplicate =
                CameraViewSlotPolicy.ResolveStableCamera(
                    replacementCameras,
                    "duplicate-follow",
                    previousNodeId: 602);
            CameraViewChoice? ambiguousReplacement =
                CameraViewSlotPolicy.ResolveStableCamera(
                    replacementCameras,
                    "duplicate-follow",
                    previousNodeId: 42);
            ulong refreshedSelection =
                CameraViewSlotPolicy.SelectAfterSceneRefresh(
                    new[]
                    {
                        new CameraViewSlotIdentity(
                            10,
                            "missing-slot",
                            IsCameraAvailable: false),
                        new CameraViewSlotIdentity(
                            20,
                            "stable-follow",
                            IsCameraAvailable: true),
                        new CameraViewSlotIdentity(
                            30,
                            "other-slot",
                            IsCameraAvailable: true),
                    },
                    selectedSlotId: 10);
            Check(
                followedReplacement is { NodeId: 501 } &&
                exactDuplicate is { NodeId: 602 } &&
                ambiguousReplacement == null &&
                refreshedSelection == 20 &&
                CameraViewSlotPolicy.SelectAfterSceneRefresh(
                    logicalSlots,
                    selectedSlotId: 4) == 4,
                "Camera View follows unique stable IDs after scene replacement and fails closed on ambiguous duplicates");

            Check(
                CameraViewRequestContract.SnapshotSize == 60 &&
                CameraViewRequestContract.IsValidExtent(1, 1) &&
                CameraViewRequestContract.IsValidExtent(8192, 4096) &&
                !CameraViewRequestContract.IsValidExtent(0, 1) &&
                !CameraViewRequestContract.IsValidExtent(8192, 8192),
                "Camera View request snapshot layout and allocation bounds match native V1");

            const ulong requestId = 0x0000_0001_0000_0001;
            CameraViewRequestSnapshot validPendingSnapshot = new()
            {
                Version = CameraViewRequestContract.Version,
                StructSize = CameraViewRequestContract.SnapshotSize,
                RequestId = requestId,
                CameraNodeId = 9,
                Width = 1280,
                Height = 720,
                TargetGeneration = 2,
                HistoryGeneration = 3,
                Flags =
                    CameraViewRequestFlags.Active |
                    CameraViewRequestFlags.Presenter |
                    CameraViewRequestFlags.TargetRecreatePending |
                    CameraViewRequestFlags.HistoryResetPending,
                TargetKind = CameraViewTargetKind.SharedSwapchain,
            };
            CameraViewRequestSnapshot validRenderedSnapshot =
                validPendingSnapshot;
            validRenderedSnapshot.LatestFrameSerial = 41;
            validRenderedSnapshot.PresentedWidth = 1280;
            validRenderedSnapshot.PresentedHeight = 720;
            validRenderedSnapshot.Flags = CameraViewRequestFlags.Active |
                                          CameraViewRequestFlags.Presenter;
            Check(
                CameraViewRequestContract.IsValidSnapshot(
                    in validPendingSnapshot,
                    requestId) &&
                CameraViewRequestContract.IsValidSnapshot(
                    in validRenderedSnapshot,
                    requestId),
                "Camera View snapshot validator accepts pending and rendered shared-swapchain presenters");

            CameraViewRequestSnapshot unknownFlags = validPendingSnapshot;
            unknownFlags.Flags |= (CameraViewRequestFlags)(1u << 31);
            CameraViewRequestSnapshot inactive = validPendingSnapshot;
            inactive.Flags &= ~CameraViewRequestFlags.Active;
            CameraViewRequestSnapshot stalePresenter = validPendingSnapshot;
            stalePresenter.Flags |= CameraViewRequestFlags.CameraStale;
            CameraViewRequestSnapshot dedicatedTarget = validPendingSnapshot;
            dedicatedTarget.TargetKind =
                CameraViewTargetKind.DedicatedOffscreen;
            Check(
                !CameraViewRequestContract.IsValidSnapshot(
                    in unknownFlags,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in inactive,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in stalePresenter,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in dedicatedTarget,
                    requestId),
                "Camera View snapshot validator rejects unknown flags, inactive records, stale presenters, and unadvertised targets");

            CameraViewRequestSnapshot invalidRequestedExtent =
                validPendingSnapshot;
            invalidRequestedExtent.Width = 0;
            CameraViewRequestSnapshot partialPresentedExtent =
                validPendingSnapshot;
            partialPresentedExtent.PresentedWidth = 1280;
            CameraViewRequestSnapshot zeroGeneration = validPendingSnapshot;
            zeroGeneration.HistoryGeneration = 0;
            CameraViewRequestSnapshot renderedWithoutSerial =
                validRenderedSnapshot;
            renderedWithoutSerial.LatestFrameSerial = 0;
            CameraViewRequestSnapshot noFrameWithoutHistoryReset =
                validPendingSnapshot;
            noFrameWithoutHistoryReset.Flags =
                CameraViewRequestFlags.Active |
                CameraViewRequestFlags.Presenter |
                CameraViewRequestFlags.TargetRecreatePending;
            Check(
                !CameraViewRequestContract.IsValidSnapshot(
                    in invalidRequestedExtent,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in partialPresentedExtent,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in zeroGeneration,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in renderedWithoutSerial,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in noFrameWithoutHistoryReset,
                    requestId),
                "Camera View snapshot validator rejects invalid extents, zero generations, and unowned frame metadata");

            CameraViewRequestSnapshot validQueuedSnapshot =
                validPendingSnapshot;
            validQueuedSnapshot.Flags =
                CameraViewRequestFlags.Active |
                CameraViewRequestFlags.TargetRecreatePending |
                CameraViewRequestFlags.HistoryResetPending;
            validQueuedSnapshot.TargetKind = CameraViewTargetKind.None;
            CameraViewRequestSnapshot queuedWithPresentedFrame =
                validQueuedSnapshot;
            queuedWithPresentedFrame.LatestFrameSerial = 42;
            queuedWithPresentedFrame.PresentedWidth = 1280;
            queuedWithPresentedFrame.PresentedHeight = 720;
            Check(
                CameraViewRequestContract.IsValidSnapshot(
                    in validQueuedSnapshot,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in queuedWithPresentedFrame,
                    requestId),
                "Camera View queued requests cannot claim another presenter's frame");

            CameraViewRequestSnapshot validMismatchedPresentation =
                validRenderedSnapshot;
            validMismatchedPresentation.PresentedWidth = 640;
            validMismatchedPresentation.PresentedHeight = 360;
            validMismatchedPresentation.Flags =
                CameraViewRequestFlags.Active |
                CameraViewRequestFlags.Presenter |
                CameraViewRequestFlags.TargetRecreatePending |
                CameraViewRequestFlags.HistoryResetPending;
            CameraViewRequestSnapshot mismatchedWithoutPendingFlags =
                validMismatchedPresentation;
            mismatchedWithoutPendingFlags.Flags =
                CameraViewRequestFlags.Active |
                CameraViewRequestFlags.Presenter;
            CameraViewRequestSnapshot matchedWithPendingFlags =
                validRenderedSnapshot;
            matchedWithPendingFlags.Flags |=
                CameraViewRequestFlags.TargetRecreatePending |
                CameraViewRequestFlags.HistoryResetPending;
            Check(
                CameraViewRequestContract.IsValidSnapshot(
                    in validMismatchedPresentation,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in mismatchedWithoutPendingFlags,
                    requestId) &&
                !CameraViewRequestContract.IsValidSnapshot(
                    in matchedWithPendingFlags,
                    requestId),
                "Camera View snapshot validator correlates presented extent with target and history pending flags");

            CameraFrustumControlState frustumAvailable =
                CameraFrustumControlPolicy.Resolve(
                    isThreeDimensionalScene: true,
                    engineAttached: true,
                    cameraAuthoringCapability: true,
                    nativeVisible: true);
            CameraFrustumControlState frustumIn2D =
                CameraFrustumControlPolicy.Resolve(
                    isThreeDimensionalScene: false,
                    engineAttached: true,
                    cameraAuthoringCapability: true,
                    nativeVisible: true);
            CameraFrustumControlState frustumWithoutCapability =
                CameraFrustumControlPolicy.Resolve(
                    isThreeDimensionalScene: true,
                    engineAttached: true,
                    cameraAuthoringCapability: false,
                    nativeVisible: true);
            CameraFrustumControlState frustumWithoutEngine =
                CameraFrustumControlPolicy.Resolve(
                    isThreeDimensionalScene: true,
                    engineAttached: false,
                    cameraAuthoringCapability: true,
                    nativeVisible: true);
            Check(
                frustumAvailable is
                    { IsVisible: true, IsEnabled: true, IsChecked: true } &&
                frustumIn2D is
                    { IsVisible: false, IsEnabled: false, IsChecked: false } &&
                frustumWithoutCapability is
                    { IsVisible: false, IsEnabled: false, IsChecked: false } &&
                frustumWithoutEngine is
                    { IsVisible: false, IsEnabled: false, IsChecked: false },
                "camera frustum control is native-synchronized only for capable 3D scenes");

            CameraAuthoringState worldCamera =
                Camera(12, "world-camera");
            ResolvedAuthoredCameraPose worldPose = Resolved(worldCamera);
            Check(
                CameraSceneViewAlignmentContract.TryPlan(
                    worldCamera,
                    worldPose,
                    currentEditorDistance: 14.0f,
                    out EditorSceneCameraPose worldPlan,
                    out _) &&
                worldPlan.ViewMode == EditorSceneViewMode.Perspective &&
                worldPlan.UsesPerspectiveFloorCompensation &&
                !worldPlan.MatchesAuthoredFraming,
                "Snap View accepts a resolved parent-aware world pose without mutating authored Camera values");

            (float eyeX, float eyeY, float eyeZ) =
                CameraSceneViewAlignmentContract.ReconstructEditorEye(
                    worldPlan);
            (float viewForwardX, float viewForwardY, float viewForwardZ) =
                CameraSceneViewAlignmentContract.ReconstructEditorForward(
                    worldPlan);
            Check(
                Nearly(eyeX, worldPose.PositionX) &&
                Nearly(eyeY, worldPose.PositionY) &&
                Nearly(eyeZ, worldPose.PositionZ) &&
                Nearly(viewForwardX, worldPose.ForwardX) &&
                Nearly(viewForwardY, worldPose.ForwardY) &&
                Nearly(viewForwardZ, worldPose.ForwardZ),
                "world eye and forward survive Euler/orbit conversion including the perspective floor guard");

            CameraAuthoringState orthographicCamera =
                Camera(13, "ortho-camera") with
                {
                    Projection = CameraProjectionMode.Orthographic,
                    OrthographicSize = 62.0f,
                };
            Check(
                CameraSceneViewAlignmentContract.TryPlan(
                    orthographicCamera,
                    Resolved(orthographicCamera),
                    currentEditorDistance: 8.0f,
                    out EditorSceneCameraPose orthographicPlan,
                    out _) &&
                orthographicPlan.ViewMode ==
                    EditorSceneViewMode.Orthographic &&
                Nearly(orthographicPlan.Distance, 100.0f) &&
                orthographicPlan.MatchesAuthoredFraming &&
                !orthographicPlan.UsesPerspectiveFloorCompensation,
                "orthographic Snap View preserves supported authored framing scale");

            ResolvedAuthoredCameraPose nonFinitePose =
                worldPose with { PositionX = float.NaN };
            ResolvedAuthoredCameraPose invalidClippingPose =
                worldPose with
                {
                    NearPlane = 10.0f,
                    FarPlane = 5.0f,
                };
            Check(
                !CameraSceneViewAlignmentContract.TryPlan(
                    worldCamera,
                    nonFinitePose,
                    14.0f,
                    out _,
                    out string nonFinitePoseDetail) &&
                nonFinitePoseDetail.Contains(
                    "finite",
                    StringComparison.Ordinal) &&
                !CameraSceneViewAlignmentContract.TryPlan(
                    worldCamera,
                    invalidClippingPose,
                    14.0f,
                    out _,
                    out string clippingDetail) &&
                clippingDetail.Contains(
                    "clipping",
                    StringComparison.Ordinal),
                "Snap View rejects non-finite world poses and invalid clipping values");

            ResolvedAuthoredCameraPose rolledPose =
                worldPose with
                {
                    UpX = 1.0f,
                    UpY = 0.0f,
                    UpZ = 0.0f,
                };
            ResolvedAuthoredCameraPose polePose =
                worldPose with
                {
                    ForwardX = 0.0f,
                    ForwardY = 0.99999f,
                    ForwardZ = 0.004f,
                    UpX = 0.0f,
                    UpY = -0.004f,
                    UpZ = 0.99999f,
                };
            Check(
                !CameraSceneViewAlignmentContract.TryPlan(
                    worldCamera,
                    rolledPose,
                    14.0f,
                    out _,
                    out string rollDetail) &&
                rollDetail.Contains("roll", StringComparison.Ordinal) &&
                !CameraSceneViewAlignmentContract.TryPlan(
                    worldCamera,
                    polePose,
                    14.0f,
                    out _,
                    out string poleDetail) &&
                poleDetail.Contains("pole", StringComparison.Ordinal),
                "Snap View fails closed when the orbit Camera cannot represent authored roll or a pole pose");

            CameraSceneViewSnapAvailability snapAvailable =
                CameraSceneViewSnapPolicy.Resolve(
                    engineAttached: true,
                    isThreeDimensionalScene: true,
                    sceneEditingBlocked: false,
                    playOrPreviewActive: false,
                    cameraAuthoringCapability: true,
                    cameraViewLeaseActive: false,
                    cameraPresent: true,
                    cameraEnabled: true);
            CameraSceneViewSnapAvailability snapDuringPlay =
                CameraSceneViewSnapPolicy.Resolve(
                    engineAttached: true,
                    isThreeDimensionalScene: true,
                    sceneEditingBlocked: true,
                    playOrPreviewActive: true,
                    cameraAuthoringCapability: true,
                    cameraViewLeaseActive: false,
                    cameraPresent: true,
                    cameraEnabled: true);
            CameraSceneViewSnapAvailability snapDuringLease =
                CameraSceneViewSnapPolicy.Resolve(
                    engineAttached: true,
                    isThreeDimensionalScene: true,
                    sceneEditingBlocked: false,
                    playOrPreviewActive: false,
                    cameraAuthoringCapability: true,
                    cameraViewLeaseActive: true,
                    cameraPresent: true,
                    cameraEnabled: true);
            CameraSceneViewSnapAvailability snapDisabledCamera =
                CameraSceneViewSnapPolicy.Resolve(
                    engineAttached: true,
                    isThreeDimensionalScene: true,
                    sceneEditingBlocked: false,
                    playOrPreviewActive: false,
                    cameraAuthoringCapability: true,
                    cameraViewLeaseActive: false,
                    cameraPresent: true,
                    cameraEnabled: false);
            Check(
                snapAvailable.CanApply &&
                !snapDuringPlay.CanApply &&
                snapDuringPlay.Detail.Contains(
                    "Play/Preview",
                    StringComparison.Ordinal) &&
                !snapDuringLease.CanApply &&
                snapDuringLease.Detail.Contains(
                    "Camera View",
                    StringComparison.Ordinal) &&
                !snapDisabledCamera.CanApply,
                "Snap View explicitly gates Play/Preview, Camera View leases, and disabled Cameras");

            Check(
                RenderSurfaceTransferPolicy.AfterFailedExternalPosition(
                    rollbackSucceeded: true) ==
                    RenderSurfaceOwnership.Embedded &&
                RenderSurfaceTransferPolicy.AfterFailedExternalPosition(
                    rollbackSucceeded: false) ==
                    RenderSurfaceOwnership.External,
                "failed render-surface rollback preserves truthful native HWND ownership");

            CameraViewOpenRecoveryPlan committedOpen =
                CameraViewOpenRecoveryPolicy.Resolve(
                    openCommitted: true,
                    renderSurfaceRestored: true,
                    failedWindowClosed: true);
            CameraViewOpenRecoveryPlan safeFailedOpen =
                CameraViewOpenRecoveryPolicy.Resolve(
                    openCommitted: false,
                    renderSurfaceRestored: true,
                    failedWindowClosed: true);
            CameraViewOpenRecoveryPlan unsafeFailedOpen =
                CameraViewOpenRecoveryPolicy.Resolve(
                    openCommitted: false,
                    renderSurfaceRestored: false,
                    failedWindowClosed: true);
            CameraViewOpenRecoveryPlan orphanedFailedOpen =
                CameraViewOpenRecoveryPolicy.Resolve(
                    openCommitted: false,
                    renderSurfaceRestored: true,
                    failedWindowClosed: false);
            Check(
                committedOpen == default &&
                safeFailedOpen is
                {
                    DiscardFailedWindow: true,
                    RestorePreviewOverride: true,
                    RestoreViewPresentation: true,
                    RestoreViewportOverlay: true,
                } &&
                unsafeFailedOpen == default &&
                orphanedFailedOpen == default,
                "Camera View open failure restores state only after safe surface recovery");

            CameraViewOpenLifecycleState asynchronousFailure =
                CameraViewOpenLifecycle.Transition(
                    CameraViewOpenLifecycleState.None,
                    CameraViewOpenLifecycleEvent.Begin);
            asynchronousFailure = CameraViewOpenLifecycle.Transition(
                asynchronousFailure,
                CameraViewOpenLifecycleEvent.OpenFailed);
            asynchronousFailure = CameraViewOpenLifecycle.Transition(
                asynchronousFailure,
                CameraViewOpenLifecycleEvent.WindowClosed);
            bool retainedUntilRollback =
                asynchronousFailure ==
                CameraViewOpenLifecycleState.RollbackRequired;
            asynchronousFailure = CameraViewOpenLifecycle.Transition(
                asynchronousFailure,
                CameraViewOpenLifecycleEvent.RollbackCompleted);

            CameraViewOpenLifecycleState successfulOpen =
                CameraViewOpenLifecycle.Transition(
                    CameraViewOpenLifecycleState.None,
                    CameraViewOpenLifecycleEvent.Begin);
            successfulOpen = CameraViewOpenLifecycle.Transition(
                successfulOpen,
                CameraViewOpenLifecycleEvent.LiveSurfaceAttached);
            bool committedOnlyAfterAttach =
                successfulOpen ==
                CameraViewOpenLifecycleState.Committed;
            successfulOpen = CameraViewOpenLifecycle.Transition(
                successfulOpen,
                CameraViewOpenLifecycleEvent.WindowClosed);
            Check(
                retainedUntilRollback &&
                asynchronousFailure ==
                    CameraViewOpenLifecycleState.None &&
                committedOnlyAfterAttach &&
                successfulOpen ==
                    CameraViewOpenLifecycleState.None,
                "Camera View keeps the pre-open snapshot through asynchronous attach failure and commits only after attachment");

            Check(
                CameraViewScenePublicationPolicy.ShouldRefresh(
                    publishedCurrentScene: true,
                    nativeReplacementAttempted: true) &&
                !CameraViewScenePublicationPolicy.ShouldRefresh(
                    publishedCurrentScene: false,
                    nativeReplacementAttempted: true) &&
                !CameraViewScenePublicationPolicy.ShouldRefresh(
                    publishedCurrentScene: true,
                    nativeReplacementAttempted: false) &&
                !CameraViewScenePublicationPolicy.ShouldRefresh(
                    publishedCurrentScene: false,
                    nativeReplacementAttempted: false),
                "Camera View refreshes once only for the current published native scene replacement");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unhandled exception: " + error);
        }

        output.WriteLine(
            $"Camera authoring self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
