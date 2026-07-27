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
