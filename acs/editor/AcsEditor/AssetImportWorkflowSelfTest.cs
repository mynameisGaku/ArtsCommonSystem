// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;

namespace AcsEditor;

internal static class AssetImportWorkflowSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-import-selftest-" + Guid.NewGuid().ToString("N"));

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

        TestProject NewProject(string name)
        {
            string projectRoot = Path.Combine(root, name);
            string assets = Path.Combine(projectRoot, "Assets");
            string target = Path.Combine(assets, "Imported");
            string sources = Path.Combine(root, name + "-sources");
            Directory.CreateDirectory(target);
            Directory.CreateDirectory(sources);
            return new TestProject(
                projectRoot,
                assets,
                target,
                sources,
                new AssetDatabase(projectRoot, assets));
        }

        static string WriteSource(
            TestProject project,
            string name,
            string contents)
        {
            string path = Path.Combine(project.Sources, name);
            File.WriteAllText(path, contents, new UTF8Encoding(false));
            return path;
        }

        try
        {
            Directory.CreateDirectory(root);

            TestProject fresh = NewProject("fresh");
            Check(!Directory.Exists(Path.Combine(
                    fresh.Assets,
                    AssetDatabase.InternalDirectoryName)),
                "fresh project begins without an asset database directory");
            AssetImportReconciliationResult freshRecovery =
                AssetImportWorkflow.Reconcile(fresh.Database);
            Check(freshRecovery == new AssetImportReconciliationResult(
                    0,
                    0,
                    0,
                    Array.Empty<string>()) ||
                  (freshRecovery.CompletedTransactions == 0 &&
                   freshRecovery.DiscardedTransactions == 0 &&
                   freshRecovery.PreservedTransactions == 0 &&
                   freshRecovery.Warnings.Count == 0),
                "startup reconciliation safely initializes a fresh project");

            string skySource = WriteSource(fresh, "sky.png", "new-sky-pixels");
            AssetImportOperationResult imported =
                AssetImportWorkflow.ImportFiles(
                    fresh.Database,
                    fresh.Target,
                    new[] { skySource });
            string skyDestination = Path.Combine(fresh.Target, "sky.png");
            bool skyIndexed = fresh.Database.TryGetByPath(
                skyDestination,
                out AssetRecord? sky);
            Check(imported.Imported.Count == 1 &&
                  imported.Failures.Count == 0 &&
                  File.ReadAllText(skyDestination) == "new-sky-pixels" &&
                  File.Exists(
                      skyDestination + AssetDatabase.MetadataSuffix) &&
                  skyIndexed &&
                  sky?.Metadata.Importer == "texture" &&
                  sky.Metadata.Source.Replace('/', Path.DirectorySeparatorChar) ==
                  Path.GetFullPath(skySource) &&
                  sky.Metadata.ImportSettings.GetValueOrDefault(
                      "sourceContentHash") == sky.ContentHash,
                "import atomically publishes payload, authoritative metadata, fingerprint, and index");
            Check(TransactionDirectories(fresh.Assets, "import-staging").Length == 0,
                "successful import removes its private transaction");

            string collisionSource = WriteSource(
                fresh,
                "sky.png.copy",
                "collision");
            string collisionNamedSource = Path.Combine(
                fresh.Sources,
                "sky.png");
            File.WriteAllText(
                collisionNamedSource,
                "collision-two",
                new UTF8Encoding(false));
            AssetImportOperationResult collision =
                AssetImportWorkflow.ImportFiles(
                    fresh.Database,
                    fresh.Target,
                    new[] { collisionNamedSource });
            Check(collision.Imported.Single().DestinationPath ==
                  Path.Combine(fresh.Target, "sky (1).png") &&
                  File.ReadAllText(skyDestination) == "new-sky-pixels",
                "import chooses a collision-free name without overwriting an asset");
            File.Delete(collisionSource);

            TestProject recursive = NewProject("recursive-folder");
            string environmentPack = Path.Combine(
                recursive.Sources,
                "EnvironmentPack");
            string nestedTextures = Path.Combine(
                environmentPack,
                "Textures",
                "Terrain");
            string emptyFolder = Path.Combine(
                environmentPack,
                "Meshes",
                "Empty");
            Directory.CreateDirectory(nestedTextures);
            Directory.CreateDirectory(emptyFolder);
            string albedoSource = Path.Combine(nestedTextures, "albedo.png");
            string meshSource = Path.Combine(environmentPack, "island.glb");
            string looseSource = Path.Combine(recursive.Sources, "readme.txt");
            File.WriteAllText(albedoSource, "albedo", new UTF8Encoding(false));
            File.WriteAllText(meshSource, "mesh", new UTF8Encoding(false));
            File.WriteAllText(looseSource, "loose", new UTF8Encoding(false));

            AssetImportOperationResult recursiveImport =
                AssetImportWorkflow.ImportExternalPaths(
                    recursive.Database,
                    recursive.Target,
                    new[] { environmentPack, looseSource });
            string importedPack = Path.Combine(
                recursive.Target,
                "EnvironmentPack");
            string importedAlbedo = Path.Combine(
                importedPack,
                "Textures",
                "Terrain",
                "albedo.png");
            string importedMesh = Path.Combine(importedPack, "island.glb");
            string importedLoose = Path.Combine(recursive.Target, "readme.txt");
            Check(recursiveImport.Imported.Count == 3 &&
                  recursiveImport.ImportedDirectories.SequenceEqual(
                      new[] { importedPack },
                      StringComparer.OrdinalIgnoreCase) &&
                  File.ReadAllText(importedAlbedo) == "albedo" &&
                  File.ReadAllText(importedMesh) == "mesh" &&
                  File.ReadAllText(importedLoose) == "loose" &&
                  Directory.Exists(Path.Combine(
                      importedPack,
                      "Meshes",
                      "Empty")) &&
                  recursive.Database.TryGetByPath(importedAlbedo, out _) &&
                  recursive.Database.TryGetByPath(importedMesh, out _) &&
                  recursive.Database.TryGetByPath(importedLoose, out _),
                "recursive drop preserves hierarchy and empty folders, imports loose files, indexes, and returns the root selection");
            Check(TransactionDirectories(
                      recursive.Assets,
                      "import-staging").Length == 0,
                "recursive import cleans every per-file and private-directory staging entry");

            AssetImportOperationResult recursiveCollision =
                AssetImportWorkflow.ImportExternalPaths(
                    recursive.Database,
                    recursive.Target,
                    new[] { environmentPack });
            string collisionPack = Path.Combine(
                recursive.Target,
                "EnvironmentPack (1)");
            Check(recursiveCollision.ImportedDirectories.SequenceEqual(
                      new[] { collisionPack },
                      StringComparer.OrdinalIgnoreCase) &&
                  File.Exists(Path.Combine(collisionPack, "island.glb")) &&
                  File.ReadAllText(importedMesh) == "mesh",
                "recursive import reserves a collision-free folder root without merging or overwriting");

            TestProject recursiveRollback = NewProject("recursive-rollback");
            string rollbackFolder = Path.Combine(
                recursiveRollback.Sources,
                "RollbackPack");
            Directory.CreateDirectory(rollbackFolder);
            File.WriteAllText(
                Path.Combine(rollbackFolder, "first.bin"),
                "first",
                new UTF8Encoding(false));
            bool recursiveRolledBack = false;
            try
            {
                _ = AssetImportWorkflow.ImportExternalPaths(
                    recursiveRollback.Database,
                    recursiveRollback.Target,
                    new[] { rollbackFolder },
                    testHooks: new AssetImportTestHooks(
                        FailAfter: AssetImportCheckpoint.AssetPublished));
            }
            catch (IOException)
            {
                recursiveRolledBack = true;
            }
            Check(recursiveRolledBack &&
                  !Directory.Exists(Path.Combine(
                      recursiveRollback.Target,
                      "RollbackPack")) &&
                  recursiveRollback.Database.Snapshot().Count == 0 &&
                  TransactionDirectories(
                      recursiveRollback.Assets,
                      "import-staging").Length == 0,
                "recursive batch failure rolls back payload, metadata, hierarchy, index, and journals");

            TestProject recursiveCrash = NewProject(
                "recursive-crash-rollback");
            string recursiveCrashFolder = Path.Combine(
                recursiveCrash.Sources,
                "CrashPack");
            Directory.CreateDirectory(Path.Combine(
                recursiveCrashFolder,
                "Empty",
                "Nested"));
            File.WriteAllText(
                Path.Combine(recursiveCrashFolder, "first.bin"),
                "first",
                new UTF8Encoding(false));
            File.WriteAllText(
                Path.Combine(recursiveCrashFolder, "second.bin"),
                "second",
                new UTF8Encoding(false));
            bool recursiveProcessTerminated =
                SimulateRecursiveImportCrash(
                    recursiveCrash,
                    recursiveCrashFolder,
                    AssetImportCheckpoint.MetadataPublished);
            string recursiveCrashDestination = Path.Combine(
                recursiveCrash.Target,
                "CrashPack");
            bool recursivePartialStateExisted =
                File.Exists(Path.Combine(
                    recursiveCrashDestination,
                    "first.bin")) &&
                !File.Exists(Path.Combine(
                    recursiveCrashDestination,
                    "second.bin")) &&
                Directory.Exists(Path.Combine(
                    recursiveCrashDestination,
                    "Empty",
                    "Nested"));
            AssetImportReconciliationResult recursiveCrashRecovery =
                AssetImportWorkflow.Reconcile(recursiveCrash.Database);
            _ = recursiveCrash.Database.Refresh(verifyContent: true);
            Check(recursiveProcessTerminated &&
                  recursivePartialStateExisted &&
                  recursiveCrashRecovery.DiscardedTransactions == 1 &&
                  recursiveCrashRecovery.PreservedTransactions == 0 &&
                  !Directory.Exists(recursiveCrashDestination) &&
                  recursiveCrash.Database.Snapshot().Count == 0 &&
                  TransactionDirectories(
                      recursiveCrash.Assets,
                      "import-staging").Length == 0,
                "startup rolls an interrupted recursive batch back as one all-or-nothing transaction");

            TestProject recursiveCommittedCrash = NewProject(
                "recursive-committed-crash");
            string committedFolder = Path.Combine(
                recursiveCommittedCrash.Sources,
                "CommittedPack");
            Directory.CreateDirectory(Path.Combine(
                committedFolder,
                "Empty"));
            File.WriteAllText(
                Path.Combine(committedFolder, "one.bin"),
                "one",
                new UTF8Encoding(false));
            File.WriteAllText(
                Path.Combine(committedFolder, "two.bin"),
                "two",
                new UTF8Encoding(false));
            bool committedCleanupInterrupted =
                SimulateRecursiveImportCrash(
                    recursiveCommittedCrash,
                    committedFolder,
                    AssetImportCheckpoint.RecursiveBatchCommitted);
            string committedDestination = Path.Combine(
                recursiveCommittedCrash.Target,
                "CommittedPack");
            string committedBatchDirectory = TransactionDirectories(
                recursiveCommittedCrash.Assets,
                "import-staging").Single();
            string partiallyCleanedChild = Directory
                .EnumerateDirectories(
                    committedBatchDirectory,
                    "*",
                    SearchOption.TopDirectoryOnly)
                .First(path => Guid.TryParseExact(
                    Path.GetFileName(path),
                    "N",
                    out _));
            File.Delete(Path.Combine(
                partiallyCleanedChild,
                "manifest.v1.json"));
            AssetImportReconciliationResult committedRecovery =
                AssetImportWorkflow.Reconcile(
                    recursiveCommittedCrash.Database);
            _ = recursiveCommittedCrash.Database.Refresh(
                verifyContent: true);
            Check(committedCleanupInterrupted &&
                  committedRecovery.CompletedTransactions == 1 &&
                  committedRecovery.PreservedTransactions == 0 &&
                  File.ReadAllText(Path.Combine(
                      committedDestination,
                      "one.bin")) == "one" &&
                  File.ReadAllText(Path.Combine(
                      committedDestination,
                      "two.bin")) == "two" &&
                  Directory.Exists(Path.Combine(
                      committedDestination,
                      "Empty")) &&
                  recursiveCommittedCrash.Database.Snapshot().Count == 2 &&
                  TransactionDirectories(
                      recursiveCommittedCrash.Assets,
                      "import-staging").Length == 0,
                "startup verifies and finalizes a committed recursive batch whose journal cleanup was interrupted");

            TestProject recursiveAmbiguousCrash = NewProject(
                "recursive-ambiguous-crash");
            string ambiguousFolder = Path.Combine(
                recursiveAmbiguousCrash.Sources,
                "AmbiguousPack");
            Directory.CreateDirectory(ambiguousFolder);
            File.WriteAllText(
                Path.Combine(ambiguousFolder, "asset.bin"),
                "owned",
                new UTF8Encoding(false));
            bool ambiguousProcessTerminated =
                SimulateRecursiveImportCrash(
                    recursiveAmbiguousCrash,
                    ambiguousFolder,
                    AssetImportCheckpoint.MetadataPublished);
            string recursiveAmbiguousDestination = Path.Combine(
                recursiveAmbiguousCrash.Target,
                "AmbiguousPack",
                "asset.bin");
            File.WriteAllText(
                recursiveAmbiguousDestination,
                "foreign-change",
                new UTF8Encoding(false));
            AssetImportReconciliationResult recursiveAmbiguousRecovery =
                AssetImportWorkflow.Reconcile(
                    recursiveAmbiguousCrash.Database);
            Check(ambiguousProcessTerminated &&
                  recursiveAmbiguousRecovery.PreservedTransactions == 1 &&
                  File.ReadAllText(recursiveAmbiguousDestination) ==
                      "foreign-change" &&
                  File.Exists(
                      recursiveAmbiguousDestination +
                      AssetDatabase.MetadataSuffix) &&
                  TransactionDirectories(
                      recursiveAmbiguousCrash.Assets,
                      "import-staging").Length == 1,
                "startup preserves the complete batch journal and changed destination when rollback ownership is ambiguous");

            TestProject recursiveSourceRace = NewProject("recursive-source-race");
            string sourceRaceFolder = Path.Combine(
                recursiveSourceRace.Sources,
                "SourceRacePack");
            Directory.CreateDirectory(sourceRaceFolder);
            string changingSource = Path.Combine(
                sourceRaceFolder,
                "changing.bin");
            File.WriteAllText(
                changingSource,
                "before",
                new UTF8Encoding(false));
            bool sourceRaceRolledBack = false;
            try
            {
                _ = AssetImportWorkflow.ImportExternalPaths(
                    recursiveSourceRace.Database,
                    recursiveSourceRace.Target,
                    new[] { sourceRaceFolder },
                    testHooks: new AssetImportTestHooks(
                        Observe: checkpoint =>
                        {
                            if (checkpoint == AssetImportCheckpoint.Prepared)
                            {
                                File.WriteAllText(
                                    changingSource,
                                    "changed-after-snapshot",
                                    new UTF8Encoding(false));
                            }
                        }));
            }
            catch (IOException)
            {
                sourceRaceRolledBack = true;
            }
            Check(sourceRaceRolledBack &&
                  !Directory.Exists(Path.Combine(
                      recursiveSourceRace.Target,
                      "SourceRacePack")) &&
                  recursiveSourceRace.Database.Snapshot().Count == 0 &&
                  TransactionDirectories(
                      recursiveSourceRace.Assets,
                      "import-staging").Length == 0,
                "recursive import rechecks its complete source snapshot and rolls back a changing tree");

            TestProject recursiveRace = NewProject("recursive-race");
            string raceFolder = Path.Combine(
                recursiveRace.Sources,
                "RacePack");
            Directory.CreateDirectory(raceFolder);
            File.WriteAllText(
                Path.Combine(raceFolder, "race.bin"),
                "source",
                new UTF8Encoding(false));
            string looseRaceSource = Path.Combine(
                recursiveRace.Sources,
                "race-loose.bin");
            File.WriteAllText(
                looseRaceSource,
                "source",
                new UTF8Encoding(false));
            string racedDestination = Path.Combine(
                recursiveRace.Target,
                "race-loose.bin");
            bool destinationRacePreserved = false;
            try
            {
                _ = AssetImportWorkflow.ImportExternalPaths(
                    recursiveRace.Database,
                    recursiveRace.Target,
                    new[] { raceFolder, looseRaceSource },
                    testHooks: new AssetImportTestHooks(
                        Observe: checkpoint =>
                        {
                            if (checkpoint == AssetImportCheckpoint.Prepared)
                            {
                                File.WriteAllText(
                                    racedDestination,
                                    "foreign",
                                    new UTF8Encoding(false));
                            }
                        }));
            }
            catch (IOException)
            {
                destinationRacePreserved = true;
            }
            bool foreignPayloadPreserved =
                File.Exists(racedDestination) &&
                File.ReadAllText(racedDestination) == "foreign";
            bool foreignMetadataAbsent = !File.Exists(
                racedDestination + AssetDatabase.MetadataSuffix);
            int racedIndexCount = recursiveRace.Database.Snapshot().Count;
            int racedJournalCount = TransactionDirectories(
                recursiveRace.Assets,
                "import-staging").Length;
            if (!destinationRacePreserved ||
                !foreignPayloadPreserved ||
                !foreignMetadataAbsent ||
                racedIndexCount != 0 ||
                racedJournalCount != 1)
            {
                output.WriteLine(
                    $"INFO: destination race state: exception={destinationRacePreserved}, " +
                    $"payload={foreignPayloadPreserved}, metadataAbsent={foreignMetadataAbsent}, " +
                    $"index={racedIndexCount}, journals={racedJournalCount}");
            }
            Check(destinationRacePreserved &&
                  foreignPayloadPreserved &&
                  foreignMetadataAbsent &&
                  racedIndexCount == 0 &&
                  racedJournalCount == 1 &&
                  !Directory.Exists(Path.Combine(
                      recursiveRace.Target,
                      "RacePack")),
                "destination race never overwrites foreign data and retains its journal as fail-closed recovery evidence");

            TestProject recursiveDirectoryRace = NewProject(
                "recursive-directory-race");
            string directoryRaceFolder = Path.Combine(
                recursiveDirectoryRace.Sources,
                "ForeignEmptyPack");
            Directory.CreateDirectory(directoryRaceFolder);
            File.WriteAllText(
                Path.Combine(directoryRaceFolder, "owned.bin"),
                "owned",
                new UTF8Encoding(false));
            string foreignEmptyDestination = Path.Combine(
                recursiveDirectoryRace.Target,
                "ForeignEmptyPack");
            bool directoryRaceProcessTerminated =
                SimulateRecursiveImportCrash(
                    recursiveDirectoryRace,
                    directoryRaceFolder,
                    AssetImportCheckpoint.RecursiveDirectoryPrepared,
                    checkpoint =>
                    {
                        if (checkpoint ==
                            AssetImportCheckpoint.RecursiveDirectoryPrepared)
                        {
                            Directory.CreateDirectory(
                                foreignEmptyDestination);
                        }
                    });
            AssetImportReconciliationResult directoryRaceRecovery =
                AssetImportWorkflow.Reconcile(
                    recursiveDirectoryRace.Database);
            bool foreignEmptyDirectoryPreserved =
                Directory.Exists(foreignEmptyDestination) &&
                !new DirectoryInfo(foreignEmptyDestination)
                    .EnumerateFileSystemInfos(
                        "*",
                        SearchOption.TopDirectoryOnly)
                    .Any();
            Check(directoryRaceProcessTerminated &&
                  foreignEmptyDirectoryPreserved &&
                  directoryRaceRecovery.DiscardedTransactions == 0 &&
                  directoryRaceRecovery.PreservedTransactions == 1 &&
                  recursiveDirectoryRace.Database.Snapshot().Count == 0 &&
                  TransactionDirectories(
                      recursiveDirectoryRace.Assets,
                      "import-staging").Length == 1,
                "recursive recovery never deletes a foreign empty directory created after its prepared journal");

            TestProject recursiveLimits = NewProject("recursive-limits");
            string depthPack = Path.Combine(
                recursiveLimits.Sources,
                "DepthPack");
            Directory.CreateDirectory(Path.Combine(depthPack, "One"));
            File.WriteAllText(
                Path.Combine(depthPack, "One", "deep.bin"),
                "depth",
                new UTF8Encoding(false));
            string countPack = Path.Combine(
                recursiveLimits.Sources,
                "CountPack");
            Directory.CreateDirectory(countPack);
            File.WriteAllText(
                Path.Combine(countPack, "one.bin"),
                "1",
                new UTF8Encoding(false));
            File.WriteAllText(
                Path.Combine(countPack, "two.bin"),
                "2",
                new UTF8Encoding(false));
            string sizePack = Path.Combine(
                recursiveLimits.Sources,
                "SizePack");
            Directory.CreateDirectory(sizePack);
            File.WriteAllText(
                Path.Combine(sizePack, "large.bin"),
                "12345",
                new UTF8Encoding(false));
            bool depthLimited = RejectRecursiveImport(
                recursiveLimits,
                depthPack,
                new AssetImportTraversalLimits(1, 100, 1024));
            bool countLimited = RejectRecursiveImport(
                recursiveLimits,
                countPack,
                new AssetImportTraversalLimits(8, 2, 1024));
            bool sizeLimited = RejectRecursiveImport(
                recursiveLimits,
                sizePack,
                new AssetImportTraversalLimits(8, 100, 4));
            Check(depthLimited && countLimited && sizeLimited &&
                  !Directory.Exists(Path.Combine(
                      recursiveLimits.Target,
                      "DepthPack")) &&
                  !Directory.Exists(Path.Combine(
                      recursiveLimits.Target,
                      "CountPack")) &&
                  !Directory.Exists(Path.Combine(
                      recursiveLimits.Target,
                      "SizePack")),
                "recursive depth, entry-count, and total-byte budgets reject before destination mutation");

            TestProject recursiveReserved = NewProject("recursive-reserved");
            string reservedPack = Path.Combine(
                recursiveReserved.Sources,
                "ReservedPack");
            Directory.CreateDirectory(Path.Combine(
                reservedPack,
                AssetDatabase.InternalDirectoryName));
            File.WriteAllText(
                Path.Combine(
                    reservedPack,
                    AssetDatabase.InternalDirectoryName,
                    "payload.bin"),
                "private",
                new UTF8Encoding(false));
            bool reservedFolderRejected = RejectRecursiveImport(
                recursiveReserved,
                reservedPack,
                new AssetImportTraversalLimits(8, 100, 1024));
            string managedPack = Path.Combine(
                recursiveReserved.Assets,
                "ManagedPack");
            Directory.CreateDirectory(managedPack);
            File.WriteAllText(
                Path.Combine(managedPack, "managed.bin"),
                "managed",
                new UTF8Encoding(false));
            bool managedSourceRejected = RejectRecursiveImport(
                recursiveReserved,
                managedPack,
                new AssetImportTraversalLimits(8, 100, 1024));
            Check(reservedFolderRejected && managedSourceRejected &&
                  !Directory.Exists(Path.Combine(
                      recursiveReserved.Target,
                      "ReservedPack")),
                "recursive import rejects nested .acsdb names and every source already below Assets");

            TestProject recursiveLink = NewProject("recursive-link");
            string linkPack = Path.Combine(
                recursiveLink.Sources,
                "LinkPack");
            string linkOutside = Path.Combine(
                recursiveLink.Sources,
                "LinkOutside");
            Directory.CreateDirectory(linkPack);
            Directory.CreateDirectory(linkOutside);
            File.WriteAllText(
                Path.Combine(linkOutside, "outside.bin"),
                "outside",
                new UTF8Encoding(false));
            try
            {
                Directory.CreateSymbolicLink(
                    Path.Combine(linkPack, "Linked"),
                    linkOutside);
                bool recursiveLinkRejected = RejectRecursiveImport(
                    recursiveLink,
                    linkPack,
                    new AssetImportTraversalLimits(8, 100, 1024));
                Check(recursiveLinkRejected &&
                      !Directory.Exists(Path.Combine(
                          recursiveLink.Target,
                          "LinkPack")),
                    "recursive import rejects nested directory symlinks without following them");
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or IOException or
                    NotSupportedException)
            {
                output.WriteLine(
                    "SKIP: recursive import reparse-point runtime test: " +
                    error.Message);
            }

            string goodBatchSource = WriteSource(fresh, "batch.txt", "batch");
            string reservedSource = WriteSource(
                fresh,
                "reserved.acsmeta",
                "not-an-asset");
            AssetImportOperationResult partialBatch =
                AssetImportWorkflow.ImportFiles(
                    fresh.Database,
                    fresh.Target,
                    new[] { reservedSource, goodBatchSource });
            Check(partialBatch.Imported.Count == 1 &&
                  partialBatch.Failures.Count == 1 &&
                  File.Exists(Path.Combine(fresh.Target, "batch.txt")) &&
                  !File.Exists(Path.Combine(fresh.Target, "reserved.acsmeta")),
                "source-specific batch failure rolls back that source and commits other inputs");

            bool traversalRejected = false;
            try
            {
                _ = AssetImportWorkflow.ImportFiles(
                    fresh.Database,
                    fresh.Sources,
                    new[] { goodBatchSource });
            }
            catch (InvalidDataException)
            {
                traversalRejected = true;
            }
            Check(traversalRejected,
                "import rejects a destination outside Assets");

            TestProject preparedCrash = NewProject("prepared-crash");
            string preparedSource = WriteSource(
                preparedCrash,
                "prepared.bin",
                "prepared");
            bool preparedTerminated = SimulateImportCrash(
                preparedCrash,
                preparedSource,
                AssetImportCheckpoint.Prepared);
            AssetImportReconciliationResult preparedResult =
                AssetImportWorkflow.Reconcile(preparedCrash.Database);
            Check(preparedTerminated &&
                  preparedResult.DiscardedTransactions == 1 &&
                  !File.Exists(Path.Combine(
                      preparedCrash.Target,
                      "prepared.bin")) &&
                  TransactionDirectories(
                      preparedCrash.Assets,
                      "import-staging").Length == 0,
                "startup discards a fully private import preparation");

            TestProject assetCrash = NewProject("asset-crash");
            string assetCrashSource = WriteSource(
                assetCrash,
                "asset.bin",
                "published-before-sidecar");
            bool assetTerminated = SimulateImportCrash(
                assetCrash,
                assetCrashSource,
                AssetImportCheckpoint.AssetPublished);
            string recoveredAsset = Path.Combine(assetCrash.Target, "asset.bin");
            Check(assetTerminated &&
                  File.Exists(recoveredAsset) &&
                  !File.Exists(recoveredAsset + AssetDatabase.MetadataSuffix),
                "crash harness exposes the payload/sidecar publication window");
            bool interruptedImportRefreshBlocked = false;
            try
            {
                _ = assetCrash.Database.Refresh(verifyContent: true);
            }
            catch (IOException)
            {
                interruptedImportRefreshBlocked = true;
            }
            Check(interruptedImportRefreshBlocked &&
                  !File.Exists(recoveredAsset + AssetDatabase.MetadataSuffix),
                "ordinary Refresh cannot synthesize metadata over an interrupted Import");
            AssetImportReconciliationResult assetRecovery =
                AssetImportWorkflow.Reconcile(assetCrash.Database);
            _ = assetCrash.Database.Refresh(verifyContent: true);
            Check(assetRecovery.CompletedTransactions == 1 &&
                  File.Exists(recoveredAsset + AssetDatabase.MetadataSuffix) &&
                  assetCrash.Database.TryGetByPath(
                      recoveredAsset,
                      out AssetRecord? recovered) &&
                  recovered?.Metadata.Source.Replace(
                      '/',
                      Path.DirectorySeparatorChar) ==
                  Path.GetFullPath(assetCrashSource),
                "startup completes a matching payload with its staged authoritative sidecar");

            TestProject metadataCrash = NewProject("metadata-crash");
            string metadataCrashSource = WriteSource(
                metadataCrash,
                "metadata.bin",
                "both-published");
            bool metadataTerminated = SimulateImportCrash(
                metadataCrash,
                metadataCrashSource,
                AssetImportCheckpoint.MetadataPublished);
            AssetImportReconciliationResult metadataRecovery =
                AssetImportWorkflow.Reconcile(metadataCrash.Database);
            _ = metadataCrash.Database.Refresh(verifyContent: true);
            Check(metadataTerminated &&
                  metadataRecovery.CompletedTransactions == 1 &&
                  metadataCrash.Database.TryGetByPath(
                      Path.Combine(metadataCrash.Target, "metadata.bin"),
                      out _),
                "startup finalizes an import that crashed before index publication");

            TestProject injectedFailure = NewProject("injected-failure");
            string injectedSource = WriteSource(
                injectedFailure,
                "rollback.bin",
                "rollback");
            AssetImportOperationResult failedOperation =
                AssetImportWorkflow.ImportFiles(
                    injectedFailure.Database,
                    injectedFailure.Target,
                    new[] { injectedSource },
                    testHooks: new AssetImportTestHooks(
                        FailAfter: AssetImportCheckpoint.AssetPublished));
            Check(failedOperation.Imported.Count == 0 &&
                  failedOperation.Failures.Count == 1 &&
                  !File.Exists(Path.Combine(
                      injectedFailure.Target,
                      "rollback.bin")) &&
                  TransactionDirectories(
                      injectedFailure.Assets,
                      "import-staging").Length == 0,
                "in-process failure rolls back a published payload and journal");

            TestProject cancelledBatch = NewProject("cancelled-batch");
            string cancelOne = WriteSource(cancelledBatch, "one.bin", "one");
            string cancelTwo = WriteSource(cancelledBatch, "two.bin", "two");
            using (var cancellation = new CancellationTokenSource())
            {
                int metadataPublished = 0;
                bool cancelled = false;
                try
                {
                    _ = AssetImportWorkflow.ImportFiles(
                        cancelledBatch.Database,
                        cancelledBatch.Target,
                        new[] { cancelOne, cancelTwo },
                        cancellation.Token,
                        new AssetImportTestHooks(
                            Observe: checkpoint =>
                            {
                                if (checkpoint ==
                                    AssetImportCheckpoint.MetadataPublished)
                                {
                                    metadataPublished++;
                                }
                                if (checkpoint == AssetImportCheckpoint.Prepared &&
                                    metadataPublished == 1)
                                {
                                    cancellation.Cancel();
                                }
                            }));
                }
                catch (OperationCanceledException)
                {
                    cancelled = true;
                }
                Check(cancelled &&
                      !File.Exists(Path.Combine(
                          cancelledBatch.Target,
                          "one.bin")) &&
                      !File.Exists(Path.Combine(
                          cancelledBatch.Target,
                          "two.bin")) &&
                      cancelledBatch.Database.Snapshot().Count == 0 &&
                      TransactionDirectories(
                          cancelledBatch.Assets,
                          "import-staging").Length == 0,
                    "batch cancellation rolls back both the current input and prior successes");
            }

            TestProject ambiguous = NewProject("ambiguous");
            string ambiguousSource = WriteSource(
                ambiguous,
                "ambiguous.bin",
                "ambiguous");
            _ = SimulateImportCrash(
                ambiguous,
                ambiguousSource,
                AssetImportCheckpoint.AssetPublished);
            string ambiguousTransaction = TransactionDirectories(
                ambiguous.Assets,
                "import-staging").Single();
            File.Delete(Path.Combine(
                ambiguousTransaction,
                "payload.acsmeta"));
            AssetImportReconciliationResult ambiguousRecovery =
                AssetImportWorkflow.Reconcile(ambiguous.Database);
            string ambiguousDestination = Path.Combine(
                ambiguous.Target,
                "ambiguous.bin");
            Check(ambiguousRecovery.PreservedTransactions == 1 &&
                  File.Exists(ambiguousDestination) &&
                  !File.Exists(
                      ambiguousDestination + AssetDatabase.MetadataSuffix) &&
                  Directory.Exists(ambiguousTransaction),
                "ambiguous recovery preserves evidence and never synthesizes a missing sidecar");

            TestProject locked = NewProject("locked");
            string lockedSource = WriteSource(locked, "locked.bin", "locked");
            bool lockRejected = false;
            using (AssetMutationLockProcessHolder holder =
                   AssetMutationLockProcessHolder.Start(locked.Assets))
            {
                try
                {
                    _ = AssetImportWorkflow.ImportFiles(
                        locked.Database,
                        locked.Target,
                        new[] { lockedSource });
                }
                catch (IOException)
                {
                    lockRejected = true;
                }
            }
            Check(lockRejected &&
                  !Directory.Exists(Path.Combine(
                      locked.Assets,
                      AssetDatabase.InternalDirectoryName,
                      "import-staging")),
                "cross-process project lock rejects import before staging");

            TestProject reimport = NewProject("reimport");
            string reimportSource = WriteSource(
                reimport,
                "texture.png",
                "version-one");
            AssetImportOperationResult firstImport =
                AssetImportWorkflow.ImportFiles(
                    reimport.Database,
                    reimport.Target,
                    new[] { reimportSource });
            string reimportDestination =
                firstImport.Imported.Single().DestinationPath;
            _ = reimport.Database.TryGetByPath(
                reimportDestination,
                out AssetRecord? beforeReimport);
            File.WriteAllText(
                reimportSource,
                "version-two",
                new UTF8Encoding(false));
            AssetReimportOperationResult reimported =
                AssetReimportWorkflow.Reimport(
                    reimport.Database,
                    beforeReimport!.AssetId);
            Check(reimported.Asset.AssetId == beforeReimport.AssetId &&
                  File.ReadAllText(reimportDestination) == "version-two" &&
                  reimported.Asset.Metadata.Source.Replace(
                      '/',
                      Path.DirectorySeparatorChar) ==
                  Path.GetFullPath(reimportSource),
                "Reimport transaction replaces content while preserving stable Asset ID");

            File.Delete(reimportSource);
            Check(!AssetReimportWorkflow.CanReimport(
                    reimport.Database,
                    beforeReimport.AssetId,
                    out _),
                "Reimport is disabled when its authoritative source is unavailable");

            TestProject reimportFailure = NewProject("reimport-failure");
            string reimportFailureSource = WriteSource(
                reimportFailure,
                "replace.bin",
                "old-version");
            AssetImportOperationResult reimportFailureImport =
                AssetImportWorkflow.ImportFiles(
                    reimportFailure.Database,
                    reimportFailure.Target,
                    new[] { reimportFailureSource });
            string reimportFailureDestination =
                reimportFailureImport.Imported.Single().DestinationPath;
            _ = reimportFailure.Database.TryGetByPath(
                reimportFailureDestination,
                out AssetRecord? oldRecord);
            File.WriteAllText(
                reimportFailureSource,
                "new-version",
                new UTF8Encoding(false));
            bool reimportRolledBack = false;
            try
            {
                _ = AssetReimportWorkflow.Reimport(
                    reimportFailure.Database,
                    oldRecord!.AssetId,
                    testHooks: new AssetReimportTestHooks(
                        FailAfter: AssetReimportCheckpoint.AssetPublished));
            }
            catch (IOException)
            {
                reimportRolledBack = true;
            }
            Check(reimportRolledBack &&
                  File.ReadAllText(reimportFailureDestination) ==
                  "old-version" &&
                  reimportFailure.Database.TryGetByAssetId(
                      oldRecord!.AssetId,
                      out AssetRecord? restoredRecord) &&
                  restoredRecord?.ContentHash == oldRecord.ContentHash &&
                  TransactionDirectories(
                      reimportFailure.Assets,
                      "reimport-staging").Length == 0,
                "Reimport failure restores original payload, sidecar, identity, and index");

            TestProject reimportBackupCrash = NewProject("reimport-backup-crash");
            (string backupSource, AssetRecord backupRecord) =
                PrepareReimport(reimportBackupCrash, "backup.bin", "old", "new");
            bool backupTerminated = SimulateReimportCrash(
                reimportBackupCrash,
                backupRecord.AssetId,
                AssetReimportCheckpoint.OriginalsBackedUp);
            bool ordinaryCreationBlocked = false;
            string blockedFolder = Path.Combine(
                reimportBackupCrash.Target,
                "NewFolder");
            try
            {
                _ = AssetCreationWorkflow.Create(
                    reimportBackupCrash.Assets,
                    reimportBackupCrash.Target,
                    AcsAssetTemplate.Folder);
            }
            catch (IOException)
            {
                ordinaryCreationBlocked = true;
            }
            Check(ordinaryCreationBlocked &&
                  !Directory.Exists(blockedFolder) &&
                  TransactionDirectories(
                      reimportBackupCrash.Assets,
                      "reimport-staging").Length == 1,
                "ordinary asset mutation is blocked before writing while Reimport recovery is pending");
            AssetImportReconciliationResult backupRecovery =
                AssetReimportWorkflow.Reconcile(
                    reimportBackupCrash.Database);
            _ = reimportBackupCrash.Database.Refresh(verifyContent: true);
            Check(backupTerminated &&
                  backupRecovery.DiscardedTransactions == 1 &&
                  File.ReadAllText(backupRecord.FullPath) == "old" &&
                  reimportBackupCrash.Database.TryGetByAssetId(
                      backupRecord.AssetId,
                      out _),
                "startup restores originals when Reimport publication never began");

            TestProject reimportPublishCrash = NewProject("reimport-publish-crash");
            (string publishSource, AssetRecord publishRecord) =
                PrepareReimport(
                    reimportPublishCrash,
                    "publish.bin",
                    "old",
                    "new");
            bool publishTerminated = SimulateReimportCrash(
                reimportPublishCrash,
                publishRecord.AssetId,
                AssetReimportCheckpoint.AssetPublished);
            bool interruptedReimportRefreshBlocked = false;
            try
            {
                _ = reimportPublishCrash.Database.Refresh(verifyContent: true);
            }
            catch (IOException)
            {
                interruptedReimportRefreshBlocked = true;
            }
            Check(interruptedReimportRefreshBlocked &&
                  !File.Exists(
                      publishRecord.FullPath + AssetDatabase.MetadataSuffix),
                "ordinary Refresh cannot synthesize metadata over an interrupted Reimport");
            AssetImportReconciliationResult publishRecovery =
                AssetReimportWorkflow.Reconcile(
                    reimportPublishCrash.Database);
            _ = reimportPublishCrash.Database.Refresh(verifyContent: true);
            Check(publishTerminated &&
                  publishRecovery.CompletedTransactions == 1 &&
                  File.ReadAllText(publishRecord.FullPath) == "new" &&
                  reimportPublishCrash.Database.TryGetByAssetId(
                      publishRecord.AssetId,
                      out AssetRecord? publishRecovered) &&
                  publishRecovered?.Metadata.Source.Replace(
                      '/',
                      Path.DirectorySeparatorChar) ==
                  Path.GetFullPath(publishSource),
                "startup completes Reimport metadata after replacement publication");

            TestProject missingJournal = NewProject("reimport-missing-journal");
            (_, AssetRecord missingJournalRecord) =
                PrepareReimport(
                    missingJournal,
                    "missing.bin",
                    "old",
                    "new");
            _ = SimulateReimportCrash(
                missingJournal,
                missingJournalRecord.AssetId,
                AssetReimportCheckpoint.OriginalsBackedUp);
            string missingJournalTransaction = TransactionDirectories(
                missingJournal.Assets,
                "reimport-staging").Single();
            File.Delete(Path.Combine(
                missingJournalTransaction,
                "manifest.v1.json"));
            AssetImportReconciliationResult missingJournalRecovery =
                AssetReimportWorkflow.Reconcile(missingJournal.Database);
            Check(missingJournalRecovery.PreservedTransactions == 1 &&
                  File.Exists(Path.Combine(
                      missingJournalTransaction,
                      "original-payload")) &&
                  File.Exists(Path.Combine(
                      missingJournalTransaction,
                      "original-payload.acsmeta")),
                "journal-less Reimport recovery never deletes the only original backups");

            TestProject reimportLocked = NewProject("reimport-locked");
            (_, AssetRecord lockedRecord) = PrepareReimport(
                reimportLocked,
                "locked-reimport.bin",
                "old",
                "new");
            bool reimportLockRejected = false;
            using (AssetMutationLockProcessHolder holder =
                   AssetMutationLockProcessHolder.Start(reimportLocked.Assets))
            {
                try
                {
                    _ = AssetReimportWorkflow.Reimport(
                        reimportLocked.Database,
                        lockedRecord.AssetId);
                }
                catch (IOException)
                {
                    reimportLockRejected = true;
                }
            }
            Check(reimportLockRejected &&
                  File.ReadAllText(lockedRecord.FullPath) == "old",
                "cross-process project lock rejects Reimport before backup");

            TestProject linked = NewProject("linked");
            string linkedOutside = Path.Combine(root, "linked-outside");
            Directory.CreateDirectory(linkedOutside);
            string linkedDirectory = Path.Combine(linked.Assets, "Linked");
            try
            {
                Directory.CreateSymbolicLink(
                    linkedDirectory,
                    linkedOutside);
                string linkedSource = WriteSource(
                    linked,
                    "link.bin",
                    "link");
                bool linkRejected = false;
                try
                {
                    _ = AssetImportWorkflow.ImportFiles(
                        linked.Database,
                        linkedDirectory,
                        new[] { linkedSource });
                }
                catch (InvalidDataException)
                {
                    linkRejected = true;
                }
                Check(linkRejected &&
                      !File.Exists(Path.Combine(linkedOutside, "link.bin")),
                    "import refuses a nested destination reparse point");
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or IOException or
                    NotSupportedException)
            {
                output.WriteLine(
                    "SKIP: import reparse-point runtime test: " +
                    error.Message);
            }
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
            }
        }

        output.WriteLine(
            $"Asset import self-test: {passed} passed, {failed} failed");
        return failed;
    }

    private static bool RejectRecursiveImport(
        TestProject project,
        string sourcePath,
        AssetImportTraversalLimits limits)
    {
        try
        {
            _ = AssetImportWorkflow.ImportExternalPaths(
                project.Database,
                project.Target,
                new[] { sourcePath },
                traversalLimits: limits);
            return false;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            return true;
        }
    }

    private static bool SimulateImportCrash(
        TestProject project,
        string source,
        AssetImportCheckpoint checkpoint)
    {
        try
        {
            _ = AssetImportWorkflow.ImportFiles(
                project.Database,
                project.Target,
                new[] { source },
                testHooks: new AssetImportTestHooks(
                    SimulateCrashAfter: checkpoint));
            return false;
        }
        catch (AssetImportSimulatedCrashException)
        {
            return true;
        }
    }

    private static bool SimulateRecursiveImportCrash(
        TestProject project,
        string source,
        AssetImportCheckpoint checkpoint,
        Action<AssetImportCheckpoint>? observe = null)
    {
        try
        {
            _ = AssetImportWorkflow.ImportExternalPaths(
                project.Database,
                project.Target,
                new[] { source },
                testHooks: new AssetImportTestHooks(
                    SimulateCrashAfter: checkpoint,
                    Observe: observe));
            return false;
        }
        catch (AssetImportSimulatedCrashException)
        {
            return true;
        }
    }

    private static (string Source, AssetRecord Record) PrepareReimport(
        TestProject project,
        string fileName,
        string oldContents,
        string newContents)
    {
        string source = Path.Combine(project.Sources, fileName);
        File.WriteAllText(source, oldContents, new UTF8Encoding(false));
        AssetImportOperationResult imported =
            AssetImportWorkflow.ImportFiles(
                project.Database,
                project.Target,
                new[] { source });
        if (!project.Database.TryGetByPath(
                imported.Imported.Single().DestinationPath,
                out AssetRecord? record) ||
            record == null)
        {
            throw new InvalidDataException(
                "Reimport test preparation did not index the imported asset.");
        }
        File.WriteAllText(source, newContents, new UTF8Encoding(false));
        return (source, record);
    }

    private static bool SimulateReimportCrash(
        TestProject project,
        string assetId,
        AssetReimportCheckpoint checkpoint)
    {
        try
        {
            _ = AssetReimportWorkflow.Reimport(
                project.Database,
                assetId,
                testHooks: new AssetReimportTestHooks(
                    SimulateCrashAfter: checkpoint));
            return false;
        }
        catch (AssetReimportSimulatedCrashException)
        {
            return true;
        }
    }

    private static string[] TransactionDirectories(
        string assets,
        string stagingName)
    {
        string staging = Path.Combine(
            assets,
            AssetDatabase.InternalDirectoryName,
            stagingName);
        return Directory.Exists(staging)
            ? Directory.GetDirectories(staging)
            : Array.Empty<string>();
    }

    private sealed record TestProject(
        string Root,
        string Assets,
        string Target,
        string Sources,
        AssetDatabase Database);
}
