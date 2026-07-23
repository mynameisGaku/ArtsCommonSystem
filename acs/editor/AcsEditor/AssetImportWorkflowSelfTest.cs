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
