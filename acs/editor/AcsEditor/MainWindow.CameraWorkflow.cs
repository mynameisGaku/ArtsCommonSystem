// SPDX-License-Identifier: Apache-2.0

using System;

namespace AcsEditor;

public partial class MainWindow
{
    private readonly record struct EditorNavigationCameraSnapshot(
        EditorSceneViewMode ViewMode,
        float YawRadians,
        float PitchRadians,
        float Distance,
        float TargetX,
        float TargetY,
        float TargetZ);

    /// <summary>
    /// One-shot navigation operation analogous to framing through an authored
    /// Camera. It never changes the Camera component, Active flag, scene dirty
    /// state, or scene undo history.
    /// </summary>
    private void SnapSceneViewToCamera(int nodeId)
    {
        bool engineAttached = Engine != IntPtr.Zero;
        bool playOrPreviewActive =
            engineAttached &&
            (EngineInterop.acs_editor_play_state(Engine) != 0 ||
             PreviewBtn.IsChecked == true);
        bool cameraViewLeaseActive =
            HasCameraViewLease ||
            DetachedCameraViewOwnsLiveSurface;

        CameraSceneViewSnapAvailability environment =
            CameraSceneViewSnapPolicy.Resolve(
                engineAttached,
                _view3d,
                IsSceneEditingBlocked,
                playOrPreviewActive,
                CameraAuthoringAvailable,
                cameraViewLeaseActive,
                cameraPresent: true,
                cameraEnabled: true);
        if (!environment.CanApply)
        {
            Log(environment.Detail, "Camera", LogLevel.Warn);
            return;
        }

        bool cameraPresent = TryGetCameraAuthoringState(
            nodeId,
            out CameraAuthoringState authored);
        CameraSceneViewSnapAvailability cameraAvailability =
            CameraSceneViewSnapPolicy.Resolve(
                engineAttached: true,
                isThreeDimensionalScene: true,
                sceneEditingBlocked: false,
                playOrPreviewActive: false,
                cameraAuthoringCapability: true,
                cameraViewLeaseActive: false,
                cameraPresent,
                cameraEnabled: cameraPresent && authored.IsEnabled);
        if (!cameraAvailability.CanApply)
        {
            Log(cameraAvailability.Detail, "Camera", LogLevel.Warn);
            return;
        }

        if (!TryCaptureEditorNavigationCamera(
                out EditorNavigationCameraSnapshot original,
                out string captureDetail))
        {
            Log(captureDetail, "Camera", LogLevel.Error);
            return;
        }
        if (!TryResolveAuthoredCameraPose(
                nodeId,
                out ResolvedAuthoredCameraPose resolved,
                out string resolveDetail))
        {
            Log(resolveDetail, "Camera", LogLevel.Error);
            return;
        }

        // Stable identity and Camera values must still match after the
        // transient native resolver round trip.
        if (!TryGetCameraAuthoringState(
                nodeId,
                out CameraAuthoringState current) ||
            current != authored)
        {
            Log(
                "The selected Camera changed while its world pose was resolved.",
                "Camera",
                LogLevel.Warn);
            return;
        }
        if (!CameraSceneViewAlignmentContract.TryPlan(
                authored,
                resolved,
                original.Distance,
                out EditorSceneCameraPose plan,
                out string planDetail))
        {
            Log(
                "Scene View was left unchanged: " + planDetail,
                "Camera",
                LogLevel.Warn);
            return;
        }

        bool applied = false;
        string failureDetail = "the editor Camera rejected the converted pose";
        try
        {
            _sceneViewMode = plan.ViewMode;
            EngineInterop.acs_editor_set_ortho3d(
                Engine,
                plan.ViewMode == EditorSceneViewMode.Orthographic ? 1 : 0);
            applied =
                EngineInterop.acs_editor_camera3d_set(
                    Engine,
                    plan.YawRadians,
                    plan.PitchRadians,
                    plan.Distance,
                    plan.TargetX,
                    plan.TargetY,
                    plan.TargetZ) != 0;
            if (applied)
                ApplySceneViewModePresentation();
        }
        catch (Exception error)
        {
            failureDetail = error.Message;
            applied = false;
        }

        if (!applied)
        {
            bool rolledBack = RestoreEditorNavigationCamera(original);
            Log(
                rolledBack
                    ? "Scene View snap failed and the previous editor Camera " +
                      "was restored: " + failureDetail
                    : "Scene View snap failed, and restoring the previous " +
                      "editor Camera also failed: " + failureDetail,
                "Camera",
                LogLevel.Error);
            return;
        }

        if (GameTabBtn.IsChecked == true)
            SetGameView(false);
        Log(
            $"Scene View snapped to Camera '{authored.StableCameraId}' " +
            $"(node {nodeId}) without modifying authored state." +
            (plan.MatchesAuthoredFraming
                ? ""
                : " Pose and projection match; Scene View uses its supported " +
                  "framing range."),
            "Camera",
            LogLevel.Success);
    }

    private bool TryCaptureEditorNavigationCamera(
        out EditorNavigationCameraSnapshot snapshot,
        out string detail)
    {
        snapshot = default;
        detail = "";
        if (Engine == IntPtr.Zero ||
            EngineInterop.acs_editor_camera3d_get(
                Engine,
                out float yaw,
                out float pitch,
                out float distance,
                out float targetX,
                out float targetY,
                out float targetZ) == 0 ||
            !IsFinite(yaw) ||
            !IsFinite(pitch) ||
            !IsFinite(distance) ||
            !IsFinite(targetX) ||
            !IsFinite(targetY) ||
            !IsFinite(targetZ))
        {
            detail = "The current editor Camera pose could not be captured.";
            return false;
        }
        snapshot = new EditorNavigationCameraSnapshot(
            _sceneViewMode,
            yaw,
            pitch,
            distance,
            targetX,
            targetY,
            targetZ);
        return true;
    }

    private bool RestoreEditorNavigationCamera(
        EditorNavigationCameraSnapshot snapshot)
    {
        if (Engine == IntPtr.Zero)
            return false;
        try
        {
            _sceneViewMode = snapshot.ViewMode;
            EngineInterop.acs_editor_set_ortho3d(
                Engine,
                snapshot.ViewMode == EditorSceneViewMode.Orthographic ? 1 : 0);
            bool restored =
                EngineInterop.acs_editor_camera3d_set(
                    Engine,
                    snapshot.YawRadians,
                    snapshot.PitchRadians,
                    snapshot.Distance,
                    snapshot.TargetX,
                    snapshot.TargetY,
                    snapshot.TargetZ) != 0;
            ApplySceneViewModePresentation();
            return restored;
        }
        catch
        {
            return false;
        }
    }

    private bool TryResolveAuthoredCameraPose(
        int nodeId,
        out ResolvedAuthoredCameraPose resolved,
        out string detail)
    {
        resolved = default;
        detail = "";
        if (Engine == IntPtr.Zero)
        {
            detail = "The editor runtime is not attached.";
            return false;
        }

        bool hadPreviousPreview =
            EngineInterop.acs_editor_game_camera_preview_get(
                Engine,
                out int previousPreviewNodeId) != 0;
        bool previewChanged =
            !hadPreviousPreview || previousPreviewNodeId != nodeId;
        bool previewMutationCommitted = false;
        bool querySucceeded = false;
        bool restorationSucceeded = true;
        string queryDetail = "";
        ResolvedAuthoredCameraPose candidate = default;

        try
        {
            if (previewChanged)
            {
                if (EngineInterop.acs_editor_game_camera_preview_set(
                        Engine,
                        nodeId) == 0)
                {
                    queryDetail =
                        "The selected Camera could not acquire the transient " +
                        "preview resolver.";
                }
                else
                {
                    previewMutationCommitted = true;
                }
            }

            if (!previewChanged || previewMutationCommitted)
            {
                var position = new float[3];
                var forward = new float[3];
                var up = new float[3];
                var projectionValues = new float[4];
                if (EngineInterop.acs_editor_game_camera3d_get(
                        Engine,
                        1.0f,
                        out int projection,
                        out int sourceNodeId,
                        position,
                        forward,
                        up,
                        projectionValues) == 0)
                {
                    queryDetail =
                        "The selected Camera world pose could not be resolved.";
                }
                else
                {
                    candidate = new ResolvedAuthoredCameraPose(
                        sourceNodeId,
                        (CameraProjectionMode)projection,
                        position[0],
                        position[1],
                        position[2],
                        forward[0],
                        forward[1],
                        forward[2],
                        up[0],
                        up[1],
                        up[2],
                        projectionValues[0],
                        projectionValues[1],
                        projectionValues[2],
                        projectionValues[3]);
                    querySucceeded = true;
                }
            }
        }
        catch (Exception error)
        {
            queryDetail =
                "The selected Camera world pose resolver failed: " +
                error.Message;
        }
        finally
        {
            if (previewMutationCommitted)
            {
                try
                {
                    if (hadPreviousPreview)
                    {
                        restorationSucceeded =
                            EngineInterop.acs_editor_game_camera_preview_set(
                                Engine,
                                previousPreviewNodeId) != 0;
                        if (!restorationSucceeded)
                        {
                            EngineInterop.acs_editor_game_camera_preview_clear(
                                Engine);
                        }
                    }
                    else
                    {
                        EngineInterop.acs_editor_game_camera_preview_clear(
                            Engine);
                    }
                }
                catch
                {
                    restorationSucceeded = false;
                }
            }
        }

        if (!restorationSucceeded)
        {
            detail =
                "The previous Camera preview could not be restored; " +
                "Scene View was left unchanged.";
            return false;
        }
        if (!querySucceeded)
        {
            detail = queryDetail.Length == 0
                ? "The selected Camera world pose could not be resolved."
                : queryDetail;
            return false;
        }
        resolved = candidate;
        return true;
    }

    private static bool IsFinite(float value) =>
        !float.IsNaN(value) && !float.IsInfinity(value);
}
