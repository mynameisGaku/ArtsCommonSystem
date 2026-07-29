// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

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
                string processedKey =
                    record.Metadata.ImportSettings["processedArtifactKey"];
                string processedEntry =
                    AssetImportDerivedDataPipeline.GetEntryPath(
                        root,
                        processedKey);
                byte[] processedEntryBytes =
                    File.Exists(processedEntry)
                        ? File.ReadAllBytes(processedEntry)
                        : [];
                Check(
                    processedKey.Length == 64 &&
                    record.Metadata.ImportSettings[
                        "processedArtifactSchema"] == "1" &&
                    record.Metadata.ImportSettings[
                        "processedProcessor"] == "texture.worker" &&
                    record.Metadata.ImportSettings[
                        "processedProcessorVersion"] == "1" &&
                    record.Metadata.ImportSettings[
                        "processedFormat"] == "png" &&
                    processedEntryBytes.Length < 256 * 1024 &&
                    !ContainsBytes(
                        processedEntryBytes,
                        Encoding.UTF8.GetBytes(root)),
                    "import worker publishes a bounded content-addressed artifact before source publication");

                Check(
                    RejectsCancelledDerivedData(root, record),
                    "processed import observes cancellation before cache reuse or publication");

                AssetImportDerivedDataResult changedRecipe =
                    AssetImportDerivedDataPipeline.GetOrCreate(
                        root,
                        record.FullPath,
                        record.Kind,
                        Path.GetExtension(record.RelativePath),
                        record.Metadata.Importer,
                        record.Metadata.ImporterVersion,
                        textureChanged.Settings,
                        record.ContentHash,
                        record.SizeBytes);
                var versionThreeSettings =
                    new SortedDictionary<string, string>(
                        textureA.Settings
                            .Where(static pair =>
                                pair.Key != "recipeHash")
                            .ToDictionary(
                                static pair => pair.Key,
                                static pair => pair.Value,
                                StringComparer.Ordinal),
                        StringComparer.Ordinal);
                versionThreeSettings["recipeHash"] =
                    AssetImporterRecipeContract.ComputeRecipeHash(
                        textureA.Importer,
                        3,
                        versionThreeSettings);
                AssetImportDerivedDataResult changedVersion =
                    AssetImportDerivedDataPipeline.GetOrCreate(
                        root,
                        record.FullPath,
                        record.Kind,
                        Path.GetExtension(record.RelativePath),
                        record.Metadata.Importer,
                        3,
                        versionThreeSettings,
                        record.ContentHash,
                        record.SizeBytes);
                Check(
                    changedRecipe.Key != processedKey &&
                    changedVersion.Key != processedKey &&
                    changedVersion.Key != changedRecipe.Key,
                    "processed identity changes with recipe settings and importer version");

                var spoofedDynamicSettings =
                    new Dictionary<string, string>(
                        record.Metadata.ImportSettings,
                        StringComparer.Ordinal)
                    {
                        ["ProcessedArtifactKey"] =
                            new string('a', 64),
                        ["processedFutureTelemetry"] = root,
                    };
                AssetImportDerivedDataResult ignoredSpoof =
                    AssetImportDerivedDataPipeline.GetOrCreate(
                        root,
                        record.FullPath,
                        record.Kind,
                        Path.GetExtension(record.RelativePath),
                        record.Metadata.Importer,
                        record.Metadata.ImporterVersion,
                        spoofedDynamicSettings,
                        record.ContentHash,
                        record.SizeBytes);
                var pathBearingRecipe =
                    new SortedDictionary<string, string>(
                        textureA.Settings
                            .Where(static pair =>
                                pair.Key != "recipeHash")
                            .ToDictionary(
                                static pair => pair.Key,
                                static pair => pair.Value,
                                StringComparer.Ordinal),
                        StringComparer.Ordinal)
                    {
                        ["privateToolHint"] = root,
                    };
                pathBearingRecipe["recipeHash"] =
                    AssetImporterRecipeContract.ComputeRecipeHash(
                        textureA.Importer,
                        textureA.ImporterVersion,
                        pathBearingRecipe);
                AssetImportDerivedDataResult pathBearing =
                    AssetImportDerivedDataPipeline.GetOrCreate(
                        root,
                        record.FullPath,
                        record.Kind,
                        Path.GetExtension(record.RelativePath),
                        record.Metadata.Importer,
                        record.Metadata.ImporterVersion,
                        pathBearingRecipe,
                        record.ContentHash,
                        record.SizeBytes);
                Check(
                    ignoredSpoof.Key == processedKey &&
                    !ContainsBytes(
                        File.ReadAllBytes(processedEntry),
                        Encoding.UTF8.GetBytes(root)) &&
                    !ContainsBytes(
                        File.ReadAllBytes(
                            AssetImportDerivedDataPipeline.GetEntryPath(
                                root,
                                pathBearing.Key)),
                        Encoding.UTF8.GetBytes(root)),
                    "reserved metadata cannot spoof identity and recipe values do not leak project paths");

                Check(
                    RejectsDerivedDataBoundaryViolations(
                        root,
                        record),
                    "processed import bounds source identity, cache keys, and recipe allocation");

                Check(
                    ConcurrentPublishSucceeds(
                        root,
                        record,
                        processedEntry,
                        processedKey),
                    "concurrent publishers converge on one canonical atomic cache entry");

                AssetImportOperationResult sameSourceImport =
                    AssetImportWorkflow.ImportFiles(
                        database,
                        assets,
                        [source],
                        importerSettings: normalized);
                string sameSourceDestination =
                    sameSourceImport.Imported.Single().DestinationPath;
                bool foundSameSource = database.TryGetByPath(
                    sameSourceDestination,
                    out AssetRecord? sameSourceRecord);
                Check(
                    foundSameSource &&
                    sameSourceRecord?.Metadata.ImportSettings[
                        "processedArtifactKey"] == processedKey,
                    "processed import identity is destination-independent");

                File.WriteAllBytes(
                    processedEntry,
                    Encoding.ASCII.GetBytes("corrupt import ddc"));
                AssetImportOperationResult rebuiltImport =
                    AssetImportWorkflow.ImportFiles(
                        database,
                        assets,
                        [source],
                        importerSettings: normalized);
                bool foundRebuilt = database.TryGetByPath(
                    rebuiltImport.Imported.Single().DestinationPath,
                    out AssetRecord? rebuiltRecord);
                Check(
                    foundRebuilt &&
                    rebuiltRecord?.Metadata.ImportSettings[
                        "processedArtifactKey"] == processedKey &&
                    new FileInfo(processedEntry).Length >
                        Encoding.ASCII.GetByteCount("corrupt import ddc"),
                    "corrupt processed import entries rebuild deterministically");

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

                record = database.UpdateImportMetadata(
                    record.AssetId,
                    record.Metadata.Source,
                    record.Metadata.Importer,
                    record.Metadata.ImporterVersion,
                    record.Metadata.Dependencies,
                    spoofedDynamicSettings);
                File.WriteAllBytes(
                    source,
                    [0x89, 0x50, 0x4e, 0x47, 0x01, 0x02, 0x03, 0x04]);
                AssetReimportOperationResult reimported =
                    AssetReimportWorkflow.Reimport(
                        database,
                        record.AssetId);
                string reimportedProcessedKey =
                    reimported.Asset.Metadata.ImportSettings[
                        "processedArtifactKey"];
                Check(
                    reimported.Asset.Metadata.Importer == "texture" &&
                    reimported.Asset.Metadata.ImporterVersion == 2 &&
                    reimported.Asset.Metadata.ImportSettings["recipeHash"] ==
                        textureA.RecipeHash &&
                    reimported.Asset.Metadata.ImportSettings["colorSpace"] ==
                        "linear" &&
                    reimported.Asset.Metadata.ImportSettings["generateMipmaps"] ==
                        "false" &&
                    !reimported.Asset.Metadata.ImportSettings.ContainsKey(
                        "ProcessedArtifactKey") &&
                    !reimported.Asset.Metadata.ImportSettings.ContainsKey(
                        "processedFutureTelemetry") &&
                    reimportedProcessedKey != processedKey &&
                    File.Exists(
                        AssetImportDerivedDataPipeline.GetEntryPath(
                            root,
                            reimportedProcessedKey)),
                    "reimport preserves its recipe and advances processed identity with source content");

                var tamperedSettings = new Dictionary<string, string>(
                    reimported.Asset.Metadata.ImportSettings,
                    StringComparer.Ordinal)
                {
                    ["colorSpace"] = "srgb",
                };
                Check(
                    RejectsDerivedData(
                        root,
                        reimported.Asset,
                        tamperedSettings),
                    "processed import rejects recipe metadata tampering before cache publication");
            }
            else
            {
                Check(
                    false,
                    "import worker publishes a bounded content-addressed artifact before source publication");
                Check(
                    false,
                    "processed import observes cancellation before cache reuse or publication");
                Check(
                    false,
                    "processed identity changes with recipe settings and importer version");
                Check(
                    false,
                    "reserved metadata cannot spoof identity and recipe values do not leak project paths");
                Check(
                    false,
                    "processed import bounds source identity, cache keys, and recipe allocation");
                Check(
                    false,
                    "concurrent publishers converge on one canonical atomic cache entry");
                Check(
                    false,
                    "processed import identity is destination-independent");
                Check(
                    false,
                    "corrupt processed import entries rebuild deterministically");
                Check(
                    false,
                    "importer recipe settings invalidate derived-data identity");
                Check(
                    false,
                    "reimport preserves its recipe and advances processed identity with source content");
                Check(
                    false,
                    "processed import rejects recipe metadata tampering before cache publication");
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
                    mesh.RecipeHash &&
                recursiveTexture.Metadata.ImportSettings[
                    "processedProcessor"] == "texture.worker" &&
                recursiveMesh.Metadata.ImportSettings[
                    "processedProcessor"] == "mesh.worker" &&
                recursiveMesh.Metadata.ImportSettings[
                    "processedFormat"] == "obj",
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

    private static bool RejectsDerivedData(
        string projectRoot,
        AssetRecord asset,
        IReadOnlyDictionary<string, string> settings)
    {
        try
        {
            _ = AssetImportDerivedDataPipeline.GetOrCreate(
                projectRoot,
                asset.FullPath,
                asset.Kind,
                Path.GetExtension(asset.RelativePath),
                asset.Metadata.Importer,
                asset.Metadata.ImporterVersion,
                settings,
                asset.ContentHash,
                asset.SizeBytes);
            return false;
        }
        catch (InvalidDataException)
        {
            return true;
        }
    }

    private static bool RejectsCancelledDerivedData(
        string projectRoot,
        AssetRecord asset)
    {
        try
        {
            _ = AssetImportDerivedDataPipeline.GetOrCreate(
                projectRoot,
                asset.FullPath,
                asset.Kind,
                Path.GetExtension(asset.RelativePath),
                asset.Metadata.Importer,
                asset.Metadata.ImporterVersion,
                asset.Metadata.ImportSettings,
                asset.ContentHash,
                asset.SizeBytes,
                new System.Threading.CancellationToken(canceled: true));
            return false;
        }
        catch (OperationCanceledException)
        {
            return true;
        }
    }

    private static bool RejectsDerivedDataBoundaryViolations(
        string projectRoot,
        AssetRecord asset)
    {
        bool wrongLength = Throws<IOException>(() =>
            AssetImportDerivedDataPipeline.GetOrCreate(
                projectRoot,
                asset.FullPath,
                asset.Kind,
                Path.GetExtension(asset.RelativePath),
                asset.Metadata.Importer,
                asset.Metadata.ImporterVersion,
                asset.Metadata.ImportSettings,
                asset.ContentHash,
                checked(asset.SizeBytes + 1)));
        bool wrongHash = Throws<IOException>(() =>
            AssetImportDerivedDataPipeline.GetOrCreate(
                projectRoot,
                asset.FullPath,
                asset.Kind,
                Path.GetExtension(asset.RelativePath),
                asset.Metadata.Importer,
                asset.Metadata.ImporterVersion,
                asset.Metadata.ImportSettings,
                new string('0', 64),
                asset.SizeBytes));
        bool invalidKey = Throws<InvalidDataException>(() =>
            AssetImportDerivedDataPipeline.GetEntryPath(
                projectRoot,
                new string('g', 64)));
        bool excessiveSettings = Throws<InvalidDataException>(() =>
            AssetImportDerivedDataPipeline.GetOrCreate(
                projectRoot,
                asset.FullPath,
                asset.Kind,
                Path.GetExtension(asset.RelativePath),
                asset.Metadata.Importer,
                asset.Metadata.ImporterVersion,
                Enumerable.Range(0, 129).Select(index =>
                    KeyValuePair.Create(
                        "setting" + index.ToString(
                            CultureInfo.InvariantCulture),
                        "value")),
                asset.ContentHash,
                asset.SizeBytes));

        string outsideRoot =
            projectRoot + "-outside-" + Guid.NewGuid().ToString("N");
        bool escapedSource;
        try
        {
            Directory.CreateDirectory(outsideRoot);
            string escapedPath = Path.Combine(outsideRoot, "payload.bin");
            File.WriteAllBytes(escapedPath, [1, 2, 3, 4]);
            escapedSource = Throws<InvalidDataException>(() =>
                AssetImportDerivedDataPipeline.GetOrCreate(
                    projectRoot,
                    escapedPath,
                    asset.Kind,
                    Path.GetExtension(asset.RelativePath),
                    asset.Metadata.Importer,
                    asset.Metadata.ImporterVersion,
                    asset.Metadata.ImportSettings,
                    Convert.ToHexString(
                            SHA256.HashData([1, 2, 3, 4]))
                        .ToLowerInvariant(),
                    4));
        }
        finally
        {
            TryDeleteTempRoot(outsideRoot);
        }

        return wrongLength &&
               wrongHash &&
               invalidKey &&
               excessiveSettings &&
               escapedSource;
    }

    private static bool ConcurrentPublishSucceeds(
        string projectRoot,
        AssetRecord asset,
        string entryPath,
        string expectedKey)
    {
        try
        {
            File.Delete(entryPath);
            using var start = new ManualResetEventSlim(false);
            Task<AssetImportDerivedDataResult>[] publishers =
                Enumerable.Range(0, 8)
                    .Select(_ => Task.Run(() =>
                    {
                        start.Wait();
                        return AssetImportDerivedDataPipeline.GetOrCreate(
                            projectRoot,
                            asset.FullPath,
                            asset.Kind,
                            Path.GetExtension(asset.RelativePath),
                            asset.Metadata.Importer,
                            asset.Metadata.ImporterVersion,
                            asset.Metadata.ImportSettings,
                            asset.ContentHash,
                            asset.SizeBytes);
                    }))
                    .ToArray();
            start.Set();
            Task.WaitAll(publishers);

            AssetImportDerivedDataResult readBack =
                AssetImportDerivedDataPipeline.GetOrCreate(
                    projectRoot,
                    asset.FullPath,
                    asset.Kind,
                    Path.GetExtension(asset.RelativePath),
                    asset.Metadata.Importer,
                    asset.Metadata.ImporterVersion,
                    asset.Metadata.ImportSettings,
                    asset.ContentHash,
                    asset.SizeBytes);
            string parent = Path.GetDirectoryName(entryPath)!;
            return publishers.All(task =>
                       task.Result.Key == expectedKey) &&
                   readBack.Key == expectedKey &&
                   readBack.Status == AssetImportDerivedDataStatus.Hit &&
                   File.Exists(entryPath) &&
                   !Directory.EnumerateFiles(
                           parent,
                           Path.GetFileName(entryPath) + ".tmp-*",
                           SearchOption.TopDirectoryOnly)
                       .Any();
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(
                "Concurrent processed-import publication failed: " + error);
            return false;
        }
    }

    private static bool Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
            return false;
        }
        catch (TException)
        {
            return true;
        }
    }

    private static bool ContainsBytes(
        ReadOnlySpan<byte> haystack,
        ReadOnlySpan<byte> needle)
    {
        if (needle.Length == 0)
            return true;
        for (int index = 0;
             index <= haystack.Length - needle.Length;
             index++)
        {
            if (haystack.Slice(index, needle.Length).SequenceEqual(needle))
                return true;
        }
        return false;
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
