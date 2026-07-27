// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Input;
using System.Windows.Threading;

namespace AcsEditor;

/// <summary>
/// Independent acknowledgements for the expensive canonical-document and active-source captures.
/// A timer may inspect these counters freely; only a managed mutation notification advances them.
/// </summary>
internal sealed class SceneMutationRevisionGate
{
    private ulong _current;
    private ulong _documentCaptured;
    private ulong _workspaceCaptured;

    internal readonly record struct Checkpoint(
        ulong Current,
        ulong DocumentCaptured,
        ulong WorkspaceCaptured);

    internal ulong Current => _current;
    internal bool DocumentCaptureRequired => _documentCaptured != _current;
    internal bool WorkspaceCaptureRequired => _workspaceCaptured != _current;

    internal ulong NotifyMutation()
    {
        if (_current == ulong.MaxValue)
        {
            // Preserve inequality across the practically unreachable wrap boundary.
            _current = 1;
            _documentCaptured = 0;
            _workspaceCaptured = 0;
        }
        else
        {
            _current++;
        }
        return _current;
    }

    internal void AcknowledgeDocument() => _documentCaptured = _current;
    internal void AcknowledgeWorkspace() => _workspaceCaptured = _current;

    internal Checkpoint CaptureCheckpoint() =>
        new(_current, _documentCaptured, _workspaceCaptured);

    internal void RestoreCheckpoint(Checkpoint checkpoint)
    {
        _current = checkpoint.Current;
        _documentCaptured = checkpoint.DocumentCaptured;
        _workspaceCaptured = checkpoint.WorkspaceCaptured;
    }

    internal static bool ShouldAcknowledgeCompletedSave(
        ulong startingRevision,
        ulong completedRevision,
        EditorDocumentSaveStatus status,
        bool isSuspended) =>
        status == EditorDocumentSaveStatus.Saved &&
        !isSuspended &&
        startingRevision == completedRevision;
}

/// <summary>
/// Common document-host adapter for one scene document. The native 2D and 3D serializers are
/// compatibility payloads inside the same world envelope; viewport presets never select a
/// different EditorDocument.
/// </summary>
public partial class MainWindow
{
    private readonly EditorDocumentHost _documentHost = new();
    private readonly Dictionary<MaterialEditorWindow, EditorDocument>
        _hostedMaterialDocuments = new();
    private readonly SceneMutationRevisionGate _sceneMutationRevision = new();
    private readonly string _standaloneDocumentSessionId = Guid.NewGuid().ToString("N");
    private readonly DispatcherTimer _documentHistoryTimer = new()
    {
        Interval = TimeSpan.FromMilliseconds(350),
    };
    private bool _documentHostInitialized;
    private bool _sceneHistorySimulationSuspended;
    private string? _hostSavedSubsystem2D;
    private string? _hostSavedSubsystem3D;
    private string _pendingSceneHistoryLabel = "Edit Scene";
    private string? _pendingSceneHistoryMergeKey;
    private bool _sceneMergeSelectionInitialized;
    private bool _sceneMergeSelection3D;
    private int _sceneMergeSelectionNodeId = -1;
    private int _sceneMergeSelectionCount;
    private ulong _sceneMergeSelectionEpoch;
    private bool _sceneOpenTransactionInProgress;

    private EditorDocumentId SceneDocumentId()
    {
        string owner = _project == null
            ? "standalone:" + _standaloneDocumentSessionId
            : "project:" + CanonicalIdentityPath(_project.RootDir);
        return new EditorDocumentId("scene", owner);
    }

    private static string CanonicalIdentityPath(string path)
    {
        string full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        return OperatingSystem.IsWindows() ? full.ToUpperInvariant() : full;
    }

    private void InitializeDocumentHost()
    {
        if (_documentHostInitialized || Engine == IntPtr.Zero)
            return;

        // Capture the two native compatibility payloads once. The same immutable state seeds the
        // host save baseline and the document, avoiding four full scene serializations during the
        // final startup dispatcher stage.
        EditorDocumentState initialState = CaptureCanonicalSceneDocumentState();
        SceneWorldDocumentEnvelope.Unpack(
            initialState.ContentFingerprint,
            out _hostSavedSubsystem2D,
            out _hostSavedSubsystem3D);
        EditorDocument scene = EnsureSceneDocumentRegistered(_view3d, initialState);
        _documentHost.DocumentStateChanged += OnHostedDocumentStateChanged;
        _documentHost.Activate(scene.Id, synchronizeOutgoing: false);
        _documentHostInitialized = true;
        MaterialDocumentHostRegistration.RequireEveryExistingDocumentHosted(
            _materialEditorWindows.ToArray(),
            TryRegisterHostedMaterialDocument,
            RollbackDocumentHostInitialization);
        _sceneMutationRevision.AcknowledgeDocument();

        _documentHistoryTimer.Tick += OnDocumentHistoryTick;
        _documentHistoryTimer.Start();
        Closed += (_, _) => _documentHistoryTimer.Stop();
    }

    private void RollbackDocumentHostInitialization()
    {
        var cleanupErrors = new List<Exception>();
        foreach ((MaterialEditorWindow materialWindow, EditorDocument document) in
                 _hostedMaterialDocuments.ToArray())
        {
            try
            {
                materialWindow.DetachHostedDocument(document);
            }
            catch (Exception error)
            {
                cleanupErrors.Add(error);
            }
        }
        _hostedMaterialDocuments.Clear();
        _documentHost.DocumentStateChanged -= OnHostedDocumentStateChanged;
        try
        {
            if (!_documentHost.Clear(discardUnsavedChanges: true))
            {
                cleanupErrors.Add(new InvalidOperationException(
                    "Document Host refused initialization rollback."));
            }
        }
        catch (Exception error)
        {
            cleanupErrors.Add(error);
        }
        finally
        {
            _documentHostInitialized = false;
            _hostSavedSubsystem2D = null;
            _hostSavedSubsystem3D = null;
        }

        if (cleanupErrors.Count != 0)
        {
            throw new AggregateException(
                "Document Host initialization rollback was incomplete.",
                cleanupErrors);
        }
    }

    private bool TryRegisterHostedMaterialDocument(
        MaterialEditorWindow materialWindow)
    {
        ArgumentNullException.ThrowIfNull(materialWindow);
        if (!_documentHostInitialized)
            return false;
        if (_hostedMaterialDocuments.TryGetValue(
                materialWindow,
                out EditorDocument? existing))
        {
            string? currentPath = materialWindow.CurrentAssetPath;
            if (!string.IsNullOrWhiteSpace(currentPath))
            {
                existing.UpdatePresentation(
                    Path.GetFileName(currentPath),
                    currentPath);
            }
            return true;
        }

        string? path = materialWindow.CurrentAssetPath;
        if (string.IsNullOrWhiteSpace(path))
            return false;
        try
        {
            EditorDocumentId id = MaterialDocumentHostRegistration.CreateId(
                path,
                materialWindow.CurrentAssetId);
            if (_documentHost.TryGet(id, out _))
            {
                Log(
                    "Material document registration rejected a duplicate identity: " +
                    id,
                    "Document",
                    LogLevel.Error);
                return false;
            }

            EditorDocumentState initialState =
                materialWindow.CaptureHostedMaterialState();
            EditorDocument document = MaterialDocumentHostRegistration.Create(
                path,
                materialWindow.CurrentAssetId,
                Path.GetFileName(path),
                materialWindow.CaptureHostedMaterialState,
                materialWindow.RestoreHostedMaterialState,
                materialWindow.SaveHostedMaterialAsync,
                initiallySaved: !materialWindow.HasUnsavedGraphChanges,
                initialState: initialState);
            _documentHost.Register(document);
            try
            {
                materialWindow.AttachHostedDocument(document);
                _hostedMaterialDocuments.Add(materialWindow, document);
            }
            catch
            {
                materialWindow.DetachHostedDocument(document);
                _documentHost.Unregister(
                    document.Id,
                    discardUnsavedChanges: true);
                throw;
            }
            return true;
        }
        catch (Exception ex)
        {
            Log(
                "Material document could not join Document Host: " +
                ex.Message,
                "Document",
                LogLevel.Error);
            return false;
        }
    }

    private void UnregisterHostedMaterialDocument(
        MaterialEditorWindow materialWindow)
    {
        if (!_hostedMaterialDocuments.Remove(
                materialWindow,
                out EditorDocument? document))
        {
            return;
        }
        materialWindow.DetachHostedDocument(document);
        if (!_documentHost.Unregister(
                document.Id,
                discardUnsavedChanges: true))
        {
            Log(
                "Material document could not be removed from Document Host: " +
                document.Id,
                "Document",
                LogLevel.Error);
        }
    }

    private void SuspendHostedMaterialDocument(
        MaterialEditorWindow materialWindow,
        AssetDocumentMutationState mutation)
    {
        if (!_hostedMaterialDocuments.TryGetValue(
                materialWindow,
                out EditorDocument? document))
        {
            return;
        }
        try
        {
            document.Suspend(synchronize: true);
            mutation.SuspendedMaterialDocuments.Add(document);
        }
        catch (Exception ex)
        {
            throw new InvalidOperationException(
                "Material document suspension failed for " +
                document.DisplayName + ".",
                ex);
        }
    }

    private void ResumeHostedMaterialDocument(
        MaterialEditorWindow materialWindow,
        AssetDocumentMutationState mutation)
    {
        if (!_hostedMaterialDocuments.TryGetValue(
                materialWindow,
                out EditorDocument? document) ||
            !mutation.SuspendedMaterialDocuments.Remove(document))
        {
            return;
        }
        document.Resume(acceptCurrentWithoutTransaction: true);
        RefreshHostedMaterialIdentity(materialWindow);
    }

    private void RefreshHostedMaterialPresentation(
        MaterialEditorWindow materialWindow)
    {
        if (!_hostedMaterialDocuments.TryGetValue(
                materialWindow,
                out EditorDocument? document) ||
            materialWindow.CurrentAssetPath is not string path)
        {
            return;
        }
        document.UpdatePresentation(Path.GetFileName(path), path);
    }

    private void RefreshHostedMaterialIdentity(
        MaterialEditorWindow materialWindow)
    {
        if (!_hostedMaterialDocuments.TryGetValue(
                materialWindow,
                out EditorDocument? oldDocument) ||
            materialWindow.CurrentAssetPath is not string path)
        {
            return;
        }
        EditorDocumentId expected =
            MaterialDocumentHostRegistration.CreateId(
                path,
                materialWindow.CurrentAssetId);
        if (oldDocument.Id == expected)
        {
            oldDocument.UpdatePresentation(Path.GetFileName(path), path);
            return;
        }

        bool initiallySaved = !oldDocument.IsDirty;
        EditorDocumentState state =
            materialWindow.CaptureHostedMaterialState();
        materialWindow.DetachHostedDocument(oldDocument);
        if (!_documentHost.Unregister(
                oldDocument.Id,
                discardUnsavedChanges: true))
        {
            materialWindow.AttachHostedDocument(oldDocument);
            throw new InvalidOperationException(
                "The old loose-file material identity could not be released.");
        }
        _hostedMaterialDocuments.Remove(materialWindow);

        EditorDocument replacement = MaterialDocumentHostRegistration.Create(
            path,
            materialWindow.CurrentAssetId,
            Path.GetFileName(path),
            materialWindow.CaptureHostedMaterialState,
            materialWindow.RestoreHostedMaterialState,
            materialWindow.SaveHostedMaterialAsync,
            initiallySaved,
            state);
        bool replacementRegistered = false;
        bool replacementAttached = false;
        try
        {
            _documentHost.Register(replacement);
            replacementRegistered = true;
            materialWindow.AttachHostedDocument(replacement);
            replacementAttached = true;
            _hostedMaterialDocuments.Add(materialWindow, replacement);
        }
        catch (Exception replacementError)
        {
            if (replacementAttached)
                materialWindow.DetachHostedDocument(replacement);
            if (replacementRegistered)
            {
                _documentHost.Unregister(
                    replacement.Id,
                    discardUnsavedChanges: true);
            }

            try
            {
                oldDocument.UpdatePresentation(
                    Path.GetFileName(path),
                    path);
                _documentHost.Register(oldDocument);
                materialWindow.AttachHostedDocument(oldDocument);
                _hostedMaterialDocuments.Add(
                    materialWindow,
                    oldDocument);
            }
            catch (Exception rollbackError)
            {
                throw new AggregateException(
                    "Material identity rebind failed and the old hosted " +
                    "document could not be restored.",
                    replacementError,
                    rollbackError);
            }
            throw;
        }
    }

    private void ApproveHostedMaterialWindowsForOwnerClose(
        bool discardUnsavedChanges)
    {
        foreach (MaterialEditorWindow materialWindow in
                 _hostedMaterialDocuments.Keys.ToArray())
        {
            materialWindow.ApproveHostedOwnerClose(discardUnsavedChanges);
        }
    }

    private EditorDocument EnsureSceneDocumentRegistered(
        bool use3D,
        EditorDocumentState? initialState = null)
    {
        EditorDocumentId id = SceneDocumentId();
        string? sourcePath = SceneDocumentPresentationPath();
        string displayName = string.IsNullOrWhiteSpace(sourcePath)
            ? "Untitled Scene"
            : Path.GetFileName(sourcePath);

        if (_documentHost.TryGet(id, out EditorDocument existing))
        {
            if (use3D == _view3d)
                existing.UpdatePresentation(displayName, sourcePath);
            return existing;
        }

        bool initiallySaved = !_scene2DDirty && !_scene3DDirty;
        var document = new EditorDocument(
            id,
            displayName,
            sourcePath,
            CaptureCanonicalSceneDocumentState,
            RestoreCanonicalSceneDocumentState,
            SaveCanonicalSceneDocumentThroughHostAsync,
            initiallySaved,
            initialState: initialState);
        return _documentHost.Register(document);
    }

    /// <summary>
    /// Reversible compatibility envelope above the current native 2D and 3D graphs. Both native
    /// serializers remain authoritative for their current formats; the managed workspace identity
    /// and transaction history stay singular so a future unified world serializer can replace this
    /// adapter without changing EditorDocumentHost.
    /// </summary>
    private EditorDocumentState CaptureCanonicalSceneDocumentState()
    {
        if (Engine == IntPtr.Zero)
            throw new InvalidOperationException("The editor engine is unavailable.");
        string legacy2D = EngineInterop.SceneText(Engine);
        string world3D = EngineInterop.Scene3DText(Engine);
        // Native scene serializers contain authored subsystem payload, not editor camera/projection
        // state. NormalizeSceneSnapshot additionally removes editor selection from dirty tracking.
        return new EditorDocumentState(
            SceneWorldDocumentEnvelope.Pack(legacy2D, world3D),
            SceneWorldDocumentEnvelope.Pack(
                NormalizeSceneSnapshot(legacy2D),
                NormalizeSceneSnapshot(world3D)));
    }

    private void RestoreCanonicalSceneDocumentState(EditorDocumentState state)
    {
        if (Engine == IntPtr.Zero)
            throw new InvalidOperationException("The editor engine is unavailable.");
        if (EngineInterop.acs_editor_play_state(Engine) != 0 ||
            PreviewBtn.IsChecked == true)
        {
            throw new InvalidOperationException(
                "Stop Play/Preview before restoring document history.");
        }

        SceneWorldDocumentEnvelope.Unpack(
            state.Payload,
            out string legacy2D,
            out string world3D);
        string rollback2D = EngineInterop.SceneText(Engine);
        string rollback3D = EngineInterop.Scene3DText(Engine);
        int loaded = EngineInterop.acs_editor_scene_document_load_text(
            Engine, legacy2D, world3D);
        if (loaded == 0)
        {
            // Best-effort rollback keeps a failed managed transaction from leaving half a world.
            EngineInterop.acs_editor_scene_document_load_text(
                Engine, rollback2D, rollback3D);
            throw new InvalidDataException(
                "The canonical scene transaction snapshot was rejected.");
        }

        RefreshAfterSceneChange();
        _sceneMutationRevision.NotifyMutation();
        RefreshDirtyStateFromNativeScene();
        // EditorDocument installs the supplied state immediately after this callback returns, so
        // the successful restore itself is the document capture for this revision.
        _sceneMutationRevision.AcknowledgeDocument();
        RememberActiveSceneDocumentState();
    }

    private async ValueTask<EditorDocumentSaveResult> SaveCanonicalSceneDocumentThroughHostAsync(
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        SaveAllResult result = await SaveAllInitializedSceneDocumentsAsync();
        cancellationToken.ThrowIfCancellationRequested();
        return result.Completion switch
        {
            SaveAllCompletion.Success =>
                EditorDocumentSaveResult.Saved(
                    SceneWorldDocumentEnvelope.Pack(
                        _hostSavedSubsystem2D ?? "",
                        _hostSavedSubsystem3D ?? "")),
            SaveAllCompletion.Cancelled =>
                EditorDocumentSaveResult.Cancelled(result.Detail),
            _ => EditorDocumentSaveResult.Failed(result.Detail),
        };
    }

    private Task<EditorDocumentSaveBatchResult> SaveAllHostedDocumentsAsync(
        CancellationToken cancellationToken = default)
    {
        if (!_documentHostInitialized)
            throw new InvalidOperationException(
                "The document host has not been initialized.");
        return _documentHost.SaveAllAsync(cancellationToken).AsTask();
    }

    private Task<EditorDocumentCloseResult> PrepareHostedDocumentsForCloseAsync(
        EditorDocumentCloseChoice choice,
        CancellationToken cancellationToken = default)
    {
        if (!_documentHostInitialized)
            throw new InvalidOperationException(
                "The document host has not been initialized.");
        return _documentHost.PrepareCloseAsync(choice, cancellationToken).AsTask();
    }

    private bool TryRefreshHostedDirtyDocuments(
        out IReadOnlyList<EditorDocument> dirtyDocuments,
        out string detail)
    {
        if (!_documentHostInitialized)
        {
            dirtyDocuments = Array.Empty<EditorDocument>();
            detail = "the document host has not been initialized";
            return false;
        }
        return _documentHost.TryRefreshDirtyDocuments(
            out dirtyDocuments,
            out detail);
    }

    private void ReportHostedSaveAllResult(
        EditorDocumentSaveBatchResult result,
        string operation = "Save All")
    {
        EditorDocumentSaveDiagnostic? firstIssue = result.Diagnostics
            .Where(diagnostic =>
                diagnostic.Status != EditorDocumentSaveStatus.Saved ||
                diagnostic.RemainsDirty)
            .Select(diagnostic => (EditorDocumentSaveDiagnostic?)diagnostic)
            .FirstOrDefault();
        string progress =
            $"{result.SavedCount}/{result.PlannedCount} saved";
        string message = result.Completion switch
        {
            EditorDocumentBatchCompletion.Success when result.PlannedCount == 0 =>
                $"{operation}: no dirty documents.",
            EditorDocumentBatchCompletion.Success =>
                $"{operation}: {progress}.",
            EditorDocumentBatchCompletion.Cancelled =>
                $"{operation} cancelled ({progress}); unsaved documents were kept.",
            _ when firstIssue is { } issue =>
                $"{operation} incomplete ({progress}) at {issue.DisplayName}: " +
                (string.IsNullOrWhiteSpace(issue.Detail)
                    ? issue.Status.ToString()
                    : issue.Detail),
            _ =>
                $"{operation} incomplete ({progress}); " +
                $"{result.RemainingDirtyDocuments.Count} document(s) remain dirty.",
        };
        StatusText.Text = message;
        Log(message, "Document", result.Completion switch
        {
            EditorDocumentBatchCompletion.Success => LogLevel.Success,
            EditorDocumentBatchCompletion.Cancelled => LogLevel.Warn,
            _ => LogLevel.Error,
        });
    }

    private async Task<bool> SaveHostedSceneDocumentAsync()
    {
        if (!TryBeginSceneSourceSave(
                out SceneSourceSaveScope? saveScope,
                out string saveBlockedReason))
        {
            string blockedMessage =
                "Scene save is unavailable: " + saveBlockedReason + ".";
            StatusText.Text = blockedMessage;
            Log(blockedMessage, "Scene", LogLevel.Warn);
            return false;
        }
        using SceneSourceSaveScope saveLease = saveScope!;

        if (!_documentHostInitialized)
            return await SaveActiveSceneAsync();

        ulong startingRevision = _sceneMutationRevision.Current;
        EditorDocumentSaveResult result = await _documentHost.SaveActiveAsync();
        bool isSuspended = _documentHost.ActiveDocument?.IsSuspended ?? true;
        if (SceneMutationRevisionGate.ShouldAcknowledgeCompletedSave(
                startingRevision,
                _sceneMutationRevision.Current,
                result.Status,
                isSuspended))
        {
            _sceneMutationRevision.AcknowledgeDocument();
        }
        string message = result.Status switch
        {
            EditorDocumentSaveStatus.Saved => "Saved the scene document.",
            EditorDocumentSaveStatus.Cancelled => "Scene save was cancelled.",
            EditorDocumentSaveStatus.Unsupported => result.Detail,
            _ => "Scene save failed: " + result.Detail,
        };
        StatusText.Text = message;
        Log(
            message,
            "Scene",
            result.Status switch
            {
                EditorDocumentSaveStatus.Saved => LogLevel.Success,
                EditorDocumentSaveStatus.Cancelled => LogLevel.Warn,
                _ => LogLevel.Error,
            });
        return result.Status == EditorDocumentSaveStatus.Saved;
    }

    private void ActivateSceneDocument(
        bool use3D,
        bool synchronizeOutgoing = true)
    {
        if (!_documentHostInitialized)
            return;
        EditorDocument document = EnsureSceneDocumentRegistered(use3D);
        _documentHost.Activate(document.Id, synchronizeOutgoing);
        CommandManager.InvalidateRequerySuggested();
    }

    private void RecordSceneDocumentChange(
        string label,
        string? mergeKey = null,
        TimeSpan? mergeWindow = null,
        int? nodeId = null)
    {
        if (!_documentHostInitialized ||
            _sceneHistorySimulationSuspended ||
            _sceneOpenTransactionInProgress ||
            Engine == IntPtr.Zero ||
            EngineInterop.acs_editor_play_state(Engine) != 0 ||
            PreviewBtn.IsChecked == true)
        {
            return;
        }

        EditorDocument active = EnsureSceneDocumentRegistered(_view3d);
        if (!ReferenceEquals(_documentHost.ActiveDocument, active))
            _documentHost.Activate(active.Id);
        active.NotifyPotentialChange();
        bool changed = active.Synchronize(
            label,
            ScopeSceneMergeKey(mergeKey, nodeId),
            mergeWindow);
        ResetPendingSceneHistoryMetadata();
        _sceneMutationRevision.AcknowledgeDocument();
        if (changed)
        {
            _sceneMutationRevision.NotifyMutation();
            _sceneMutationRevision.AcknowledgeDocument();
            SetSceneDirty(true);
        }
    }

    private IDisposable BeginSceneDocumentTransaction(
        string label,
        string? mergeKey = null,
        TimeSpan? mergeWindow = null,
        int? nodeId = null)
    {
        if (!_documentHostInitialized ||
            _sceneHistorySimulationSuspended ||
            Engine == IntPtr.Zero)
        {
            return EmptyDisposable.Instance;
        }

        EditorDocument active = EnsureSceneDocumentRegistered(_view3d);
        NotifySceneMutationPending();
        IDisposable inner = active.BeginTransaction(
            label,
            ScopeSceneMergeKey(mergeKey, nodeId),
            mergeWindow);
        return new SceneDocumentTransactionScope(this, active, inner);
    }

    private void ResetSceneDocumentHistory(bool use3D, bool markSaved)
    {
        if (!_documentHostInitialized)
            return;
        EditorDocument document = EnsureSceneDocumentRegistered(use3D);
        EditorDocumentState currentState = CaptureCanonicalSceneDocumentState();
        SceneWorldDocumentEnvelope.Unpack(
            currentState.ContentFingerprint,
            out _hostSavedSubsystem2D,
            out _hostSavedSubsystem3D);
        document.ResetHistory(markSaved, currentState);
        ResetPendingSceneHistoryMetadata();
        _sceneMutationRevision.AcknowledgeDocument();
        if (_view3d == use3D)
            _documentHost.Activate(document.Id, synchronizeOutgoing: false);
        CommandManager.InvalidateRequerySuggested();
    }

    private void NotifySceneDocumentSaved(bool use3D, string? sourcePath)
    {
        if (!_documentHostInitialized)
            return;
        EditorDocument document = EnsureSceneDocumentRegistered(_view3d);
        if (use3D == _view3d)
        {
            document.UpdatePresentation(
                string.IsNullOrWhiteSpace(sourcePath)
                    ? "Untitled Scene"
                    : Path.GetFileName(sourcePath),
                sourcePath);
        }

        if (use3D)
        {
            _hostSavedSubsystem3D = _scene3DSavedSnapshot ??
                NormalizeSceneSnapshot(EngineInterop.Scene3DText(Engine));
        }
        else
        {
            _hostSavedSubsystem2D = _scene2DSavedSnapshot ??
                NormalizeSceneSnapshot(EngineInterop.SceneText(Engine));
        }

        // The compatibility adapter has two source durability boundaries. The singular managed
        // document becomes clean only after every initialized native graph is source-clean.
        if ((!_scene2DInitialized || !_scene2DDirty) &&
            (!_scene3DInitialized || !_scene3DDirty))
        {
            document.MarkSavedFingerprint(SceneWorldDocumentEnvelope.Pack(
                _hostSavedSubsystem2D ?? "",
                _hostSavedSubsystem3D ?? ""));
            _sceneMutationRevision.AcknowledgeDocument();
        }
        CommandManager.InvalidateRequerySuggested();
    }

    private void SuspendSceneDocumentHistoryForSimulation()
    {
        if (!_documentHostInitialized || _sceneHistorySimulationSuspended)
            return;
        EditorDocument active = EnsureSceneDocumentRegistered(_view3d);
        active.Synchronize("Edit Scene");
        _sceneMutationRevision.AcknowledgeDocument();
        active.Suspend(synchronize: false);
        _sceneHistorySimulationSuspended = true;
        CommandManager.InvalidateRequerySuggested();
    }

    private void ResumeSceneDocumentHistoryAfterSimulation()
    {
        if (!_documentHostInitialized || !_sceneHistorySimulationSuspended)
            return;
        _sceneHistorySimulationSuspended = false;
        EditorDocument active = EnsureSceneDocumentRegistered(_view3d);
        active.Resume(acceptCurrentWithoutTransaction: true);
        _sceneMutationRevision.AcknowledgeDocument();
        RefreshDirtyStateFromNativeScene();
        RememberActiveSceneDocumentState();
        CommandManager.InvalidateRequerySuggested();
    }

    private void OnDocumentHistoryTick(object? sender, EventArgs e)
    {
        if (!_documentHostInitialized ||
            _sceneHistorySimulationSuspended ||
            Engine == IntPtr.Zero)
        {
            return;
        }

        if (!_sceneMutationRevision.DocumentCaptureRequired)
            return;
        if (EngineInterop.acs_editor_play_state(Engine) != 0 ||
            PreviewBtn.IsChecked == true)
            return;

        // Fallback capture for component editors whose controls are generated dynamically. Explicit
        // structural/transform hooks below give better labels. The revision gate makes this timer
        // free while idle: no native serializer runs until a mutation entry point explicitly marks
        // the canonical document pending.
        EditorDocument active = EnsureSceneDocumentRegistered(_view3d);
        if (!active.HasPendingChanges)
            return; // An explicit transaction captures and acknowledges itself when it closes.
        active.Synchronize(
            _pendingSceneHistoryLabel,
            mergeKey: _pendingSceneHistoryMergeKey,
            mergeWindow: _pendingSceneHistoryMergeKey == null
                ? TimeSpan.Zero
                : TimeSpan.FromSeconds(1));
        ResetPendingSceneHistoryMetadata();
        _sceneMutationRevision.AcknowledgeDocument();
    }

    private bool CanUseSceneDocumentHistory()
    {
        if (!_documentHostInitialized ||
            Engine == IntPtr.Zero ||
            IsSceneEditingBlocked ||
            _sceneHistorySimulationSuspended ||
            EngineInterop.acs_editor_play_state(Engine) != 0 ||
            PreviewBtn.IsChecked == true)
        {
            return false;
        }

        // Text controls own their local character undo stack. Scene transactions resume when focus
        // returns to the viewport/hierarchy/Inspector chrome.
        return Keyboard.FocusedElement is not System.Windows.Controls.Primitives.TextBoxBase;
    }

    private void OnHostedDocumentStateChanged(object? sender, EventArgs e) =>
        CommandManager.InvalidateRequerySuggested();

    /// <summary>
    /// Cheap mutation-side signal used by generated Inspector controls and other native setters
    /// that deliberately defer the canonical two-subsystem history capture to the debounce timer.
    /// </summary>
    private void NotifySceneMutationPending(
        string label = "Edit Scene",
        string? mergeKey = null,
        int? nodeId = null)
    {
        if (Engine == IntPtr.Zero ||
            _sceneHistorySimulationSuspended ||
            EngineInterop.acs_editor_play_state(Engine) != 0 ||
            PreviewBtn.IsChecked == true)
        {
            return;
        }

        _sceneMutationRevision.NotifyMutation();
        SetSceneDirty(true);
        if (!_documentHostInitialized)
            return;

        EditorDocument active = EnsureSceneDocumentRegistered(_view3d);
        if (!ReferenceEquals(_documentHost.ActiveDocument, active))
            _documentHost.Activate(active.Id);
        string? scopedMergeKey = ScopeSceneMergeKey(mergeKey, nodeId);
        if (!active.HasPendingChanges)
        {
            _pendingSceneHistoryLabel = string.IsNullOrWhiteSpace(label)
                ? "Edit Scene"
                : label.Trim();
            _pendingSceneHistoryMergeKey = scopedMergeKey;
        }
        else if (!string.Equals(
                     _pendingSceneHistoryMergeKey,
                     scopedMergeKey,
                     StringComparison.Ordinal))
        {
            // Different deferred edits can share one debounce snapshot, but that snapshot must not
            // subsequently merge with either property's previous transaction.
            _pendingSceneHistoryLabel = "Edit Scene";
            _pendingSceneHistoryMergeKey = null;
        }
        active.NotifyPotentialChange();
    }

    internal static string BuildSceneMergeKey(
        bool use3D,
        int nodeId,
        string propertyIdentity,
        ulong selectionEpoch = 0)
    {
        string identity = string.IsNullOrWhiteSpace(propertyIdentity)
            ? "edit"
            : propertyIdentity.Trim();
        return $"scene.{(use3D ? "3d" : "2d")}.node.{nodeId}.{identity}.selection.{selectionEpoch}";
    }

    private string? ScopeSceneMergeKey(string? mergeKey, int? nodeId = null)
    {
        if (string.IsNullOrWhiteSpace(mergeKey))
            return null;
        int currentNode = CurrentSceneMergeNodeId();
        int selectedNode = nodeId ?? currentNode;
        return BuildSceneMergeKey(
            _view3d,
            selectedNode,
            mergeKey,
            _sceneMergeSelectionEpoch);
    }

    private int CurrentSceneMergeNodeId()
    {
        if (Engine == IntPtr.Zero)
            return -1;
        int nodeId = _view3d
            ? EngineInterop.acs_editor_selected3d(Engine)
            : EngineInterop.acs_editor_selected(Engine);
        int selectionCount = _view3d
            ? EngineInterop.acs_editor_selected3d_count(Engine)
            : EngineInterop.acs_editor_selection_count(Engine);
        ObserveSceneSelectionForMerge(_view3d, nodeId, selectionCount);
        return nodeId;
    }

    private void ObserveSceneSelectionForMerge(
        bool use3D,
        int nodeId,
        int selectionCount)
    {
        if (_sceneMergeSelectionInitialized &&
            (_sceneMergeSelection3D != use3D ||
             _sceneMergeSelectionNodeId != nodeId ||
             _sceneMergeSelectionCount != selectionCount))
        {
            _sceneMergeSelectionEpoch = _sceneMergeSelectionEpoch == ulong.MaxValue
                ? 1
                : _sceneMergeSelectionEpoch + 1;
        }

        _sceneMergeSelectionInitialized = true;
        _sceneMergeSelection3D = use3D;
        _sceneMergeSelectionNodeId = nodeId;
        _sceneMergeSelectionCount = selectionCount;
    }

    private void ResetPendingSceneHistoryMetadata()
    {
        _pendingSceneHistoryLabel = "Edit Scene";
        _pendingSceneHistoryMergeKey = null;
    }

    private void CompleteSceneDocumentTransaction(EditorDocument document)
    {
        if (document.IsInTransaction)
            return;
        // The inner scope has already captured the final state. Advance once more because a long
        // drag may have had its initial revision inspected by the workspace timer mid-gesture.
        _sceneMutationRevision.NotifyMutation();
        _sceneMutationRevision.AcknowledgeDocument();
        SetSceneDirty(true);
    }

    private sealed class SceneDocumentTransactionScope : IDisposable
    {
        private MainWindow? _owner;
        private EditorDocument? _document;
        private IDisposable? _inner;

        internal SceneDocumentTransactionScope(
            MainWindow owner,
            EditorDocument document,
            IDisposable inner)
        {
            _owner = owner;
            _document = document;
            _inner = inner;
        }

        public void Dispose()
        {
            IDisposable? inner = Interlocked.Exchange(ref _inner, null);
            if (inner == null)
                return;
            inner.Dispose();
            MainWindow? owner = Interlocked.Exchange(ref _owner, null);
            EditorDocument? document = Interlocked.Exchange(ref _document, null);
            if (owner != null && document != null)
                owner.CompleteSceneDocumentTransaction(document);
        }
    }

    private sealed class EmptyDisposable : IDisposable
    {
        internal static readonly EmptyDisposable Instance = new();
        public void Dispose() { }
    }
}
