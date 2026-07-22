// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal static class SceneAutosaveSelfTest
{
    internal static int Run(TextWriter log)
    {
        int failures = 0;
        string root = Path.Combine(
            Path.GetTempPath(), "acs-autosave-selftest-" + Guid.NewGuid().ToString("N"));
        string sourcePath = Path.Combine(root, "project", "Assets", "main.acscene");
        string projectPath = Path.Combine(root, "project", "SelfTest.acsproject");

        void Check(bool condition, string name)
        {
            if (condition)
                log.WriteLine("PASS  " + name);
            else
            {
                ++failures;
                log.WriteLine("FAIL  " + name);
            }
        }

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(sourcePath)!);
            File.WriteAllText(sourcePath, "ORIGINAL-SOURCE", new UTF8Encoding(false));
            File.WriteAllText(projectPath, "{}", new UTF8Encoding(false));

            var store = new SceneAutosaveStore(
                Path.Combine(root, "recovery-root"),
                retention: 3);
            SceneAutosaveIdentity identity2D = SceneAutosaveStore.CreateIdentity(
                projectPath, sourcePath, SceneDocumentMode.TwoD);
            SceneAutosaveIdentity identity3D = SceneAutosaveStore.CreateIdentity(
                projectPath,
                Path.Combine(root, "project", "Assets", "scene3d.acs3d"),
                SceneDocumentMode.ThreeD);
            SceneAutosaveIdentity otherProject = SceneAutosaveStore.CreateIdentity(
                Path.Combine(root, "other", "Other.acsproject"),
                sourcePath,
                SceneDocumentMode.TwoD);

            Check(identity2D.DocumentId != identity3D.DocumentId,
                "2D and 3D documents have distinct identities");
            Check(identity2D.ProjectId != otherProject.ProjectId,
                "projects have distinct identities");
            Check(Path.GetFullPath(store.EntryDirectoryForTest(identity2D)).StartsWith(
                    Path.GetFullPath(Path.Combine(root, "recovery-root")),
                    StringComparison.OrdinalIgnoreCase),
                "hashed identity cannot escape recovery root");

            DateTimeOffset epoch = new(2030, 1, 2, 3, 4, 5, TimeSpan.Zero);
            for (int i = 0; i < 5; ++i)
                store.WriteSnapshot(new SceneAutosaveCapture(
                    identity2D, "scene-version-" + i, epoch.AddSeconds(i)));

            SceneRecoveryCandidate? latest = store.FindLatest(identity2D);
            Check(latest != null, "latest recovery is discovered");
            Check(latest != null && store.ReadVerifiedContent(latest) == "scene-version-4",
                "latest recovery content and checksum verify");
            Check(File.ReadAllText(sourcePath) == "ORIGINAL-SOURCE",
                "autosave never overwrites source scene");

            string entry2D = store.EntryDirectoryForTest(identity2D);
            Check(Directory.EnumerateFiles(
                    entry2D, "recovery-*.json", SearchOption.TopDirectoryOnly).Count() == 3,
                "retention is bounded");
            Check(!Directory.EnumerateFiles(
                    entry2D, "*.tmp", SearchOption.TopDirectoryOnly).Any(),
                "atomic writes leave no temporary files");

            if (latest != null)
            {
                File.AppendAllText(latest.SnapshotPath, "-CORRUPT");
                SceneRecoveryCandidate? fallback = store.FindLatest(identity2D);
                Check(fallback != null &&
                      store.ReadVerifiedContent(fallback) == "scene-version-3",
                    "checksum corruption falls back to previous valid generation");

                if (fallback != null)
                {
                    JsonNode metadata = JsonNode.Parse(File.ReadAllText(fallback.MetadataPath))!;
                    metadata["SnapshotFile"] = ".." + Path.DirectorySeparatorChar + "main.acscene";
                    File.WriteAllText(
                        fallback.MetadataPath,
                        metadata.ToJsonString(),
                        new UTF8Encoding(false));
                    SceneRecoveryCandidate? traversalFallback = store.FindLatest(identity2D);
                    Check(traversalFallback != null &&
                          store.ReadVerifiedContent(traversalFallback) == "scene-version-2",
                        "metadata traversal is rejected");
                }
            }

            store.WriteSnapshot(new SceneAutosaveCapture(
                identity3D, "ACS3D SELF TEST", epoch.AddMinutes(1)));
            SceneRecoveryCandidate? latest3D = store.FindLatest(identity3D);
            Check(latest3D != null &&
                  store.ReadVerifiedContent(latest3D) == "ACS3D SELF TEST",
                "3D recovery is isolated and verified");

            store.Discard(identity2D);
            Check(store.FindLatest(identity2D) == null,
                "discard removes every generation for one document");
            Check(store.FindLatest(identity3D) != null,
                "discard does not remove another document mode");
            Check(File.ReadAllText(sourcePath) == "ORIGINAL-SOURCE",
                "discard leaves source scene untouched");
            Check(RecoveryApplyGuard.CanReplace("clean-native", "clean-native"),
                "recovery guard accepts a freshly verified clean native snapshot");
            Check(!RecoveryApplyGuard.CanReplace("clean-native", "just-edited-native"),
                "recovery guard rejects edits newer than the cached dirty tick");
            Check(!RecoveryApplyGuard.CanReplace(null, "native"),
                "recovery guard rejects a missing clean baseline");

            CheckGenerationDrainContractAsync(
                store,
                projectPath,
                sourcePath,
                epoch,
                Check).GetAwaiter().GetResult();
            CheckReparseDefense(store, projectPath, root, epoch, Check, log);
        }
        catch (Exception ex)
        {
            ++failures;
            log.WriteLine("FAIL  unexpected exception");
            log.WriteLine(ex);
        }
        finally
        {
            try
            {
                string full = Path.GetFullPath(root);
                string temp = Path.GetFullPath(Path.GetTempPath());
                if (full.StartsWith(temp, StringComparison.OrdinalIgnoreCase) &&
                    Path.GetFileName(full).StartsWith("acs-autosave-selftest-", StringComparison.Ordinal))
                    Directory.Delete(full, recursive: true);
            }
            catch (Exception ex)
            {
                log.WriteLine("WARN  self-test cleanup: " + ex.Message);
            }
        }

        log.WriteLine($"Scene autosave self-test: {failures} failure(s)");
        return failures;
    }

    private static async Task CheckGenerationDrainContractAsync(
        SceneAutosaveStore store,
        string projectPath,
        string sourcePath,
        DateTimeOffset capturedUtc,
        Action<bool, string> check)
    {
        SceneAutosaveIdentity identity = SceneAutosaveStore.CreateIdentity(
            projectPath,
            Path.Combine(Path.GetDirectoryName(sourcePath)!, "race.acscene"),
            SceneDocumentMode.TwoD);
        store.Discard(identity);

        var gate = new AutosaveGenerationGate();
        var started = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseNonCancellableWrite = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        bool accepted = gate.TryStart(
            async _ =>
            {
                started.TrySetResult();
                // Deliberately model a filesystem call which has passed its cancellation point.
                await releaseNonCancellableWrite.Task.ConfigureAwait(false);
                store.WriteSnapshot(new SceneAutosaveCapture(
                    identity,
                    "old-generation",
                    capturedUtc.AddHours(2)));
            },
            out Task oldWorker);
        check(accepted, "autosave generation accepts one worker");
        await started.Task.ConfigureAwait(false);

        Task drain = gate.InvalidateAndWaitAsync();
        check(gate.IsSuppressed, "generation invalidation suppresses new workers");
        check(!drain.IsCompleted,
            "generation drain waits for an already-running non-cancellable write");

        releaseNonCancellableWrite.TrySetResult();
        await drain.ConfigureAwait(false);
        await oldWorker.ConfigureAwait(false);
        store.Discard(identity);
        check(store.FindLatest(identity) == null,
            "discard after generation drain removes the old worker snapshot");

        bool staleRestart = gate.TryStart(
            _ =>
            {
                store.WriteSnapshot(new SceneAutosaveCapture(
                    identity,
                    "must-not-recreate",
                    capturedUtc.AddHours(3)));
                return Task.CompletedTask;
            },
            out _);
        check(!staleRestart && store.FindLatest(identity) == null,
            "suppressed generation cannot recreate recovery after discard");

        gate.Resume();
        bool resumed = gate.TryStart(
            token =>
            {
                store.WriteSnapshot(
                    new SceneAutosaveCapture(
                        identity,
                        "new-generation",
                        capturedUtc.AddHours(4)),
                    token);
                return Task.CompletedTask;
            },
            out Task resumedWorker);
        await resumedWorker.ConfigureAwait(false);
        check(resumed && store.FindLatest(identity) != null,
            "explicit resume permits a new autosave generation");
        await gate.InvalidateAndWaitAsync().ConfigureAwait(false);
        store.Discard(identity);

        using var alreadyCancelled = new CancellationTokenSource();
        alreadyCancelled.Cancel();
        bool cancellationObserved = false;
        try
        {
            store.WriteSnapshot(
                new SceneAutosaveCapture(
                    identity,
                    "cancelled-generation",
                    capturedUtc.AddHours(5)),
                alreadyCancelled.Token);
        }
        catch (OperationCanceledException)
        {
            cancellationObserved = true;
        }
        check(cancellationObserved && store.FindLatest(identity) == null,
            "cancelled store write commits no recovery generation");
        gate.Dispose();
    }

    private static void CheckReparseDefense(
        SceneAutosaveStore store,
        string projectPath,
        string root,
        DateTimeOffset capturedUtc,
        Action<bool, string> check,
        TextWriter log)
    {
        string outside = Path.Combine(root, "reparse-target");
        Directory.CreateDirectory(outside);
        var identity = SceneAutosaveStore.CreateIdentity(
            projectPath,
            Path.Combine(root, "project", "Assets", "reparse.acscene"),
            SceneDocumentMode.TwoD);
        string entry = store.EntryDirectoryForTest(identity);
        Directory.CreateDirectory(Path.GetDirectoryName(entry)!);
        try
        {
            Directory.CreateSymbolicLink(entry, outside);
            bool refused = false;
            try
            {
                store.WriteSnapshot(new SceneAutosaveCapture(
                    identity, "must-not-follow-link", capturedUtc));
            }
            catch (InvalidDataException)
            {
                refused = true;
            }
            check(refused, "reparse-point recovery directory is refused");
            check(!Directory.EnumerateFiles(outside).Any(),
                "reparse target receives no snapshot");
        }
        catch (UnauthorizedAccessException)
        {
            log.WriteLine("SKIP  reparse defense runtime test (symbolic-link privilege unavailable)");
        }
        catch (PlatformNotSupportedException)
        {
            log.WriteLine("SKIP  reparse defense runtime test (symbolic links unsupported)");
        }
        catch (IOException ex)
        {
            log.WriteLine("SKIP  reparse defense runtime test: " + ex.Message);
        }
    }
}
