// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;

namespace AcsEditor;

internal static class AssetPackageReadinessSelfTest
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

        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-package-readiness-selftest-" +
            Guid.NewGuid().ToString("N"));
        try
        {
            string assets = Path.Combine(root, "Assets");
            Directory.CreateDirectory(assets);
            string scene = Path.Combine(assets, "main.acs3d");
            File.WriteAllText(
                scene,
                MinimalScene(),
                new UTF8Encoding(false));

            var database = new AssetDatabase(root, assets);
            database.Refresh(verifyContent: true);
            AssetRecord sceneAsset = database.Snapshot().Single(item =>
                item.RelativePath == "main.acs3d");
            sceneAsset = database.UpdateImportMetadata(
                sceneAsset.AssetId,
                sceneAsset.Metadata.Source,
                "legacy-acs3d",
                2,
                [],
                new System.Collections.Generic.Dictionary<string, string>
                {
                    ["scene.subsystems"] = "renderer3d",
                });

            var request = new AssetPackageReadinessRequest(
                "ReadinessFixture",
                root,
                assets,
                sceneAsset.AssetId);
            AssetPackageReadinessReport ready =
                AssetPackageReadiness.Analyze(request);
            Check(
                ready.Ready &&
                ready.RequiredAssetCount == 1 &&
                ready.ErrorCount == 0 &&
                ready.RootAssetPath == "main.acs3d" &&
                ready.GraphHash.Length == 64,
                "canonical supported Scene produces one deterministic package-ready closure");

            byte[] firstJson =
                AssetPackageReadiness.SerializeJson(ready);
            byte[] secondJson =
                AssetPackageReadiness.SerializeJson(ready);
            using (JsonDocument document = JsonDocument.Parse(firstJson))
            {
                Check(
                    firstJson.SequenceEqual(secondJson) &&
                    document.RootElement.GetProperty("schemaVersion")
                        .GetInt32() == 1 &&
                    document.RootElement.GetProperty("ready")
                        .GetBoolean() &&
                    document.RootElement.GetProperty("assets")
                        .GetArrayLength() == 1,
                    "Package Readiness JSON is deterministic, schema-versioned, and machine-readable");
            }

            string reportPath = Path.Combine(root, "readiness.json");
            AssetPackageReadiness.WriteNewJsonAsync(
                    reportPath,
                    ready)
                .GetAwaiter()
                .GetResult();
            bool overwriteRejected = Throws<IOException>(() =>
                AssetPackageReadiness.WriteNewJsonAsync(
                        reportPath,
                        ready)
                    .GetAwaiter()
                    .GetResult());
            Check(
                File.Exists(reportPath) &&
                overwriteRejected &&
                !Directory.EnumerateFiles(
                    root,
                    ".readiness.json.*.tmp",
                    SearchOption.TopDirectoryOnly).Any(),
                "JSON publication is atomic, refuses overwrite, and leaves no private temporary file");

            using (var cancelled = new CancellationTokenSource())
            {
                cancelled.Cancel();
                Check(
                    Throws<OperationCanceledException>(() =>
                        AssetPackageReadiness.AnalyzeAsync(
                                request,
                                cancelled.Token)
                            .GetAwaiter()
                            .GetResult()),
                    "Package Readiness honors pre-cancellation before filesystem traversal");
            }

            string unsupported = Path.Combine(
                assets,
                "required.unsupported-e2e");
            File.WriteAllText(
                unsupported,
                "unsupported",
                new UTF8Encoding(false));
            File.WriteAllText(
                scene,
                MinimalScene() +
                "MSH3D 1 Assets/required.unsupported-e2e\n",
                new UTF8Encoding(false));
            database.Refresh(verifyContent: true);
            sceneAsset = database.Snapshot().Single(item =>
                item.AssetId == sceneAsset.AssetId);
            AssetRecord unsupportedAsset = database.Snapshot().Single(item =>
                item.RelativePath == "required.unsupported-e2e");
            database.UpdateImportMetadata(
                sceneAsset.AssetId,
                sceneAsset.Metadata.Source,
                "legacy-acs3d",
                2,
                [unsupportedAsset.AssetId],
                sceneAsset.Metadata.ImportSettings);

            AssetPackageReadinessReport blocked =
                AssetPackageReadiness.Analyze(request);
            AssetPackageReadinessDiagnostic? unsupportedDiagnostic =
                blocked.Diagnostics.FirstOrDefault(item =>
                    item.Code == "ASSET_TYPE_UNSUPPORTED");
            Check(
                !blocked.Ready &&
                blocked.RequiredAssetCount == 2 &&
                blocked.ErrorCount > 0 &&
                unsupportedDiagnostic is
                {
                    AssetPath: "required.unsupported-e2e",
                    CanLocate: true,
                    Resolution.Length: > 0,
                },
                "unsupported reachable dependency blocks Package with a locatable repair action");

            File.Delete(
                unsupported + AssetDatabase.MetadataSuffix);
            AssetPackageReadinessReport missingMetadata =
                AssetPackageReadiness.Analyze(request);
            Check(
                !missingMetadata.Ready &&
                missingMetadata.Diagnostics.Any(item =>
                    item.Code == "ASSET_METADATA_MISSING") &&
                missingMetadata.Diagnostics
                    .Where(item =>
                        item.Severity ==
                        AssetCookDiagnosticSeverity.Error)
                    .All(item => item.Resolution.Length != 0),
                "missing authoritative metadata fails closed and every blocker has a repair route");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unexpected exception: " + error);
        }
        finally
        {
            TryDeleteFixture(root);
        }

        output.WriteLine(
            $"Asset Package Readiness self-test: {passed} passed, {failed} failed");
        return failed;
    }

    private static string MinimalScene() =>
        "ACS3D v2\n" +
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n";

    private static bool Throws<T>(Action action)
        where T : Exception
    {
        try
        {
            action();
            return false;
        }
        catch (T)
        {
            return true;
        }
    }

    private static void TryDeleteFixture(string root)
    {
        try
        {
            string full = Path.GetFullPath(root);
            string temp = Path.GetFullPath(Path.GetTempPath());
            if (!full.StartsWith(
                    temp,
                    StringComparison.OrdinalIgnoreCase) ||
                !Path.GetFileName(full).StartsWith(
                    "acs-package-readiness-selftest-",
                    StringComparison.Ordinal) ||
                !Directory.Exists(full) ||
                (File.GetAttributes(full) &
                 FileAttributes.ReparsePoint) != 0)
            {
                return;
            }
            Directory.Delete(full, recursive: true);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException)
        {
        }
    }
}
