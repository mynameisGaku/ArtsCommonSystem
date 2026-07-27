// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal static class MaterialDocumentHostSelfTest
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
            "acs-material-document-host-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(root);
            string firstPath = Path.Combine(root, "Surface.acsmat");
            string renamedPath = Path.Combine(root, "Renamed.acsmat");
            const string assetId = "0123456789abcdef0123456789abcdef";

            EditorDocumentId persistentFirst =
                MaterialDocumentHostRegistration.CreateId(firstPath, assetId);
            EditorDocumentId persistentRenamed =
                MaterialDocumentHostRegistration.CreateId(renamedPath, assetId);
            Check(
                persistentFirst == persistentRenamed &&
                persistentFirst.StableId == "asset:" + assetId,
                "material document identity follows the persistent Asset ID across rename/move");

            EditorDocumentId looseFirst =
                MaterialDocumentHostRegistration.CreateId(firstPath);
            EditorDocumentId looseAlias =
                MaterialDocumentHostRegistration.CreateId(
                    Path.Combine(root, ".", "Surface.acsmat"));
            Check(
                looseFirst == looseAlias,
                "loose material identity canonicalizes equivalent absolute paths");
            bool malformedRejected = false;
            try
            {
                _ = MaterialDocumentHostRegistration.CreateId(
                    firstPath,
                    "not-an-asset-id");
            }
            catch (ArgumentException)
            {
                malformedRejected = true;
            }
            Check(
                malformedRejected,
                "a supplied malformed Asset ID is rejected instead of changing identity domain");

            string projectRoot = Path.Combine(root, "PreIndexProject");
            string assetsRoot = Path.Combine(projectRoot, "Assets");
            Directory.CreateDirectory(assetsRoot);
            string projectManifest =
                Path.Combine(projectRoot, "PreIndexProject.acsproject");
            File.WriteAllText(projectManifest, "{}");
            var project = new Project
            {
                ProjectFilePath = projectManifest,
                Name = "PreIndexProject",
            };
            string preIndexPath =
                Path.Combine(assetsRoot, "PreIndex.acsmat");
            File.WriteAllText(preIndexPath, "ACSMAT 1\n");
            var seededDatabase =
                new AssetDatabase(projectRoot, assetsRoot);
            _ = seededDatabase.Refresh(verifyContent: true);
            bool seeded =
                seededDatabase.TryGetByPath(
                    preIndexPath,
                    out AssetRecord? seededRecord) &&
                seededRecord != null;
            var coldDatabase =
                new AssetDatabase(projectRoot, assetsRoot);
            bool browserIndexStillCold =
                !coldDatabase.TryGetByPath(preIndexPath, out _);
            string? preIndexAssetId =
                MaterialDocumentHostRegistration.ResolveAssetIdForOpen(
                    project,
                    preIndexPath,
                    suppliedAssetId: null);
            EditorDocumentId preIndexDocumentId =
                MaterialDocumentHostRegistration.CreateId(
                    preIndexPath,
                    preIndexAssetId);
            Check(
                seeded &&
                browserIndexStillCold &&
                preIndexAssetId == seededRecord!.AssetId &&
                preIndexDocumentId.StableId ==
                    "asset:" + seededRecord.AssetId,
                "pre-index Inspector open reads the authoritative material sidecar before host registration");

            string missingSidecarPath =
                Path.Combine(assetsRoot, "MissingSidecar.acsmat");
            File.WriteAllText(missingSidecarPath, "ACSMAT 1\n");
            bool missingSidecarRejected = false;
            try
            {
                _ = MaterialDocumentHostRegistration.ResolveAssetIdForOpen(
                    project,
                    missingSidecarPath,
                    suppliedAssetId: null);
            }
            catch (FileNotFoundException)
            {
                missingSidecarRejected = true;
            }
            Check(
                missingSidecarRejected,
                "project material without authoritative metadata never falls back to path identity");

            var existing = new[] { 1, 2, 3 };
            var attempted = new System.Collections.Generic.List<int>();
            bool rolledBack = false;
            bool existingFailureRejected = false;
            try
            {
                MaterialDocumentHostRegistration
                    .RequireEveryExistingDocumentHosted(
                        existing,
                        item =>
                        {
                            attempted.Add(item);
                            return item != 2;
                        },
                        () => rolledBack = true);
            }
            catch (InvalidOperationException)
            {
                existingFailureRejected = true;
            }
            Check(
                existingFailureRejected &&
                rolledBack &&
                attempted.SequenceEqual(new[] { 1, 2 }),
                "Document Host initialization rolls back when any existing Material Editor cannot register");

            var registrationFault =
                new InvalidOperationException("registration fault");
            var rollbackFault = new IOException("rollback fault");
            AggregateException? combinedFailure = null;
            try
            {
                MaterialDocumentHostRegistration
                    .RequireEveryExistingDocumentHosted(
                        new[] { 1 },
                        _ => throw registrationFault,
                        () => throw rollbackFault);
            }
            catch (AggregateException ex)
            {
                combinedFailure = ex.Flatten();
            }
            Check(
                combinedFailure != null &&
                combinedFailure.InnerExceptions.Any(
                    error => ReferenceEquals(error, registrationFault)) &&
                combinedFailure.InnerExceptions.Any(
                    error => ReferenceEquals(error, rollbackFault)),
                "Document Host initialization preserves both registration and rollback failures");

            string retryFirstState = "dirty-first";
            string retrySecondState = "dirty-second";
            EditorDocument retryFirst =
                MaterialDocumentHostRegistration.Create(
                    Path.Combine(root, "RetryFirst.acsmat"),
                    "11111111111111111111111111111111",
                    "RetryFirst.acsmat",
                    () => EditorDocumentState.Text(retryFirstState),
                    restored => retryFirstState = restored.Payload,
                    _ => ValueTask.FromResult(
                        EditorDocumentSaveResult.Saved(retryFirstState)),
                    initiallySaved: false);
            EditorDocument retrySecond =
                MaterialDocumentHostRegistration.Create(
                    Path.Combine(root, "RetrySecond.acsmat"),
                    "22222222222222222222222222222222",
                    "RetrySecond.acsmat",
                    () => EditorDocumentState.Text(retrySecondState),
                    restored => retrySecondState = restored.Payload,
                    _ => ValueTask.FromResult(
                        EditorDocumentSaveResult.Saved(retrySecondState)),
                    initiallySaved: false);
            var retryHost = new EditorDocumentHost();
            int retryRollbackCount = 0;
            int retryStateNotifications = 0;
            retryHost.DocumentStateChanged +=
                (_, _) => retryStateNotifications++;
            bool partialAttemptRejected = false;
            try
            {
                MaterialDocumentHostRegistration
                    .RequireEveryExistingDocumentHosted(
                        new[] { retryFirst, retrySecond },
                        candidate =>
                        {
                            retryHost.Register(candidate);
                            return !ReferenceEquals(candidate, retrySecond);
                        },
                        () =>
                        {
                            retryRollbackCount++;
                            if (!retryHost.Clear(discardUnsavedChanges: true))
                            {
                                throw new InvalidOperationException(
                                    "retry rollback was rejected");
                            }
                        });
            }
            catch (InvalidOperationException)
            {
                partialAttemptRejected = true;
            }
            bool retrySucceeded = true;
            try
            {
                MaterialDocumentHostRegistration
                    .RequireEveryExistingDocumentHosted(
                        new[] { retryFirst, retrySecond },
                        candidate =>
                        {
                            retryHost.Register(candidate);
                            return true;
                        },
                        () => retryHost.Clear(discardUnsavedChanges: true));
            }
            catch
            {
                retrySucceeded = false;
            }
            retryFirst.UpdatePresentation(
                retryFirst.DisplayName,
                retryFirst.SourcePath);
            Check(
                partialAttemptRejected &&
                retrySucceeded &&
                retryRollbackCount == 1 &&
                retryHost.Documents.Count == 2 &&
                retryHost.DirtyDocuments.Count == 2 &&
                retryFirst.IsDirty &&
                retrySecond.IsDirty &&
                retryStateNotifications == 1,
                "initialization rollback permits a clean retry without duplicate subscriptions or dirty loss");

            string state = "saved";
            int saveCalls = 0;
            bool failWriter = false;
            var host = new EditorDocumentHost();
            EditorDocument document = MaterialDocumentHostRegistration.Create(
                firstPath,
                assetId,
                "Surface.acsmat",
                () => EditorDocumentState.Text(state),
                restored => state = restored.Payload,
                cancellationToken =>
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    saveCalls++;
                    return ValueTask.FromResult(
                        failWriter
                            ? EditorDocumentSaveResult.Failed(
                                "runtime rejected the material graph")
                            : EditorDocumentSaveResult.Saved(state));
                },
                initiallySaved: true);
            host.Register(document);

            state = "dirty";
            document.NotifyPotentialChange();
            document.Synchronize("Edit Material Graph");
            Check(
                document.IsDirty && document.SaveOrder ==
                    MaterialDocumentHostRegistration.SaveOrder,
                "material graph edits become hosted dirty state with deterministic save priority");

            IDisposable transaction =
                document.BeginTransaction("Drag Material Node");
            state = "transaction-open";
            EditorDocumentSaveBatchResult blocked =
                host.SaveAllAsync().AsTask().GetAwaiter().GetResult();
            Check(
                blocked.Completion == EditorDocumentBatchCompletion.Failed &&
                blocked.Diagnostics.Count == 1 &&
                blocked.Diagnostics[0].Detail.Contains(
                    "open transaction",
                    StringComparison.OrdinalIgnoreCase) &&
                saveCalls == 0,
                "Save All fails closed without invoking the writer during an open material transaction");

            transaction.Dispose();
            EditorDocumentSaveBatchResult saved =
                host.SaveAllAsync().AsTask().GetAwaiter().GetResult();
            Check(
                saved.Completion == EditorDocumentBatchCompletion.Success &&
                saved.SavedCount == 1 &&
                !document.IsDirty &&
                saveCalls == 1,
                "Save All commits a completed material transaction and clears dirty state");

            state = "writer-failure";
            document.NotifyPotentialChange();
            document.Synchronize("Edit Material Graph");
            failWriter = true;
            EditorDocumentCloseResult failedClose =
                host.PrepareCloseAsync(EditorDocumentCloseChoice.Save)
                    .AsTask().GetAwaiter().GetResult();
            Check(
                !failedClose.CanClose &&
                failedClose.SaveResult?.Completion ==
                    EditorDocumentBatchCompletion.Failed &&
                document.IsDirty,
                "failed material writer blocks close and preserves dirty state");

            string injectedState = "writer-baseline";
            bool injectedWriterRan = false;
            bool injectedLocalCheckpointCommitted = false;
            EditorDocument injectedFailure =
                MaterialDocumentHostRegistration.Create(
                    Path.Combine(root, "InjectedFailure.acsmat"),
                    "33333333333333333333333333333333",
                    "InjectedFailure.acsmat",
                    () => EditorDocumentState.Text(injectedState),
                    restored => injectedState = restored.Payload,
                    _ =>
                    {
                        EditorDocumentState before =
                            EditorDocumentState.Text(injectedState);
                        injectedWriterRan = true;
                        injectedState = "writer-committed-different-content";
                        EditorDocumentState committed =
                            EditorDocumentState.Text(injectedState);
                        return ValueTask.FromResult(
                            MaterialDocumentHostRegistration
                                .CompleteSuccessfulWrite(
                                    before,
                                    committed,
                                    () =>
                                        injectedLocalCheckpointCommitted =
                                            true));
                    },
                    initiallySaved: true);
            var injectedHost = new EditorDocumentHost();
            injectedHost.Register(injectedFailure);
            injectedState = "dirty-before-injected-write";
            injectedFailure.NotifyPotentialChange();
            injectedFailure.Synchronize("Edit Material Graph");
            EditorDocumentSaveResult injectedResult =
                injectedFailure.SaveAsync()
                    .AsTask().GetAwaiter().GetResult();
            Check(
                injectedResult.Status == EditorDocumentSaveStatus.Failed &&
                injectedWriterRan &&
                !injectedLocalCheckpointCommitted &&
                injectedFailure.IsDirty,
                "post-write fingerprint failure never publishes either local or hosted saved checkpoint");

            EditorDocumentCloseResult discarded =
                host.PrepareCloseAsync(EditorDocumentCloseChoice.Discard)
                    .AsTask().GetAwaiter().GetResult();
            Check(
                discarded.CanClose &&
                discarded.DirtyDocuments.SequenceEqual(
                    new[] { persistentFirst }),
                "explicit Discard is the only hosted close path that may retain unsaved material state");

            document.Suspend(synchronize: true);
            EditorDocumentSaveBatchResult suspended =
                host.SaveAllAsync(CancellationToken.None)
                    .AsTask().GetAwaiter().GetResult();
            Check(
                suspended.Completion == EditorDocumentBatchCompletion.Failed &&
                suspended.Diagnostics.Single().Detail.Contains(
                    "suspended",
                    StringComparison.OrdinalIgnoreCase),
                "path-mutation suspension blocks material writes");
            document.Resume();
        }
        catch (Exception ex)
        {
            failed++;
            output.WriteLine(
                "FAIL: material document host self-test threw " +
                ex.GetType().Name + ": " + ex.Message);
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
                // A failed temp cleanup must not hide the contract result.
            }
        }

        output.WriteLine(
            $"Material document host self-test: passed={passed} failed={failed}");
        return failed;
    }
}
