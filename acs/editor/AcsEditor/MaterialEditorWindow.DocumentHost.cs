// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

public partial class MaterialEditorWindow
{
    private EditorDocument? _hostedDocument;
    private IDisposable? _hostedGraphTransaction;
    private bool _hostedOwnerCloseApproved;
    private bool _hostedOwnerCloseDiscardsChanges;

    internal string? CurrentAssetId { get; private set; }
    internal bool HasUnsavedGraphChanges => _graphDirty;
    internal EditorDocument? HostedDocument => _hostedDocument;

    internal void SetAssetIdentity(string? assetId)
    {
        string? normalized =
            MaterialDocumentHostRegistration.NormalizeAssetIdOrNull(assetId);
        if (!string.IsNullOrWhiteSpace(assetId) && normalized == null)
        {
            throw new ArgumentException(
                "Material Asset ID must be a non-zero 32-digit GUID.",
                nameof(assetId));
        }
        CurrentAssetId = normalized;
    }

    internal void AttachHostedDocument(EditorDocument document)
    {
        ArgumentNullException.ThrowIfNull(document);
        if (!string.Equals(document.Id.Kind, "material", StringComparison.Ordinal))
            throw new ArgumentException(
                "Only a material document may be attached to Material Editor.",
                nameof(document));
        if (_hostedDocument != null && !ReferenceEquals(_hostedDocument, document))
            throw new InvalidOperationException(
                "Material Editor is already attached to another hosted document.");
        _hostedDocument = document;
    }

    internal void DetachHostedDocument(EditorDocument document)
    {
        if (ReferenceEquals(_hostedDocument, document))
        {
            CompleteHostedGraphTransaction();
            _hostedDocument = null;
        }
    }

    internal EditorDocumentState CaptureHostedMaterialState()
    {
        if (_lifetimeEnded)
            throw new InvalidOperationException(
                "The material editor lifetime has ended.");

        MaterialGraphSnapshot snapshot = CaptureGraphHistorySnapshot();
        string payload = JsonSerializer.Serialize(snapshot);
        string fingerprint = JsonSerializer.Serialize(
            CreateSemanticGraphSnapshot(snapshot));
        return new EditorDocumentState(payload, fingerprint);
    }

    internal void RestoreHostedMaterialState(EditorDocumentState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        if (_lifetimeEnded || !CanAccessAssetPath)
            throw new InvalidOperationException(
                "The material asset is unavailable.");
        if (_historyTransactionDepth != 0 || _graphDragHistory != null)
            throw new InvalidOperationException(
                "Complete the active material graph transaction before restoring state.");

        MaterialGraphSnapshot snapshot;
        try
        {
            snapshot = JsonSerializer.Deserialize<MaterialGraphSnapshot>(
                state.Payload) ?? throw new InvalidDataException(
                "Material graph state is empty.");
            ValidateHostedSnapshot(snapshot);
        }
        catch (JsonException ex)
        {
            throw new InvalidDataException(
                "Material graph state is not valid JSON.",
                ex);
        }

        string fingerprint = JsonSerializer.Serialize(
            CreateSemanticGraphSnapshot(snapshot));
        if (!string.Equals(
                fingerprint,
                state.ContentFingerprint,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Material graph state does not match its declared fingerprint.");
        }

        RestoreGraphHistorySnapshot(snapshot);
    }

    internal ValueTask<EditorDocumentSaveResult> SaveHostedMaterialAsync(
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_lifetimeEnded || !CanAccessAssetPath)
        {
            return ValueTask.FromResult(
                EditorDocumentSaveResult.Failed(
                    "The material asset is unavailable."));
        }
        if (_historyTransactionDepth != 0 || _graphDragHistory != null)
        {
            return ValueTask.FromResult(
                EditorDocumentSaveResult.Failed(
                    "The material graph still has an open transaction."));
        }

        EditorDocumentState before;
        try
        {
            before = CaptureHostedMaterialState();
            if (!WriteRuntimeGraph(showDiagnostics: true))
            {
                return ValueTask.FromResult(
                    EditorDocumentSaveResult.Failed(
                        "The runtime rejected the material graph."));
            }
            EditorDocumentState committed = CaptureHostedMaterialState();
            return ValueTask.FromResult(
                MaterialDocumentHostRegistration.CompleteSuccessfulWrite(
                    before,
                    committed,
                    CompleteLocalGraphSaveCheckpoint));
        }
        catch (Exception ex) when (
            ex is not OperationCanceledException)
        {
            return ValueTask.FromResult(
                EditorDocumentSaveResult.Failed(
                    ex.GetType().Name + ": " + ex.Message));
        }
    }

    private bool SaveRuntimeGraphThroughDocumentHost(bool showDiagnostics)
    {
        EditorDocument? document = _hostedDocument;
        if (document == null)
        {
            if (!WriteRuntimeGraph(showDiagnostics))
                return false;
            CompleteLocalGraphSaveCheckpoint();
            return true;
        }

        try
        {
            // The hosted material writer is deliberately synchronous today. Running the
            // EditorDocument contract here still makes it the only authority that publishes
            // the hosted saved fingerprint after the callback reports success.
            EditorDocumentSaveResult result = document
                .SaveAsync(CancellationToken.None)
                .AsTask()
                .GetAwaiter()
                .GetResult();
            if (result.Status == EditorDocumentSaveStatus.Saved)
                return true;
            if (showDiagnostics && !string.IsNullOrWhiteSpace(result.Detail))
            {
                SetDiagnostics(new[]
                {
                    "ERROR  " + result.Detail
                });
            }
            return false;
        }
        catch (Exception ex) when (
            ex is not OperationCanceledException)
        {
            if (showDiagnostics)
            {
                SetDiagnostics(new[]
                {
                    "ERROR  Hosted material save failed: " +
                    ex.GetType().Name + ": " + ex.Message
                });
            }
            return false;
        }
    }

    internal void ApproveHostedOwnerClose(bool discardUnsavedChanges)
    {
        _hostedOwnerCloseApproved = true;
        _hostedOwnerCloseDiscardsChanges = discardUnsavedChanges;
    }

    private void BeginHostedGraphTransaction(
        string label,
        string? coalesceKey)
    {
        if (_hostedGraphTransaction != null || _hostedDocument == null)
            return;
        try
        {
            _hostedGraphTransaction = _hostedDocument.BeginTransaction(
                label,
                coalesceKey,
                GraphHistoryCoalesceWindow);
        }
        catch (Exception ex)
        {
            DiagnosticsList.Items.Add(
                "ERROR  Hosted material transaction could not start: " +
                ex.Message);
            NotifyHostedGraphPotentialChange();
        }
    }

    private void CompleteHostedGraphTransaction()
    {
        IDisposable? transaction =
            Interlocked.Exchange(ref _hostedGraphTransaction, null);
        if (transaction == null)
            return;
        try
        {
            transaction.Dispose();
        }
        catch (Exception ex)
        {
            try { _hostedDocument?.NotifyPotentialChange(); }
            catch { }
            try
            {
                DiagnosticsList.Items.Add(
                    "ERROR  Hosted material transaction failed: " +
                    ex.Message);
                StatusText.Text = "Document transaction failed";
            }
            catch
            {
                // The host remains fail-closed; diagnostics are best effort
                // while the window itself is tearing down.
            }
        }
    }

    private void NotifyHostedGraphPotentialChange()
    {
        try
        {
            _hostedDocument?.NotifyPotentialChange();
        }
        catch (Exception ex)
        {
            DiagnosticsList.Items.Add(
                "ERROR  Hosted material dirty tracking failed: " +
                ex.Message);
        }
    }

    private static MaterialGraphSnapshot CreateSemanticGraphSnapshot(
        MaterialGraphSnapshot source)
    {
        MaterialGraphSnapshot semantic = CloneHistorySnapshot(source);
        semantic.SelectedClosureId = null;
        semantic.SelectedExpressionId = null;
        semantic.SelectedClosureIndex = -1;
        semantic.SelectedExpressionIndex = -1;
        semantic.OutputX = 0;
        semantic.OutputY = 0;
        semantic.Zoom = 1;
        semantic.HorizontalOffset = 0;
        semantic.VerticalOffset = 0;
        foreach (GraphNode node in semantic.Closures)
        {
            node.IsExpanded = false;
            node.X = 0;
            node.Y = 0;
        }
        foreach (ExpressionNode node in semantic.Expressions)
        {
            node.X = 0;
            node.Y = 0;
        }
        return semantic;
    }

    private void ValidateHostedSnapshot(MaterialGraphSnapshot snapshot)
    {
        if (snapshot.Closures == null ||
            snapshot.Expressions == null ||
            snapshot.TexturePaths == null)
        {
            throw new InvalidDataException(
                "Material graph state has missing collections.");
        }
        if (snapshot.Closures.Count == 0 ||
            snapshot.Closures.Count > _runtimeMaxNodes ||
            snapshot.Expressions.Count > _expressionMaxNodes)
        {
            throw new InvalidDataException(
                "Material graph state exceeds runtime capacity.");
        }
        if (snapshot.TexturePaths.Length != _expressionTexturePaths.Length)
            throw new InvalidDataException(
                "Material graph texture-slot count is invalid.");
        if (snapshot.TexturePaths.Any(path => path == null) ||
            !double.IsFinite(snapshot.OutputX) ||
            !double.IsFinite(snapshot.OutputY) ||
            !double.IsFinite(snapshot.Zoom) ||
            snapshot.Zoom <= 0 ||
            !double.IsFinite(snapshot.HorizontalOffset) ||
            !double.IsFinite(snapshot.VerticalOffset))
        {
            throw new InvalidDataException(
                "Material graph presentation state is invalid.");
        }
        if (snapshot.SubstrateRoot < 0 ||
            snapshot.SubstrateRoot >= snapshot.Closures.Count)
        {
            throw new InvalidDataException(
                "Material graph root is invalid.");
        }
        if (snapshot.Closures
                .Select(node => node.StableId)
                .Distinct(StringComparer.Ordinal)
                .Count() != snapshot.Closures.Count ||
            snapshot.Expressions
                .Select(node => node.StableId)
                .Distinct(StringComparer.Ordinal)
                .Count() != snapshot.Expressions.Count)
        {
            throw new InvalidDataException(
                "Material graph stable IDs must be unique within each node domain.");
        }

        foreach (GraphNode node in snapshot.Closures)
        {
            if (string.IsNullOrWhiteSpace(node.StableId) ||
                node.Slab == null ||
                node.Slab.Length != SlabScalarCount ||
                node.ExpressionRoots == null ||
                node.ExpressionRoots.Length != SlabScalarCount ||
                node.Type < 0 ||
                node.Type > 5 ||
                !float.IsFinite(node.Factor) ||
                node.InputA < -1 ||
                node.InputA >= snapshot.Closures.Count ||
                node.InputB < -1 ||
                node.InputB >= snapshot.Closures.Count ||
                node.ExpressionRoots.Any(index =>
                    index < -1 ||
                    index >= snapshot.Expressions.Count) ||
                node.Slab.Any(value => !float.IsFinite(value)) ||
                !double.IsFinite(node.X) ||
                !double.IsFinite(node.Y))
            {
                throw new InvalidDataException(
                    "Material closure state is invalid.");
            }
        }
        if (snapshot.ExpressionRoot < -1 ||
            snapshot.ExpressionRoot >= snapshot.Expressions.Count)
        {
            throw new InvalidDataException(
                "Material shader-expression root is invalid.");
        }
        foreach (ExpressionNode node in snapshot.Expressions)
        {
            if (string.IsNullOrWhiteSpace(node.StableId) ||
                node.Inputs == null ||
                node.Inputs.Length != 3 ||
                node.Value == null ||
                node.Value.Length != 4 ||
                node.Op < 0 ||
                node.Op >= ExpressionOpNames.Length ||
                node.DeclaredType < 0 ||
                node.DeclaredType > 4 ||
                node.TextureSlot < 0 ||
                node.TextureSlot >= _expressionTexturePaths.Length ||
                node.ComponentIndex < 0 ||
                node.ComponentIndex > 3 ||
                node.Inputs.Any(index =>
                    index < -1 ||
                    index >= snapshot.Expressions.Count) ||
                node.Value.Any(value => !float.IsFinite(value)) ||
                !double.IsFinite(node.X) ||
                !double.IsFinite(node.Y))
            {
                throw new InvalidDataException(
                    "Material expression state is invalid.");
            }
        }
    }
}
