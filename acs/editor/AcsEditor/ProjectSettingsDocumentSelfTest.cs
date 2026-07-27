// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using AcsEditor.Packaging;

namespace AcsEditor;

internal static class ProjectSettingsDocumentSelfTest
{
    internal static int Run(TextWriter log)
    {
        int passed = 0;
        int failed = 0;
        string? tempRoot = null;

        void Check(bool condition, string name)
        {
            if (condition)
            {
                passed++;
                log.WriteLine($"PASS  {name}");
            }
            else
            {
                failed++;
                log.WriteLine($"FAIL  {name}");
            }
        }

        try
        {
            const string source =
                "; retained semantically\n[Plugin.Cloud]\nMode=Experimental\n";
            const string canonical =
                "[Game]\nWindowWidth=1280\n\n[Plugin.Cloud]\nMode=Experimental\n";
            ProjectSettingsDocumentContract.EnsureSourceEntriesPreserved(
                source,
                canonical);
            Check(
                ProjectSettingsDocumentContract.Parse(canonical)
                    .TryGetValue(
                        new ProjectSettingKey("Plugin.Cloud", "Mode"),
                        out string? unknownValue) &&
                unknownValue == "Experimental",
                "unknown project settings survive canonical round trip");

            bool duplicateRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    "[Plugin]\nMode=A\nMode=B\n"));
            bool malformedRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    "Mode=MissingSection\n"));
            bool lostUnknownRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.EnsureSourceEntriesPreserved(
                    source,
                    "[Game]\nWindowWidth=1280\n"));
            Check(
                duplicateRejected && malformedRejected && lostUnknownRejected,
                "settings preflight rejects malformed, duplicate, and lossy loads");

            bool lineLimitRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    new string('x', 512)));
            bool sectionLimitRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    "[" + new string('s', 32) + "]\n"));
            bool keyLimitRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    "[S]\n" + new string('k', 64) + "=v\n"));
            bool valueLimitRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    "[S]\nK=" + new string('v', 192) + "\n"));
            string tooManyEntries =
                "[S]\n" +
                string.Concat(
                    Enumerable.Range(0, 1025)
                        .Select(index => $"K{index}=v\n"));
            bool entryLimitRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    tooManyEntries));
            string tooManyLines =
                string.Concat(Enumerable.Repeat(";\n", 4097));
            bool lineCountRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.Parse(
                    tooManyLines));
            Check(
                lineLimitRejected &&
                sectionLimitRejected &&
                keyLimitRejected &&
                valueLimitRejected &&
                entryLimitRejected &&
                lineCountRejected,
                "settings preflight enforces native line, field, and entry byte ceilings");

            tempRoot = Path.Combine(
                Path.GetTempPath(),
                "acs-settings-document-" + Guid.NewGuid().ToString("N"));
            string settingsPath = Path.Combine(
                tempRoot,
                "Config",
                "ProjectSettings.ini");
            Directory.CreateDirectory(Path.GetDirectoryName(settingsPath)!);
            byte[] bomSource = Encoding.UTF8.GetBytes(source);
            File.WriteAllBytes(
                settingsPath,
                new byte[] { 0xEF, 0xBB, 0xBF }.Concat(bomSource).ToArray());
            bool bomRead =
                ProjectSettingsDocumentContract.ReadSource(
                    tempRoot,
                    settingsPath) ==
                source;
            string asynchronousRead =
                ProjectSettingsDocumentContract.ReadSourceAsync(
                    tempRoot,
                    settingsPath,
                    CancellationToken.None).GetAwaiter().GetResult();
            using var cancelledRead = new CancellationTokenSource();
            cancelledRead.Cancel();
            bool asynchronousReadCancelled = ThrowsOperationCanceled(
                () => ProjectSettingsDocumentContract.ReadSourceAsync(
                        tempRoot,
                        settingsPath,
                        cancelledRead.Token)
                    .GetAwaiter()
                    .GetResult());
            bool escapedPathRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.ReadSource(
                    tempRoot,
                    Path.Combine(tempRoot, "Elsewhere.ini")));
            bool reparseFixtureAvailable = false;
            bool reparseRejected = false;
            string linkedSettingsTarget = Path.Combine(
                tempRoot,
                "LinkedProjectSettingsTarget.ini");
            try
            {
                File.WriteAllText(linkedSettingsTarget, source, Encoding.UTF8);
                File.Delete(settingsPath);
                File.CreateSymbolicLink(settingsPath, linkedSettingsTarget);
                reparseFixtureAvailable = true;
                reparseRejected = ThrowsInvalidData(
                    () => ProjectSettingsDocumentContract.ReadSource(
                        tempRoot,
                        settingsPath));
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or
                PlatformNotSupportedException or
                IOException)
            {
                log.WriteLine(
                    "INFO  reparse-point fixture unavailable on this host");
            }
            finally
            {
                try { File.Delete(settingsPath); } catch { }
            }
            if (reparseFixtureAvailable)
            {
                Check(
                    reparseRejected,
                    "settings source read rejects reparse-point files");
            }
            File.WriteAllBytes(settingsPath, new byte[] { 0xFF, 0xFE, 0xFF });
            bool invalidUtf8Rejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.ReadSource(
                    tempRoot,
                    settingsPath));
            File.WriteAllBytes(
                settingsPath,
                new byte[
                    ProjectSettingsSerialization.MaximumUtf8Bytes + 1]);
            bool oversizedFileRejected = ThrowsInvalidData(
                () => ProjectSettingsDocumentContract.ReadSource(
                    tempRoot,
                    settingsPath));
            Check(
                bomRead &&
                asynchronousRead == source &&
                asynchronousReadCancelled &&
                escapedPathRejected &&
                invalidUtf8Rejected &&
                oversizedFileRejected,
                "settings source read is project-contained, accepts legacy BOM, and rejects invalid UTF-8 or over-1MiB files");

            bool configReparseFixtureAvailable = false;
            bool configReparseRejected = false;
            string configPath = Path.Combine(tempRoot, "Config");
            string configTarget = Path.Combine(tempRoot, "LinkedConfigTarget");
            try
            {
                File.Delete(settingsPath);
                Directory.Delete(configPath);
                Directory.CreateDirectory(configTarget);
                File.WriteAllText(
                    Path.Combine(configTarget, "ProjectSettings.ini"),
                    source,
                    Encoding.UTF8);
                Directory.CreateSymbolicLink(configPath, configTarget);
                configReparseFixtureAvailable = true;
                configReparseRejected = ThrowsInvalidData(
                    () => ProjectSettingsDocumentContract.ReadSource(
                        tempRoot,
                        settingsPath));
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or
                PlatformNotSupportedException or
                IOException)
            {
                log.WriteLine(
                    "INFO  Config reparse-point fixture unavailable on this host");
            }
            finally
            {
                try
                {
                    if (Directory.Exists(configPath) &&
                        (File.GetAttributes(configPath) &
                         FileAttributes.ReparsePoint) != 0)
                    {
                        Directory.Delete(configPath);
                    }
                }
                catch
                {
                }
                Directory.CreateDirectory(configPath);
            }
            if (configReparseFixtureAvailable)
            {
                Check(
                    configReparseRejected,
                    "settings source read rejects a reparse-point Config directory");
            }

            var persistenceGate = new ProjectSettingsPersistenceGate();
            persistenceGate.Latch(
                "synthetic restore rollback could not be verified",
                nativeStateIsUncertain: true);
            persistenceGate.Latch("a later operation must not weaken the latch");
            Check(
                persistenceGate.IsBlocked &&
                persistenceGate.BlockedReason ==
                    "synthetic restore rollback could not be verified",
                "uncertain native restore rollback blocks persistence until verified reload");
            EditorDocumentState protectedCapture =
                persistenceGate.ProtectCapture(
                    ProjectSettingsDocumentContract.CreateState(
                        "[Plugin]\nMode=A\n"));
            Check(
                protectedCapture.Payload == "[Plugin]\nMode=A\n" &&
                protectedCapture.ContentFingerprint != protectedCapture.Payload,
                "blocked persistence forces hosted dirty state without changing native payload");
            persistenceGate.ClearAfterVerifiedLoad();
            Check(
                !persistenceGate.IsBlocked,
                "only an explicit verified source load clears the persistence latch");

            int deniedApplyCalls = 0;
            ProjectSettingsMutationResult denied =
                ProjectSettingsMutationAdmission.TryApply(
                canEdit: () => false,
                apply: () =>
                {
                    deniedApplyCalls++;
                    return true;
                },
                record: () => true,
                rollback: () => true);
            int rollbackCalls = 0;
            ProjectSettingsMutationResult unhosted =
                ProjectSettingsMutationAdmission.TryApply(
                canEdit: () => true,
                apply: () => true,
                record: () => false,
                rollback: () =>
                {
                    rollbackCalls++;
                    return true;
                });
            ProjectSettingsMutationResult recordException =
                ProjectSettingsMutationAdmission.TryApply(
                    canEdit: () => true,
                    apply: () => true,
                    record: () => throw new InvalidOperationException(
                        "synthetic record failure"),
                    rollback: () => true);
            ProjectSettingsMutationResult uncertainRollback =
                ProjectSettingsMutationAdmission.TryApply(
                    canEdit: () => true,
                    apply: () => true,
                    record: () => false,
                    rollback: () => false);
            Check(
                !denied.Succeeded &&
                deniedApplyCalls == 0 &&
                !unhosted.Succeeded &&
                !unhosted.NativeStateUncertain &&
                rollbackCalls == 1 &&
                !recordException.Succeeded &&
                !recordException.NativeStateUncertain &&
                uncertainRollback.NativeStateUncertain,
                "settings mutation admission vetoes before apply, contains record errors, and detects rollback uncertainty");

            string current = "[Plugin]\nMode=A\n";
            EditorDocumentSaveResult nextSave =
                EditorDocumentSaveResult.Failed("synthetic write failure");
            var host = new EditorDocumentHost();
            EditorDocument document = host.Register(
                ProjectSettingsDocumentRegistration.Create(
                    settingsPath,
                    () => ProjectSettingsDocumentContract.CreateState(current),
                    state => current = state.Payload,
                    _ => ValueTask.FromResult(nextSave),
                    initiallySaved: true));

            Check(
                document.Id ==
                    ProjectSettingsDocumentRegistration.CreateId(settingsPath) &&
                document.SaveOrder == ProjectSettingsDocumentRegistration.SaveOrder,
                "settings document has stable file identity and deterministic save order");

            current = "[Plugin]\nMode=B\n";
            document.NotifyPotentialChange();
            bool transactionRecorded = document.Synchronize(
                "Edit Plugin.Mode",
                "settings.Plugin.Mode",
                TimeSpan.FromSeconds(1));
            bool undoWorked =
                document.Undo(out _) && current.Contains("Mode=A");
            bool redoWorked =
                document.Redo(out _) && current.Contains("Mode=B");
            Check(
                transactionRecorded && undoWorked && redoWorked && document.IsDirty,
                "settings changes participate in hosted transaction undo and redo");

            EditorDocumentSaveResult failedSave =
                document.SaveAsync().AsTask().GetAwaiter().GetResult();
            Check(
                failedSave.Status == EditorDocumentSaveStatus.Failed &&
                document.IsDirty,
                "failed settings write remains dirty");

            EditorDocumentCloseResult failedClose =
                host.PrepareCloseAsync(EditorDocumentCloseChoice.Save)
                    .AsTask().GetAwaiter().GetResult();
            EditorDocumentCloseResult discardClose =
                host.PrepareCloseAsync(EditorDocumentCloseChoice.Discard)
                    .AsTask().GetAwaiter().GetResult();
            Check(
                failedClose.Completion == EditorDocumentCloseCompletion.Failed &&
                !failedClose.CanClose &&
                discardClose.CanClose &&
                discardClose.DirtyDocuments.Contains(document.Id),
                "owner close blocks failed settings save and permits explicit discard");

            nextSave = EditorDocumentSaveResult.Saved(current);
            EditorDocumentCloseResult savedClose =
                host.PrepareCloseAsync(EditorDocumentCloseChoice.Save)
                    .AsTask().GetAwaiter().GetResult();
            Check(
                savedClose.CanClose && !document.IsDirty,
                "owner close becomes ready after settings save succeeds");

            string asynchronousCurrent = "[Plugin]\nMode=A\n";
            var writeCompletion =
                new TaskCompletionSource<EditorDocumentSaveResult>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
            using var writeStarted = new ManualResetEventSlim();
            var asyncHost = new EditorDocumentHost();
            EditorDocument asyncDocument = asyncHost.Register(
                ProjectSettingsDocumentRegistration.Create(
                    Path.Combine(tempRoot, "Other", "ProjectSettings.ini"),
                    () => ProjectSettingsDocumentContract.CreateState(
                        asynchronousCurrent),
                    state => asynchronousCurrent = state.Payload,
                    async _ =>
                    {
                        writeStarted.Set();
                        return await writeCompletion.Task.ConfigureAwait(false);
                    },
                    initiallySaved: true));
            asynchronousCurrent = "[Plugin]\nMode=C\n";
            asyncDocument.NotifyPotentialChange();
            asyncDocument.Synchronize("Edit Plugin.Mode");
            Task<EditorDocumentSaveBatchResult> pendingSave =
                Task.Run(async () =>
                    await asyncHost.SaveAllAsync().ConfigureAwait(false));
            bool writerWasReached = writeStarted.Wait(TimeSpan.FromSeconds(5));
            Check(
                writerWasReached && !pendingSave.IsCompleted,
                "settings host save awaits asynchronous persistence without synchronous completion");
            writeCompletion.SetResult(
                EditorDocumentSaveResult.Saved(asynchronousCurrent));
            EditorDocumentSaveBatchResult completedSave =
                pendingSave.GetAwaiter().GetResult();
            Check(
                completedSave.Completion == EditorDocumentBatchCompletion.Success &&
                !asyncDocument.IsDirty,
                "asynchronous settings persistence publishes its committed fingerprint");

            EditorDocumentState crossEditorCommitted =
                ProjectSettingsDocumentContract.CreateState(
                    "[Game]\nDefaultScene=Assets/Old.acs3d\n");
            EditorDocumentState crossEditorDurable =
                ProjectSettingsDocumentContract.CreateState(
                    "[Game]\nDefaultScene=Assets/Authoritative.acs3d\n");
            EditorDocumentState crossEditorLive = crossEditorCommitted;
            int durableRestoreCalls = 0;
            EditorDocumentSaveResult converged =
                ProjectSettingsDurableConvergence.Converge(
                    crossEditorCommitted,
                    crossEditorDurable,
                    () => crossEditorLive,
                    state =>
                    {
                        durableRestoreCalls++;
                        crossEditorLive = state;
                    });
            Check(
                converged.Status == EditorDocumentSaveStatus.Saved &&
                durableRestoreCalls == 1 &&
                crossEditorLive == crossEditorDurable &&
                converged.SavedFingerprint ==
                    crossEditorDurable.ContentFingerprint,
                "cross-editor DefaultScene rewrite converges native, host, and durable settings");

            EditorDocumentState newerLive =
                ProjectSettingsDocumentContract.CreateState(
                    "[Game]\nDefaultScene=Assets/NewerLive.acs3d\n");
            crossEditorLive = newerLive;
            durableRestoreCalls = 0;
            EditorDocumentSaveResult driftedConvergence =
                ProjectSettingsDurableConvergence.Converge(
                    crossEditorCommitted,
                    crossEditorDurable,
                    () => crossEditorLive,
                    _ => durableRestoreCalls++);
            Check(
                driftedConvergence.Status == EditorDocumentSaveStatus.Failed &&
                durableRestoreCalls == 0 &&
                crossEditorLive == newerLive,
                "durable settings convergence never overwrites a newer live edit");

            using var settingsLoadGeneration =
                new ProjectSettingsLoadGenerationGate();
            ProjectSettingsLoadTicket firstLoad =
                settingsLoadGeneration.Begin();
            ProjectSettingsLoadTicket secondLoad =
                settingsLoadGeneration.Begin();
            string appliedLoad = "";
            if (settingsLoadGeneration.IsCurrent(secondLoad))
                appliedLoad = "second";
            if (settingsLoadGeneration.IsCurrent(firstLoad))
                appliedLoad = "late-first";
            bool firstWasCancelled =
                firstLoad.CancellationToken.IsCancellationRequested;
            settingsLoadGeneration.Invalidate();
            Check(
                firstWasCancelled &&
                appliedLoad == "second" &&
                secondLoad.CancellationToken.IsCancellationRequested &&
                !settingsLoadGeneration.IsCurrent(secondLoad),
                "settings load generation rejects late results and cancels superseded or closed loads");

            string cleanSettings = "[Plugin]\nMode=Clean\n";
            int cleanWriterCalls = 0;
            var cleanBuildHost = new EditorDocumentHost();
            EditorDocument cleanBuildDocument = cleanBuildHost.Register(
                ProjectSettingsDocumentRegistration.Create(
                    Path.Combine(
                        tempRoot,
                        "CleanBuild",
                        "ProjectSettings.ini"),
                    () => ProjectSettingsDocumentContract.CreateState(
                        cleanSettings),
                    state => cleanSettings = state.Payload,
                    _ =>
                    {
                        cleanWriterCalls++;
                        return ValueTask.FromResult(
                            EditorDocumentSaveResult.Saved(
                                cleanSettings));
                    },
                    initiallySaved: true));
            EditorDocumentSaveResult cleanBuildDurability =
                ProjectSettingsBuildDurabilityGate.SaveAsync(
                    cleanBuildHost,
                    cleanBuildDocument,
                    CancellationToken.None).AsTask().GetAwaiter().GetResult();

            using var preCancelledCleanBuild = new CancellationTokenSource();
            preCancelledCleanBuild.Cancel();
            EditorDocumentSaveResult preCancelledCleanResult =
                ProjectSettingsBuildDurabilityGate.SaveAsync(
                    cleanBuildHost,
                    cleanBuildDocument,
                    preCancelledCleanBuild.Token).AsTask().GetAwaiter().GetResult();

            cleanBuildDocument.Suspend();
            EditorDocumentSaveResult suspendedCleanResult =
                ProjectSettingsBuildDurabilityGate.SaveAsync(
                    cleanBuildHost,
                    cleanBuildDocument,
                    CancellationToken.None).AsTask().GetAwaiter().GetResult();
            cleanBuildDocument.Resume();

            EditorDocumentSaveResult transactionCleanResult;
            using (cleanBuildDocument.BeginTransaction("Open settings transaction"))
            {
                transactionCleanResult =
                    ProjectSettingsBuildDurabilityGate.SaveAsync(
                        cleanBuildHost,
                        cleanBuildDocument,
                        CancellationToken.None).AsTask().GetAwaiter().GetResult();
            }
            Check(
                cleanBuildDurability.Status == EditorDocumentSaveStatus.Saved &&
                cleanWriterCalls == 1 &&
                preCancelledCleanResult.Status ==
                    EditorDocumentSaveStatus.Cancelled &&
                suspendedCleanResult.Status == EditorDocumentSaveStatus.Failed &&
                transactionCleanResult.Status == EditorDocumentSaveStatus.Failed,
                "clean Build/Run/Package still verifies durability while cancel, suspend, and open transaction fail before the writer");

            string projectReferenceRoot =
                Path.Combine(tempRoot, "ProjectReference");
            Directory.CreateDirectory(
                Path.Combine(projectReferenceRoot, "Assets"));
            var projectReference = new Project
            {
                ProjectFilePath =
                    Path.Combine(projectReferenceRoot, "Game.acsproject"),
                InitialScene = "Assets/Old.acs3d",
            };
            EditorDocumentState authoritativeSettings =
                ProjectSettingsDocumentContract.CreateState(
                    "[Game]\nDefaultScene=Assets/Authoritative.acs3d\n");
            bool projectReferenceReconciled =
                ProjectSettingsProjectReferenceConvergence.TryReconcile(
                    projectReference,
                    expectedLiveReference: "Assets/Old.acs3d",
                    authoritativeReference: "Assets/Authoritative.acs3d",
                    durableSettings: authoritativeSettings,
                    out string projectReferenceError);
            projectReference.InitialScene = "Assets/NewerLive.acs3d";
            bool projectReferenceDriftRejected =
                !ProjectSettingsProjectReferenceConvergence.TryReconcile(
                    projectReference,
                    expectedLiveReference: "Assets/Old.acs3d",
                    authoritativeReference: "Assets/Authoritative.acs3d",
                    durableSettings: authoritativeSettings,
                    out _) &&
                projectReference.InitialScene == "Assets/NewerLive.acs3d";
            Check(
                projectReferenceReconciled &&
                projectReferenceError.Length == 0 &&
                projectReferenceDriftRejected,
                "durable DefaultScene reconciles Project.InitialScene only when live Project state is unchanged");

            string buildSettings = "[Plugin]\nMode=EditorDirty\n";
            string buildConfig = Path.Combine(tempRoot, "Build", "Config");
            string buildSettingsPath =
                Path.Combine(buildConfig, "ProjectSettings.ini");
            string stagedBuildConfig =
                Path.Combine(tempRoot, "Build", "StagedConfig");
            Directory.CreateDirectory(buildConfig);
            File.WriteAllText(
                buildSettingsPath,
                buildSettings,
                new UTF8Encoding(false));
            var buildHost = new EditorDocumentHost();
            EditorDocument buildSettingsDocument = buildHost.Register(
                ProjectSettingsDocumentRegistration.Create(
                    buildSettingsPath,
                    () => ProjectSettingsDocumentContract.CreateState(
                        buildSettings),
                    state => buildSettings = state.Payload,
                    _ =>
                    {
                        File.WriteAllText(
                            buildSettingsPath,
                            buildSettings,
                            new UTF8Encoding(false));
                        return ValueTask.FromResult(
                            EditorDocumentSaveResult.Saved(
                                buildSettings));
                    },
                    initiallySaved: true));
            buildSettings = "[Plugin]\nMode=PackagedDirty\n";
            buildSettingsDocument.NotifyPotentialChange();
            buildSettingsDocument.Synchronize("Edit build setting");
            EditorDocumentSaveResult buildDurability =
                ProjectSettingsBuildDurabilityGate.SaveAsync(
                    buildHost,
                    buildSettingsDocument,
                    CancellationToken.None).AsTask().GetAwaiter().GetResult();
            PackageCore.PackageDirectorySnapshot stagedBuildSnapshot =
                Task.Run(
                    async () => await PackageCore.StageDirectorySnapshotAsync(
                            buildConfig,
                            stagedBuildConfig,
                            CancellationToken.None)
                        .ConfigureAwait(false))
                    .GetAwaiter()
                    .GetResult();
            string packagedSettings = File.ReadAllText(
                Path.Combine(
                    stagedBuildConfig,
                    "ProjectSettings.ini"),
                Encoding.UTF8);
            bool durableCheckpointAccepted = false;
            if (buildDurability.SavedFingerprint is string durableSource)
            {
                ProjectSettingsDurabilityCheckpoint durableCheckpoint =
                    ProjectSettingsDurabilityCheckpoint.Create(durableSource);
                PackageCore.ValidateProjectSettingsCheckpoint(
                    stagedBuildSnapshot,
                    durableCheckpoint.Utf8Sha256);
                durableCheckpointAccepted = true;
            }
            Check(
                buildDurability.Status == EditorDocumentSaveStatus.Saved &&
                stagedBuildSnapshot.Existed &&
                packagedSettings == "[Plugin]\nMode=PackagedDirty\n" &&
                !buildSettingsDocument.IsDirty &&
                durableCheckpointAccepted,
                "Build/Run/Package stages the exact durable Settings bytes from the host checkpoint");

            string failedBuildSettings = "[Plugin]\nMode=A\n";
            var failedBuildHost = new EditorDocumentHost();
            EditorDocument failedBuildDocument = failedBuildHost.Register(
                ProjectSettingsDocumentRegistration.Create(
                    Path.Combine(tempRoot, "FailedBuild", "ProjectSettings.ini"),
                    () => ProjectSettingsDocumentContract.CreateState(
                        failedBuildSettings),
                    state => failedBuildSettings = state.Payload,
                    _ => ValueTask.FromResult(
                        EditorDocumentSaveResult.Failed(
                            "synthetic package settings failure")),
                    initiallySaved: true));
            failedBuildSettings = "[Plugin]\nMode=B\n";
            failedBuildDocument.NotifyPotentialChange();
            failedBuildDocument.Synchronize("Edit failed build setting");
            EditorDocumentSaveResult failedBuildDurability =
                ProjectSettingsBuildDurabilityGate.SaveAsync(
                    failedBuildHost,
                    failedBuildDocument,
                    CancellationToken.None).AsTask().GetAwaiter().GetResult();
            using var cancelledBuild = new CancellationTokenSource();
            cancelledBuild.Cancel();
            EditorDocumentSaveResult cancelledBuildDurability =
                ProjectSettingsBuildDurabilityGate.SaveAsync(
                    failedBuildHost,
                    failedBuildDocument,
                    cancelledBuild.Token).AsTask().GetAwaiter().GetResult();
            Check(
                failedBuildDurability.Status == EditorDocumentSaveStatus.Failed &&
                cancelledBuildDurability.Status ==
                    EditorDocumentSaveStatus.Cancelled &&
                failedBuildDocument.IsDirty,
                "Build/Run/Package cannot cross failed or cancelled settings durability");
        }
        catch (Exception error)
        {
            failed++;
            log.WriteLine("FAIL  unhandled project settings document self-test: " + error);
        }
        finally
        {
            if (tempRoot != null)
            {
                try
                {
                    Directory.Delete(tempRoot, recursive: true);
                }
                catch
                {
                    // A cleanup failure must not hide the contract result on constrained CI hosts.
                }
            }
        }

        log.WriteLine(
            $"Project settings document self-test: {passed} passed, {failed} failed");
        return failed;
    }

    private static bool ThrowsInvalidData(Action action)
    {
        try
        {
            action();
            return false;
        }
        catch (InvalidDataException)
        {
            return true;
        }
    }

    private static bool ThrowsOperationCanceled(Action action)
    {
        try
        {
            action();
            return false;
        }
        catch (OperationCanceledException)
        {
            return true;
        }
    }
}
