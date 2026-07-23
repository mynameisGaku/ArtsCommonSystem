// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal static class AssetTrashWorkflowSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-trash-selftest-" + Guid.NewGuid().ToString("N"));
        string project = Path.Combine(root, "Project");
        string assets = Path.Combine(project, "Assets");

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

        static bool Throws<T>(Action action) where T : Exception
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

        static bool RejectsForHeldMutationLock(Action action)
        {
            try
            {
                action();
                return false;
            }
            catch (IOException error)
            {
                return error.Message.Contains(
                           "another editor",
                           StringComparison.OrdinalIgnoreCase) &&
                       error.Message.Contains(
                           "mutation lock",
                           StringComparison.OrdinalIgnoreCase);
            }
        }

        static void Write(string path, string value)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, value, new UTF8Encoding(false));
        }

        static string RequireId(AssetDatabase database, string path)
        {
            if (!database.TryGetByPath(path, out AssetRecord? record) ||
                record == null)
            {
                throw new InvalidDataException("Self-test asset was not indexed: " + path);
            }
            return record.AssetId;
        }

        try
        {
            Directory.CreateDirectory(assets);
            var database = new AssetDatabase(project, assets);

            string material = Path.Combine(assets, "Water.acsmat");
            string graph = material + ".graph.json";
            Write(material, "native-material");
            Write(graph, "{\"nodes\":[]}");
            database.Refresh(verifyContent: true);
            string graphMetadata = graph + AssetDatabase.MetadataSuffix;
            Write(graphMetadata, "graph-companion-metadata");
            string materialId = RequireId(database, material);
            byte[] materialMetadata = File.ReadAllBytes(
                material + AssetDatabase.MetadataSuffix);
            byte[] graphMetadataBytes = File.ReadAllBytes(graphMetadata);

            var workflow = new AssetTrashWorkflow(database);
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.Trash(new[] { material })) &&
                    File.Exists(material) &&
                    File.Exists(material + AssetDatabase.MetadataSuffix) &&
                    File.Exists(graph) &&
                    File.Exists(graphMetadata),
                    "a separate process mutation lock rejects Trash before any mutation");
            }
            AssetTrashResult trashedMaterial = workflow.Trash(new[] { material });
            Check(
                !File.Exists(material) &&
                !File.Exists(material + AssetDatabase.MetadataSuffix) &&
                !File.Exists(graph) &&
                !File.Exists(graphMetadata) &&
                !database.TryGetByAssetId(materialId, out _),
                "released project mutation lock can be reacquired and Trash publishes normally");
            IReadOnlyList<AssetTrashEntry> materialEntries = workflow.ListEntries();
            Check(
                materialEntries.Count == 1 &&
                materialEntries[0].EntryId == trashedMaterial.EntryId &&
                materialEntries[0].OriginalRelativePaths.SequenceEqual(
                    new[] { "Water.acsmat" }) &&
                materialEntries[0].StoredBytes > 0,
                "published manifest exposes stable identity, original paths, and accounting");
            Check(
                workflow.InspectRestore(trashedMaterial.EntryId).CanRestore,
                "restore inspection accepts vacant original material paths");

            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.Restore(trashedMaterial.EntryId)) &&
                    !File.Exists(material) &&
                    workflow.ListEntries().Any(
                        entry => entry.EntryId == trashedMaterial.EntryId),
                    "a separate process mutation lock rejects Restore before claiming an entry");
            }
            AssetTrashRestoreResult restoredMaterial =
                workflow.Restore(trashedMaterial.EntryId);
            Check(
                restoredMaterial.DeferredCleanupPath == null &&
                File.Exists(material) &&
                File.Exists(graph) &&
                File.ReadAllBytes(material + AssetDatabase.MetadataSuffix)
                    .SequenceEqual(materialMetadata) &&
                File.ReadAllBytes(graphMetadata).SequenceEqual(graphMetadataBytes) &&
                RequireId(database, material) == materialId,
                "restore preserves asset GUID, sidecar bytes, and material graph companions");
            Check(
                workflow.ListEntries().Count == 0,
                "successful restore consumes its published trash entry");

            string crossEditorProtected = Path.Combine(
                assets,
                "CrossEditorTrashProtected.acsmat");
            string crossEditorOwner = Path.Combine(
                assets,
                "CrossEditorTrashOwner.txt");
            Write(crossEditorProtected, "native-material");
            Write(crossEditorOwner, "metadata-only cross-editor reference");
            database.Refresh(verifyContent: true);
            string crossEditorProtectedId = RequireId(database, crossEditorProtected);
            string crossEditorOwnerId = RequireId(database, crossEditorOwner);
            var competingDatabase = new AssetDatabase(project, assets);
            competingDatabase.Refresh(verifyContent: true);
            competingDatabase.TryGetByPath(
                crossEditorOwner,
                out AssetRecord? competingOwner);
            competingDatabase.UpdateImportMetadata(
                competingOwner!.AssetId,
                competingOwner.Metadata.Source,
                competingOwner.Metadata.Importer,
                competingOwner.Metadata.ImporterVersion,
                new[] { crossEditorProtectedId },
                competingOwner.Metadata.ImportSettings);
            Check(
                Throws<AssetOperationBlockedException>(() =>
                    workflow.Trash(new[] { crossEditorProtected })) &&
                File.Exists(crossEditorProtected) &&
                database.GetDirectReferencers(crossEditorProtectedId).Any(
                    referencer => string.Equals(
                        referencer.AssetId,
                        crossEditorOwnerId,
                        StringComparison.OrdinalIgnoreCase)),
                "Trash refreshes under its lease and honors a competing metadata dependency");

            string trashRollback = Path.Combine(assets, "TrashRollback.acsbp");
            Write(trashRollback, "ACSBP 1\n");
            database.Refresh(verifyContent: true);
            byte[] trashRollbackMetadata = File.ReadAllBytes(
                trashRollback + AssetDatabase.MetadataSuffix);
            var failingTrash = new AssetTrashWorkflow(
                database,
                (point, count) =>
                {
                    if (point == AssetTrashFaultPoint.AfterTrashItemMoved && count == 1)
                        throw new InvalidOperationException("injected trash failure");
                });
            Check(
                Throws<InvalidOperationException>(() =>
                    failingTrash.Trash(new[] { trashRollback })) &&
                File.Exists(trashRollback) &&
                File.ReadAllBytes(trashRollback + AssetDatabase.MetadataSuffix)
                    .SequenceEqual(trashRollbackMetadata) &&
                workflow.ListEntries().Count == 0,
                "trash mutation rolls back byte-identical source family on failure");

            string restoreRollback = Path.Combine(assets, "RestoreRollback.acsbp");
            Write(restoreRollback, "ACSBP 1\n");
            database.Refresh(verifyContent: true);
            string restoreRollbackId = RequireId(database, restoreRollback);
            AssetTrashResult rollbackEntry = workflow.Trash(new[] { restoreRollback });
            var failingRestore = new AssetTrashWorkflow(
                database,
                (point, count) =>
                {
                    if (point == AssetTrashFaultPoint.AfterRestoreItemMoved && count == 1)
                        throw new InvalidOperationException("injected restore failure");
                });
            Check(
                Throws<InvalidOperationException>(() =>
                    failingRestore.Restore(rollbackEntry.EntryId)) &&
                !File.Exists(restoreRollback) &&
                workflow.ListEntries().Any(
                    entry => entry.EntryId == rollbackEntry.EntryId),
                "restore failure rolls moved items back under the original trash entry");
            workflow.Restore(rollbackEntry.EntryId);
            Check(
                RequireId(database, restoreRollback) == restoreRollbackId,
                "retry after restore rollback succeeds without changing identity");

            string collision = Path.Combine(assets, "Collision.txt");
            Write(collision, "original");
            database.Refresh(verifyContent: true);
            AssetTrashResult collisionEntry = workflow.Trash(new[] { collision });
            Write(collision, "replacement");
            AssetTrashRestoreInspection collisionInspection =
                workflow.InspectRestore(collisionEntry.EntryId);
            Check(
                !collisionInspection.CanRestore &&
                collisionInspection.Collisions.Any(
                    path => path.Contains("Collision.txt", StringComparison.Ordinal)) &&
                Throws<AssetTrashCollisionException>(() =>
                    workflow.Restore(collisionEntry.EntryId)) &&
                File.ReadAllText(collision) == "replacement" &&
                workflow.ListEntries().Any(
                    entry => entry.EntryId == collisionEntry.EntryId),
                "restore collision is reported without overwriting replacement content");
            File.Delete(collision);
            string collisionMetadata = collision + AssetDatabase.MetadataSuffix;
            Write(collisionMetadata, "replacement-sidecar");
            Check(
                !workflow.InspectRestore(collisionEntry.EntryId).CanRestore &&
                Throws<AssetTrashCollisionException>(() =>
                    workflow.Restore(collisionEntry.EntryId)) &&
                File.ReadAllText(collisionMetadata) == "replacement-sidecar",
                "a newly occupied sidecar path also blocks the complete family restore");
            File.Delete(collisionMetadata);
            workflow.Restore(collisionEntry.EntryId);
            Check(
                File.ReadAllText(collision) == "original",
                "collision-free retry restores the original payload");

            string pack = Path.Combine(assets, "Pack");
            string first = Path.Combine(pack, "First.txt");
            string second = Path.Combine(pack, "Nested", "Second.txt");
            Write(first, "first");
            Write(second, "second");
            database.Refresh(verifyContent: true);
            string firstId = RequireId(database, first);
            string secondId = RequireId(database, second);
            AssetTrashResult folderEntry = workflow.Trash(new[] { second, pack });
            Check(
                !Directory.Exists(pack) &&
                folderEntry.AssetCount == 2 &&
                folderEntry.FolderCount >= 2,
                "nested selection collapses to one folder-tree trash payload");
            workflow.Restore(folderEntry.EntryId);
            Check(
                RequireId(database, first) == firstId &&
                RequireId(database, second) == secondId,
                "folder-tree restore preserves every nested asset identity");

            string outside = Path.Combine(root, "Outside.txt");
            Write(outside, "outside");
            Check(
                Throws<InvalidDataException>(() => workflow.Trash(new[] { outside })) &&
                Throws<InvalidDataException>(() =>
                    workflow.Trash(new[] { graph })),
                "outside, sidecar, and material graph direct targets are rejected");

            string escape = Path.Combine(assets, "Escape.txt");
            Write(escape, "escape");
            database.Refresh(verifyContent: true);
            AssetTrashResult escapeEntry = workflow.Trash(new[] { escape });
            string manifestPath = Path.Combine(
                workflow.TrashRoot,
                "entries",
                escapeEntry.EntryId,
                "manifest.v1.json");
            string manifestText = File.ReadAllText(manifestPath);
            string tampered = manifestText.Replace(
                "\"relativePath\": \"Escape.txt\"",
                "\"relativePath\": \"../../Outside.txt\"",
                StringComparison.Ordinal);
            File.WriteAllText(manifestPath, tampered, new UTF8Encoding(false));
            Check(
                !string.Equals(manifestText, tampered, StringComparison.Ordinal) &&
                Throws<InvalidDataException>(() =>
                    workflow.InspectRestore(escapeEntry.EntryId)) &&
                File.ReadAllText(outside) == "outside",
                "tampered manifest path escape fails closed before restore mutation");
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                Check(
                    RejectsForHeldMutationLock(() => workflow.EmptyTrash()) &&
                    Directory.Exists(Path.GetDirectoryName(manifestPath)),
                    "a separate process mutation lock rejects Empty Trash before unpublishing");
            }
            AssetTrashCleanupResult corruptCleanup = workflow.EmptyTrash();
            Check(
                corruptCleanup.RemovedEntries == 1 &&
                corruptCleanup.PurgedOriginalRelativePaths.Count == 0 &&
                workflow.ListEntries().Count == 0,
                "Empty Trash removes an ordinary corrupt entry without trusting its manifest");

            string callbackRollbackPath = Path.Combine(assets, "CallbackRollback.txt");
            Write(callbackRollbackPath, "keep");
            database.Refresh(verifyContent: true);
            AssetTrashResult callbackRollbackEntry =
                workflow.Trash(new[] { callbackRollbackPath });
            Check(
                Throws<IOException>(() =>
                    workflow.EmptyTrash(_ =>
                        throw new IOException("injected saved-source cleanup failure"))) &&
                workflow.ListEntries().Count == 1 &&
                workflow.ListEntries()[0].EntryId == callbackRollbackEntry.EntryId &&
                !File.Exists(callbackRollbackPath),
                "failed permanent-delete callback rolls a valid entry back to published Trash");
            workflow.EmptyTrash();

            string linkedTarget = Path.Combine(root, "LinkedTarget");
            string linked = Path.Combine(assets, "Linked");
            Directory.CreateDirectory(linkedTarget);
            try
            {
                Directory.CreateSymbolicLink(linked, linkedTarget);
                Check(
                    Throws<InvalidDataException>(() => workflow.Trash(new[] { linked })),
                    "reparse-point target is rejected before quarantine mutation");
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or IOException or NotSupportedException)
            {
                output.WriteLine("SKIP: reparse-point runtime test: " + error.Message);
            }
            finally
            {
                try
                {
                    if (Directory.Exists(linked))
                        Directory.Delete(linked, recursive: false);
                }
                catch
                {
                }
            }

            string unsafeLockAssets = Path.Combine(root, "UnsafeLockProject", "Assets");
            string unsafeLockTarget = Path.Combine(root, "UnsafeLockTarget");
            string unsafeDatabaseLink = Path.Combine(
                unsafeLockAssets,
                AssetDatabase.InternalDirectoryName);
            Directory.CreateDirectory(unsafeLockAssets);
            Directory.CreateDirectory(unsafeLockTarget);
            try
            {
                Directory.CreateSymbolicLink(unsafeDatabaseLink, unsafeLockTarget);
                Check(
                    Throws<InvalidDataException>(() =>
                        AssetMutationLock.Acquire(
                            unsafeLockAssets,
                            "Unsafe lock path self-test").Dispose()),
                    "project mutation lock rejects a reparse-point .acsdb directory");
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or IOException or NotSupportedException)
            {
                output.WriteLine(
                    "SKIP: mutation-lock reparse runtime test: " + error.Message);
            }
            finally
            {
                try
                {
                    if (Directory.Exists(unsafeDatabaseLink))
                        Directory.Delete(unsafeDatabaseLink, recursive: false);
                }
                catch
                {
                }
            }

            string retainedA = Path.Combine(assets, "RetainedA.txt");
            string retainedB = Path.Combine(assets, "RetainedB.txt");
            Write(retainedA, "a");
            Write(retainedB, "b");
            database.Refresh(verifyContent: true);
            workflow.Trash(new[] { retainedA });
            workflow.Trash(new[] { retainedB });
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.ApplyRetention(
                            new AssetTrashRetentionPolicy(
                                TimeSpan.FromDays(365),
                                MaxEntries: 1,
                                MaxBytes: long.MaxValue))) &&
                    workflow.ListEntries().Count == 2,
                    "a separate process mutation lock rejects retention before purging");
            }
            AssetTrashCleanupResult retention = workflow.ApplyRetention(
                new AssetTrashRetentionPolicy(
                    TimeSpan.FromDays(365),
                    MaxEntries: 1,
                    MaxBytes: long.MaxValue));
            Check(
                retention.RemovedEntries == 1 &&
                workflow.ListEntries().Count == 1,
                "retention enforces newest-first entry count without touching the kept entry");
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                Check(
                    RejectsForHeldMutationLock(() => workflow.EmptyTrash()) &&
                    workflow.ListEntries().Count == 1,
                    "Empty Trash keeps published entries intact while another editor owns the lock");
            }
            AssetTrashCleanupResult emptied = workflow.EmptyTrash();
            string[] retainedPurges = retention.PurgedOriginalRelativePaths
                .Concat(emptied.PurgedOriginalRelativePaths)
                .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
                .ToArray();
            Check(
                emptied.RemovedEntries == 1 &&
                emptied.DeferredPaths.Count == 0 &&
                retainedPurges.SequenceEqual(
                    new[] { "RetainedA.txt", "RetainedB.txt" },
                    StringComparer.OrdinalIgnoreCase) &&
                workflow.ListEntries().Count == 0,
                "permanent purge reports original roots and removes all ready entries");

            string ageLimited = Path.Combine(assets, "AgeLimited.txt");
            Write(ageLimited, "age");
            database.Refresh(verifyContent: true);
            workflow.Trash(new[] { ageLimited });
            AssetTrashCleanupResult aged = workflow.ApplyRetention(
                new AssetTrashRetentionPolicy(
                    TimeSpan.FromDays(1),
                    MaxEntries: 10,
                    MaxBytes: long.MaxValue),
                nowUtc: DateTimeOffset.UtcNow.AddDays(2));
            Check(
                aged.RemovedEntries == 1 &&
                aged.PurgedOriginalRelativePaths.Single() == "AgeLimited.txt" &&
                workflow.ListEntries().Count == 0,
                "retention expires entries older than the configured maximum age");

            string byteLimited = Path.Combine(assets, "ByteLimited.txt");
            Write(byteLimited, "bytes");
            database.Refresh(verifyContent: true);
            workflow.Trash(new[] { byteLimited });
            AssetTrashCleanupResult overBudget = workflow.ApplyRetention(
                new AssetTrashRetentionPolicy(
                    TimeSpan.FromDays(365),
                    MaxEntries: 10,
                    MaxBytes: 0));
            Check(
                overBudget.RemovedEntries == 1 &&
                overBudget.PurgedOriginalRelativePaths.Single() == "ByteLimited.txt" &&
                workflow.ListEntries().Count == 0,
                "retention enforces stored-byte budget including sidecars");
            Check(
                Throws<ArgumentOutOfRangeException>(() =>
                    workflow.ApplyRetention(
                        new AssetTrashRetentionPolicy(
                            TimeSpan.FromDays(-1),
                            1,
                            1))) &&
                workflow.ListDeferredTransactionPaths().Count == 0,
                "retention rejects negative limits and successful tests leave no transactions");
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
            $"Asset trash self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
