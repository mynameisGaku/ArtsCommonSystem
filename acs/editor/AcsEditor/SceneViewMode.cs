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
    Perspective = 0,
    // Keep the historical TwoD ordinal for any editor-state snapshots while naming the
    // behavior precisely: this is a projection preset, never a second scene type.
    Orthographic = 1,
}

internal readonly record struct EditorSceneViewDescriptor(
    bool IsOrthographic,
    string ToolbarLabel,
    string StatusLabel,
    string Description);

/// <summary>
/// Initial source-adapter and view selection for one unpublished editor document.
/// A missing source is a new canonical 3D world, not an implicit legacy adapter.
/// </summary>
internal readonly record struct EditorSceneStartupPlan(
    SceneDocumentMode SourceMode,
    EditorSceneViewMode ViewMode,
    string SourceExtension)
{
    internal bool Uses3D => SourceMode == SceneDocumentMode.ThreeD;
}

internal static class EditorSceneStartupPolicy
{
    internal static EditorSceneStartupPlan Resolve(
        string? scenePath,
        string? projectTemplate)
    {
        if (string.IsNullOrWhiteSpace(scenePath))
        {
            return new EditorSceneStartupPlan(
                SceneDocumentMode.ThreeD,
                EditorSceneViewMode.Perspective,
                ".acs3d");
        }

        string extension = Path.GetExtension(scenePath);
        if (string.Equals(
                extension,
                ".acs3d",
                StringComparison.OrdinalIgnoreCase))
        {
            return new EditorSceneStartupPlan(
                SceneDocumentMode.ThreeD,
                EditorSceneViewModePolicy.InitialForProject(
                    scenePath,
                    projectTemplate),
                ".acs3d");
        }
        if (string.Equals(
                extension,
                ".acscene",
                StringComparison.OrdinalIgnoreCase))
        {
            return new EditorSceneStartupPlan(
                SceneDocumentMode.TwoD,
                EditorSceneViewMode.Orthographic,
                ".acscene");
        }

        throw new InvalidDataException(
            "Initial scene sources must use the .acs3d or .acscene extension.");
    }
}

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
        EditorSceneViewMode.Orthographic => new(
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
            : EditorSceneViewMode.Orthographic;

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
            ? EditorSceneViewMode.Orthographic
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
            mode = EditorSceneViewMode.Orthographic;
            return true;
        }

        mode = default;
        return false;
    }
}
