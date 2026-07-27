// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Linq;

namespace AcsEditor;

internal enum CameraProjectionMode
{
    Perspective = 0,
    Orthographic = 1,
}

/// <summary>
/// Managed view of one native authored camera. The native CAM3D record remains
/// authoritative; this value is only the validation, ordering, and WPF binding
/// boundary.
/// </summary>
internal readonly record struct CameraAuthoringState(
    int NodeId,
    string StableCameraId,
    bool IsActive,
    bool IsEnabled,
    CameraProjectionMode Projection,
    float FieldOfViewDegrees,
    float OrthographicSize,
    float NearPlane,
    float FarPlane,
    int Priority);

internal static class CameraAuthoringContract
{
    internal const int MaximumStableCameraIdLength = 64;
    internal const int MaximumCameraCount = 256;
    internal const float MinimumFieldOfViewDegrees = 1.0f;
    internal const float MaximumFieldOfViewDegrees = 179.0f;
    internal const float MinimumOrthographicSize = 0.001f;
    internal const float MaximumOrthographicSize = 1_000_000.0f;
    internal const float MinimumNearPlane = 0.0001f;
    internal const float MaximumNearPlane = 1_000_000.0f;
    internal const float MaximumFarPlane = 1_000_000_000.0f;
    internal const int MinimumPriority = -1_000_000;
    internal const int MaximumPriority = 1_000_000;

    /// <summary>
    /// Camera-stack order shared by Details, Outliner badges, and preview
    /// selection. Explicit active designation wins, followed by priority and
    /// stable identities. NodeId is the final guard for malformed duplicate
    /// stable IDs, so the result remains deterministic.
    /// </summary>
    internal static IOrderedEnumerable<CameraAuthoringState> Order(
        IEnumerable<CameraAuthoringState> cameras)
    {
        ArgumentNullException.ThrowIfNull(cameras);
        return cameras
            .OrderByDescending(static camera => camera.IsActive)
            .ThenByDescending(static camera => camera.Priority)
            .ThenBy(
                static camera => camera.StableCameraId,
                StringComparer.Ordinal)
            .ThenBy(static camera => camera.NodeId);
    }

    internal static CameraAuthoringState? ChooseDesignated(
        IEnumerable<CameraAuthoringState> cameras)
    {
        ArgumentNullException.ThrowIfNull(cameras);
        CameraAuthoringState[] enabled = cameras
            .Where(static camera => camera.IsEnabled)
            .ToArray();
        return enabled.Length == 0 ? null : Order(enabled).First();
    }

    internal static CameraAuthoringState[] ActivateExclusively(
        IEnumerable<CameraAuthoringState> cameras,
        string stableCameraId,
        int nodeId)
    {
        ArgumentNullException.ThrowIfNull(cameras);
        CameraAuthoringState[] snapshot = cameras.ToArray();
        int target = Array.FindIndex(
            snapshot,
            camera =>
                string.Equals(
                    camera.StableCameraId,
                    stableCameraId,
                    StringComparison.Ordinal) &&
                camera.NodeId == nodeId);
        if (target < 0)
            throw new KeyNotFoundException(
                "The selected camera is no longer present in the authored scene.");

        for (int i = 0; i < snapshot.Length; i++)
            snapshot[i] = snapshot[i] with { IsActive = i == target };
        return snapshot;
    }

    internal static bool TryNormalize(
        CameraAuthoringState candidate,
        out CameraAuthoringState normalized,
        out string detail)
    {
        normalized = default;
        detail = "";
        if (candidate.NodeId < 0)
        {
            detail = "Camera node identity is invalid.";
            return false;
        }
        if (!IsValidStableCameraId(candidate.StableCameraId))
        {
            detail = "Camera stable identity is invalid.";
            return false;
        }
        if (!Enum.IsDefined(candidate.Projection))
        {
            detail = "Camera projection mode is invalid.";
            return false;
        }
        if (!IsFinite(candidate.FieldOfViewDegrees) ||
            !IsFinite(candidate.OrthographicSize) ||
            !IsFinite(candidate.NearPlane) ||
            !IsFinite(candidate.FarPlane))
        {
            detail = "Camera projection values must be finite.";
            return false;
        }

        float fieldOfView = Math.Clamp(
            candidate.FieldOfViewDegrees,
            MinimumFieldOfViewDegrees,
            MaximumFieldOfViewDegrees);
        float orthographicSize = Math.Clamp(
            candidate.OrthographicSize,
            MinimumOrthographicSize,
            MaximumOrthographicSize);
        float nearPlane = Math.Max(
            candidate.NearPlane,
            MinimumNearPlane);
        nearPlane = Math.Min(nearPlane, MaximumNearPlane);
        float farPlane = Math.Clamp(
            candidate.FarPlane,
            MinimumNearPlane * 2.0f,
            MaximumFarPlane);
        if (farPlane <= nearPlane)
            farPlane = Math.Min(
                Math.Max(nearPlane + MinimumNearPlane, nearPlane * 1.01f),
                MaximumFarPlane);
        if (farPlane <= nearPlane)
        {
            detail = "Camera far plane must be greater than its near plane.";
            return false;
        }

        normalized = candidate with
        {
            FieldOfViewDegrees = fieldOfView,
            OrthographicSize = orthographicSize,
            NearPlane = nearPlane,
            FarPlane = farPlane,
            Priority = Math.Clamp(
                candidate.Priority,
                MinimumPriority,
                MaximumPriority),
        };
        return true;
    }

    private static bool IsFinite(float value) =>
        !float.IsNaN(value) && !float.IsInfinity(value);

    internal static bool IsValidStableCameraId(string? value)
    {
        if (string.IsNullOrEmpty(value) ||
            value.Length > MaximumStableCameraIdLength ||
            !IsAsciiLetterOrDigit(value[0]))
        {
            return false;
        }

        for (int i = 1; i < value.Length; i++)
        {
            char c = value[i];
            if (!IsAsciiLetterOrDigit(c) &&
                c != '_' && c != '.' && c != '-')
            {
                return false;
            }
        }
        return true;
    }

    private static bool IsAsciiLetterOrDigit(char value) =>
        value is >= 'a' and <= 'z' or
            >= 'A' and <= 'Z' or
            >= '0' and <= '9';
}
