// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

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
                        AcsAssetTemplate.Blueprint,
                    }),
                "New menu exposes only Folder and the two usable ACS asset formats");

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
            Check(indexResult.Warnings.Count == 0 &&
                  indexedBlueprint && blueprintRecord?.Kind == "blueprint" &&
                  blueprintRecord.Metadata.Importer == "blueprint" &&
                  indexedMaterial && materialRecord?.Kind == "material" &&
                  materialRecord.Metadata.Importer == "material",
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
