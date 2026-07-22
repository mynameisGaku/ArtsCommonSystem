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

    internal static Result Evaluate(bool runtimeSourceIs3D, string? runtimeSourcePath)
    {
        if (!runtimeSourceIs3D)
            return new Result(true, "", "", "");

        string displayPath = string.IsNullOrWhiteSpace(runtimeSourcePath)
            ? "(unsaved .acs3d source)"
            : runtimeSourcePath.Trim();
        return new Result(
            false,
            Unsupported3DCode,
            "The loaded legacy runtime source is .acs3d, but the generated runtime still loads only .acscene data.",
            $"Loaded source: {displayPath}. No unrelated .acscene payload was written and no build pipeline was started.");
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

        BuildSceneCompatibility.Result result =
            BuildSceneCompatibility.Evaluate(
                loadedSourceIs3D,
                loadedSource);
        if (result.IsSupported)
            return true;

        string operation = string.IsNullOrWhiteSpace(operationName)
            ? "Build pipeline"
            : operationName.Trim();
        ShowBottomTab("build");
        BuildLog($"Build compatibility failed: [{result.Code}] {operation} blocked. {result.Summary}");
        BuildLog($"Build compatibility failed: [{result.Code}] {result.Detail}");
        BuildLog(
            $"Build compatibility failed: [{result.Code}] ACS3D runtime/manifest parity must be implemented before 3D Build, Run or Package can be enabled.");
        return false;
    }
}
