using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace AcsEditor;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--self-test")
                return RunSelfTest();
            if (args.Length == 2 && args[0] == "--index")
            {
                string assets = Path.GetFullPath(args[1]);
                string project = Directory.GetParent(assets)?.FullName
                    ?? throw new InvalidDataException("Assets directory has no project parent.");
                var database = new AssetDatabase(project, assets);
                AssetDatabaseRefreshResult result = database.Refresh(verifyContent: true);
                Console.WriteLine(
                    $"Indexed {result.AssetCount} assets; metadata created " +
                    $"{result.CreatedMetadataCount}; identities recovered " +
                    $"{result.RecoveredIdentityCount}.");
                foreach (string warning in result.Warnings)
                    Console.Error.WriteLine("warning: " + warning);
                foreach (AssetRecord record in database.Snapshot())
                    Console.WriteLine($"{record.AssetId}  {record.Kind,-10}  {record.RelativePath}");
                return result.Warnings.Count == 0 ? 0 : 2;
            }

            Console.Error.WriteLine(
                "Usage:\n" +
                "  acsassetdb --self-test\n" +
                "  acsassetdb --index <project-Assets-directory>");
            return 64;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    private static int RunSelfTest()
    {
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-assetdb-selftest-" + Guid.NewGuid().ToString("N"));
        string assets = Path.Combine(root, "Assets");
        string outside = Path.Combine(root, "Outside");
        Directory.CreateDirectory(assets);
        Directory.CreateDirectory(outside);
        int passed = 0;

        try
        {
            string source = Path.Combine(assets, "Textures", "water.bin");
            Directory.CreateDirectory(Path.GetDirectoryName(source)!);
            File.WriteAllBytes(source, Encoding.UTF8.GetBytes("stable-water-source-v1"));

            var database = new AssetDatabase(root, assets);
            AssetDatabaseRefreshResult first = database.Refresh(verifyContent: true);
            Require(first.AssetCount == 1, "first refresh indexes one asset");
            Require(first.CreatedMetadataCount == 1, "first refresh creates metadata");
            AssetRecord original = database.Snapshot().Single();
            Require(
                Guid.TryParseExact(original.AssetId, "N", out Guid guid) && guid != Guid.Empty,
                "asset id is a stable non-zero GUID");
            Require(
                File.Exists(source + AssetDatabase.MetadataSuffix),
                "adjacent metadata exists");
            passed += 4;

            byte[] metadataBefore = File.ReadAllBytes(source + AssetDatabase.MetadataSuffix);
            byte[] indexBefore = File.ReadAllBytes(
                Path.Combine(assets, AssetDatabase.InternalDirectoryName, "index.v1.json"));
            AssetDatabaseRefreshResult second = database.Refresh(verifyContent: true);
            Require(second.CreatedMetadataCount == 0, "second refresh reuses metadata");
            Require(
                metadataBefore.SequenceEqual(
                    File.ReadAllBytes(source + AssetDatabase.MetadataSuffix)),
                "metadata serialization is deterministic");
            Require(
                indexBefore.SequenceEqual(File.ReadAllBytes(
                    Path.Combine(assets, AssetDatabase.InternalDirectoryName, "index.v1.json"))),
                "index serialization is deterministic");
            Require(
                !Directory.EnumerateFiles(
                    assets,
                    "*.tmp-*",
                    SearchOption.AllDirectories).Any(),
                "atomic writes leave no temporary files");
            var reopenedDatabase = new AssetDatabase(root, assets);
            reopenedDatabase.Refresh(verifyContent: true);
            Require(
                reopenedDatabase.Snapshot().Single().AssetId == original.AssetId,
                "identity persists across database restart");
            passed += 5;

            database.UpdateImportMetadata(
                original.AssetId,
                "Sources/water.psd",
                "texture",
                7,
                dependencies: new[]
                {
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                },
                importSettings: new Dictionary<string, string>
                {
                    ["srgb"] = "true",
                    ["compression"] = "bc7",
                });
            AssetRecord imported = database.Snapshot().Single();
            Require(
                imported.Metadata.Dependencies.SequenceEqual(new[]
                {
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                }),
                "dependencies are sorted and deduplicated");
            Require(
                imported.Metadata.Source == "Sources/water.psd" &&
                imported.Metadata.Importer == "texture" &&
                imported.Metadata.ImporterVersion == 7,
                "source/importer fields round-trip");
            Require(
                database.Query("water", "file", "Textures").Single().AssetId ==
                original.AssetId,
                "query API filters path, kind, and folder");
            passed += 3;

            string movedByApi = Path.Combine(assets, "Water", "surface.bin");
            AssetRecord moved = database.MoveAsset(original.AssetId, "Water/surface.bin");
            Require(moved.RelativePath == "Water/surface.bin", "MoveAsset updates path");
            Require(moved.AssetId == original.AssetId, "MoveAsset preserves identity");
            Require(
                File.Exists(movedByApi + AssetDatabase.MetadataSuffix) &&
                !File.Exists(source + AssetDatabase.MetadataSuffix),
                "MoveAsset moves its sidecar");
            passed += 3;

            string externallyRenamed = Path.Combine(assets, "Water", "renamed.bin");
            File.Move(movedByApi, externallyRenamed);
            AssetDatabaseRefreshResult renameRefresh = database.Refresh(verifyContent: true);
            AssetRecord recovered = database.Snapshot().Single();
            Require(
                renameRefresh.RecoveredIdentityCount == 1,
                "external asset-only rename is detected");
            Require(
                recovered.AssetId == original.AssetId,
                "external rename recovers the stable identity");
            Require(
                File.Exists(externallyRenamed + AssetDatabase.MetadataSuffix) &&
                !File.Exists(movedByApi + AssetDatabase.MetadataSuffix),
                "recovery relocates the orphan sidecar");
            passed += 3;

            ExpectThrows<InvalidDataException>(
                () => database.MoveAsset(original.AssetId, "../escape.bin"),
                "traversal destination is rejected");
            Require(
                !File.Exists(Path.Combine(root, "escape.bin")),
                "traversal rejection writes nothing outside Assets");
            passed += 2;

            string outsideAsset = Path.Combine(outside, "secret.bin");
            File.WriteAllText(outsideAsset, "must-not-index", Encoding.UTF8);
            string link = Path.Combine(assets, "LinkedOutside");
            bool linkCreated = TryCreateDirectoryLink(link, outside);
            if (linkCreated)
            {
                AssetDatabaseRefreshResult linkedRefresh = database.Refresh(verifyContent: true);
                Require(
                    linkedRefresh.Warnings.Any(warning =>
                        warning.Contains("Reparse point skipped", StringComparison.Ordinal)),
                    "reparse traversal is reported");
                Require(
                    database.Snapshot().All(record =>
                        !record.RelativePath.Contains("secret", StringComparison.OrdinalIgnoreCase)),
                    "reparse target outside Assets is not indexed");
                passed += 2;
                Directory.Delete(link);
            }
            else
            {
                Console.WriteLine(
                    "SKIP: OS policy did not permit creation of a directory symlink/junction.");
            }

            string graphDir = Path.Combine(assets, "Graph");
            Directory.CreateDirectory(graphDir);
            File.WriteAllText(Path.Combine(graphDir, "dependency-a.bin"), "dep-a", Encoding.UTF8);
            File.WriteAllText(Path.Combine(graphDir, "dependency-b.bin"), "dep-b", Encoding.UTF8);
            File.WriteAllText(Path.Combine(graphDir, "referencer-a.bin"), "ref-a", Encoding.UTF8);
            File.WriteAllText(Path.Combine(graphDir, "referencer-b.bin"), "ref-b", Encoding.UTF8);
            database.Refresh(verifyContent: true);
            AssetRecord dependencyA = database.Snapshot().Single(record =>
                record.RelativePath == "Graph/dependency-a.bin");
            AssetRecord dependencyB = database.Snapshot().Single(record =>
                record.RelativePath == "Graph/dependency-b.bin");
            AssetRecord referencerA = database.Snapshot().Single(record =>
                record.RelativePath == "Graph/referencer-a.bin");
            AssetRecord referencerB = database.Snapshot().Single(record =>
                record.RelativePath == "Graph/referencer-b.bin");
            string missingId = "dddddddddddddddddddddddddddddddd";

            static void SetDependencies(
                AssetDatabase db,
                AssetRecord asset,
                params string[] dependencies)
            {
                db.UpdateImportMetadata(
                    asset.AssetId,
                    asset.Metadata.Source,
                    asset.Metadata.Importer,
                    asset.Metadata.ImporterVersion,
                    dependencies,
                    asset.Metadata.ImportSettings);
            }

            SetDependencies(database, recovered, dependencyA.AssetId, missingId);
            SetDependencies(database, dependencyA, dependencyB.AssetId);
            SetDependencies(database, dependencyB, dependencyA.AssetId);
            SetDependencies(database, referencerA, recovered.AssetId);
            SetDependencies(database, referencerB, referencerA.AssetId);

            AssetReferenceAnalysis graph = database.AnalyzeReferences(
                recovered.AssetId,
                maxDepth: 8);
            Require(
                graph.DirectDependencies.Count == 2 &&
                graph.DirectDependencies.Any(node => node.AssetId == dependencyA.AssetId) &&
                graph.DirectDependencies.Any(node => node.AssetId == missingId && node.IsMissing),
                "direct dependency query retains missing IDs");
            Require(
                graph.TransitiveDependencies.Count == 3 &&
                graph.TransitiveDependencies.Single(node =>
                    node.AssetId == dependencyB.AssetId).Depth == 2,
                "transitive dependency closure reports deterministic depth");
            Require(
                graph.DirectReferencers.Count == 1 &&
                graph.DirectReferencers[0].AssetId == referencerA.AssetId &&
                graph.TransitiveReferencers.Single(node =>
                    node.AssetId == referencerB.AssetId).Depth == 2,
                "direct and transitive referencer queries are complete");
            Require(
                graph.MissingAssetIds.SequenceEqual(new[] { missingId }),
                "missing dependency diagnostics are deterministic");
            Require(
                database.GetDirectReferencers(missingId).Single().AssetId == recovered.AssetId,
                "referencers can be queried for a missing target");
            Require(
                graph.Cycles.Count == 1 &&
                graph.Cycles[0].AssetIds[0] == graph.Cycles[0].AssetIds[^1] &&
                graph.Cycles[0].AssetIds.Take(graph.Cycles[0].AssetIds.Count - 1)
                    .OrderBy(static id => id, StringComparer.Ordinal)
                    .SequenceEqual(
                        new[] { dependencyA.AssetId, dependencyB.AssetId }
                            .OrderBy(static id => id, StringComparer.Ordinal)),
                "dependency cycle is canonical and complete");
            string graphOrder = string.Join(
                "|",
                graph.TransitiveDependencies.Select(node =>
                    $"{node.Depth}:{node.RelativePath}:{node.AssetId}"));
            string graphOrderAgain = string.Join(
                "|",
                database.AnalyzeReferences(recovered.AssetId, 8)
                    .TransitiveDependencies.Select(node =>
                        $"{node.Depth}:{node.RelativePath}:{node.AssetId}"));
            Require(graphOrder == graphOrderAgain, "reference query order is deterministic");
            passed += 7;

            string invalidAsset = Path.Combine(assets, "invalid.bin");
            File.WriteAllText(invalidAsset, "invalid-meta-source", Encoding.UTF8);
            string invalidMetadata = invalidAsset + AssetDatabase.MetadataSuffix;
            byte[] invalidBytes = Encoding.UTF8.GetBytes("{\"schemaVersion\":1,\"id\":\"bad\"}\n");
            File.WriteAllBytes(invalidMetadata, invalidBytes);
            AssetDatabaseRefreshResult invalidRefresh = database.Refresh(verifyContent: true);
            Require(
                invalidRefresh.Warnings.Any(warning =>
                    warning.Contains("Metadata rejected", StringComparison.Ordinal)),
                "malformed metadata is reported");
            Require(
                File.ReadAllBytes(invalidMetadata).SequenceEqual(invalidBytes),
                "malformed metadata is never overwritten");
            Require(
                !database.Snapshot().Any(record =>
                    record.RelativePath.Equals("invalid.bin", StringComparison.OrdinalIgnoreCase)),
                "asset with malformed identity is fail-closed");
            passed += 3;

            File.WriteAllText(
                invalidMetadata,
                "{",
                new UTF8Encoding(false));
            AssetDatabaseRefreshResult malformedJsonRefresh =
                database.Refresh(verifyContent: true);
            Require(
                malformedJsonRefresh.Warnings.Any(warning =>
                    warning.Contains("Metadata rejected", StringComparison.Ordinal)),
                "syntactically malformed metadata is reported instead of escaping validation");
            passed++;

            passed += RunCookPlannerSelfTests(root);
            passed += RunDerivedDataCacheSelfTests(root);

            Console.WriteLine(
                $"SELF-TEST PASS: {passed} checks; stable GUID, deterministic metadata/index, " +
                "atomic writes, dependency-closure Cook planning, DDC invalidation/corruption " +
                "recovery, move recovery, query/import fields, traversal and reparse guards.");
            return 0;
        }
        finally
        {
            TryRemoveTree(root);
        }
    }

    private static int RunCookPlannerSelfTests(string parentRoot)
    {
        string root = Path.Combine(parentRoot, "CookPlannerProject");
        string assets = Path.Combine(root, "Assets");
        string scenePath = Path.Combine(assets, "Scenes", "main.acscene");
        string materialPath = Path.Combine(assets, "Materials", "water.acsmat");
        string albedoPath = Path.Combine(assets, "Textures", "albedo.png");
        string normalPath = Path.Combine(assets, "Textures", "normal.png");
        string unusedPath = Path.Combine(assets, "Unused", "not-cooked.bin");
        Directory.CreateDirectory(Path.GetDirectoryName(scenePath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(materialPath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(albedoPath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(unusedPath)!);

        const string sceneText =
            "ACSCENE v1\n2\n" +
            "SPRT 1 Assets/Textures/albedo.png\n" +
            "MAT 1 Assets/Materials/water.acsmat\n";
        const string materialText =
            "ACSMAT 1\n" +
            "albedo Assets/Textures/albedo.png\n" +
            "normal Assets/Textures/normal.png\n";
        File.WriteAllText(scenePath, sceneText, Encoding.UTF8);
        File.WriteAllText(materialPath, materialText, Encoding.UTF8);
        File.WriteAllBytes(albedoPath, [1, 2, 3, 4]);
        File.WriteAllBytes(normalPath, [5, 6, 7, 8]);
        File.WriteAllText(unusedPath, "unreachable", Encoding.UTF8);

        var database = new AssetDatabase(root, assets);
        database.Refresh(verifyContent: true);
        AssetRecord scene = ByPath(database, "Scenes/main.acscene");
        AssetRecord material = ByPath(database, "Materials/water.acsmat");
        AssetRecord albedo = ByPath(database, "Textures/albedo.png");
        AssetRecord normal = ByPath(database, "Textures/normal.png");
        AssetRecord unused = ByPath(database, "Unused/not-cooked.bin");
        Require(
            scene.Metadata.Importer == "legacy-acscene" &&
            scene.Metadata.ImporterVersion == 1 &&
            scene.Kind == "scene",
            "new legacy 2D scene metadata records importer provenance separately from scene kind");
        int passed = 1;
        UpdateDependencies(
            database,
            scene,
            [albedo.AssetId, material.AssetId],
            importer: "legacy-acscene",
            importerVersion: 1,
            importSettings: new Dictionary<string, string>
            {
                ["scene.subsystems"] = "renderer2d,physics2d",
            });
        UpdateDependencies(database, material, [albedo.AssetId, normal.AssetId]);

        byte[] indexBefore = File.ReadAllBytes(
            Path.Combine(assets, AssetDatabase.InternalDirectoryName, "index.v1.json"));
        var planner = new AssetCookPlanner(root, assets);
        AssetCookPlan first = planner.BuildByAssetId(scene.AssetId);
        Require(!first.HasErrors, "canonical Asset ID produces a valid Cook closure");
        Require(
            first.Root?.AssetId == scene.AssetId &&
            first.Assets.Select(static item => item.AssetId).ToHashSet(
                    StringComparer.OrdinalIgnoreCase)
                .SetEquals([scene.AssetId, material.AssetId, albedo.AssetId, normal.AssetId]),
            "Cook closure includes exactly reachable assets");
        Require(
            first.Assets.All(item => item.AssetId != unused.AssetId),
            "unreachable allowlisted assets are excluded from Cook");
        AssetCookPlan repeated = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            first.GraphHash == repeated.GraphHash &&
            first.Assets.Select(static item => item.RelativePath)
                .SequenceEqual(repeated.Assets.Select(static item => item.RelativePath)),
            "Cook graph hash and order are deterministic");
        Require(
            File.ReadAllBytes(Path.Combine(
                    assets,
                    AssetDatabase.InternalDirectoryName,
                    "index.v1.json"))
                .SequenceEqual(indexBefore),
            "strict Cook refresh does not rewrite the persistent index");
        AssetCookPlan legacyAdapter = new AssetCookPlanner(root, assets).Build(scenePath);
        Require(
            legacyAdapter.Root?.AssetId == scene.AssetId &&
            legacyAdapter.GraphHash == first.GraphHash,
            "legacy scene path adapter resolves once to the canonical Asset ID");
        passed += 6;

        File.WriteAllText(
            scenePath,
            sceneText + "SPRT 2 Assets/Textures/normal.png\n",
            Encoding.UTF8);
        AssetCookPlan stale = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            stale.Diagnostics.Any(static item => item.Code == "ASSET_METADATA_STALE"),
            "source/metadata dependency drift fails closed");
        File.WriteAllText(scenePath, sceneText, Encoding.UTF8);
        database.Refresh(verifyContent: true);
        scene = ByPath(database, "Scenes/main.acscene");
        UpdateDependencies(
            database,
            scene,
            [albedo.AssetId, material.AssetId],
            importer: "legacy-acscene",
            importerVersion: 1,
            importSettings: new Dictionary<string, string>
            {
                ["scene.subsystems"] = "renderer2d,physics2d",
            });
        passed++;

        string missingId = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
        UpdateDependencies(
            database,
            scene,
            [albedo.AssetId, material.AssetId, missingId],
            importer: "legacy-acscene",
            importerVersion: 1);
        AssetCookPlan missing = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            missing.Diagnostics.Any(item =>
                item.Code == "ASSET_DEPENDENCY_MISSING" &&
                item.AssetId == missingId),
            "missing dependency GUID fails closed");
        UpdateDependencies(
            database,
            scene,
            [albedo.AssetId, material.AssetId],
            importer: "legacy-acscene",
            importerVersion: 1,
            importSettings: new Dictionary<string, string>
            {
                ["scene.subsystems"] = "renderer2d,physics2d",
            });
        passed++;

        string projectTextureDirectory = Path.Combine(root, "Textures");
        Directory.CreateDirectory(projectTextureDirectory);
        File.WriteAllBytes(Path.Combine(projectTextureDirectory, "albedo.png"), [9, 9, 9]);
        File.WriteAllText(
            scenePath,
            sceneText.Replace(
                "Assets/Textures/albedo.png",
                "Textures/albedo.png",
                StringComparison.Ordinal),
            Encoding.UTF8);
        AssetCookPlan ambiguousReference = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            ambiguousReference.Diagnostics.Any(static item =>
                item.Code == "ASSET_REFERENCE_AMBIGUOUS"),
            "ambiguous path reference fails closed");
        File.WriteAllText(scenePath, sceneText, Encoding.UTF8);
        Directory.Delete(projectTextureDirectory, recursive: true);
        passed++;

        File.WriteAllText(
            scenePath,
            sceneText.Replace(
                "Assets/Textures/albedo.png",
                "../outside.png",
                StringComparison.Ordinal),
            Encoding.UTF8);
        AssetCookPlan traversalReference = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            traversalReference.Diagnostics.Any(static item =>
                item.Code == "ASSET_REFERENCE_ESCAPE"),
            "dependency reference traversal emits an explicit fail-closed diagnostic");
        File.WriteAllText(scenePath, sceneText, Encoding.UTF8);
        passed++;

        const string cyclicMaterialText =
            "ACSMAT 1\n" +
            "albedo Assets/Textures/albedo.png\n" +
            "normal Assets/Scenes/main.acscene\n";
        File.WriteAllText(materialPath, cyclicMaterialText, Encoding.UTF8);
        database.Refresh(verifyContent: true);
        material = ByPath(database, "Materials/water.acsmat");
        UpdateDependencies(database, material, [albedo.AssetId, scene.AssetId]);
        AssetCookPlan cyclic = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            cyclic.Diagnostics.Any(static item => item.Code == "ASSET_DEPENDENCY_CYCLE"),
            "reachable dependency cycle fails closed");
        File.WriteAllText(materialPath, materialText, Encoding.UTF8);
        database.Refresh(verifyContent: true);
        material = ByPath(database, "Materials/water.acsmat");
        UpdateDependencies(database, material, [albedo.AssetId, normal.AssetId]);
        passed++;

        string duplicatePath = Path.Combine(assets, "ZDuplicate.bin");
        File.WriteAllText(duplicatePath, "duplicate", Encoding.UTF8);
        database.Refresh(verifyContent: true);
        AssetRecord duplicate = ByPath(database, "ZDuplicate.bin");
        string duplicateMetadata = duplicatePath + AssetDatabase.MetadataSuffix;
        string metadataText = File.ReadAllText(duplicateMetadata, Encoding.UTF8);
        File.WriteAllText(
            duplicateMetadata,
            metadataText.Replace(
                duplicate.AssetId,
                scene.AssetId,
                StringComparison.Ordinal),
            new UTF8Encoding(false));
        AssetCookPlan ambiguousId = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            ambiguousId.Diagnostics.Any(static item => item.Code == "ASSET_ID_AMBIGUOUS"),
            "duplicate Asset ID fails closed as ambiguous metadata");
        File.Delete(duplicatePath);
        File.Delete(duplicateMetadata);
        passed++;

        string missingMetadataPath = Path.Combine(assets, "missing-sidecar.bin");
        File.WriteAllText(missingMetadataPath, "no-sidecar", Encoding.UTF8);
        AssetCookPlan missingMetadata = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene.AssetId);
        Require(
            missingMetadata.Diagnostics.Any(static item =>
                item.Code == "ASSET_METADATA_MISSING"),
            "missing authoritative sidecar fails closed");
        File.Delete(missingMetadataPath);
        passed++;

        string scene3DPath = Path.Combine(assets, "Scenes", "world.acs3d");
        string meshPath = Path.Combine(assets, "Models", "cube.glb");
        Directory.CreateDirectory(Path.GetDirectoryName(meshPath)!);
        File.WriteAllText(
            scene3DPath,
            "ACS3D v2\n" +
            "N3D 1 -1 3 0 0 0 0 0 0 1 1 1 1 1 1 1 Cube\n" +
            "MSH3D 1 Assets/Models/cube.glb\n" +
            "MAT3D 1 Assets/Materials/water.acsmat\n" +
            "MAT3D 2 0.250 0.750\n",
            Encoding.UTF8);
        File.WriteAllBytes(meshPath, [0x67, 0x6c, 0x54, 0x46]);
        database.Refresh(verifyContent: true);
        AssetRecord scene3D = ByPath(database, "Scenes/world.acs3d");
        AssetRecord mesh = ByPath(database, "Models/cube.glb");
        Require(
            scene3D.Metadata.Importer == "legacy-acs3d" &&
            scene3D.Metadata.ImporterVersion == 2 &&
            scene3D.Kind == "scene",
            "new legacy 3D scene metadata records importer provenance separately from scene kind");
        passed++;
        UpdateDependencies(
            database,
            scene3D,
            [mesh.AssetId, material.AssetId],
            importer: "legacy-acs3d",
            importerVersion: 2,
            importSettings: new Dictionary<string, string>
            {
                ["scene.subsystems"] = "renderer3d,mesh",
            });
        AssetCookPlan plan3D = new AssetCookPlanner(root, assets)
            .BuildByAssetId(scene3D.AssetId);
        Require(
            !plan3D.HasErrors &&
            plan3D.Assets.Any(item => item.AssetId == mesh.AssetId) &&
            plan3D.Assets.Any(item => item.AssetId == material.AssetId),
            "legacy ACS3D importer extracts mesh/material dependencies without projection coupling");
        passed++;

        string strictRoot = Path.Combine(parentRoot, "StrictReadOnlyProject");
        string strictAssets = Path.Combine(strictRoot, "Assets");
        Directory.CreateDirectory(strictAssets);
        string strictAsset = Path.Combine(strictAssets, "root.bin");
        File.WriteAllBytes(strictAsset, [4, 3, 2, 1]);
        string strictId = Guid.NewGuid().ToString("N");
        File.WriteAllText(
            strictAsset + AssetDatabase.MetadataSuffix,
            $$"""
            {
              "schemaVersion": 1,
              "id": "{{strictId}}",
              "kind": "file",
              "source": "root.bin",
              "importer": "passthrough",
              "importerVersion": 1,
              "dependencies": [],
              "importSettings": {}
            }
            """,
            new UTF8Encoding(false));
        var strictDatabase = new AssetDatabase(strictRoot, strictAssets);
        AssetDatabaseRefreshResult strictRefresh = strictDatabase.RefreshForCook();
        Require(
            strictRefresh.AssetCount == 1 &&
            !Directory.Exists(Path.Combine(
                strictAssets,
                AssetDatabase.InternalDirectoryName)),
            "strict Cook refresh is read-only and does not create acceleration storage");
        passed++;

        return passed;
    }

    private static int RunDerivedDataCacheSelfTests(string parentRoot)
    {
        string root = Path.Combine(parentRoot, "DerivedDataProject");
        string assets = Path.Combine(root, "Assets");
        Directory.CreateDirectory(assets);
        string source = Path.Combine(assets, "source.bin");
        File.WriteAllBytes(source, Encoding.UTF8.GetBytes("source-v1"));
        var database = new AssetDatabase(root, assets);
        database.Refresh(verifyContent: true);
        AssetRecord asset = database.Snapshot().Single();
        string cacheRoot = Path.Combine(root, "Temp", "DerivedDataCache");
        var cache = new DerivedDataCache(root, cacheRoot);
        var settings = new Dictionary<string, string>
        {
            ["platform"] = "win-x64",
            ["quality"] = "shipping",
        };
        int producerCalls = 0;

        DerivedDataCacheResult miss = cache.GetOrCreate(
            asset,
            "selftest-cooker-v1",
            settings,
            () =>
            {
                producerCalls++;
                return Encoding.UTF8.GetBytes("derived-v1");
            });
        Require(
            miss.Status == DerivedDataCacheStatus.Miss &&
            producerCalls == 1,
            "DDC miss produces and stores derived payload");
        DerivedDataCacheResult hit = cache.GetOrCreate(
            asset,
            "selftest-cooker-v1",
            settings.Reverse(),
            () =>
            {
                producerCalls++;
                return Encoding.UTF8.GetBytes("must-not-run");
            });
        Require(
            hit.Status == DerivedDataCacheStatus.Hit &&
            hit.Payload.SequenceEqual(miss.Payload) &&
            producerCalls == 1,
            "DDC hit is deterministic across settings enumeration order");
        int passed = 2;

        File.WriteAllBytes(source, Encoding.UTF8.GetBytes("source-v2"));
        database.Refresh(verifyContent: true);
        AssetRecord contentChanged = database.Snapshot().Single();
        DerivedDataCacheResult contentMiss = cache.GetOrCreate(
            contentChanged,
            "selftest-cooker-v1",
            settings,
            () => Encoding.UTF8.GetBytes("derived-v2"));
        Require(
            contentMiss.Status == DerivedDataCacheStatus.Miss &&
            contentMiss.Key != miss.Key,
            "source content hash invalidates DDC key");
        passed++;

        database.UpdateImportMetadata(
            contentChanged.AssetId,
            contentChanged.Metadata.Source,
            "binary-importer",
            2,
            contentChanged.Metadata.Dependencies,
            new Dictionary<string, string>
            {
                ["endianness"] = "little",
            });
        AssetRecord importerChanged = database.Snapshot().Single();
        string importerKey = DerivedDataCache.ComputeKey(
            importerChanged,
            "selftest-cooker-v1",
            settings);
        Require(
            importerKey != contentMiss.Key &&
            importerKey != DerivedDataCache.ComputeKey(
                importerChanged,
                "selftest-cooker-v2",
                settings) &&
            importerKey != DerivedDataCache.ComputeKey(
                importerChanged,
                "selftest-cooker-v1",
                new Dictionary<string, string>
                {
                    ["platform"] = "win-x64",
                    ["quality"] = "development",
                }),
            "importer, importer settings, cooker version, and cooker settings key the DDC");
        passed++;

        DerivedDataCacheResult importerMiss = cache.GetOrCreate(
            importerChanged,
            "selftest-cooker-v1",
            settings,
            () => Encoding.UTF8.GetBytes("derived-importer-v2"));
        string entry = Path.Combine(
            cache.CacheRoot,
            importerMiss.Key[..2],
            importerMiss.Key + ".ddc");
        File.WriteAllBytes(entry, Encoding.UTF8.GetBytes("corrupt"));
        DerivedDataCacheResult rebuilt = cache.GetOrCreate(
            importerChanged,
            "selftest-cooker-v1",
            settings,
            () => Encoding.UTF8.GetBytes("rebuilt"));
        Require(
            rebuilt.Status == DerivedDataCacheStatus.RebuiltCorruptEntry &&
            Encoding.UTF8.GetString(rebuilt.Payload) == "rebuilt",
            "corrupt DDC entry is detected and atomically rebuilt");
        Require(
            !Directory.EnumerateFiles(
                    cache.CacheRoot,
                    "*.tmp-*",
                    SearchOption.AllDirectories)
                .Any(),
            "DDC atomic writes leave no temporary files");
        passed += 2;

        ExpectThrows<InvalidDataException>(
            () => _ = new DerivedDataCache(
                root,
                Path.Combine(root, "..", "outside-ddc")),
            "DDC root traversal outside the project is rejected");
        passed++;

        string outside = Path.Combine(parentRoot, "DdcOutsideTarget");
        Directory.CreateDirectory(outside);
        string link = Path.Combine(root, "DdcLink");
        if (TryCreateDirectoryLink(link, outside))
        {
            ExpectThrows<InvalidDataException>(
                () => _ = new DerivedDataCache(root, link),
                "DDC reparse-point root is rejected");
            passed++;
            Directory.Delete(link);
        }
        else
        {
            Console.WriteLine(
                "SKIP: OS policy did not permit DDC reparse-point self-test.");
        }

        return passed;
    }

    private static AssetRecord ByPath(AssetDatabase database, string relativePath) =>
        database.Snapshot().Single(item =>
            string.Equals(
                item.RelativePath,
                relativePath,
                StringComparison.OrdinalIgnoreCase));

    private static void UpdateDependencies(
        AssetDatabase database,
        AssetRecord asset,
        IEnumerable<string> dependencies,
        string? importer = null,
        int? importerVersion = null,
        IReadOnlyDictionary<string, string>? importSettings = null)
    {
        database.UpdateImportMetadata(
            asset.AssetId,
            asset.Metadata.Source,
            importer ?? asset.Metadata.Importer,
            importerVersion ?? asset.Metadata.ImporterVersion,
            dependencies,
            importSettings ?? asset.Metadata.ImportSettings);
    }

    private static bool TryCreateDirectoryLink(string link, string target)
    {
        try
        {
            Directory.CreateSymbolicLink(link, target);
            return true;
        }
        catch (Exception ex) when (
            ex is UnauthorizedAccessException or IOException or PlatformNotSupportedException)
        {
            if (!OperatingSystem.IsWindows())
                return false;
            try
            {
                var start = new ProcessStartInfo
                {
                    FileName = "cmd.exe",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                };
                start.ArgumentList.Add("/d");
                start.ArgumentList.Add("/c");
                start.ArgumentList.Add("mklink");
                start.ArgumentList.Add("/J");
                start.ArgumentList.Add(link);
                start.ArgumentList.Add(target);
                using Process process = Process.Start(start)
                    ?? throw new IOException("Could not start mklink.");
                process.WaitForExit();
                return process.ExitCode == 0 && Directory.Exists(link);
            }
            catch
            {
                return false;
            }
        }
    }

    private static void TryRemoveTree(string root)
    {
        try
        {
            if (!Directory.Exists(root))
                return;
            foreach (string entry in Directory.EnumerateFileSystemEntries(
                root,
                "*",
                SearchOption.TopDirectoryOnly))
            {
                FileAttributes attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    if ((attributes & FileAttributes.Directory) != 0)
                        Directory.Delete(entry);
                    else
                        File.Delete(entry);
                }
            }
            Directory.Delete(root, recursive: true);
        }
        catch
        {
            // A self-test cleanup failure must not hide the test result.
        }
    }

    private static void Require(bool condition, string label)
    {
        if (!condition)
            throw new InvalidOperationException("SELF-TEST FAIL: " + label);
        Console.WriteLine("PASS: " + label);
    }

    private static void ExpectThrows<T>(Action action, string label)
        where T : Exception
    {
        try
        {
            action();
        }
        catch (T)
        {
            Console.WriteLine("PASS: " + label);
            return;
        }
        throw new InvalidOperationException("SELF-TEST FAIL: " + label);
    }
}
