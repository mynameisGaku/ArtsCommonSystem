// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Stable, session-independent identity for an editor document. Identity is deliberately separate
/// from SourcePath: Save As and asset moves must not silently replace the undo stack.
/// </summary>
internal readonly record struct EditorDocumentId
{
    internal EditorDocumentId(string kind, string stableId)
    {
        if (string.IsNullOrWhiteSpace(kind))
            throw new ArgumentException("Document kind is required.", nameof(kind));
        if (string.IsNullOrWhiteSpace(stableId))
            throw new ArgumentException("Document stable identity is required.", nameof(stableId));
        Kind = kind.Trim().ToLowerInvariant();
        StableId = stableId.Trim();
    }

    internal string Kind { get; }
    internal string StableId { get; }

    internal static EditorDocumentId ForFile(string kind, string path)
    {
        string full = Path.GetFullPath(path);
        return new EditorDocumentId(kind, OperatingSystem.IsWindows()
            ? full.ToUpperInvariant()
            : full);
    }

    public override string ToString() => $"{Kind}:{StableId}";
}

/// <summary>
/// Immutable state captured at a transaction boundary. Payload may include editor-only state such
/// as selection, while ContentFingerprint must describe only durable document content.
/// </summary>
internal sealed record EditorDocumentState(string Payload, string ContentFingerprint)
{
    internal static EditorDocumentState Text(
        string payload,
        Func<string, string>? contentNormalizer = null)
    {
        payload ??= "";
        return new EditorDocumentState(
            payload,
            contentNormalizer == null ? payload : contentNormalizer(payload));
    }
}

internal enum EditorDocumentSaveStatus
{
    Saved,
    Cancelled,
    Failed,
    Unsupported,
}

internal readonly record struct EditorDocumentSaveResult(
    EditorDocumentSaveStatus Status,
    string Detail = "",
    string? SavedFingerprint = null)
{
    internal static EditorDocumentSaveResult Saved(string? savedFingerprint = null) =>
        new(EditorDocumentSaveStatus.Saved, "", savedFingerprint);

    internal static EditorDocumentSaveResult Cancelled(string detail = "") =>
        new(EditorDocumentSaveStatus.Cancelled, detail);

    internal static EditorDocumentSaveResult Failed(string detail) =>
        new(EditorDocumentSaveStatus.Failed, detail);

    internal static EditorDocumentSaveResult Unsupported(string detail = "") =>
        new(EditorDocumentSaveStatus.Unsupported, detail);
}

internal enum EditorDocumentBatchCompletion
{
    Success,
    Cancelled,
    Failed,
}

internal readonly record struct EditorDocumentSaveDiagnostic(
    EditorDocumentId DocumentId,
    string DisplayName,
    EditorDocumentSaveStatus Status,
    string Detail,
    bool RemainsDirty);

internal sealed class EditorDocumentSaveBatchResult
{
    internal EditorDocumentSaveBatchResult(
        EditorDocumentBatchCompletion completion,
        IReadOnlyList<EditorDocumentSaveDiagnostic> diagnostics,
        IReadOnlyList<EditorDocumentId> remainingDirtyDocuments,
        int plannedCount)
    {
        if (plannedCount < 0)
            throw new ArgumentOutOfRangeException(nameof(plannedCount));
        Completion = completion;
        Diagnostics = Array.AsReadOnly(
            (diagnostics ?? throw new ArgumentNullException(nameof(diagnostics))).ToArray());
        RemainingDirtyDocuments = Array.AsReadOnly(
            (remainingDirtyDocuments ??
             throw new ArgumentNullException(nameof(remainingDirtyDocuments))).ToArray());
        PlannedCount = plannedCount;
    }

    internal EditorDocumentBatchCompletion Completion { get; }
    internal IReadOnlyList<EditorDocumentSaveDiagnostic> Diagnostics { get; }
    internal IReadOnlyList<EditorDocumentId> RemainingDirtyDocuments { get; }
    internal int PlannedCount { get; }
    internal int SavedCount =>
        Diagnostics.Count(result => result.Status == EditorDocumentSaveStatus.Saved);
    internal int FailureCount =>
        Diagnostics.Count(result =>
            result.Status is EditorDocumentSaveStatus.Failed or
                EditorDocumentSaveStatus.Unsupported);
}

internal enum EditorDocumentCloseChoice
{
    Save,
    Discard,
    Cancel,
}

internal enum EditorDocumentCloseCompletion
{
    Ready,
    Cancelled,
    Failed,
}

internal sealed class EditorDocumentCloseResult
{
    internal EditorDocumentCloseResult(
        EditorDocumentCloseCompletion completion,
        IReadOnlyList<EditorDocumentId> dirtyDocuments,
        EditorDocumentSaveBatchResult? saveResult,
        string detail)
    {
        Completion = completion;
        DirtyDocuments = Array.AsReadOnly(
            (dirtyDocuments ?? throw new ArgumentNullException(nameof(dirtyDocuments))).ToArray());
        SaveResult = saveResult;
        Detail = detail ?? "";
    }

    internal EditorDocumentCloseCompletion Completion { get; }
    internal IReadOnlyList<EditorDocumentId> DirtyDocuments { get; }
    internal EditorDocumentSaveBatchResult? SaveResult { get; }
    internal string Detail { get; }
    internal bool CanClose => Completion == EditorDocumentCloseCompletion.Ready;
}

internal delegate ValueTask<EditorDocumentSaveResult> EditorDocumentSaveContract(
    CancellationToken cancellationToken);

internal sealed record EditorDocumentTransactionInfo(
    string Label,
    DateTimeOffset Timestamp,
    string? MergeKey);

/// <summary>
/// A document owns identity, dirty/save-point state, and a bounded transaction history. Capture and
/// restore delegates keep this class independent from WPF, native scenes, material graphs, and
/// Blueprint graphs.
/// </summary>
internal sealed class EditorDocument
{
    private sealed record Transaction(
        string Label,
        EditorDocumentState Before,
        EditorDocumentState After,
        DateTimeOffset Timestamp,
        string? MergeKey);

    private readonly Func<EditorDocumentState> _capture;
    private readonly Action<EditorDocumentState> _restore;
    private readonly EditorDocumentSaveContract? _save;
    private readonly List<Transaction> _history = new();
    private readonly int _historyLimit;
    private EditorDocumentState _observed;
    private string? _savedFingerprint;
    private int _cursor;
    private int _suspendDepth;
    private int _transactionDepth;
    private EditorDocumentState? _transactionBefore;
    private string _transactionLabel = "";
    private string? _transactionMergeKey;
    private TimeSpan _transactionMergeWindow;
    private bool _restoring;
    private bool _hasPendingChanges;

    /// <summary>
    /// In-memory rollback point for a larger document transaction such as File/Open. Native
    /// payloads are owned by the caller; this checkpoint preserves history, save-point and
    /// presentation metadata without invoking capture/restore delegates.
    /// </summary>
    internal sealed class Checkpoint
    {
        private readonly Transaction[] _history;
        private readonly string _displayName;
        private readonly string? _sourcePath;
        private readonly EditorDocumentState _observed;
        private readonly string? _savedFingerprint;
        private readonly int _cursor;
        private readonly int _suspendDepth;
        private readonly int _transactionDepth;
        private readonly EditorDocumentState? _transactionBefore;
        private readonly string _transactionLabel;
        private readonly string? _transactionMergeKey;
        private readonly TimeSpan _transactionMergeWindow;
        private readonly bool _restoring;
        private readonly bool _hasPendingChanges;

        private Checkpoint(EditorDocument document)
        {
            _history = document._history.ToArray();
            _displayName = document.DisplayName;
            _sourcePath = document.SourcePath;
            _observed = document._observed;
            _savedFingerprint = document._savedFingerprint;
            _cursor = document._cursor;
            _suspendDepth = document._suspendDepth;
            _transactionDepth = document._transactionDepth;
            _transactionBefore = document._transactionBefore;
            _transactionLabel = document._transactionLabel;
            _transactionMergeKey = document._transactionMergeKey;
            _transactionMergeWindow = document._transactionMergeWindow;
            _restoring = document._restoring;
            _hasPendingChanges = document._hasPendingChanges;
        }

        internal static Checkpoint Capture(EditorDocument document) =>
            new(document);

        internal void Restore(EditorDocument document)
        {
            document._history.Clear();
            document._history.AddRange(_history);
            document.DisplayName = _displayName;
            document.SourcePath = _sourcePath;
            document._observed = _observed;
            document._savedFingerprint = _savedFingerprint;
            document._cursor = _cursor;
            document._suspendDepth = _suspendDepth;
            document._transactionDepth = _transactionDepth;
            document._transactionBefore = _transactionBefore;
            document._transactionLabel = _transactionLabel;
            document._transactionMergeKey = _transactionMergeKey;
            document._transactionMergeWindow = _transactionMergeWindow;
            document._restoring = _restoring;
            document._hasPendingChanges = _hasPendingChanges;
        }
    }

    internal EditorDocument(
        EditorDocumentId id,
        string displayName,
        string? sourcePath,
        Func<EditorDocumentState> capture,
        Action<EditorDocumentState> restore,
        EditorDocumentSaveContract? save = null,
        bool initiallySaved = true,
        int historyLimit = 128,
        EditorDocumentState? initialState = null,
        int saveOrder = 0)
    {
        if (historyLimit < 1)
            throw new ArgumentOutOfRangeException(nameof(historyLimit));
        Id = id;
        DisplayName = string.IsNullOrWhiteSpace(displayName) ? id.ToString() : displayName;
        SourcePath = sourcePath;
        _capture = capture ?? throw new ArgumentNullException(nameof(capture));
        _restore = restore ?? throw new ArgumentNullException(nameof(restore));
        _save = save;
        _historyLimit = historyLimit;
        SaveOrder = saveOrder;
        _observed = initialState == null
            ? CaptureVerified()
            : VerifyState(initialState);
        _savedFingerprint = initiallySaved ? _observed.ContentFingerprint : null;
    }

    internal EditorDocumentId Id { get; }
    internal int SaveOrder { get; }
    internal string DisplayName { get; private set; }
    internal string? SourcePath { get; private set; }
    internal Exception? LastObserverError { get; private set; }
    internal bool IsDirty =>
        _hasPendingChanges ||
        _savedFingerprint == null ||
        !string.Equals(
            _observed.ContentFingerprint,
            _savedFingerprint,
            StringComparison.Ordinal);
    internal bool CanUndo =>
        _suspendDepth == 0 && _transactionDepth == 0 && _cursor > 0;
    internal bool CanRedo =>
        _suspendDepth == 0 &&
        _transactionDepth == 0 &&
        _cursor < _history.Count &&
        !HasPendingChanges;
    internal bool IsSuspended => _suspendDepth > 0;
    internal bool IsInTransaction => _transactionDepth > 0;
    internal bool IsRestoring => _restoring;
    internal int UndoCount => _cursor;
    internal int RedoCount => _history.Count - _cursor;
    internal EditorDocumentTransactionInfo? NextUndo =>
        _cursor <= 0 ? null : Info(_history[_cursor - 1]);
    internal EditorDocumentTransactionInfo? NextRedo =>
        _cursor >= _history.Count ? null : Info(_history[_cursor]);

    internal event EventHandler? StateChanged;

    internal Checkpoint CaptureCheckpoint() => Checkpoint.Capture(this);

    internal void RestoreCheckpoint(Checkpoint checkpoint)
    {
        ArgumentNullException.ThrowIfNull(checkpoint);
        checkpoint.Restore(this);
        RaiseChanged();
    }

    /// <summary>
    /// Cached notification state only. Querying command availability must never invoke the native
    /// serializer; mutation entry points call NotifyPotentialChange before the next synchronization.
    /// </summary>
    internal bool HasPendingChanges =>
        !_restoring &&
        _suspendDepth == 0 &&
        _transactionDepth == 0 &&
        _hasPendingChanges;

    internal void NotifyPotentialChange()
    {
        if (_restoring || _suspendDepth > 0 || _hasPendingChanges)
            return;
        _hasPendingChanges = true;
        RaiseChanged();
    }

    internal void UpdatePresentation(string displayName, string? sourcePath)
    {
        if (!string.IsNullOrWhiteSpace(displayName))
            DisplayName = displayName;
        SourcePath = sourcePath;
        RaiseChanged();
    }

    /// <summary>
    /// Records a change made outside an explicit transaction. A merge key coalesces high-frequency
    /// Inspector typing/gizmo samples without combining unrelated structural edits.
    /// </summary>
    internal bool Synchronize(
        string label,
        string? mergeKey = null,
        TimeSpan? mergeWindow = null)
    {
        if (_restoring || _suspendDepth > 0 || _transactionDepth > 0)
            return false;

        EditorDocumentState current = CaptureVerified();
        bool wasPending = _hasPendingChanges;
        _hasPendingChanges = false;
        if (HistoryEquivalent(current, _observed))
        {
            // Selection is editor state, not a transaction by itself. Keep the latest payload so a
            // following content edit restores the selection that actually preceded it.
            _observed = current;
            if (wasPending)
                RaiseChanged();
            return false;
        }

        Record(
            string.IsNullOrWhiteSpace(label) ? "Edit" : label.Trim(),
            _observed,
            current,
            mergeKey,
            mergeWindow ?? TimeSpan.Zero);
        _observed = current;
        RaiseChanged();
        return true;
    }

    internal EditorDocumentTransactionScope BeginTransaction(
        string label,
        string? mergeKey = null,
        TimeSpan? mergeWindow = null)
    {
        if (_suspendDepth > 0)
            return new EditorDocumentTransactionScope(this, active: false);

        if (_transactionDepth == 0)
        {
            Synchronize("Edit");
            // Synchronize already captured the latest payload. Reusing it avoids a second complete
            // native scene serialization at the beginning of every gizmo/Inspector transaction.
            _transactionBefore = _observed;
            _transactionLabel = string.IsNullOrWhiteSpace(label) ? "Edit" : label.Trim();
            _transactionMergeKey = mergeKey;
            _transactionMergeWindow = mergeWindow ?? TimeSpan.Zero;
        }
        _transactionDepth++;
        return new EditorDocumentTransactionScope(this, active: true);
    }

    internal bool Undo(out EditorDocumentTransactionInfo? transaction)
    {
        transaction = null;
        if (_suspendDepth > 0 || _transactionDepth > 0)
            return false;
        Synchronize("Edit");
        if (_cursor <= 0)
            return false;

        Transaction entry = _history[_cursor - 1];
        RestoreVerified(entry.Before);
        _cursor--;
        _observed = entry.Before;
        _hasPendingChanges = false;
        transaction = Info(entry);
        RaiseChanged();
        return true;
    }

    internal bool Redo(out EditorDocumentTransactionInfo? transaction)
    {
        transaction = null;
        if (_suspendDepth > 0 || _transactionDepth > 0)
            return false;
        // An edit after Undo creates a new branch and invalidates Redo.
        Synchronize("Edit");
        if (_cursor >= _history.Count)
            return false;

        Transaction entry = _history[_cursor];
        RestoreVerified(entry.After);
        _cursor++;
        _observed = entry.After;
        _hasPendingChanges = false;
        transaction = Info(entry);
        RaiseChanged();
        return true;
    }

    /// <summary>
    /// Establishes a durability save point without clearing history. Undoing past this point marks
    /// the document dirty; redoing back to it becomes clean again.
    /// </summary>
    internal void MarkSaved(EditorDocumentState? savedState = null)
    {
        if (_suspendDepth > 0)
            throw new InvalidOperationException("A suspended document cannot be marked saved.");
        Synchronize("Edit");
        // Synchronize already captured the live state. A supplied state remains useful for callers
        // that committed an explicitly frozen payload.
        EditorDocumentState current = savedState ?? _observed;
        _observed = current;
        _hasPendingChanges = false;
        _savedFingerprint = current.ContentFingerprint;
        RaiseChanged();
    }

    /// <summary>
    /// Establishes the fingerprint that was durably written while retaining the freshly captured
    /// live state. This prevents edits made during an asynchronous save-cleanup window from being
    /// mistaken for part of the saved source.
    /// </summary>
    internal void MarkSavedFingerprint(string savedFingerprint)
    {
        if (savedFingerprint == null)
            throw new ArgumentNullException(nameof(savedFingerprint));
        if (_suspendDepth > 0)
            throw new InvalidOperationException("A suspended document cannot be marked saved.");
        Synchronize("Edit");
        _savedFingerprint = savedFingerprint;
        RaiseChanged();
    }

    /// <summary>
    /// New/Open/Recovery boundary. Source load is never undoable into a previous file. A new or
    /// recovered document passes markSaved=false so it remains dirty with an empty history.
    /// </summary>
    internal void ResetHistory(
        bool markSaved,
        EditorDocumentState? currentState = null)
    {
        EditorDocumentState current = currentState == null
            ? CaptureVerified()
            : VerifyState(currentState);
        _history.Clear();
        _cursor = 0;
        _transactionDepth = 0;
        _transactionBefore = null;
        _observed = current;
        _hasPendingChanges = false;
        _savedFingerprint = markSaved ? current.ContentFingerprint : null;
        RaiseChanged();
    }

    internal void ApplyRecoveredState(EditorDocumentState recoveredState)
    {
        ArgumentNullException.ThrowIfNull(recoveredState);
        if (_suspendDepth > 0)
            throw new InvalidOperationException(
                "A suspended document cannot apply recovery state.");
        if (_transactionDepth > 0)
            throw new InvalidOperationException(
                "Complete the active transaction before applying recovery state.");

        EditorDocumentState verifiedRecovery = VerifyState(recoveredState);
        EditorDocumentState rollbackState = CaptureVerified();
        Checkpoint rollbackCheckpoint = CaptureCheckpoint();
        try
        {
            RestoreVerified(verifiedRecovery);
            EditorDocumentState liveRecovery = CaptureVerified();
            if (!HistoryEquivalent(verifiedRecovery, liveRecovery))
            {
                throw new InvalidDataException(
                    "Recovered content does not match its declared fingerprint.");
            }
            ResetHistory(markSaved: false, currentState: liveRecovery);
        }
        catch (Exception recoveryError)
        {
            try
            {
                RestoreVerified(rollbackState);
                rollbackCheckpoint.Restore(this);
                RaiseChanged();
            }
            catch (Exception rollbackError)
            {
                throw new AggregateException(
                    "Document recovery failed and the live-state rollback also failed.",
                    recoveryError,
                    rollbackError);
            }
            throw;
        }
    }

    /// <summary>Clears undo/redo while preserving the existing dirty/save point relationship.</summary>
    internal void ClearHistory()
    {
        EditorDocumentState current = CaptureVerified();
        _history.Clear();
        _cursor = 0;
        _transactionDepth = 0;
        _transactionBefore = null;
        _observed = current;
        _hasPendingChanges = false;
        RaiseChanged();
    }

    internal void Suspend(bool synchronize = true)
    {
        if (_suspendDepth == 0 && synchronize)
            Synchronize("Edit");
        _suspendDepth++;
        RaiseChanged();
    }

    /// <summary>
    /// Runtime Play/Preview is non-destructive. On resume, accept the restored edit state without
    /// creating a transaction for simulation frames.
    /// </summary>
    internal void Resume(bool acceptCurrentWithoutTransaction = true)
    {
        if (_suspendDepth == 0)
            return;
        _suspendDepth--;
        if (_suspendDepth == 0 && acceptCurrentWithoutTransaction)
        {
            _observed = CaptureVerified();
            _hasPendingChanges = false;
        }
        RaiseChanged();
    }

    internal async ValueTask<EditorDocumentSaveResult> SaveAsync(
        CancellationToken cancellationToken = default)
    {
        if (_save == null)
            return EditorDocumentSaveResult.Unsupported(
                $"{DisplayName} has no save contract.");
        if (_suspendDepth > 0)
            return EditorDocumentSaveResult.Failed(
                $"{DisplayName} is suspended by Play/Preview.");
        if (_transactionDepth > 0)
            return EditorDocumentSaveResult.Failed(
                $"{DisplayName} still has an open transaction.");

        Synchronize("Edit");
        cancellationToken.ThrowIfCancellationRequested();
        EditorDocumentSaveResult result = await _save(cancellationToken);
        if (result.Status == EditorDocumentSaveStatus.Saved)
        {
            if (result.SavedFingerprint != null)
                MarkSavedFingerprint(result.SavedFingerprint);
            else
                MarkSaved();
        }
        return result;
    }

    private void CompleteTransaction()
    {
        if (_transactionDepth <= 0)
            return;
        _transactionDepth--;
        if (_transactionDepth != 0)
            return;

        EditorDocumentState before = _transactionBefore ?? _observed;
        EditorDocumentState after = CaptureVerified();
        _transactionBefore = null;
        _hasPendingChanges = false;
        if (!HistoryEquivalent(before, after))
        {
            Record(
                _transactionLabel,
                before,
                after,
                _transactionMergeKey,
                _transactionMergeWindow);
        }
        _observed = after;
        RaiseChanged();
    }

    private void Record(
        string label,
        EditorDocumentState before,
        EditorDocumentState after,
        string? mergeKey,
        TimeSpan mergeWindow)
    {
        if (HistoryEquivalent(before, after))
            return;

        if (_cursor < _history.Count)
            _history.RemoveRange(_cursor, _history.Count - _cursor);

        DateTimeOffset now = DateTimeOffset.UtcNow;
        bool merge = mergeKey != null &&
                     mergeWindow > TimeSpan.Zero &&
                     _cursor > 0 &&
                     _cursor == _history.Count;
        if (merge)
        {
            Transaction previous = _history[_cursor - 1];
            merge =
                string.Equals(previous.MergeKey, mergeKey, StringComparison.Ordinal) &&
                now - previous.Timestamp <= mergeWindow &&
                HistoryEquivalent(previous.After, before);
            if (merge)
            {
                _history[_cursor - 1] = previous with
                {
                    Label = label,
                    After = after,
                    Timestamp = now,
                };
                return;
            }
        }

        _history.Add(new Transaction(label, before, after, now, mergeKey));
        _cursor = _history.Count;
        while (_history.Count > _historyLimit)
        {
            _history.RemoveAt(0);
            _cursor--;
        }
    }

    private EditorDocumentState CaptureVerified() =>
        VerifyState(
            _capture() ?? throw new InvalidOperationException(
                $"Document capture returned null for {Id}."));

    private EditorDocumentState VerifyState(EditorDocumentState state)
    {
        if (state.Payload == null)
            throw new InvalidOperationException(
                $"Document payload is null for {Id}.");
        if (state.ContentFingerprint == null)
            throw new InvalidOperationException(
                $"Document content fingerprint is null for {Id}.");
        return state;
    }

    private void RestoreVerified(EditorDocumentState state)
    {
        if (_restoring)
            throw new InvalidOperationException("Recursive document restore is not allowed.");
        _restoring = true;
        try
        {
            _restore(state);
        }
        finally
        {
            _restoring = false;
        }
    }

    private static bool HistoryEquivalent(
        EditorDocumentState first,
        EditorDocumentState second) =>
        string.Equals(
            first.ContentFingerprint,
            second.ContentFingerprint,
            StringComparison.Ordinal);

    private static EditorDocumentTransactionInfo Info(Transaction transaction) =>
        new(transaction.Label, transaction.Timestamp, transaction.MergeKey);

    private void RaiseChanged()
    {
        EventHandler? handlers = StateChanged;
        if (handlers == null)
            return;

        // State mutation is authoritative; UI/host observers are diagnostics and invalidation
        // consumers. An observer exception must never turn a committed transaction into an
        // apparent record failure that a caller then rolls back while leaving history behind.
        foreach (Delegate candidate in handlers.GetInvocationList())
        {
            try
            {
                ((EventHandler)candidate)(this, EventArgs.Empty);
            }
            catch (Exception error)
            {
                LastObserverError = error;
            }
        }
    }

    internal sealed class EditorDocumentTransactionScope : IDisposable
    {
        private EditorDocument? _owner;

        internal EditorDocumentTransactionScope(EditorDocument owner, bool active)
        {
            _owner = active ? owner : null;
        }

        public void Dispose()
        {
            EditorDocument? owner = Interlocked.Exchange(ref _owner, null);
            owner?.CompleteTransaction();
        }
    }
}

/// <summary>
/// Registry and active-document router. This is the common host boundary used by scene documents
/// now and by Material/Blueprint/Prefab/Settings adapters in later milestones.
/// </summary>
internal sealed class EditorDocumentHost
{
    private readonly Dictionary<EditorDocumentId, EditorDocument> _documents = new();
    private readonly SemaphoreSlim _saveGate = new(1, 1);

    internal EditorDocument? ActiveDocument { get; private set; }
    internal IReadOnlyList<EditorDocument> Documents =>
        Array.AsReadOnly(SnapshotInDeterministicOrder());
    internal IReadOnlyList<EditorDocument> DirtyDocuments =>
        Array.AsReadOnly(
            SnapshotInDeterministicOrder()
                .Where(document => document.IsDirty)
                .ToArray());

    internal event EventHandler? ActiveDocumentChanged;
    internal event EventHandler? DocumentStateChanged;

    internal EditorDocument Register(EditorDocument document)
    {
        ArgumentNullException.ThrowIfNull(document);
        if (_documents.ContainsKey(document.Id))
            throw new InvalidOperationException(
                $"Document is already registered: {document.Id}");
        _documents.Add(document.Id, document);
        document.StateChanged += OnDocumentStateChanged;
        return document;
    }

    internal bool TryGet(EditorDocumentId id, out EditorDocument document) =>
        _documents.TryGetValue(id, out document!);

    internal EditorDocument Get(EditorDocumentId id) =>
        _documents.TryGetValue(id, out EditorDocument? document)
            ? document
            : throw new KeyNotFoundException($"Document is not registered: {id}");

    internal EditorDocument Activate(
        EditorDocumentId id,
        bool synchronizeOutgoing = true)
    {
        EditorDocument target = Get(id);
        if (ReferenceEquals(target, ActiveDocument))
            return target;

        if (synchronizeOutgoing && ActiveDocument is { IsSuspended: false } outgoing)
            outgoing.Synchronize("Edit");
        ActiveDocument = target;
        ActiveDocumentChanged?.Invoke(this, EventArgs.Empty);
        return target;
    }

    internal bool Unregister(
        EditorDocumentId id,
        bool discardUnsavedChanges = false)
    {
        if (!_documents.TryGetValue(id, out EditorDocument? document))
            return false;
        if (!discardUnsavedChanges)
        {
            if (document.IsSuspended || document.IsInTransaction)
                return false;
            try
            {
                document.Synchronize("Edit");
            }
            catch
            {
                return false;
            }
            if (document.IsDirty)
                return false;
        }

        if (!_documents.Remove(id))
            return false;
        document.StateChanged -= OnDocumentStateChanged;
        if (ReferenceEquals(document, ActiveDocument))
        {
            ActiveDocument = null;
            ActiveDocumentChanged?.Invoke(this, EventArgs.Empty);
        }
        return true;
    }

    internal bool Clear(bool discardUnsavedChanges = false)
    {
        EditorDocument[] documents = SnapshotInDeterministicOrder();
        if (!discardUnsavedChanges)
        {
            foreach (EditorDocument document in documents)
            {
                if (document.IsSuspended || document.IsInTransaction)
                    return false;
                try
                {
                    document.Synchronize("Edit");
                }
                catch
                {
                    return false;
                }
                if (document.IsDirty)
                    return false;
            }
        }

        foreach (EditorDocument document in documents)
            document.StateChanged -= OnDocumentStateChanged;
        _documents.Clear();
        ActiveDocument = null;
        ActiveDocumentChanged?.Invoke(this, EventArgs.Empty);
        return true;
    }

    internal bool Undo(out EditorDocumentTransactionInfo? transaction)
    {
        transaction = null;
        return ActiveDocument != null && ActiveDocument.Undo(out transaction);
    }

    internal bool Redo(out EditorDocumentTransactionInfo? transaction)
    {
        transaction = null;
        return ActiveDocument != null && ActiveDocument.Redo(out transaction);
    }

    internal bool TryRefreshDirtyDocuments(
        out IReadOnlyList<EditorDocument> dirtyDocuments,
        out string detail)
    {
        EditorDocument[] refreshed =
            RefreshDirtyDocumentsDeterministically(out detail);
        dirtyDocuments = Array.AsReadOnly(refreshed);
        return string.IsNullOrEmpty(detail);
    }

    internal async ValueTask<EditorDocumentSaveResult> SaveActiveAsync(
        CancellationToken cancellationToken = default)
    {
        await _saveGate.WaitAsync(cancellationToken);
        try
        {
            return ActiveDocument == null
                ? EditorDocumentSaveResult.Unsupported(
                    "There is no active document.")
                : await ActiveDocument.SaveAsync(cancellationToken);
        }
        finally
        {
            _saveGate.Release();
        }
    }

    internal async ValueTask<EditorDocumentSaveResult> SaveDocumentAsync(
        EditorDocumentId id,
        CancellationToken cancellationToken = default)
    {
        try
        {
            await _saveGate.WaitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return EditorDocumentSaveResult.Cancelled(
                "Document save was cancelled before it acquired the host gate.");
        }

        try
        {
            return _documents.TryGetValue(id, out EditorDocument? document)
                ? await document.SaveAsync(cancellationToken)
                : EditorDocumentSaveResult.Unsupported(
                    $"Document is not registered: {id}");
        }
        finally
        {
            _saveGate.Release();
        }
    }

    internal async ValueTask<EditorDocumentSaveBatchResult> SaveAllAsync(
        CancellationToken cancellationToken = default)
    {
        try
        {
            await _saveGate.WaitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return CancelledBatch(Array.Empty<EditorDocumentSaveDiagnostic>());
        }

        try
        {
            return await SaveAllUnderGateAsync(cancellationToken);
        }
        finally
        {
            _saveGate.Release();
        }
    }

    internal async ValueTask<EditorDocumentCloseResult> PrepareCloseAsync(
        EditorDocumentCloseChoice choice,
        CancellationToken cancellationToken = default)
    {
        try
        {
            await _saveGate.WaitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return new EditorDocumentCloseResult(
                EditorDocumentCloseCompletion.Cancelled,
                DirtyIds(),
                null,
                "Close preparation was cancelled.");
        }

        try
        {
            EditorDocument[] dirtyDocuments =
                RefreshDirtyDocumentsDeterministically(
                    out string inspectionFailure);
            EditorDocumentId[] dirtyBefore = dirtyDocuments
                .Select(document => document.Id)
                .ToArray();
            if (choice == EditorDocumentCloseChoice.Cancel)
            {
                return new EditorDocumentCloseResult(
                    EditorDocumentCloseCompletion.Cancelled,
                    dirtyBefore,
                    null,
                    string.IsNullOrEmpty(inspectionFailure)
                        ? "Close was cancelled before any document was saved or discarded."
                        : "Close was cancelled; document inspection also reported: " +
                          inspectionFailure);
            }
            if (choice == EditorDocumentCloseChoice.Discard)
            {
                return new EditorDocumentCloseResult(
                    string.IsNullOrEmpty(inspectionFailure)
                        ? EditorDocumentCloseCompletion.Ready
                        : EditorDocumentCloseCompletion.Failed,
                    dirtyBefore,
                    null,
                    inspectionFailure);
            }
            if (!string.IsNullOrEmpty(inspectionFailure))
            {
                return new EditorDocumentCloseResult(
                    EditorDocumentCloseCompletion.Failed,
                    dirtyBefore,
                    null,
                    "Close cannot save because one or more documents could not be inspected: " +
                    inspectionFailure);
            }

            EditorDocumentSaveBatchResult saveResult =
                await SaveAllUnderGateAsync(cancellationToken);
            EditorDocumentCloseCompletion completion = saveResult.Completion switch
            {
                EditorDocumentBatchCompletion.Success =>
                    EditorDocumentCloseCompletion.Ready,
                EditorDocumentBatchCompletion.Cancelled =>
                    EditorDocumentCloseCompletion.Cancelled,
                _ => EditorDocumentCloseCompletion.Failed,
            };
            string detail = completion switch
            {
                EditorDocumentCloseCompletion.Ready => "",
                EditorDocumentCloseCompletion.Cancelled =>
                    "Close was cancelled while saving documents.",
                _ => "One or more documents could not be saved.",
            };
            return new EditorDocumentCloseResult(
                completion,
                dirtyBefore,
                saveResult,
                detail);
        }
        finally
        {
            _saveGate.Release();
        }
    }

    private async ValueTask<EditorDocumentSaveBatchResult> SaveAllUnderGateAsync(
        CancellationToken cancellationToken)
    {
        var diagnostics = new List<EditorDocumentSaveDiagnostic>();
        var savePlan = new List<EditorDocument>();
        foreach (EditorDocument document in SnapshotInDeterministicOrder())
        {
            if (cancellationToken.IsCancellationRequested)
                return CancelledBatch(diagnostics);

            if (document.IsSuspended)
            {
                diagnostics.Add(new EditorDocumentSaveDiagnostic(
                    document.Id,
                    document.DisplayName,
                    EditorDocumentSaveStatus.Failed,
                    "Document is suspended by Play/Preview.",
                    document.IsDirty));
                continue;
            }
            if (document.IsInTransaction)
            {
                diagnostics.Add(new EditorDocumentSaveDiagnostic(
                    document.Id,
                    document.DisplayName,
                    EditorDocumentSaveStatus.Failed,
                    "Document still has an open transaction.",
                    document.IsDirty));
                continue;
            }

            try
            {
                document.Synchronize("Edit");
            }
            catch (Exception ex)
            {
                diagnostics.Add(new EditorDocumentSaveDiagnostic(
                    document.Id,
                    document.DisplayName,
                    EditorDocumentSaveStatus.Failed,
                    "State capture failed: " + ExceptionDetail(ex),
                    RemainsDirty: true));
                continue;
            }

            if (document.IsDirty)
                savePlan.Add(document);
        }

        bool cancelled = false;
        int plannedCount = diagnostics.Count + savePlan.Count;
        foreach (EditorDocument document in savePlan)
        {
            if (cancellationToken.IsCancellationRequested)
            {
                cancelled = true;
                break;
            }

            EditorDocumentSaveResult result;
            try
            {
                result = await document.SaveAsync(cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                result = EditorDocumentSaveResult.Cancelled(
                    "The save operation was cancelled.");
            }
            catch (Exception ex)
            {
                result = EditorDocumentSaveResult.Failed(ExceptionDetail(ex));
            }

            bool remainsDirty = document.IsDirty;
            string detail = result.Detail ?? "";
            if (result.Status == EditorDocumentSaveStatus.Saved && remainsDirty)
            {
                detail = string.IsNullOrWhiteSpace(detail)
                    ? "The document changed while it was being saved and remains dirty."
                    : detail + " The document remains dirty.";
            }
            diagnostics.Add(new EditorDocumentSaveDiagnostic(
                document.Id,
                document.DisplayName,
                result.Status,
                detail,
                remainsDirty));

            if (result.Status == EditorDocumentSaveStatus.Cancelled)
            {
                cancelled = true;
                break;
            }
        }

        EditorDocumentId[] remainingDirty = DirtyIds();
        bool failed = diagnostics.Any(result =>
            result.Status is EditorDocumentSaveStatus.Failed or
                EditorDocumentSaveStatus.Unsupported) ||
            remainingDirty.Length > 0;
        EditorDocumentBatchCompletion completion = cancelled
            ? EditorDocumentBatchCompletion.Cancelled
            : failed
                ? EditorDocumentBatchCompletion.Failed
                : EditorDocumentBatchCompletion.Success;
        return new EditorDocumentSaveBatchResult(
            completion,
            diagnostics,
            remainingDirty,
            plannedCount);
    }

    private EditorDocumentSaveBatchResult CancelledBatch(
        IReadOnlyList<EditorDocumentSaveDiagnostic> diagnostics) =>
        new(
            EditorDocumentBatchCompletion.Cancelled,
            diagnostics,
            DirtyIds(),
            diagnostics.Count);

    private EditorDocumentId[] DirtyIds() =>
        SnapshotInDeterministicOrder()
            .Where(document => document.IsDirty)
            .Select(document => document.Id)
            .ToArray();

    private EditorDocument[] RefreshDirtyDocumentsDeterministically(
        out string detail)
    {
        var failures = new List<string>();
        foreach (EditorDocument document in SnapshotInDeterministicOrder())
        {
            if (document.IsSuspended)
            {
                failures.Add($"{document.Id}: document is suspended");
                continue;
            }
            if (document.IsInTransaction)
            {
                failures.Add($"{document.Id}: a transaction is still open");
                continue;
            }
            try
            {
                document.Synchronize("Edit");
            }
            catch (Exception ex)
            {
                failures.Add($"{document.Id}: {ExceptionDetail(ex)}");
            }
        }

        detail = string.Join("; ", failures);
        return SnapshotInDeterministicOrder()
            .Where(document => document.IsDirty)
            .ToArray();
    }

    private EditorDocument[] SnapshotInDeterministicOrder() =>
        _documents.Values
            .OrderBy(document => document.SaveOrder)
            .ThenBy(document => document.Id.Kind, StringComparer.Ordinal)
            .ThenBy(document => document.Id.StableId, StringComparer.Ordinal)
            .ToArray();

    private static string ExceptionDetail(Exception exception) =>
        exception.GetType().Name + ": " + exception.Message;

    private void OnDocumentStateChanged(object? sender, EventArgs e) =>
        DocumentStateChanged?.Invoke(sender, e);
}
