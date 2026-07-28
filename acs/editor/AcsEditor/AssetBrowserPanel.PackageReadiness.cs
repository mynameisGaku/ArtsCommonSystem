// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Windows;

namespace AcsEditor;

public partial class AssetBrowserPanel
{
    private void OnPackageReadiness(object sender, RoutedEventArgs e)
    {
        if (_project == null ||
            _assetDatabase == null ||
            !CanStartAssetOperation("Open Package Readiness"))
        {
            return;
        }

        Project project = _project;
        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        var request = new AssetPackageReadinessRequest(
            project.Name,
            project.RootDir,
            project.AssetsDir,
            project.CanonicalSceneAssetId);
        var window = new AssetPackageReadinessWindow(request)
        {
            Owner = Window.GetWindow(this),
        };
        window.LocateRequested += (_, requestArgs) =>
        {
            if (!IsCurrentOperationContext(database, generation))
                return;
            LocateReadinessAsset(project, requestArgs.AssetPath);
        };
        window.ReferenceViewerRequested += (_, requestArgs) =>
        {
            if (!IsCurrentOperationContext(database, generation))
            {
                return;
            }
            AssetRecord? record = null;
            if (requestArgs.AssetId.Length != 0)
            {
                _ = database.TryGetByAssetId(
                    requestArgs.AssetId,
                    out record);
            }
            if (record == null &&
                requestArgs.AssetPath.Length != 0)
            {
                string candidate = Path.Combine(
                    project.AssetsDir,
                    requestArgs.AssetPath.Replace(
                        '/',
                        Path.DirectorySeparatorChar));
                _ = database.TryGetByPath(candidate, out record);
            }
            if (record == null)
                return;
            var viewer = new AssetReferenceViewerWindow(
                database,
                record.AssetId)
            {
                Owner = Window.GetWindow(this),
            };
            viewer.Show();
        };
        window.Show();
    }

    private void LocateReadinessAsset(
        Project project,
        string relativePath)
    {
        if (string.IsNullOrWhiteSpace(relativePath))
            return;
        try
        {
            string candidate = Path.GetFullPath(
                Path.Combine(
                    project.AssetsDir,
                    relativePath.Replace(
                        '/',
                        Path.DirectorySeparatorChar)));
            if (!AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                    project.AssetsDir,
                    candidate,
                    requireDirectory: false,
                    out string full))
            {
                Log?.Invoke(
                    "Package Readiness asset is missing or unsafe: " +
                    relativePath);
                return;
            }
            string? parent = Path.GetDirectoryName(full);
            if (parent == null ||
                !NavigateToDirectory(parent, addHistory: true))
            {
                return;
            }
            SelectCreatedAsset(full, beginRename: false);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                ArgumentException or NotSupportedException)
        {
            Log?.Invoke(
                "Package Readiness asset could not be located: " +
                error.Message);
        }
    }
}
