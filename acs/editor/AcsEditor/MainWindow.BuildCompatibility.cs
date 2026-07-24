// SPDX-License-Identifier: Apache-2.0

using System;

namespace AcsEditor;

/// <summary>
/// Compatibility check shared by Build, Run and Package entry points. Runtime capability follows
/// the loaded legacy source format, never the editor's Perspective/Orthographic/2D view.
/// </summary>
internal static class BuildSceneCompatibility
{
    internal const string Unsupported3DCode = "ACS-BUILD-3D-PARITY-001";

    internal readonly record struct Result(
        bool IsSupported,
        string Code,
        string Summary,
        string Detail);

    internal static Result Evaluate(
        bool runtimeSourceIs3D,
        string? runtimeSourcePath,
        RuntimeBuildCapabilities runtimeCapabilities)
    {
        if (!runtimeSourceIs3D)
            return new Result(true, "", "", "");
        if (runtimeCapabilities.SupportsLegacyScene3D)
            return new Result(true, "", "", runtimeCapabilities.Evidence);

        string displayPath = string.IsNullOrWhiteSpace(runtimeSourcePath)
            ? "(unsaved .acs3d source)"
            : runtimeSourcePath.Trim();
        return new Result(
            false,
            Unsupported3DCode,
            "The loaded source is .acs3d, but this project's runtime has no verified legacy-3D bootstrap.",
            $"Loaded source: {displayPath}. {runtimeCapabilities.Evidence} No scene payload was written and no build pipeline was started.");
    }
}

public partial class MainWindow
{
    /// <summary>
    /// Stops build-like operations before SaveSceneForBuild can serialize an unrelated adapter.
    /// View presets are editor-only and therefore cannot enable or disable the build pipeline.
    /// </summary>
    private bool EnsureBuildSceneCompatibility(string operationName)
    {
        string? loadedSource = SceneDocumentPresentationPath();
        if (string.IsNullOrWhiteSpace(loadedSource) && _project != null)
            loadedSource = ResolveConfiguredProjectScenePath();
        bool loadedSourceIs3D = _legacySceneSourceMode == SceneDocumentMode.ThreeD;
        RuntimeBuildCapabilities runtimeCapabilities =
            _project == null
                ? new RuntimeBuildCapabilities(
                    false,
                    "No project is loaded, so runtime capability cannot be verified.")
                : ProjectManager.DetectRuntimeBuildCapabilities(_project);

        BuildSceneCompatibility.Result result =
            BuildSceneCompatibility.Evaluate(
                loadedSourceIs3D,
                loadedSource,
                runtimeCapabilities);
        if (result.IsSupported)
            return true;

        string operation = string.IsNullOrWhiteSpace(operationName)
            ? "Build pipeline"
            : operationName.Trim();
        ShowBottomTab("build");
        BuildLog($"Build compatibility failed: [{result.Code}] {operation} blocked. {result.Summary}");
        BuildLog($"Build compatibility failed: [{result.Code}] {result.Detail}");
        BuildLog(
            $"Build compatibility failed: [{result.Code}] Add an explicit FLegacyScene3DAdapter bootstrap to this project's Source/Game.cpp before 3D Build, Run or Package can be enabled.");
        return false;
    }
}
