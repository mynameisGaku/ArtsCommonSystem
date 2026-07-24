// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

/// <summary>
/// Editor-only ways to inspect and manipulate one scene document. These values are deliberately
/// absent from scene serialization: changing the viewport must not change authored world content.
/// </summary>
internal enum EditorSceneViewMode
{
    Perspective,
    TwoD,
}

internal readonly record struct EditorSceneViewDescriptor(
    bool IsOrthographic,
    string ToolbarLabel,
    string StatusLabel,
    string Description);

/// <summary>
/// Pure state transition used by the WPF shell and headless self-test. SourceMode identifies the
/// loaded compatibility payload and cannot be changed by TryChangeView.
/// </summary>
internal readonly record struct EditorSceneViewState(
    SceneDocumentMode SourceMode,
    EditorSceneViewMode ViewMode)
{
    internal string ActivePayloadKey =>
        SourceMode == SceneDocumentMode.ThreeD ? "acs3d" : "acscene";

    internal bool TryChangeView(
        EditorSceneViewMode requested,
        out EditorSceneViewState next)
    {
        if (!EditorSceneViewModePolicy.IsSupportedByLegacySource(
                requested,
                SourceMode))
        {
            next = this;
            return false;
        }
        next = this with { ViewMode = requested };
        return true;
    }
}

/// <summary>
/// Describes editor-only viewport behavior. Legacy source adapter selection is intentionally a
/// separate input: a view preset must never choose a different native payload.
/// </summary>
internal static class EditorSceneViewModePolicy
{
    internal static EditorSceneViewDescriptor Describe(EditorSceneViewMode mode) => mode switch
    {
        EditorSceneViewMode.Perspective => new(
            IsOrthographic: false,
            ToolbarLabel: "Perspective",
            StatusLabel: "PERSPECTIVE",
            Description: "Perspective view of the scene's 3D world."),
        EditorSceneViewMode.TwoD => new(
            IsOrthographic: true,
            ToolbarLabel: "2D (Orthographic)",
            StatusLabel: "2D",
            Description:
                "XY-front orthographic view with orbit disabled and pan navigation."),
        _ => throw new ArgumentOutOfRangeException(nameof(mode)),
    };

    /// <summary>
    /// Legacy sources choose a useful initial view, but their extension is not a permanent editor
    /// mode. Users can switch view presets without converting or renaming the source.
    /// </summary>
    internal static EditorSceneViewMode InitialForLegacySource(string? path) =>
        string.Equals(
            Path.GetExtension(path),
            ".acs3d",
            StringComparison.OrdinalIgnoreCase)
            ? EditorSceneViewMode.Perspective
            : EditorSceneViewMode.TwoD;

    /// <summary>
    /// New 2D projects still author an ACS3D world. Their template selects only the initial
    /// editor camera preset; it never changes the source adapter or serialized payload.
    /// Existing .acscene projects retain their legacy 2D-only behavior.
    /// </summary>
    internal static EditorSceneViewMode InitialForProject(
        string? path,
        string? projectTemplate) =>
        string.Equals(
            Path.GetExtension(path),
            ".acs3d",
            StringComparison.OrdinalIgnoreCase) &&
        string.Equals(
            projectTemplate,
            "2d",
            StringComparison.OrdinalIgnoreCase)
            ? EditorSceneViewMode.TwoD
            : InitialForLegacySource(path);

    internal static bool IsSupportedByLegacySource(
        EditorSceneViewMode mode,
        SceneDocumentMode sourceMode) =>
        sourceMode == SceneDocumentMode.ThreeD ||
        mode != EditorSceneViewMode.Perspective;

    internal static bool TryParse(string? value, out EditorSceneViewMode mode)
    {
        if (string.Equals(value, "perspective", StringComparison.OrdinalIgnoreCase))
        {
            mode = EditorSceneViewMode.Perspective;
            return true;
        }
        if (string.Equals(value, "2d", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(value, "twod", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(value, "orthographic", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(value, "ortho", StringComparison.OrdinalIgnoreCase))
        {
            mode = EditorSceneViewMode.TwoD;
            return true;
        }

        mode = default;
        return false;
    }
}
