// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal static class EditorDocumentHostSelfTest
{
    internal static int Run(TextWriter log)
    {
        int passed = 0;
        int failed = 0;

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
            string content = "A\nSEL 1";
            string restored = "";
            int saveCalls = 0;
            int captureCalls = 0;
            static string Durable(string value) =>
                string.Join('\n', value.Split('\n', StringSplitOptions.None)
                    .Where(line => !line.StartsWith("SEL ", StringComparison.Ordinal)));

            string packedWorld = SceneWorldDocumentEnvelope.Pack(
                "ACSCENE\nnode Ω\nACS_EDITOR_WORLD 1\n",
                "ACS3D\nnode 雲\n");
            SceneWorldDocumentEnvelope.Unpack(
                packedWorld,
                out string unpacked2D,
                out string unpacked3D);
            Check(
                unpacked2D == "ACSCENE\nnode Ω\nACS_EDITOR_WORLD 1\n" &&
                unpacked3D == "ACS3D\nnode 雲\n",
                "world envelope preserves 2D and 3D subsystem payloads exactly");
            EditorSceneViewMode[] editorViews =
                Enum.GetValues<EditorSceneViewMode>();
            string[] viewFingerprints = editorViews
                .Select(_ => SceneWorldDocumentEnvelope.Pack("2D payload", "3D payload"))
                .Distinct(StringComparer.Ordinal)
                .ToArray();
            Check(
                viewFingerprints.Length == 1,
                "Perspective and 2D Orthographic presets do not affect document content");

            var worldViewState = new EditorSceneViewState(
                SceneDocumentMode.ThreeD,
                EditorSceneViewMode.Perspective);
            string worldPayloadKey = worldViewState.ActivePayloadKey;
            bool everyWorldViewAccepted = true;
            foreach (EditorSceneViewMode view in editorViews)
            {
                everyWorldViewAccepted &=
                    worldViewState.TryChangeView(view, out worldViewState);
            }
            Check(
                everyWorldViewAccepted &&
                worldViewState.SourceMode == SceneDocumentMode.ThreeD &&
                worldViewState.ActivePayloadKey == worldPayloadKey,
                "all .acs3d view presets keep the same legacy source adapter and visible content");

            var legacy2DViewState = new EditorSceneViewState(
                SceneDocumentMode.TwoD,
                EditorSceneViewMode.TwoD);
            string legacy2DPayloadKey = legacy2DViewState.ActivePayloadKey;
            bool perspectiveRejected = !legacy2DViewState.TryChangeView(
                EditorSceneViewMode.Perspective,
                out EditorSceneViewState rejected2DView);
            Check(
                perspectiveRejected &&
                rejected2DView == legacy2DViewState &&
                rejected2DView.ActivePayloadKey == legacy2DPayloadKey,
                "unconverted .acscene never switches to a hidden 3D payload");
            Check(
                !EditorSceneViewModePolicy.Describe(
                    EditorSceneViewMode.Perspective).IsOrthographic &&
                EditorSceneViewModePolicy.Describe(
                    EditorSceneViewMode.TwoD).IsOrthographic,
                "2D preset is the honest XY Orthographic projection supported by native editor");
            Check(
                EditorSceneViewModePolicy.InitialForProject(
                    "Assets/main.acs3d",
                    "3d") == EditorSceneViewMode.Perspective &&
                EditorSceneViewModePolicy.InitialForProject(
                    "Assets/main.acs3d",
                    "2d") == EditorSceneViewMode.TwoD &&
                EditorSceneViewModePolicy.InitialForProject(
                    "Assets/main.acscene",
                    "blank") == EditorSceneViewMode.TwoD,
                "project template selects only the initial ACS3D camera preset while legacy .acscene stays 2D");

            bool malformedWorldRejected = false;
            try
            {
                SceneWorldDocumentEnvelope.Unpack(
                    "ACS_EDITOR_WORLD 1\n999\n0\nshort",
                    out _,
                    out _);
            }
            catch (InvalidDataException)
            {
                malformedWorldRejected = true;
            }
            Check(malformedWorldRejected, "world envelope rejects malformed lengths");

            var revisionGate = new SceneMutationRevisionGate();
            Check(
                !revisionGate.DocumentCaptureRequired &&
                !revisionGate.WorkspaceCaptureRequired,
                "scene revision gate starts fully acknowledged");
            ulong firstRevision = revisionGate.NotifyMutation();
            Check(
                firstRevision == revisionGate.Current &&
                revisionGate.DocumentCaptureRequired &&
                revisionGate.WorkspaceCaptureRequired,
                "one mutation independently invalidates document and workspace captures");
            revisionGate.AcknowledgeDocument();
            Check(
                !revisionGate.DocumentCaptureRequired &&
                revisionGate.WorkspaceCaptureRequired,
                "document capture does not hide a pending workspace dirty check");
            ulong secondRevision = revisionGate.NotifyMutation();
            revisionGate.AcknowledgeWorkspace();
            Check(
                secondRevision > firstRevision &&
                revisionGate.DocumentCaptureRequired &&
                !revisionGate.WorkspaceCaptureRequired,
                "workspace capture does not hide newer document history work");
            revisionGate.AcknowledgeDocument();
            Check(
                SceneMutationRevisionGate.ShouldAcknowledgeCompletedSave(
                    secondRevision,
                    secondRevision,
                    EditorDocumentSaveStatus.Saved,
                    isSuspended: false) &&
                !SceneMutationRevisionGate.ShouldAcknowledgeCompletedSave(
                    secondRevision,
                    secondRevision + 1,
                    EditorDocumentSaveStatus.Saved,
                    isSuspended: false) &&
                !SceneMutationRevisionGate.ShouldAcknowledgeCompletedSave(
                    secondRevision,
                    secondRevision,
                    EditorDocumentSaveStatus.Failed,
                    isSuspended: false) &&
                !SceneMutationRevisionGate.ShouldAcknowledgeCompletedSave(
                    secondRevision,
                    secondRevision,
                    EditorDocumentSaveStatus.Saved,
                    isSuspended: true),
                "async save acknowledgement requires success and an unchanged revision");

            int seededCaptureCalls = 0;
            var seededDocument = new EditorDocument(
                new EditorDocumentId("scene", "seeded"),
                "seeded",
                null,
                () =>
                {
                    seededCaptureCalls++;
                    return EditorDocumentState.Text("unexpected capture");
                },
                _ => { },
                initialState: EditorDocumentState.Text("seed"));
            Check(
                seededCaptureCalls == 0 &&
                !seededDocument.IsDirty &&
                !seededDocument.CanUndo,
                "an injected initial state avoids constructor serialization and seeds clean state");

            EditorDocumentId id = new("scene", "project-a");
            var document = new EditorDocument(
                id,
                "main.acscene",
                @"C:\Project\Assets\main.acscene",
                () =>
                {
                    captureCalls++;
                    return EditorDocumentState.Text(content, Durable);
                },
                state =>
                {
                    restored = state.Payload;
                    content = state.Payload;
                },
                _ =>
                {
                    saveCalls++;
                    return ValueTask.FromResult(EditorDocumentSaveResult.Saved());
                });
            var host = new EditorDocumentHost();
            host.Register(document);
            host.Activate(id);

            Check(host.ActiveDocument == document, "host activates a registered document");
            Check(!document.IsDirty, "registered saved document starts clean");
            Check(!document.CanUndo && !document.CanRedo, "history starts empty");

            EditorDocumentId viewIndependentId = document.Id;
            int viewIndependentUndoCount = document.UndoCount;
            var documentViewState = new EditorSceneViewState(
                SceneDocumentMode.ThreeD,
                EditorSceneViewMode.Perspective);
            foreach (EditorSceneViewMode view in editorViews)
                Check(
                    documentViewState.TryChangeView(view, out documentViewState),
                    $"view state accepts {view} for .acs3d source");
            Check(
                host.ActiveDocument == document &&
                document.Id == viewIndependentId &&
                !document.IsDirty &&
                !document.HasPendingChanges &&
                document.UndoCount == viewIndependentUndoCount,
                "view preset changes preserve document identity, dirty state, and undo history");

            content = "A\nSEL 2";
            int capturesBeforeQueries = captureCalls;
            Check(!document.HasPendingChanges, "selection-only changes are not transactions");
            Check(
                captureCalls == capturesBeforeQueries,
                "HasPendingChanges never captures an unnotified scene");
            Check(!document.Synchronize("Select"), "selection-only synchronization is a no-op");

            content = "B\nSEL 2";
            document.NotifyPotentialChange();
            capturesBeforeQueries = captureCalls;
            Check(
                document.HasPendingChanges &&
                document.HasPendingChanges &&
                document.IsDirty,
                "explicit mutation notification updates cached pending and dirty state");
            Check(
                captureCalls == capturesBeforeQueries,
                "repeated pending/dirty command queries never invoke document capture");
            Check(document.Synchronize("Rename", "name"), "external edit becomes a transaction");
            Check(!document.HasPendingChanges, "synchronization clears the cached mutation signal");
            Check(document.CanUndo && document.UndoCount == 1, "recorded transaction enables undo");
            Check(document.IsDirty, "edit after save point is dirty");
            Check(document.NextUndo?.Label == "Rename", "undo label is retained");

            Check(host.Undo(out EditorDocumentTransactionInfo? undo) &&
                  undo?.Label == "Rename" &&
                  Durable(restored) == "A",
                "host undo restores the before snapshot");
            Check(!document.IsDirty, "undo to save point becomes clean");
            capturesBeforeQueries = captureCalls;
            Check(document.CanRedo, "undo enables redo");
            Check(
                captureCalls == capturesBeforeQueries,
                "CanRedo command queries use cached state without document capture");

            Check(host.Redo(out EditorDocumentTransactionInfo? redo) &&
                  redo?.Label == "Rename" &&
                  Durable(restored) == "B",
                "host redo restores the after snapshot");
            Check(document.IsDirty, "redo away from save point becomes dirty");

            document.MarkSaved();
            Check(!document.IsDirty, "MarkSaved establishes a save point");
            Check(document.CanUndo, "saving does not erase undo history");
            Check(host.Undo(out _) && document.IsDirty,
                "undoing past a save point marks the document dirty");
            Check(host.Redo(out _) && !document.IsDirty,
                "redoing to a save point becomes clean");

            content = "C";
            document.Synchronize("Property", "property", TimeSpan.FromSeconds(2));
            content = "D";
            document.Synchronize("Property", "property", TimeSpan.FromSeconds(2));
            Check(document.UndoCount == 2,
                "matching interactive edits merge into one history entry");
            Check(host.Undo(out _) && content == "B\nSEL 2",
                "merged transaction undoes the entire interaction");
            content = "branch";
            document.Synchronize("Branch");
            Check(!document.CanRedo && document.RedoCount == 0,
                "editing after undo discards the redo branch");

            document.MarkSaved();
            content = "transaction-start";
            document.Synchronize("Before explicit transaction");
            int beforeScopedCount = document.UndoCount;
            using (document.BeginTransaction("Gizmo Drag", "gizmo", TimeSpan.FromSeconds(2)))
            {
                content = "drag-1";
                Check(
                    !document.CanUndo &&
                    !document.CanRedo &&
                    !document.Undo(out _) &&
                    !document.Redo(out _) &&
                    content == "drag-1",
                    "undo and redo are unavailable and rejected while a transaction is open");
                content = "drag-2";
            }
            Check(document.UndoCount == beforeScopedCount + 1,
                "explicit transaction groups multiple mutations");
            Check(document.NextUndo?.Label == "Gizmo Drag",
                "explicit transaction retains its label");

            document.Suspend();
            content = "runtime-only";
            Check(document.IsSuspended && !document.CanUndo && !document.HasPendingChanges,
                "Play/Preview suspension blocks history commands");
            document.Resume(acceptCurrentWithoutTransaction: true);
            Check(!document.IsSuspended && !document.HasPendingChanges,
                "simulation resume accepts restored runtime state without history");

            string checkpointContent = content;
            int checkpointUndoCount = document.UndoCount;
            int checkpointRedoCount = document.RedoCount;
            bool checkpointDirty = document.IsDirty;
            string checkpointDisplayName = document.DisplayName;
            string? checkpointSourcePath = document.SourcePath;
            EditorDocument.Checkpoint checkpoint = document.CaptureCheckpoint();
            document.UpdatePresentation("Replacement", "replacement.acscene");
            content = "replacement";
            document.ResetHistory(
                markSaved: true,
                currentState: EditorDocumentState.Text(content, Durable));
            content = checkpointContent;
            document.RestoreCheckpoint(checkpoint);
            Check(
                document.UndoCount == checkpointUndoCount &&
                document.RedoCount == checkpointRedoCount &&
                document.IsDirty == checkpointDirty &&
                document.DisplayName == checkpointDisplayName &&
                document.SourcePath == checkpointSourcePath,
                "document checkpoint restores history, save point, and presentation metadata");

            content = "recovered";
            int capturesBeforeReset = captureCalls;
            document.ResetHistory(
                markSaved: false,
                currentState: EditorDocumentState.Text(content, Durable));
            Check(document.IsDirty && !document.CanUndo && !document.CanRedo,
                "recovery boundary clears history and remains dirty");
            Check(
                captureCalls == capturesBeforeReset,
                "a reset boundary reuses its canonical state instead of serializing again");
            document.MarkSaved();
            content = "post-save edit";
            document.Synchronize("Edit");
            document.ClearHistory();
            Check(document.IsDirty && !document.CanUndo,
                "ClearHistory preserves dirty state while removing history");

            EditorDocumentSaveResult save = document.SaveAsync().AsTask().GetAwaiter().GetResult();
            Check(save.Status == EditorDocumentSaveStatus.Saved &&
                  saveCalls == 1 &&
                  !document.IsDirty,
                "save contract marks the committed state clean");

            content = "durably-written";
            document.Synchronize("Edit before save");
            string durableFingerprint = Durable(content);
            content = "edited-during-cleanup";
            document.MarkSavedFingerprint(durableFingerprint);
            Check(document.IsDirty && document.HasPendingChanges == false,
                "save fingerprint does not absorb edits made during async cleanup");
            document.MarkSaved();
            document.ClearHistory();

            string raceContent = "disk";
            var raceDocument = new EditorDocument(
                new EditorDocumentId("asset", "save-race"),
                "save-race",
                null,
                () => EditorDocumentState.Text(raceContent),
                state => raceContent = state.Payload,
                _ =>
                {
                    raceContent = "late-edit";
                    return ValueTask.FromResult(
                        EditorDocumentSaveResult.Saved("disk"));
                });
            EditorDocumentSaveResult raceSave =
                raceDocument.SaveAsync().AsTask().GetAwaiter().GetResult();
            Check(
                raceSave.Status == EditorDocumentSaveStatus.Saved &&
                raceDocument.IsDirty,
                "save contract fingerprint preserves edits made before async completion");

            string singleCaptureContent = "before";
            int singleCaptureCalls = 0;
            var singleCaptureDocument = new EditorDocument(
                new EditorDocumentId("asset", "single-undo-capture"),
                "single-undo-capture",
                null,
                () =>
                {
                    singleCaptureCalls++;
                    return EditorDocumentState.Text(singleCaptureContent);
                },
                state => singleCaptureContent = state.Payload,
                initialState: EditorDocumentState.Text(singleCaptureContent));
            singleCaptureContent = "after";
            singleCaptureDocument.NotifyPotentialChange();
            Check(singleCaptureDocument.Synchronize("Edit"),
                "single-capture undo fixture records an edit");
            int capturesBeforeUndo = singleCaptureCalls;
            Check(
                singleCaptureDocument.Undo(out _) &&
                singleCaptureContent == "before" &&
                singleCaptureCalls == capturesBeforeUndo + 1,
                "undo performs one preflight capture before restoring history");

            var unsupported = new EditorDocument(
                new EditorDocumentId("material", "asset-1"),
                "M.acsmat",
                null,
                () => EditorDocumentState.Text("material"),
                _ => { });
            Check(
                unsupported.SaveAsync().AsTask().GetAwaiter().GetResult().Status ==
                EditorDocumentSaveStatus.Unsupported,
                "documents without a save contract fail explicitly");

            string otherContent = "X";
            EditorDocumentId otherId = new("material", "asset-material");
            var other = new EditorDocument(
                otherId,
                "surface.acsmat",
                null,
                () => EditorDocumentState.Text(otherContent),
                state => otherContent = state.Payload);
            host.Register(other);
            content = "outgoing edit";
            host.Activate(otherId);
            Check(document.UndoCount == 1,
                "activating another document synchronizes the outgoing document");
            Check(host.ActiveDocument == other && !other.IsDirty,
                "document histories and save points are independent");
            host.Activate(id);
            Check(host.ActiveDocument == document && other.UndoCount == 0,
                "reactivation preserves each document history");

            document.UpdatePresentation("renamed.acscene", @"C:\Project\Assets\renamed.acscene");
            Check(document.Id == id &&
                  document.DisplayName == "renamed.acscene" &&
                  document.SourcePath?.EndsWith("renamed.acscene", StringComparison.Ordinal) == true,
                "Save As updates source presentation without replacing identity");

            bool duplicateRejected = false;
            try { host.Register(document); }
            catch (InvalidOperationException) { duplicateRejected = true; }
            Check(duplicateRejected, "duplicate stable identity is rejected");

            bool unknownRejected = false;
            try { host.Activate(new EditorDocumentId("scene2d", "missing")); }
            catch (System.Collections.Generic.KeyNotFoundException) { unknownRejected = true; }
            Check(unknownRejected, "unknown document activation fails closed");

            string failureContent = "one";
            bool failRestore = false;
            var failing = new EditorDocument(
                new EditorDocumentId("asset", "failure"),
                "failure",
                null,
                () => EditorDocumentState.Text(failureContent),
                state =>
                {
                    if (failRestore) throw new IOException("restore failed");
                    failureContent = state.Payload;
                });
            failureContent = "two";
            failing.Synchronize("Edit");
            failRestore = true;
            bool restoreFailed = false;
            try { failing.Undo(out _); }
            catch (IOException) { restoreFailed = true; }
            Check(restoreFailed && failing.UndoCount == 1 && failureContent == "two",
                "restore failure leaves history cursor and live state unchanged");

            var limitedContent = "0";
            var limited = new EditorDocument(
                new EditorDocumentId("asset", "limited"),
                "limited",
                null,
                () => EditorDocumentState.Text(limitedContent),
                state => limitedContent = state.Payload,
                historyLimit: 2);
            for (int i = 1; i <= 4; i++)
            {
                limitedContent = i.ToString();
                limited.Synchronize($"Edit {i}");
            }
            Check(limited.UndoCount == 2,
                "history is bounded to the configured capacity");
            limited.Undo(out _);
            limited.Undo(out _);
            Check(limitedContent == "2",
                "capacity trimming retains the newest transactions deterministically");

            bool cancellationObserved = false;
            var cancelledDocument = new EditorDocument(
                new EditorDocumentId("asset", "cancelled"),
                "cancelled",
                null,
                () => EditorDocumentState.Text("state"),
                _ => { },
                token =>
                {
                    token.ThrowIfCancellationRequested();
                    return ValueTask.FromResult(EditorDocumentSaveResult.Saved());
                });
            using (var cts = new CancellationTokenSource())
            {
                cts.Cancel();
                try
                {
                    cancelledDocument.SaveAsync(cts.Token).AsTask().GetAwaiter().GetResult();
                }
                catch (OperationCanceledException)
                {
                    cancellationObserved = true;
                }
            }
            Check(cancellationObserved, "save contract honors cancellation");
        }
        catch (Exception ex)
        {
            failed++;
            log.WriteLine("FAIL  unhandled exception: " + ex);
        }

        log.WriteLine($"EditorDocumentHost self-test: passed={passed} failed={failed}");
        return failed;
    }
}
