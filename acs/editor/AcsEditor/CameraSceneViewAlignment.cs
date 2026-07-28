// SPDX-License-Identifier: Apache-2.0

using System;

namespace AcsEditor;

/// <summary>
/// Exact world-space camera sample returned by the native game-camera
/// resolver. The resolver already accounts for parent transforms; the managed
/// contract only converts that resolved pose into the editor orbit-camera
/// representation.
/// </summary>
internal readonly record struct ResolvedAuthoredCameraPose(
    int SourceNodeId,
    CameraProjectionMode Projection,
    float PositionX,
    float PositionY,
    float PositionZ,
    float ForwardX,
    float ForwardY,
    float ForwardZ,
    float UpX,
    float UpY,
    float UpZ,
    float FieldOfViewDegrees,
    float OrthographicHeight,
    float NearPlane,
    float FarPlane);

internal readonly record struct EditorSceneCameraPose(
    EditorSceneViewMode ViewMode,
    float YawRadians,
    float PitchRadians,
    float Distance,
    float TargetX,
    float TargetY,
    float TargetZ,
    bool MatchesAuthoredFraming,
    bool UsesPerspectiveFloorCompensation);

internal readonly record struct CameraSceneViewSnapAvailability(
    bool CanApply,
    string Detail);

/// <summary>
/// Fail-closed gate for the one-shot "Snap Scene View to Camera" command.
/// The operation changes editor navigation only, so it is never allowed while
/// Play/Preview or a Camera View render-surface lease owns camera presentation.
/// </summary>
internal static class CameraSceneViewSnapPolicy
{
    internal static CameraSceneViewSnapAvailability Resolve(
        bool engineAttached,
        bool isThreeDimensionalScene,
        bool sceneEditingBlocked,
        bool playOrPreviewActive,
        bool cameraAuthoringCapability,
        bool cameraViewLeaseActive,
        bool cameraPresent,
        bool cameraEnabled)
    {
        if (!engineAttached)
            return new(false, "The editor runtime is not attached.");
        if (!isThreeDimensionalScene)
            return new(
                false,
                "Camera pose snapping requires a three-dimensional world.");
        if (playOrPreviewActive)
        {
            return new(
                false,
                "Stop Play/Preview before snapping Scene View to a Camera.");
        }
        if (sceneEditingBlocked)
            return new(false, "Scene editing is currently blocked.");
        if (!cameraAuthoringCapability)
        {
            return new(
                false,
                "The loaded runtime does not support authored cameras.");
        }
        if (cameraViewLeaseActive)
        {
            return new(
                false,
                "Re-dock or close Camera View before snapping Scene View.");
        }
        if (!cameraPresent)
            return new(false, "The selected Camera is no longer in the scene.");
        if (!cameraEnabled)
            return new(false, "Disabled Cameras cannot drive Scene View.");
        return new(true, "");
    }
}

/// <summary>
/// Converts a resolved authored-camera world pose into the native editor
/// orbit-camera representation. Unsupported roll/pole poses are rejected
/// instead of silently producing a visibly different view.
/// </summary>
internal static class CameraSceneViewAlignmentContract
{
    internal const float MinimumEditorDistance = 1.0f;
    internal const float MaximumEditorDistance = 200.0f;
    internal const float EditorOrthographicHeightPerDistance = 0.62f;
    internal const float EditorPerspectiveFieldOfViewDegrees = 50.0f;
    internal const float PerspectiveEyeFloorOffset = 0.30f;
    internal const float MaximumEditorPitchRadians = 1.5533f;

    private const float MinimumDirectionLengthSquared = 1.0e-12f;
    private const float MaximumAxisDot = 1.0e-3f;
    private const float MinimumUpAlignment = 0.999f;

    internal static bool TryPlan(
        CameraAuthoringState authored,
        ResolvedAuthoredCameraPose resolved,
        float currentEditorDistance,
        out EditorSceneCameraPose plan,
        out string detail)
    {
        plan = default;
        detail = "";

        if (!CameraAuthoringContract.TryNormalize(
                authored,
                out CameraAuthoringState normalized,
                out detail) ||
            normalized != authored)
        {
            if (detail.Length == 0)
                detail = "Authored Camera values are outside supported bounds.";
            return false;
        }
        if (!authored.IsEnabled)
        {
            detail = "Disabled Cameras cannot drive Scene View.";
            return false;
        }
        if (resolved.SourceNodeId != authored.NodeId)
        {
            detail = "The resolved Camera identity changed during the operation.";
            return false;
        }
        if (!Enum.IsDefined(resolved.Projection) ||
            resolved.Projection != authored.Projection)
        {
            detail = "The resolved Camera projection changed during the operation.";
            return false;
        }
        if (!IsFinite(resolved.PositionX) ||
            !IsFinite(resolved.PositionY) ||
            !IsFinite(resolved.PositionZ) ||
            !IsFinite(resolved.ForwardX) ||
            !IsFinite(resolved.ForwardY) ||
            !IsFinite(resolved.ForwardZ) ||
            !IsFinite(resolved.UpX) ||
            !IsFinite(resolved.UpY) ||
            !IsFinite(resolved.UpZ) ||
            !IsFinite(resolved.FieldOfViewDegrees) ||
            !IsFinite(resolved.OrthographicHeight) ||
            !IsFinite(resolved.NearPlane) ||
            !IsFinite(resolved.FarPlane) ||
            !IsFinite(currentEditorDistance))
        {
            detail = "Resolved Camera pose and clipping values must be finite.";
            return false;
        }
        if (resolved.FieldOfViewDegrees <
                CameraAuthoringContract.MinimumFieldOfViewDegrees ||
            resolved.FieldOfViewDegrees >
                CameraAuthoringContract.MaximumFieldOfViewDegrees ||
            resolved.OrthographicHeight <
                CameraAuthoringContract.MinimumOrthographicSize ||
            resolved.OrthographicHeight >
                CameraAuthoringContract.MaximumOrthographicSize ||
            resolved.NearPlane <
                CameraAuthoringContract.MinimumNearPlane ||
            resolved.NearPlane >
                CameraAuthoringContract.MaximumNearPlane ||
            resolved.FarPlane <= resolved.NearPlane ||
            resolved.FarPlane >
                CameraAuthoringContract.MaximumFarPlane)
        {
            detail = "Resolved Camera clipping or projection values are invalid.";
            return false;
        }
        if (!NearlyEqual(
                resolved.FieldOfViewDegrees,
                authored.FieldOfViewDegrees) ||
            !NearlyEqual(
                resolved.OrthographicHeight,
                authored.OrthographicSize) ||
            !NearlyEqual(resolved.NearPlane, authored.NearPlane) ||
            !NearlyEqual(resolved.FarPlane, authored.FarPlane))
        {
            detail = "The authored Camera changed while its pose was resolved.";
            return false;
        }
        if (currentEditorDistance < MinimumEditorDistance ||
            currentEditorDistance > MaximumEditorDistance)
        {
            detail = "The current editor Camera distance is invalid.";
            return false;
        }

        if (!TryNormalize(
                resolved.ForwardX,
                resolved.ForwardY,
                resolved.ForwardZ,
                out float forwardX,
                out float forwardY,
                out float forwardZ) ||
            !TryNormalize(
                resolved.UpX,
                resolved.UpY,
                resolved.UpZ,
                out float upX,
                out float upY,
                out float upZ))
        {
            detail = "Resolved Camera forward/up axes are degenerate.";
            return false;
        }
        if (MathF.Abs(
                forwardX * upX +
                forwardY * upY +
                forwardZ * upZ) > MaximumAxisDot)
        {
            detail = "Resolved Camera forward/up axes are not orthogonal.";
            return false;
        }

        float pitch = -MathF.Asin(Math.Clamp(forwardY, -1.0f, 1.0f));
        float yaw = MathF.Atan2(-forwardX, -forwardZ);
        if (!IsFinite(pitch) ||
            !IsFinite(yaw) ||
            MathF.Abs(pitch) > MaximumEditorPitchRadians)
        {
            detail =
                "This Camera is too close to the editor orbit-camera pole.";
            return false;
        }

        float sinYaw = MathF.Sin(yaw);
        float cosYaw = MathF.Cos(yaw);
        float sinPitch = MathF.Sin(pitch);
        float cosPitch = MathF.Cos(pitch);
        float expectedUpX = -sinYaw * sinPitch;
        float expectedUpY = cosPitch;
        float expectedUpZ = -cosYaw * sinPitch;
        float upAlignment =
            expectedUpX * upX +
            expectedUpY * upY +
            expectedUpZ * upZ;
        if (!IsFinite(upAlignment) || upAlignment < MinimumUpAlignment)
        {
            detail =
                "Scene View cannot represent this Camera's roll without loss.";
            return false;
        }

        bool orthographic =
            resolved.Projection == CameraProjectionMode.Orthographic;
        float desiredDistance = orthographic
            ? resolved.OrthographicHeight /
                EditorOrthographicHeightPerDistance
            : currentEditorDistance;
        float distance = Math.Clamp(
            desiredDistance,
            MinimumEditorDistance,
            MaximumEditorDistance);
        bool matchesFraming = orthographic
            ? NearlyEqual(distance, desiredDistance)
            : NearlyEqual(
                resolved.FieldOfViewDegrees,
                EditorPerspectiveFieldOfViewDegrees);

        float naturalTargetY =
            resolved.PositionY + forwardY * distance;
        bool usesFloorCompensation =
            !orthographic &&
            naturalTargetY >
                resolved.PositionY - PerspectiveEyeFloorOffset;
        float targetY = usesFloorCompensation
            ? resolved.PositionY - PerspectiveEyeFloorOffset
            : naturalTargetY;
        float targetX = resolved.PositionX + forwardX * distance;
        float targetZ = resolved.PositionZ + forwardZ * distance;
        if (!IsFinite(distance) ||
            !IsFinite(targetX) ||
            !IsFinite(targetY) ||
            !IsFinite(targetZ))
        {
            detail = "The converted editor Camera pose is not finite.";
            return false;
        }

        plan = new EditorSceneCameraPose(
            orthographic
                ? EditorSceneViewMode.Orthographic
                : EditorSceneViewMode.Perspective,
            yaw,
            pitch,
            distance,
            targetX,
            targetY,
            targetZ,
            matchesFraming,
            usesFloorCompensation);
        return true;
    }

    internal static (float X, float Y, float Z) ReconstructEditorForward(
        EditorSceneCameraPose pose)
    {
        float cosPitch = MathF.Cos(pose.PitchRadians);
        return (
            -cosPitch * MathF.Sin(pose.YawRadians),
            -MathF.Sin(pose.PitchRadians),
            -cosPitch * MathF.Cos(pose.YawRadians));
    }

    internal static (float X, float Y, float Z) ReconstructEditorEye(
        EditorSceneCameraPose pose)
    {
        (float forwardX, float forwardY, float forwardZ) =
            ReconstructEditorForward(pose);
        float eyeX = pose.TargetX - forwardX * pose.Distance;
        float eyeY = pose.TargetY - forwardY * pose.Distance;
        float eyeZ = pose.TargetZ - forwardZ * pose.Distance;
        if (pose.ViewMode == EditorSceneViewMode.Perspective)
        {
            eyeY = MathF.Max(
                eyeY,
                pose.TargetY + PerspectiveEyeFloorOffset);
        }
        return (eyeX, eyeY, eyeZ);
    }

    private static bool TryNormalize(
        float x,
        float y,
        float z,
        out float normalizedX,
        out float normalizedY,
        out float normalizedZ)
    {
        normalizedX = 0.0f;
        normalizedY = 0.0f;
        normalizedZ = 0.0f;
        float lengthSquared = x * x + y * y + z * z;
        if (!IsFinite(lengthSquared) ||
            lengthSquared <= MinimumDirectionLengthSquared)
        {
            return false;
        }
        float inverseLength = 1.0f / MathF.Sqrt(lengthSquared);
        normalizedX = x * inverseLength;
        normalizedY = y * inverseLength;
        normalizedZ = z * inverseLength;
        return IsFinite(normalizedX) &&
               IsFinite(normalizedY) &&
               IsFinite(normalizedZ);
    }

    private static bool NearlyEqual(float left, float right)
    {
        float scale = MathF.Max(
            1.0f,
            MathF.Max(MathF.Abs(left), MathF.Abs(right)));
        return MathF.Abs(left - right) <= scale * 1.0e-4f;
    }

    private static bool IsFinite(float value) =>
        !float.IsNaN(value) && !float.IsInfinity(value);
}
