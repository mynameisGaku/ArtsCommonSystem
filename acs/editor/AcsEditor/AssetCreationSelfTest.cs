// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using AcsEditor.Packaging;

namespace AcsEditor;

internal static class AssetCreationSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-creation-selftest-" + Guid.NewGuid().ToString("N"));
        string assets = Path.Combine(root, "Project", "Assets");
        string materials = Path.Combine(assets, "Materials");

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

        static bool FakeCanonicalMaterialWriter(string path, string name)
        {
            File.WriteAllText(
                path,
                "ACSMAT 1\nname " + name + "\nkind pbr\ncanonical-test 1\n",
                new UTF8Encoding(false));
            return true;
        }

        try
        {
            Directory.CreateDirectory(materials);

            Check(AssetCreationWorkflow.Definitions.Select(value => value.Template)
                    .SequenceEqual(new[]
                    {
                        AcsAssetTemplate.Folder,
                        AcsAssetTemplate.Material,
                        AcsAssetTemplate.Scene,
                        AcsAssetTemplate.Blueprint,
                        AcsAssetTemplate.Prefab,
                    }),
                "New menu exposes only currently usable Folder, Material, Scene, Blueprint and Prefab formats");

            AcsAssetCreationResult scene = AssetCreationWorkflow.Create(
                assets,
                materials,
                AcsAssetTemplate.Scene);
            byte[] sceneBytes = File.ReadAllBytes(scene.FullPath);
            const string canonicalScene = "ACS3D v2\n";
            Check(
                scene.FullPath == Path.Combine(materials, "Scene.acs3d") &&
                Encoding.UTF8.GetString(sceneBytes) == canonicalScene &&
                !CanonicalSceneAdapter
                    .InspectText(canonicalScene, ".acs3d")
                    .HasErrors &&
                (sceneBytes.Length < 3 ||
                 !(sceneBytes[0] == 0xEF &&
                   sceneBytes[1] == 0xBB &&
                   sceneBytes[2] == 0xBF)),
                "Scene creation writes a valid empty BOM-free unified ACS3D document");

            AcsAssetCreationResult blueprint = AssetCreationWorkflow.Create(
                assets,
                materials,
                AcsAssetTemplate.Blueprint);
            byte[] blueprintBytes = File.ReadAllBytes(blueprint.FullPath);
            Check(blueprint.FullPath == Path.Combine(materials, "Blueprint.acsbp") &&
                  Encoding.UTF8.GetString(blueprintBytes) == "ACSBP 1\n",
                "Blueprint creation writes the minimal canonical schema in the current folder");
            Check(blueprintBytes.Length < 3 ||
                  !(blueprintBytes[0] == 0xEF && blueprintBytes[1] == 0xBB && blueprintBytes[2] == 0xBF),
                "Blueprint creation uses UTF-8 without a BOM");

            AcsAssetCreationResult prefab = AssetCreationWorkflow.Create(
                assets,
                materials,
                AcsAssetTemplate.Prefab);
            byte[] prefabBytes = File.ReadAllBytes(prefab.FullPath);
            const string canonicalPrefab =
                "ACS3D v2\n" +
                "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 PrefabRoot\n" +
                "EMPTY3D 1\n";
            Check(
                prefab.FullPath == Path.Combine(materials, "Prefab.acsprefab") &&
                Encoding.UTF8.GetString(prefabBytes) == canonicalPrefab &&
                !CanonicalSceneAdapter
                    .InspectText(canonicalPrefab, ".acs3d")
                    .HasErrors &&
                (prefabBytes.Length < 3 ||
                 !(prefabBytes[0] == 0xEF &&
                   prefabBytes[1] == 0xBB &&
                   prefabBytes[2] == 0xBF)),
                "Prefab creation writes the exact BOM-free ACS3D subtree used by the unified scene model");

            AssetScenePlacementDecision matching3D =
                AssetScenePlacementPolicy.Evaluate(
                    activeSourceUses3D: true,
                    payloadUses3D: true);
            AssetScenePlacementDecision matchingLegacy2D =
                AssetScenePlacementPolicy.Evaluate(
                    activeSourceUses3D: false,
                    payloadUses3D: false);
            AssetScenePlacementDecision hidden3DMutation =
                AssetScenePlacementPolicy.Evaluate(
                    activeSourceUses3D: false,
                    payloadUses3D: true);
            AssetScenePlacementDecision hidden2DMutation =
                AssetScenePlacementPolicy.Evaluate(
                    activeSourceUses3D: true,
                    payloadUses3D: false);
            int mutationCalls = 0;
            if (hidden3DMutation == AssetScenePlacementDecision.Allow)
                mutationCalls++;
            if (hidden2DMutation == AssetScenePlacementDecision.Allow)
                mutationCalls++;
            string rejectionGuidance =
                AssetScenePlacementPolicy.RejectionMessage(
                    hidden3DMutation,
                    "Prefab",
                    "Water.acsprefab");
            Check(
                matching3D == AssetScenePlacementDecision.Allow &&
                matchingLegacy2D == AssetScenePlacementDecision.Allow &&
                hidden3DMutation ==
                    AssetScenePlacementDecision.RejectUnified3DIntoLegacy2D &&
                hidden2DMutation ==
                    AssetScenePlacementDecision.RejectLegacy2DIntoUnified3D &&
                mutationCalls == 0 &&
                rejectionGuidance.Contains(".acs3d", StringComparison.Ordinal) &&
                rejectionGuidance.Contains(
                    "シーングラフは変更されていません。",
                    StringComparison.Ordinal),
                "asset placement rejects dimension-mismatched payloads before any hidden graph mutation");

            File.WriteAllText(Path.Combine(materials, "Material.acsmat"), "existing");
            Directory.CreateDirectory(Path.Combine(materials, "Material1.acsmat"));
            string? writtenName = null;
            AcsAssetCreationResult material = AssetCreationWorkflow.Create(
                assets,
                materials,
                AcsAssetTemplate.Material,
                (path, name) =>
                {
                    writtenName = name;
                    return FakeCanonicalMaterialWriter(path, name);
                });
            Check(material.FullPath == Path.Combine(materials, "Material2.acsmat") &&
                  writtenName == "Material2" &&
                  File.ReadAllText(material.FullPath).StartsWith(
                      "ACSMAT 1\nname Material2\n",
                      StringComparison.Ordinal),
                "Material creation skips file and directory collisions and delegates schema writing");
            Check(File.ReadAllText(Path.Combine(materials, "Material.acsmat")) == "existing",
                "Material creation never overwrites an existing asset");

            string lockedMaterialPath = Path.Combine(
                assets,
                "LockedInspectorMaterial.acsmat");
            int lockedWriterCalls = 0;
            int lockedRefreshCalls = 0;
            bool externalMaterialLockRejected = false;
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                try
                {
                    _ = MaterialAssetWorkflow.CreateProjectMaterial(
                        assets,
                        (path, name) =>
                        {
                            lockedWriterCalls++;
                            return FakeCanonicalMaterialWriter(path, name);
                        },
                        _ => lockedRefreshCalls++,
                        "LockedInspectorMaterial");
                }
                catch (IOException)
                {
                    externalMaterialLockRejected = true;
                }
            }
            Check(
                externalMaterialLockRejected &&
                lockedWriterCalls == 0 &&
                lockedRefreshCalls == 0 &&
                !File.Exists(lockedMaterialPath) &&
                !File.Exists(lockedMaterialPath + AssetDatabase.MetadataSuffix),
                "external project lock rejects Inspector material creation before reservation");

            int inspectorRefreshCalls = 0;
            string inspectorMaterial = MaterialAssetWorkflow.CreateProjectMaterial(
                assets,
                FakeCanonicalMaterialWriter,
                createdPath =>
                {
                    inspectorRefreshCalls++;
                    var authoritative = new AssetDatabase(
                        Path.Combine(root, "Project"),
                        assets);
                    _ = authoritative.Refresh(verifyContent: true);
                    if (!authoritative.TryGetByPath(
                            createdPath,
                            out AssetRecord? indexed) ||
                        indexed?.Kind != "material")
                    {
                        throw new IOException(
                            "Inspector material was not indexed by the test database.");
                    }
                },
                "InspectorMaterial");
            Check(
                inspectorRefreshCalls == 1 &&
                File.Exists(inspectorMaterial) &&
                File.Exists(inspectorMaterial + AssetDatabase.MetadataSuffix),
                "Inspector material transaction serializes and indexes under one project lease");

            string rollbackMaterialPath = Path.Combine(assets, "RollbackMaterial.acsmat");
            bool rollbackMaterialRejected = false;
            try
            {
                _ = MaterialAssetWorkflow.CreateProjectMaterial(
                    assets,
                    (path, _) =>
                    {
                        File.WriteAllText(path, "partial-material");
                        File.WriteAllText(
                            path + AssetDatabase.MetadataSuffix,
                            "partial-material-meta");
                        File.WriteAllText(path + ".graph.json", "partial-graph");
                        File.WriteAllText(
                            path + ".graph.json" + AssetDatabase.MetadataSuffix,
                            "partial-graph-meta");
                        return false;
                    },
                    _ => throw new InvalidOperationException(
                        "Refresh must not run after serializer rejection."),
                    "RollbackMaterial");
            }
            catch (InvalidDataException)
            {
                rollbackMaterialRejected = true;
            }
            Check(
                rollbackMaterialRejected &&
                !File.Exists(rollbackMaterialPath) &&
                !File.Exists(rollbackMaterialPath + AssetDatabase.MetadataSuffix) &&
                !File.Exists(rollbackMaterialPath + ".graph.json") &&
                !File.Exists(
                    rollbackMaterialPath +
                    ".graph.json" +
                    AssetDatabase.MetadataSuffix),
                "Inspector material rollback removes source, graph, and both metadata sidecars");

            var database = new AssetDatabase(
                Path.Combine(root, "Project"),
                assets);
            AssetDatabaseRefreshResult indexResult = database.Refresh(verifyContent: true);
            bool indexedBlueprint = database.TryGetByPath(
                blueprint.FullPath,
                out AssetRecord? blueprintRecord);
            bool indexedMaterial = database.TryGetByPath(
                material.FullPath,
                out AssetRecord? materialRecord);
            bool indexedScene = database.TryGetByPath(
                scene.FullPath,
                out AssetRecord? sceneRecord);
            bool indexedPrefab = database.TryGetByPath(
                prefab.FullPath,
                out AssetRecord? prefabRecord);
            Check(indexResult.Warnings.Count == 0 &&
                  indexedScene && sceneRecord?.Kind == "scene" &&
                  sceneRecord.Metadata.Importer == "legacy-acs3d" &&
                  indexedBlueprint && blueprintRecord?.Kind == "blueprint" &&
                  blueprintRecord.Metadata.Importer == "blueprint" &&
                  indexedMaterial && materialRecord?.Kind == "material" &&
                  materialRecord.Metadata.Importer == "material" &&
                  indexedPrefab && prefabRecord?.Kind == "prefab" &&
                  prefabRecord.Metadata.Importer == "prefab",
                "created ACS assets receive authoritative kind-specific metadata immediately");
            Check(database.Snapshot().All(record =>
                    !AssetCreationWorkflow.IsTemporaryPath(record.FullPath)),
                "asset indexing never promotes a creation temporary into an asset");

            AcsAssetCreationResult folder = AssetCreationWorkflow.Create(
                assets,
                materials,
                AcsAssetTemplate.Folder);
            File.WriteAllText(Path.Combine(materials, "NewFolder1"), "collision");
            AcsAssetCreationResult secondFolder = AssetCreationWorkflow.Create(
                assets,
                materials,
                AcsAssetTemplate.Folder);
            Check(Directory.Exists(folder.FullPath) &&
                  folder.FullPath.EndsWith("NewFolder", StringComparison.Ordinal) &&
                  Directory.Exists(secondFolder.FullPath) &&
                  secondFolder.FullPath.EndsWith("NewFolder2", StringComparison.Ordinal),
                "Folder creation is collision-free across existing files and directories");

            Task<AcsAssetCreationResult>[] concurrent = Enumerable.Range(0, 12)
                .Select(_ => Task.Run(() => AssetCreationWorkflow.Create(
                    assets,
                    materials,
                    AcsAssetTemplate.Blueprint)))
                .ToArray();
            Task.WaitAll(concurrent);
            string[] concurrentPaths = concurrent.Select(task => task.Result.FullPath).ToArray();
            Check(concurrentPaths.Distinct(StringComparer.OrdinalIgnoreCase).Count() ==
                  concurrentPaths.Length && concurrentPaths.All(File.Exists),
                "parallel creation publishes unique assets without a check-then-write race");
            Check(concurrentPaths.All(path => File.ReadAllText(path) == "ACSBP 1\n"),
                "parallel creation never exposes partial Blueprint contents");

            string phase2 = Path.Combine(assets, "Phase2");
            Directory.CreateDirectory(phase2);
            AcsAssetCreationResult namedScene = AssetCreationWorkflow.CreateNamed(
                assets,
                phase2,
                AcsAssetTemplate.Scene,
                "Gameplay");
            Check(
                namedScene.FullPath == Path.Combine(phase2, "Gameplay.acs3d") &&
                File.ReadAllText(namedScene.FullPath) == "ACS3D v2\n",
                "explicit-name creation keeps the template-owned extension and canonical payload");

            File.WriteAllText(
                Path.Combine(phase2, "weather.ACS3D"),
                "case-insensitive collision");
            AcsAssetCreationResult caseCollision =
                AssetCreationWorkflow.CreateNamed(
                    assets,
                    phase2,
                    AcsAssetTemplate.Scene,
                    "Weather");
            Check(
                caseCollision.FullPath == Path.Combine(phase2, "Weather1.acs3d") &&
                File.ReadAllText(
                    Path.Combine(phase2, "weather.ACS3D")) ==
                "case-insensitive collision",
                "explicit-name collision suffixing is case-insensitive and preserves the occupant");

            string metadataReservation =
                Path.Combine(phase2, "MetadataOwned.acs3d") +
                AssetDatabase.MetadataSuffix;
            File.WriteAllText(metadataReservation, "orphan metadata reservation");
            AcsAssetCreationResult metadataCollision =
                AssetCreationWorkflow.CreateNamed(
                    assets,
                    phase2,
                    AcsAssetTemplate.Scene,
                    "MetadataOwned");
            Check(
                metadataCollision.FullPath ==
                Path.Combine(phase2, "MetadataOwned1.acs3d") &&
                File.ReadAllText(metadataReservation) ==
                "orphan metadata reservation",
                "payload creation treats a pre-existing metadata sidecar as an occupied asset family");

            string graphReservation =
                Path.Combine(phase2, "GraphOwned.acsmat.graph.json");
            File.WriteAllText(graphReservation, "orphan graph reservation");
            AcsAssetCreationResult graphCollision =
                AssetCreationWorkflow.CreateNamed(
                    assets,
                    phase2,
                    AcsAssetTemplate.Material,
                    "GraphOwned",
                    FakeCanonicalMaterialWriter);
            Check(
                graphCollision.FullPath ==
                Path.Combine(phase2, "GraphOwned1.acsmat") &&
                File.ReadAllText(graphReservation) ==
                "orphan graph reservation",
                "Material creation reserves graph and graph-metadata companion names");

            string graphMetadataReservation =
                Path.Combine(phase2, "GraphMetadataOwned.acsmat.graph.json") +
                AssetDatabase.MetadataSuffix;
            File.WriteAllText(
                graphMetadataReservation,
                "orphan graph metadata reservation");
            AcsAssetCreationResult graphMetadataCollision =
                AssetCreationWorkflow.CreateNamed(
                    assets,
                    phase2,
                    AcsAssetTemplate.Material,
                    "GraphMetadataOwned",
                    FakeCanonicalMaterialWriter);
            Check(
                graphMetadataCollision.FullPath ==
                Path.Combine(phase2, "GraphMetadataOwned1.acsmat") &&
                File.ReadAllText(graphMetadataReservation) ==
                "orphan graph metadata reservation",
                "Material creation treats graph metadata as a case-insensitive family reservation");

            string[] reservedNames =
            {
                "CON",
                "lpt9.txt",
                ".acsdb",
                "Asset.tmp-stage",
                "Asset.acsmeta",
                "bad/name",
                "trailing.",
                new string('x', 129),
            };
            int rejectedReservedNames = 0;
            foreach (string reservedName in reservedNames)
            {
                try
                {
                    _ = AssetCreationWorkflow.CreateNamed(
                        assets,
                        phase2,
                        AcsAssetTemplate.Blueprint,
                        reservedName);
                }
                catch (InvalidDataException)
                {
                    rejectedReservedNames++;
                }
            }
            Check(
                rejectedReservedNames == reservedNames.Length,
                "explicit-name creation rejects Windows devices, paths, metadata, and staging names");

            using (var cancelledBeforeStart = new CancellationTokenSource())
            {
                cancelledBeforeStart.Cancel();
                bool cancellationObserved = false;
                try
                {
                    _ = AssetCreationWorkflow.CreateNamed(
                        assets,
                        phase2,
                        AcsAssetTemplate.Scene,
                        "CancelledScene",
                        cancellationToken: cancelledBeforeStart.Token);
                }
                catch (OperationCanceledException)
                {
                    cancellationObserved = true;
                }
                Check(
                    cancellationObserved &&
                    !File.Exists(Path.Combine(phase2, "CancelledScene.acs3d")),
                    "pre-cancelled creation performs no filesystem publication");
            }

            using (var retiredProjectToken = new CancellationTokenSource())
            {
                retiredProjectToken.Cancel();
                CancellationTokenSource renewedProjectToken =
                    AssetBrowserPanel.RenewProjectOperationCancellation(
                        retiredProjectToken);
                try
                {
                    Check(
                        !ReferenceEquals(
                            retiredProjectToken,
                            renewedProjectToken) &&
                        !renewedProjectToken.IsCancellationRequested,
                        "resuming a cancelled editor close renews the project operation generation");
                }
                finally
                {
                    renewedProjectToken.Dispose();
                }
            }
            using (var activeProjectToken = new CancellationTokenSource())
            {
                Check(
                    ReferenceEquals(
                        activeProjectToken,
                        AssetBrowserPanel.RenewProjectOperationCancellation(
                            activeProjectToken)),
                    "resume preserves a still-active project operation generation");
            }

            using (var switchedProject = new CancellationTokenSource())
            using (var writerEntered = new ManualResetEventSlim(false))
            using (var releaseWriter = new ManualResetEventSlim(false))
            {
                Task<bool> cancelledCreation = Task.Run(() =>
                {
                    try
                    {
                        _ = AssetCreationWorkflow.CreateNamed(
                            assets,
                            phase2,
                            AcsAssetTemplate.Material,
                            "CancelledMaterial",
                            (path, unusedName) =>
                            {
                                _ = unusedName;
                                File.WriteAllText(path, "partial material");
                                File.WriteAllText(
                                    path + AssetDatabase.MetadataSuffix,
                                    "partial metadata");
                                File.WriteAllText(
                                    path + ".graph.json",
                                    "partial graph");
                                File.WriteAllText(
                                    path + ".graph.json" +
                                    AssetDatabase.MetadataSuffix,
                                    "partial graph metadata");
                                writerEntered.Set();
                                releaseWriter.Wait(TimeSpan.FromSeconds(5));
                                // Cancellation wins over a serializer rejection so the UI can
                                // retire a switched project without reporting a corrupt asset.
                                return false;
                            },
                            switchedProject.Token);
                        return false;
                    }
                    catch (OperationCanceledException)
                    {
                        return true;
                    }
                });
                bool writerStarted = writerEntered.Wait(TimeSpan.FromSeconds(5));
                switchedProject.Cancel();
                releaseWriter.Set();
                bool completed = cancelledCreation.Wait(TimeSpan.FromSeconds(5));
                Check(
                    writerStarted &&
                    completed &&
                    cancelledCreation.Result &&
                    !File.Exists(
                        Path.Combine(phase2, "CancelledMaterial.acsmat")) &&
                    !Directory.EnumerateFileSystemEntries(phase2)
                        .Any(AssetCreationWorkflow.IsTemporaryPath),
                    "project-switch cancellation during serialization removes the entire temporary material family");
            }

            string switchedProjectRoot = Path.Combine(root, "SwitchedProject");
            string switchedAssets = Path.Combine(switchedProjectRoot, "Assets");
            Directory.CreateDirectory(switchedAssets);
            var switchedDatabase = new AssetDatabase(
                switchedProjectRoot,
                switchedAssets);
            _ = switchedDatabase.Refresh(verifyContent: true);
            AcsAssetCreationResult publishedBeforeSwitch =
                AssetCreationWorkflow.CreateNamed(
                    switchedAssets,
                    switchedAssets,
                    AcsAssetTemplate.Scene,
                    "PublishedBeforeSwitch");
            bool cancelledIndex = false;
            using (var cancelledRefresh = new CancellationTokenSource())
            {
                cancelledRefresh.Cancel();
                try
                {
                    _ = switchedDatabase.Refresh(
                        cancellationToken: cancelledRefresh.Token);
                }
                catch (OperationCanceledException)
                {
                    cancelledIndex = true;
                }
            }
            AssetDatabaseRefreshResult recoveredAfterSwitch =
                switchedDatabase.Refresh(verifyContent: true);
            bool recoveredPublishedAsset = switchedDatabase.TryGetByPath(
                publishedBeforeSwitch.FullPath,
                out AssetRecord? recoveredRecord);
            Check(
                cancelledIndex &&
                recoveredAfterSwitch.Warnings.Count == 0 &&
                recoveredPublishedAsset &&
                recoveredRecord?.Kind == "scene" &&
                File.ReadAllText(publishedBeforeSwitch.FullPath) == "ACS3D v2\n" &&
                !Directory.EnumerateFileSystemEntries(
                        switchedAssets,
                        "*",
                        SearchOption.AllDirectories)
                    .Any(AssetCreationWorkflow.IsTemporaryPath),
                "project switch after atomic publication is recovered by the next authoritative index refresh");

            string metadataDirectory = Path.Combine(phase2, "Hidden.acsmeta");
            string stagingDirectory = Path.Combine(phase2, "Preview.tmp-staging");
            string internalStaging = Path.Combine(
                assets,
                AssetDatabase.InternalDirectoryName,
                AssetDatabase.ImportStagingDirectoryName);
            Directory.CreateDirectory(metadataDirectory);
            Directory.CreateDirectory(stagingDirectory);
            Directory.CreateDirectory(internalStaging);
            int rejectedInternalTargets = 0;
            foreach (string reservedTarget in new[]
                     {
                         metadataDirectory,
                         stagingDirectory,
                         internalStaging,
                     })
            {
                try
                {
                    _ = AssetCreationWorkflow.Create(
                        assets,
                        reservedTarget,
                        AcsAssetTemplate.Scene);
                }
                catch (InvalidDataException)
                {
                    rejectedInternalTargets++;
                }
            }
            Check(
                rejectedInternalTargets == 3 &&
                !File.Exists(Path.Combine(metadataDirectory, "Scene.acs3d")) &&
                !File.Exists(Path.Combine(stagingDirectory, "Scene.acs3d")) &&
                !File.Exists(Path.Combine(internalStaging, "Scene.acs3d")),
                "creation refuses metadata, temporary, and .acsdb staging directories");

            bool missingWriterRejected = false;
            try
            {
                AssetCreationWorkflow.Create(
                    assets,
                    materials,
                    AcsAssetTemplate.Material);
            }
            catch (InvalidOperationException)
            {
                missingWriterRejected = true;
            }
            Check(missingWriterRejected,
                "Material creation refuses to guess or hand-write the native ACSMAT schema");

            bool failedWriterRejected = false;
            try
            {
                AssetCreationWorkflow.Create(
                    assets,
                    materials,
                    AcsAssetTemplate.Material,
                    (path, _) =>
                    {
                        File.WriteAllText(path, "partial");
                        return false;
                    });
            }
            catch (InvalidDataException)
            {
                failedWriterRejected = true;
            }
            Check(failedWriterRejected &&
                  !Directory.EnumerateFileSystemEntries(materials)
                      .Any(AssetCreationWorkflow.IsTemporaryPath),
                "failed native material serialization rolls back every temporary asset");

            string outside = Path.Combine(root, "Project", "AssetsSibling");
            Directory.CreateDirectory(outside);
            bool outsideRejected = false;
            try
            {
                AssetCreationWorkflow.Create(assets, outside, AcsAssetTemplate.Blueprint);
            }
            catch (InvalidDataException)
            {
                outsideRejected = true;
            }
            Check(outsideRejected && !File.Exists(Path.Combine(outside, "Blueprint.acsbp")),
                "creation rejects sibling paths that merely share the Assets prefix");

            string linked = Path.Combine(assets, "Linked");
            try
            {
                Directory.CreateSymbolicLink(linked, outside);
                bool linkRejected = false;
                try
                {
                    AssetCreationWorkflow.Create(assets, linked, AcsAssetTemplate.Blueprint);
                }
                catch (InvalidDataException)
                {
                    linkRejected = true;
                }
                Check(linkRejected && !File.Exists(Path.Combine(outside, "Blueprint.acsbp")),
                    "creation refuses nested reparse points instead of escaping Assets");
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or IOException or NotSupportedException)
            {
                output.WriteLine("SKIP: reparse-point runtime test: " + error.Message);
            }

            bool unknownRejected = false;
            try
            {
                AssetCreationWorkflow.Create(
                    assets,
                    materials,
                    (AcsAssetTemplate)999);
            }
            catch (ArgumentOutOfRangeException)
            {
                unknownRejected = true;
            }
            Check(unknownRejected, "unknown asset template identifiers fail closed");
            Check(!Directory.EnumerateFileSystemEntries(materials)
                    .Any(AssetCreationWorkflow.IsTemporaryPath),
                "successful creation leaves no temporary files or directories");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unhandled exception: " + error);
        }
        finally
        {
            try
            {
                if (Directory.Exists(root))
                    Directory.Delete(root, recursive: true);
            }
            catch
            {
                // The directory is unique; cleanup failure is not a product failure.
            }
        }

        output.WriteLine(
            $"Asset creation self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
