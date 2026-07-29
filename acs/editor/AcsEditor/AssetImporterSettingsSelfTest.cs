// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal static class AssetImporterSettingsSelfTest
{
    internal static int Run(TextWriter output)
    {
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string name)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS  " + name);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL  " + name);
            }
        }

        AssetImporterSettings custom =
            AssetImporterSettings.Default with
            {
                TextureColorSpace = "LINEAR",
                TextureCompression = "normal-map",
                TextureGenerateMipmaps = false,
                MeshUniformScale = 0.01,
                MeshGenerateCollision = true,
                AudioStreaming = true,
                AudioSampleRate = 48000,
            };
        AssetImporterSettings normalized = custom.Normalize();
        Check(
            normalized.TextureColorSpace == "linear" &&
            normalized.MeshUniformScale == 0.01 &&
            normalized.AudioSampleRate == 48000,
            "importer settings normalize finite, enumerated values");

        Check(
            Rejects(
                AssetImporterSettings.Default with
                {
                    TextureCompression = "arbitrary-shader",
                }) &&
            Rejects(
                AssetImporterSettings.Default with
                {
                    MeshUniformScale = double.NaN,
                }) &&
            Rejects(
                AssetImporterSettings.Default with
                {
                    AudioSampleRate = 12345,
                }),
            "invalid importer settings fail closed");

        AssetImporterRecipe textureA =
            AssetImporterRecipeContract.Create("image", normalized);
        AssetImporterRecipe textureB =
            AssetImporterRecipeContract.Create("image", normalized);
        AssetImporterRecipe textureChanged =
            AssetImporterRecipeContract.Create(
                "image",
                normalized with { TextureGenerateMipmaps = true });
        AssetImporterRecipe mesh =
            AssetImporterRecipeContract.Create("mesh", normalized);
        Check(
            textureA.Importer == textureB.Importer &&
            textureA.ImporterVersion == textureB.ImporterVersion &&
            textureA.RecipeHash == textureB.RecipeHash &&
            textureA.Settings.SequenceEqual(textureB.Settings) &&
            textureA.Importer == "texture" &&
            textureA.ImporterVersion == 2 &&
            textureA.RecipeHash.Length == 64 &&
            textureA.Settings["colorSpace"] == "linear" &&
            textureA.Settings["recipeHash"] == textureA.RecipeHash &&
            textureA.RecipeHash != textureChanged.RecipeHash &&
            textureA.RecipeHash != mesh.RecipeHash,
            "canonical recipes are deterministic and setting-sensitive");

        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-importer-settings-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            AssetImporterSettingsStore.Save(root, normalized);
            AssetImporterSettings loaded =
                AssetImporterSettingsStore.Load(root, out string? warning);
            string profilePath =
                AssetImporterSettingsStore.GetProfilePath(root);
            byte[] storedBytes = File.ReadAllBytes(profilePath);
            Check(
                warning == null &&
                loaded == normalized &&
                !storedBytes.AsSpan().StartsWith(
                    Encoding.UTF8.GetPreamble()) &&
                !Directory.EnumerateFiles(
                        Path.GetDirectoryName(profilePath)!,
                        "*.tmp-*",
                        SearchOption.TopDirectoryOnly)
                    .Any(),
                "project-local importer profile round-trips atomically without BOM");

            File.WriteAllText(
                profilePath,
                """
                {
                  "schemaVersion": 1,
                  "textureColorSpace": "auto",
                  "textureCompression": "default",
                  "textureGenerateMipmaps": true,
                  "textureDetectNormalMap": true,
                  "meshUniformScale": 1.0,
                  "meshImportTangents": true,
                  "meshGenerateCollision": false,
                  "audioStreaming": false,
                  "audioNormalize": false,
                  "audioSampleRate": 0,
                  "unknownFutureSetting": true
                }
                """,
                new UTF8Encoding(false));
            AssetImporterSettings fallback =
                AssetImporterSettingsStore.Load(root, out warning);
            Check(
                fallback == AssetImporterSettings.Default &&
                warning?.Contains(
                    "rejected",
                    StringComparison.OrdinalIgnoreCase) == true,
                "unknown profile fields are rejected and fall back safely");

            File.WriteAllText(
                profilePath,
                """
                {
                  "schemaVersion": 1,
                  "textureColorSpace": "auto",
                  "textureCompression": "default",
                  "textureGenerateMipmaps": true,
                  "textureGenerateMipmaps": false,
                  "textureDetectNormalMap": true,
                  "meshUniformScale": 1.0,
                  "meshImportTangents": true,
                  "meshGenerateCollision": false,
                  "audioStreaming": false,
                  "audioNormalize": false,
                  "audioSampleRate": 0
                }
                """,
                new UTF8Encoding(false));
            fallback =
                AssetImporterSettingsStore.Load(root, out warning);
            Check(
                fallback == AssetImporterSettings.Default &&
                warning?.Contains(
                    "duplicate",
                    StringComparison.OrdinalIgnoreCase) == true,
                "duplicate profile fields are rejected instead of taking the last value");

            string assets = Path.Combine(root, "Assets");
            string sourceDirectory = Path.Combine(root, "External");
            Directory.CreateDirectory(assets);
            Directory.CreateDirectory(sourceDirectory);
            string source = Path.Combine(sourceDirectory, "normal_detail.png");
            File.WriteAllBytes(
                source,
                [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
            var database = new AssetDatabase(root, assets);
            _ = database.Refresh();
            AssetImportOperationResult imported =
                AssetImportWorkflow.ImportFiles(
                    database,
                    assets,
                    [source],
                    importerSettings: normalized);
            string destination = imported.Imported.Single().DestinationPath;
            Check(
                database.TryGetByPath(destination, out AssetRecord? record) &&
                record != null &&
                record.Metadata.Importer == "texture" &&
                record.Metadata.ImporterVersion == 2 &&
                record.Metadata.ImportSettings["colorSpace"] == "linear" &&
                record.Metadata.ImportSettings["generateMipmaps"] == "false" &&
                record.Metadata.ImportSettings["recipeHash"] ==
                    textureA.RecipeHash,
                "transactional import publishes canonical recipe metadata");

            if (record != null)
            {
                string cacheA =
                    DerivedDataCache.ComputeKey(
                        record,
                        "import-recipe-contract-v1",
                        cookerSettings: null);
                var changedMetadata = record.Metadata with
                {
                    ImportSettings =
                        new Dictionary<string, string>(
                            record.Metadata.ImportSettings,
                            StringComparer.Ordinal)
                        {
                            ["colorSpace"] = "srgb",
                        },
                };
                string cacheB =
                    DerivedDataCache.ComputeKey(
                        record with { Metadata = changedMetadata },
                        "import-recipe-contract-v1",
                        cookerSettings: null);
                Check(
                    cacheA != cacheB,
                    "importer recipe settings invalidate derived-data identity");

                File.WriteAllBytes(
                    source,
                    [0x89, 0x50, 0x4e, 0x47, 0x01, 0x02, 0x03, 0x04]);
                AssetReimportOperationResult reimported =
                    AssetReimportWorkflow.Reimport(
                        database,
                        record.AssetId);
                Check(
                    reimported.Asset.Metadata.Importer == "texture" &&
                    reimported.Asset.Metadata.ImporterVersion == 2 &&
                    reimported.Asset.Metadata.ImportSettings["recipeHash"] ==
                        textureA.RecipeHash &&
                    reimported.Asset.Metadata.ImportSettings["colorSpace"] ==
                        "linear" &&
                    reimported.Asset.Metadata.ImportSettings["generateMipmaps"] ==
                        "false",
                    "reimport preserves the accepted canonical recipe");
            }
            else
            {
                Check(
                    false,
                    "importer recipe settings invalidate derived-data identity");
                Check(
                    false,
                    "reimport preserves the accepted canonical recipe");
            }

            string recursiveSource = Path.Combine(sourceDirectory, "Batch");
            Directory.CreateDirectory(
                Path.Combine(recursiveSource, "Nested"));
            File.WriteAllBytes(
                Path.Combine(recursiveSource, "albedo.png"),
                [0x89, 0x50, 0x4e, 0x47]);
            File.WriteAllText(
                Path.Combine(recursiveSource, "Nested", "shape.obj"),
                "v 0 0 0\n",
                new UTF8Encoding(false));
            AssetImportOperationResult recursive =
                AssetImportWorkflow.ImportExternalPaths(
                    database,
                    assets,
                    [recursiveSource],
                    importerSettings: normalized);
            AssetRecord[] recursiveRecords = recursive.Imported
                .Select(item =>
                    database.TryGetByPath(
                        item.DestinationPath,
                        out AssetRecord? candidate)
                        ? candidate
                        : null)
                .Where(static candidate => candidate != null)
                .Cast<AssetRecord>()
                .ToArray();
            AssetRecord? recursiveTexture = recursiveRecords.SingleOrDefault(
                static candidate => candidate.Kind == "image");
            AssetRecord? recursiveMesh = recursiveRecords.SingleOrDefault(
                static candidate => candidate.Kind == "mesh");
            Check(
                recursive.Imported.Count == 2 &&
                recursiveTexture?.Metadata.ImporterVersion == 2 &&
                recursiveTexture.Metadata.ImportSettings["compression"] ==
                    "normal-map" &&
                recursiveMesh?.Metadata.ImporterVersion == 2 &&
                recursiveMesh.Metadata.ImportSettings["uniformScale"] == "0.01" &&
                recursiveTexture.Metadata.ImportSettings["recipeHash"] ==
                    textureA.RecipeHash &&
                recursiveMesh.Metadata.ImportSettings["recipeHash"] ==
                    mesh.RecipeHash,
                "recursive import propagates one immutable recipe snapshot to every file");

            string? sourceRoot = FindManagedSourceRoot();
            if (sourceRoot == null)
            {
                Check(false, "import settings UI source root was located");
            }
            else
            {
                string xaml = File.ReadAllText(
                    Path.Combine(
                        sourceRoot,
                        "AssetImportSettingsWindow.xaml"));
                string codeBehind = File.ReadAllText(
                    Path.Combine(
                        sourceRoot,
                        "AssetImportSettingsWindow.xaml.cs"));
                string visualFixture = File.ReadAllText(
                    Path.Combine(
                        sourceRoot,
                        "AssetImportSettingsVisualFixture.cs"));
                string panel = File.ReadAllText(
                    Path.Combine(
                        sourceRoot,
                        "AssetBrowserPanel.xaml.cs"));
                int dialog = panel.IndexOf(
                    "new AssetImportSettingsWindow(",
                    StringComparison.Ordinal);
                int import = panel.IndexOf(
                    "await ImportAssetsAsync(",
                    dialog >= 0 ? dialog : 0,
                    StringComparison.Ordinal);
                Check(
                    xaml.Contains(
                        "Configure import recipes",
                        StringComparison.Ordinal) &&
                    xaml.Contains(
                        "AutomationProperties.Name=\"Texture color space\"",
                        StringComparison.Ordinal) &&
                    xaml.Contains(
                        "AutomationProperties.Name=\"Mesh uniform scale\"",
                        StringComparison.Ordinal) &&
                    xaml.Contains(
                        "AutomationProperties.Name=\"Audio sample rate\"",
                        StringComparison.Ordinal) &&
                    xaml.Contains(
                        "x:Name=\"ImportButton\"",
                        StringComparison.Ordinal) &&
                    codeBehind.Contains(
                        "ImportButton.IsEnabled = false",
                        StringComparison.Ordinal) &&
                    visualFixture.Contains(
                        "ValidateVisualFixtureInvalidState",
                        StringComparison.Ordinal) &&
                    dialog >= 0 &&
                    import > dialog,
                    "Asset View exposes validated importer settings before publication");
            }
        }
        finally
        {
            TryDeleteTempRoot(root);
        }

        output.WriteLine(
            $"Asset importer settings self-test: passed={passed} failed={failed}");
        return failed;
    }

    private static bool Rejects(AssetImporterSettings settings)
    {
        try
        {
            _ = settings.Normalize();
            return false;
        }
        catch (InvalidDataException)
        {
            return true;
        }
    }

    private static string? FindManagedSourceRoot()
    {
        foreach (string start in new[]
                 {
                     Directory.GetCurrentDirectory(),
                     AppContext.BaseDirectory,
                 })
        {
            var directory = new DirectoryInfo(Path.GetFullPath(start));
            while (directory != null)
            {
                string direct = Path.Combine(
                    directory.FullName,
                    "AssetImportSettingsWindow.xaml");
                if (File.Exists(direct))
                    return directory.FullName;
                string nested = Path.Combine(
                    directory.FullName,
                    "editor",
                    "AcsEditor");
                if (File.Exists(Path.Combine(
                        nested,
                        "AssetImportSettingsWindow.xaml")))
                {
                    return nested;
                }
                directory = directory.Parent;
            }
        }
        return null;
    }

    private static void TryDeleteTempRoot(string root)
    {
        try
        {
            string fullRoot =
                Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
            string tempRoot =
                Path.TrimEndingDirectorySeparator(Path.GetFullPath(Path.GetTempPath()));
            if (fullRoot.StartsWith(
                    tempRoot + Path.DirectorySeparatorChar,
                    StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(fullRoot) &&
                (File.GetAttributes(fullRoot) & FileAttributes.ReparsePoint) == 0)
            {
                Directory.Delete(fullRoot, recursive: true);
            }
        }
        catch
        {
            // A failed test cleanup is non-authoritative and isolated by GUID.
        }
    }
}
