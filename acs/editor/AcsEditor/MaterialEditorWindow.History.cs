// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;

namespace AcsEditor;

public partial class MaterialEditorWindow
{
    private const int GraphHistoryLimit = 128;
    private static readonly TimeSpan GraphHistoryCoalesceWindow =
        TimeSpan.FromMilliseconds(1250);

    private sealed class MaterialGraphSnapshot
    {
        public List<GraphNode> Closures { get; set; } = new();
        public List<ExpressionNode> Expressions { get; set; } = new();
        public string[] TexturePaths { get; set; } = Array.Empty<string>();
        public int SubstrateRoot { get; set; } = -1;
        public int ExpressionRoot { get; set; } = -1;
        public string? SelectedClosureId { get; set; }
        public string? SelectedExpressionId { get; set; }
        public int SelectedClosureIndex { get; set; } = -1;
        public int SelectedExpressionIndex { get; set; } = -1;
        public double OutputX { get; set; }
        public double OutputY { get; set; }
        public double Zoom { get; set; } = 1;
        public double HorizontalOffset { get; set; }
        public double VerticalOffset { get; set; }
    }

    private sealed class GraphHistoryEntry
    {
        public required string Label { get; init; }
        public string? CoalesceKey { get; set; }
        public required MaterialGraphSnapshot Before { get; init; }
        public required MaterialGraphSnapshot After { get; set; }
        public DateTime TimestampUtc { get; set; }
    }

    private sealed class GraphHistoryLedger
    {
        private readonly int _limit;
        private readonly List<GraphHistoryEntry> _undo = new();
        private readonly List<GraphHistoryEntry> _redo = new();

        public GraphHistoryLedger(int limit) =>
            _limit = Math.Max(1, limit);

        public int UndoCount => _undo.Count;
        public int RedoCount => _redo.Count;
        public string? UndoLabel => _undo.Count == 0 ? null : _undo[^1].Label;
        public string? RedoLabel => _redo.Count == 0 ? null : _redo[^1].Label;

        public void Clear()
        {
            _undo.Clear();
            _redo.Clear();
        }

        public void BreakCoalescing()
        {
            if (_undo.Count != 0)
                _undo[^1].CoalesceKey = null;
        }

        public bool Record(
            string label,
            string? coalesceKey,
            MaterialGraphSnapshot before,
            MaterialGraphSnapshot after,
            DateTime timestampUtc)
        {
            if (FullHistoryStateEquals(before, after))
                return false;

            bool coalesce =
                coalesceKey != null &&
                _redo.Count == 0 &&
                _undo.Count > 0 &&
                string.Equals(
                    _undo[^1].CoalesceKey,
                    coalesceKey,
                    StringComparison.Ordinal) &&
                timestampUtc >= _undo[^1].TimestampUtc &&
                timestampUtc - _undo[^1].TimestampUtc <=
                    GraphHistoryCoalesceWindow;
            if (coalesce)
            {
                GraphHistoryEntry entry = _undo[^1];
                if (FullHistoryStateEquals(entry.Before, after))
                {
                    _undo.RemoveAt(_undo.Count - 1);
                }
                else
                {
                    entry.After = CloneHistorySnapshot(after);
                    entry.TimestampUtc = timestampUtc;
                }
                return true;
            }

            _redo.Clear();
            _undo.Add(new GraphHistoryEntry
            {
                Label = label,
                CoalesceKey = coalesceKey,
                Before = CloneHistorySnapshot(before),
                After = CloneHistorySnapshot(after),
                TimestampUtc = timestampUtc
            });
            if (_undo.Count > _limit)
                _undo.RemoveRange(0, _undo.Count - _limit);
            return true;
        }

        public bool TryUndo(
            out MaterialGraphSnapshot snapshot,
            out string label)
        {
            if (_undo.Count == 0)
            {
                snapshot = new MaterialGraphSnapshot();
                label = "";
                return false;
            }
            GraphHistoryEntry entry = _undo[^1];
            _undo.RemoveAt(_undo.Count - 1);
            _redo.Add(entry);
            snapshot = CloneHistorySnapshot(entry.Before);
            label = entry.Label;
            return true;
        }

        public bool TryRedo(
            out MaterialGraphSnapshot snapshot,
            out string label)
        {
            if (_redo.Count == 0)
            {
                snapshot = new MaterialGraphSnapshot();
                label = "";
                return false;
            }
            GraphHistoryEntry entry = _redo[^1];
            _redo.RemoveAt(_redo.Count - 1);
            _undo.Add(entry);
            snapshot = CloneHistorySnapshot(entry.After);
            label = entry.Label;
            return true;
        }
    }

    private sealed class GraphHistoryScope : IDisposable
    {
        private MaterialEditorWindow? _owner;

        public GraphHistoryScope(MaterialEditorWindow owner) =>
            _owner = owner;

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.EndGraphHistoryChange();
    }

    private readonly GraphHistoryLedger _graphHistory =
        new(GraphHistoryLimit);
    private MaterialGraphSnapshot? _savedGraphSnapshot;
    private MaterialGraphSnapshot? _historyTransactionBefore;
    private string _historyTransactionLabel = "";
    private string? _historyTransactionCoalesceKey;
    private int _historyTransactionDepth;
    private bool _graphHistoryInitialized;
    private bool _restoringGraphHistory;
    private GraphHistoryScope? _graphDragHistory;

    private void InitializeGraphHistory()
    {
        MaterialGraphSnapshot initial = CaptureGraphHistorySnapshot();
        _graphHistory.Clear();
        _savedGraphSnapshot = CloneHistorySnapshot(initial);
        _historyTransactionBefore = null;
        _historyTransactionDepth = 0;
        _graphHistoryInitialized = true;
        UpdateGraphDirtyFromHistory(initial);
        UpdateGraphHistoryButtons();
    }

    private GraphHistoryScope BeginGraphHistoryChange(
        string label,
        string? coalesceKey = null)
    {
        if (_graphHistoryInitialized && !_restoringGraphHistory &&
            _historyTransactionDepth++ == 0)
        {
            _historyTransactionBefore = CaptureGraphHistorySnapshot();
            _historyTransactionLabel = label;
            _historyTransactionCoalesceKey = coalesceKey;
            BeginHostedGraphTransaction(label, coalesceKey);
        }
        return new GraphHistoryScope(this);
    }

    private void EndGraphHistoryChange()
    {
        if (!_graphHistoryInitialized || _restoringGraphHistory ||
            _historyTransactionDepth <= 0)
            return;
        if (--_historyTransactionDepth != 0)
            return;

        try
        {
            MaterialGraphSnapshot? before = _historyTransactionBefore;
            _historyTransactionBefore = null;
            if (before == null)
                return;
            MaterialGraphSnapshot after = CaptureGraphHistorySnapshot();
            bool recorded = _graphHistory.Record(
                _historyTransactionLabel,
                _historyTransactionCoalesceKey,
                before,
                after,
                DateTime.UtcNow);
            _historyTransactionLabel = "";
            _historyTransactionCoalesceKey = null;
            UpdateGraphDirtyFromHistory(after);
            if (recorded)
                UpdateGraphHistoryButtons();
        }
        finally
        {
            CompleteHostedGraphTransaction();
        }
    }

    private void BeginGraphDragHistory(string label)
    {
        _graphDragHistory?.Dispose();
        _graphDragHistory = BeginGraphHistoryChange(label);
    }

    private void EndGraphDragHistory()
    {
        GraphHistoryScope? scope =
            Interlocked.Exchange(ref _graphDragHistory, null);
        scope?.Dispose();
    }

    private void MarkGraphHistorySaved()
    {
        if (!_graphHistoryInitialized)
            return;
        MaterialGraphSnapshot current = CaptureGraphHistorySnapshot();
        _graphHistory.BreakCoalescing();
        _savedGraphSnapshot = CloneHistorySnapshot(current);
        UpdateGraphDirtyFromHistory(current);
        UpdateGraphHistoryButtons();
    }

    private void UndoGraphEdit()
    {
        EndGraphDragHistory();
        if (!_graphHistory.TryUndo(out MaterialGraphSnapshot snapshot, out string label))
            return;
        RestoreGraphHistorySnapshot(snapshot);
        if (!_graphDirty)
            SaveGraphLayout();
        StatusText.Text = $"Undo: {label}";
        StatusText.Foreground = (Brush)FindResource("InfoFg");
        UpdateGraphHistoryButtons();
    }

    private void RedoGraphEdit()
    {
        EndGraphDragHistory();
        if (!_graphHistory.TryRedo(out MaterialGraphSnapshot snapshot, out string label))
            return;
        RestoreGraphHistorySnapshot(snapshot);
        if (!_graphDirty)
            SaveGraphLayout();
        StatusText.Text = $"Redo: {label}";
        StatusText.Foreground = (Brush)FindResource("InfoFg");
        UpdateGraphHistoryButtons();
    }

    private void RestoreGraphHistorySnapshot(MaterialGraphSnapshot snapshot)
    {
        _restoringGraphHistory = true;
        try
        {
            _wireSource = -1;
            _expressionWireSource = -1;
            _dragNode = -1;
            _dragExpressionNode = -1;
            _draggingNode = false;
            _draggingExpressionNode = false;
            _draggingOutput = false;

            _graphNodes.Clear();
            _graphNodes.AddRange(snapshot.Closures.Select(CloneGraphNodeForHistory));
            _expressionNodes.Clear();
            _expressionNodes.AddRange(
                snapshot.Expressions.Select(CloneExpressionNodeForHistory));
            Array.Fill(_expressionTexturePaths, "");
            Array.Copy(
                snapshot.TexturePaths,
                _expressionTexturePaths,
                Math.Min(snapshot.TexturePaths.Length, _expressionTexturePaths.Length));
            _substrateRoot = snapshot.SubstrateRoot;
            _expressionRoot = snapshot.ExpressionRoot;
            _outputX = snapshot.OutputX;
            _outputY = snapshot.OutputY;
            _graphZoom = snapshot.Zoom;
            UpdateGraphZoom();

            int selectedExpression = ResolveHistorySelection(
                _expressionNodes.Select(node => node.StableId),
                snapshot.SelectedExpressionId,
                snapshot.SelectedExpressionIndex);
            int selectedClosure = ResolveHistorySelection(
                _graphNodes.Select(node => node.StableId),
                snapshot.SelectedClosureId,
                snapshot.SelectedClosureIndex);
            if (ValidExpressionIndex(selectedExpression))
                SelectExpressionNode(selectedExpression);
            else
                SelectGraphNode(selectedClosure);

            GraphViewport.ScrollToHorizontalOffset(
                Math.Max(0, snapshot.HorizontalOffset));
            GraphViewport.ScrollToVerticalOffset(
                Math.Max(0, snapshot.VerticalOffset));
            Dispatcher.BeginInvoke(
                DispatcherPriority.Background,
                new Action(() =>
                {
                    GraphViewport.ScrollToHorizontalOffset(
                        Math.Max(0, snapshot.HorizontalOffset));
                    GraphViewport.ScrollToVerticalOffset(
                        Math.Max(0, snapshot.VerticalOffset));
                }));
            UpdateGraphDirtyFromHistory(snapshot);
            CompileGraph(userInitiated: false);
        }
        finally
        {
            _restoringGraphHistory = false;
        }
    }

    private static int ResolveHistorySelection(
        IEnumerable<string> stableIds,
        string? selectedStableId,
        int fallbackIndex)
    {
        if (!string.IsNullOrWhiteSpace(selectedStableId))
        {
            int index = 0;
            foreach (string stableId in stableIds)
            {
                if (string.Equals(
                        stableId,
                        selectedStableId,
                        StringComparison.Ordinal))
                    return index;
                index++;
            }
        }
        int count = stableIds.Count();
        return fallbackIndex >= 0 && fallbackIndex < count
            ? fallbackIndex
            : -1;
    }

    private MaterialGraphSnapshot CaptureGraphHistorySnapshot() =>
        new()
        {
            Closures = _graphNodes.Select(CloneGraphNodeForHistory).ToList(),
            Expressions = _expressionNodes
                .Select(CloneExpressionNodeForHistory)
                .ToList(),
            TexturePaths = (string[])_expressionTexturePaths.Clone(),
            SubstrateRoot = _substrateRoot,
            ExpressionRoot = _expressionRoot,
            SelectedClosureId = ValidNodeIndex(_selectedNode)
                ? _graphNodes[_selectedNode].StableId
                : null,
            SelectedExpressionId = ValidExpressionIndex(_selectedExpression)
                ? _expressionNodes[_selectedExpression].StableId
                : null,
            SelectedClosureIndex = _selectedNode,
            SelectedExpressionIndex = _selectedExpression,
            OutputX = _outputX,
            OutputY = _outputY,
            Zoom = _graphZoom,
            HorizontalOffset = GraphViewport.HorizontalOffset,
            VerticalOffset = GraphViewport.VerticalOffset
        };

    private static MaterialGraphSnapshot CloneHistorySnapshot(
        MaterialGraphSnapshot source) =>
        new()
        {
            Closures = source.Closures.Select(CloneGraphNodeForHistory).ToList(),
            Expressions = source.Expressions
                .Select(CloneExpressionNodeForHistory)
                .ToList(),
            TexturePaths = (string[])source.TexturePaths.Clone(),
            SubstrateRoot = source.SubstrateRoot,
            ExpressionRoot = source.ExpressionRoot,
            SelectedClosureId = source.SelectedClosureId,
            SelectedExpressionId = source.SelectedExpressionId,
            SelectedClosureIndex = source.SelectedClosureIndex,
            SelectedExpressionIndex = source.SelectedExpressionIndex,
            OutputX = source.OutputX,
            OutputY = source.OutputY,
            Zoom = source.Zoom,
            HorizontalOffset = source.HorizontalOffset,
            VerticalOffset = source.VerticalOffset
        };

    private static GraphNode CloneGraphNodeForHistory(GraphNode source) =>
        new()
        {
            StableId = source.StableId,
            Type = source.Type,
            InputA = source.InputA,
            InputB = source.InputB,
            Factor = source.Factor,
            Flags = source.Flags,
            Slab = (float[])source.Slab.Clone(),
            ExpressionRoots = (int[])source.ExpressionRoots.Clone(),
            IsExpanded = source.IsExpanded,
            X = source.X,
            Y = source.Y
        };

    private static ExpressionNode CloneExpressionNodeForHistory(
        ExpressionNode source) =>
        new()
        {
            StableId = source.StableId,
            Op = source.Op,
            DeclaredType = source.DeclaredType,
            TextureSlot = source.TextureSlot,
            TextureFlags = source.TextureFlags,
            ComponentIndex = source.ComponentIndex,
            Inputs = (int[])source.Inputs.Clone(),
            ParameterId = source.ParameterId,
            TextureAssetIdLow = source.TextureAssetIdLow,
            TextureAssetIdHigh = source.TextureAssetIdHigh,
            Value = (float[])source.Value.Clone(),
            ParameterName = source.ParameterName,
            X = source.X,
            Y = source.Y
        };

    private static bool AssetHistoryStateEquals(
        MaterialGraphSnapshot left,
        MaterialGraphSnapshot right)
    {
        if (left.SubstrateRoot != right.SubstrateRoot ||
            left.ExpressionRoot != right.ExpressionRoot ||
            left.Closures.Count != right.Closures.Count ||
            left.Expressions.Count != right.Expressions.Count ||
            !left.TexturePaths.SequenceEqual(
                right.TexturePaths,
                StringComparer.Ordinal))
            return false;

        for (int i = 0; i < left.Closures.Count; ++i)
        {
            GraphNode a = left.Closures[i];
            GraphNode b = right.Closures[i];
            if (!string.Equals(a.StableId, b.StableId, StringComparison.Ordinal) ||
                a.Type != b.Type ||
                a.InputA != b.InputA ||
                a.InputB != b.InputB ||
                a.Factor != b.Factor ||
                a.Flags != b.Flags ||
                !a.Slab.SequenceEqual(b.Slab) ||
                !a.ExpressionRoots.SequenceEqual(b.ExpressionRoots))
                return false;
        }
        for (int i = 0; i < left.Expressions.Count; ++i)
        {
            ExpressionNode a = left.Expressions[i];
            ExpressionNode b = right.Expressions[i];
            if (!string.Equals(a.StableId, b.StableId, StringComparison.Ordinal) ||
                a.Op != b.Op ||
                a.DeclaredType != b.DeclaredType ||
                a.TextureSlot != b.TextureSlot ||
                a.TextureFlags != b.TextureFlags ||
                a.ComponentIndex != b.ComponentIndex ||
                a.ParameterId != b.ParameterId ||
                a.TextureAssetIdLow != b.TextureAssetIdLow ||
                a.TextureAssetIdHigh != b.TextureAssetIdHigh ||
                !string.Equals(
                    a.ParameterName,
                    b.ParameterName,
                    StringComparison.Ordinal) ||
                !a.Inputs.SequenceEqual(b.Inputs) ||
                !a.Value.SequenceEqual(b.Value))
                return false;
        }
        return true;
    }

    private static bool FullHistoryStateEquals(
        MaterialGraphSnapshot left,
        MaterialGraphSnapshot right)
    {
        if (!AssetHistoryStateEquals(left, right) ||
            !string.Equals(
                left.SelectedClosureId,
                right.SelectedClosureId,
                StringComparison.Ordinal) ||
            !string.Equals(
                left.SelectedExpressionId,
                right.SelectedExpressionId,
                StringComparison.Ordinal) ||
            left.SelectedClosureIndex != right.SelectedClosureIndex ||
            left.SelectedExpressionIndex != right.SelectedExpressionIndex ||
            left.OutputX != right.OutputX ||
            left.OutputY != right.OutputY ||
            left.Zoom != right.Zoom ||
            left.HorizontalOffset != right.HorizontalOffset ||
            left.VerticalOffset != right.VerticalOffset)
            return false;
        for (int i = 0; i < left.Closures.Count; ++i)
        {
            GraphNode a = left.Closures[i];
            GraphNode b = right.Closures[i];
            if (a.IsExpanded != b.IsExpanded ||
                a.X != b.X ||
                a.Y != b.Y)
                return false;
        }
        for (int i = 0; i < left.Expressions.Count; ++i)
        {
            ExpressionNode a = left.Expressions[i];
            ExpressionNode b = right.Expressions[i];
            if (a.X != b.X || a.Y != b.Y)
                return false;
        }
        return true;
    }

    private void UpdateGraphDirtyFromHistory(MaterialGraphSnapshot current)
    {
        _graphDirty =
            _savedGraphSnapshot != null &&
            !AssetHistoryStateEquals(current, _savedGraphSnapshot);
        GraphAssetStateText.Text = _graphDirty
            ? "ACSMAT Substrate DAG - unsaved"
            : _substrateEnabled != 0
                ? "ACSMAT Substrate DAG - saved"
                : "Legacy surface preview - Save converts to Substrate";
        PreviewStateText.Text = "Preview: last applied asset";
        UpdateGraphStatus();
    }

    private void UpdateGraphHistoryButtons()
    {
        if (UndoGraphButton == null || RedoGraphButton == null)
            return;
        UndoGraphButton.IsEnabled = _graphHistory.UndoCount > 0;
        RedoGraphButton.IsEnabled = _graphHistory.RedoCount > 0;
        UndoGraphButton.ToolTip = _graphHistory.UndoLabel is string undo
            ? $"Undo {undo} (Ctrl+Z)"
            : "Nothing to undo (Ctrl+Z)";
        RedoGraphButton.ToolTip = _graphHistory.RedoLabel is string redo
            ? $"Redo {redo} (Ctrl+Y / Ctrl+Shift+Z)"
            : "Nothing to redo (Ctrl+Y / Ctrl+Shift+Z)";
    }

    private void OnUndoGraphClicked(object sender, RoutedEventArgs e) =>
        UndoGraphEdit();

    private void OnRedoGraphClicked(object sender, RoutedEventArgs e) =>
        RedoGraphEdit();

    internal static (int Passed, int Failed) RunHistoryContractSelfTest(
        TextWriter output)
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

        MaterialGraphSnapshot Snapshot(
            string closureId = "closure-root",
            float factor = 0.5f,
            double x = 10,
            string? selectedClosure = "closure-root")
        {
            var closure = new GraphNode
            {
                StableId = closureId,
                Type = 0,
                Factor = factor,
                Slab = Enumerable.Range(0, SlabScalarCount)
                    .Select(index => (float)index)
                    .ToArray(),
                ExpressionRoots = Enumerable.Repeat(-1, SlabScalarCount).ToArray(),
                X = x,
                Y = 20
            };
            var expression = new ExpressionNode
            {
                StableId = "expr-root",
                Op = 0,
                DeclaredType = 1,
                Inputs = new[] { -1, -1, -1 },
                Value = new[] { 0.5f, 0f, 0f, 0f },
                X = 30,
                Y = 40
            };
            return new MaterialGraphSnapshot
            {
                Closures = new List<GraphNode> { closure },
                Expressions = new List<ExpressionNode> { expression },
                TexturePaths = new[] { "", "", "", "" },
                SubstrateRoot = 0,
                ExpressionRoot = 0,
                SelectedClosureId = selectedClosure,
                SelectedClosureIndex = selectedClosure == null ? -1 : 0,
                OutputX = 900,
                OutputY = 430,
                Zoom = 0.9
            };
        }

        MaterialGraphSnapshot baseline = Snapshot();
        MaterialGraphSnapshot copy = CloneHistorySnapshot(baseline);
        copy.Closures[0].Slab[0] = -10;
        copy.Closures[0].ExpressionRoots[0] = 0;
        copy.Expressions[0].Inputs[0] = 0;
        copy.Expressions[0].Value[0] = 1;
        Check(
            baseline.Closures[0].Slab[0] == 0 &&
            baseline.Closures[0].ExpressionRoots[0] == -1 &&
            baseline.Expressions[0].Inputs[0] == -1 &&
            baseline.Expressions[0].Value[0] == 0.5f,
            "history snapshots deep-copy closure, binding, connection, and value arrays");
        Check(
            copy.Closures[0].StableId == baseline.Closures[0].StableId &&
            copy.Expressions[0].StableId == baseline.Expressions[0].StableId,
            "history snapshots preserve StableId identity");

        var ledger = new GraphHistoryLedger(16);
        DateTime now = DateTime.UnixEpoch;
        MaterialGraphSnapshot current = CloneHistorySnapshot(baseline);
        void Record(
            string label,
            Action<MaterialGraphSnapshot> mutate,
            string? key = null)
        {
            MaterialGraphSnapshot before = CloneHistorySnapshot(current);
            mutate(current);
            ledger.Record(label, key, before, current, now);
            now += TimeSpan.FromMilliseconds(10);
        }

        Record("Add Closure", state =>
        {
            GraphNode added = CloneGraphNodeForHistory(state.Closures[0]);
            added.StableId = "closure-added";
            added.Type = 1;
            added.X = 200;
            state.Closures.Add(added);
            state.SelectedClosureId = "closure-added";
            state.SelectedClosureIndex = 1;
        });
        Record("Connect Closures", state => state.Closures[1].InputA = 0);
        Record(
            "Edit Factor",
            state => state.Closures[1].Factor = 0.25f,
            "closure-added:factor");
        Record("Move Closure", state => state.Closures[1].X = 333);
        Record("Set Front Material", state => state.SubstrateRoot = 1);
        Record(
            "Bind Shader Expression",
            state => state.Closures[0].ExpressionRoots[0] = 0);
        Record("Duplicate Shader Expression", state =>
        {
            ExpressionNode added =
                CloneExpressionNodeForHistory(state.Expressions[0]);
            added.StableId = "expr-duplicate";
            added.Inputs[0] = 0;
            added.X = 420;
            state.Expressions.Add(added);
            state.SelectedClosureId = null;
            state.SelectedClosureIndex = -1;
            state.SelectedExpressionId = "expr-duplicate";
            state.SelectedExpressionIndex = 1;
        });
        Record(
            "Disconnect Shader Input",
            state => state.Expressions[1].Inputs[0] = -1);
        Record("Delete Closure", state =>
        {
            state.Closures.RemoveAt(1);
            state.SubstrateRoot = 0;
            state.SelectedExpressionId = null;
            state.SelectedExpressionIndex = -1;
            state.SelectedClosureId = "closure-root";
            state.SelectedClosureIndex = 0;
        });

        int recordedCount = ledger.UndoCount;
        while (ledger.TryUndo(out MaterialGraphSnapshot previous, out _))
            current = previous;
        Check(
            recordedCount == 9 &&
            FullHistoryStateEquals(current, baseline),
            "add/delete/connect/disconnect/property/drag/root/binding/duplicate undo as semantic steps");
        while (ledger.TryRedo(out MaterialGraphSnapshot next, out _))
            current = next;
        Check(
            current.Closures.Count == 1 &&
            current.Expressions.Count == 2 &&
            current.Expressions[1].StableId == "expr-duplicate" &&
            current.SelectedClosureId == "closure-root" &&
            current.OutputX == baseline.OutputX,
            "redo restores topology, StableIds, selection, and layout");

        ledger.TryUndo(out MaterialGraphSnapshot branchPoint, out _);
        current = branchPoint;
        MaterialGraphSnapshot branchAfter = CloneHistorySnapshot(current);
        branchAfter.Closures[0].Factor = 0.75f;
        ledger.Record(
            "Branch Property Edit",
            null,
            current,
            branchAfter,
            now);
        Check(
            ledger.RedoCount == 0,
            "a new semantic edit invalidates the redo branch");

        var bounded = new GraphHistoryLedger(3);
        MaterialGraphSnapshot boundedCurrent = Snapshot();
        for (int i = 0; i < 7; ++i)
        {
            MaterialGraphSnapshot before = CloneHistorySnapshot(boundedCurrent);
            boundedCurrent.Closures[0].Factor = i;
            bounded.Record(
                "Bounded Edit",
                null,
                before,
                boundedCurrent,
                now + TimeSpan.FromSeconds(i));
        }
        Check(
            bounded.UndoCount == 3,
            "history is bounded and discards only the oldest entries");

        var coalesced = new GraphHistoryLedger(8);
        MaterialGraphSnapshot coalesceStart = Snapshot();
        MaterialGraphSnapshot first = CloneHistorySnapshot(coalesceStart);
        first.Closures[0].Factor = 0.6f;
        coalesced.Record(
            "Edit Factor",
            "closure-root:factor",
            coalesceStart,
            first,
            now);
        MaterialGraphSnapshot second = CloneHistorySnapshot(first);
        second.Closures[0].Factor = 0.7f;
        coalesced.Record(
            "Edit Factor",
            "closure-root:factor",
            first,
            second,
            now + TimeSpan.FromMilliseconds(50));
        coalesced.TryUndo(out MaterialGraphSnapshot coalescedUndo, out _);
        Check(
            coalesced.UndoCount == 0 &&
            coalescedUndo.Closures[0].Factor ==
                coalesceStart.Closures[0].Factor,
            "continuous property edits coalesce without losing the pre-edit state");

        var revertedCoalesce = new GraphHistoryLedger(8);
        MaterialGraphSnapshot revertedStart = Snapshot();
        MaterialGraphSnapshot revertedFirst =
            CloneHistorySnapshot(revertedStart);
        revertedFirst.Closures[0].Factor = 0.8f;
        revertedCoalesce.Record(
            "Edit Factor",
            "closure-root:factor",
            revertedStart,
            revertedFirst,
            now);
        MaterialGraphSnapshot revertedSecond =
            CloneHistorySnapshot(revertedFirst);
        revertedSecond.Closures[0].Factor =
            revertedStart.Closures[0].Factor;
        revertedCoalesce.Record(
            "Edit Factor",
            "closure-root:factor",
            revertedFirst,
            revertedSecond,
            now + TimeSpan.FromMilliseconds(50));
        Check(
            revertedCoalesce.UndoCount == 0,
            "coalescing back to the initial value removes the no-op history entry");

        var savedBoundary = new GraphHistoryLedger(8);
        MaterialGraphSnapshot beforeSave = Snapshot();
        MaterialGraphSnapshot savedValue = CloneHistorySnapshot(beforeSave);
        savedValue.Closures[0].Factor = 0.6f;
        savedBoundary.Record(
            "Edit Factor",
            "closure-root:factor",
            beforeSave,
            savedValue,
            now);
        savedBoundary.BreakCoalescing();
        MaterialGraphSnapshot afterSave = CloneHistorySnapshot(savedValue);
        afterSave.Closures[0].Factor = 0.7f;
        savedBoundary.Record(
            "Edit Factor",
            "closure-root:factor",
            savedValue,
            afterSave,
            now + TimeSpan.FromMilliseconds(50));
        savedBoundary.TryUndo(
            out MaterialGraphSnapshot undoToSavedValue,
            out _);
        Check(
            undoToSavedValue.Closures[0].Factor ==
                savedValue.Closures[0].Factor,
            "saving breaks property coalescing so Undo returns to the save checkpoint");

        MaterialGraphSnapshot layoutOnly = CloneHistorySnapshot(baseline);
        layoutOnly.Closures[0].X += 25;
        layoutOnly.SelectedClosureId = null;
        layoutOnly.SelectedClosureIndex = -1;
        Check(
            AssetHistoryStateEquals(layoutOnly, baseline) &&
            !FullHistoryStateEquals(layoutOnly, baseline),
            "selection and node layout are undoable without making the material asset dirty");
        MaterialGraphSnapshot authored = CloneHistorySnapshot(baseline);
        authored.Closures[0].Factor = 0.9f;
        Check(
            !AssetHistoryStateEquals(authored, baseline),
            "authored properties differ from the saved dirty-state checkpoint");

        string baselineFingerprint = JsonSerializer.Serialize(
            CreateSemanticGraphSnapshot(baseline));
        string layoutFingerprint = JsonSerializer.Serialize(
            CreateSemanticGraphSnapshot(layoutOnly));
        string authoredFingerprint = JsonSerializer.Serialize(
            CreateSemanticGraphSnapshot(authored));
        Check(
            baselineFingerprint == layoutFingerprint &&
            baselineFingerprint != authoredFingerprint,
            "hosted material fingerprint excludes layout but includes authored graph state");

        string hostedPayload = JsonSerializer.Serialize(baseline);
        MaterialGraphSnapshot? hostedRoundTrip =
            JsonSerializer.Deserialize<MaterialGraphSnapshot>(hostedPayload);
        Check(
            hostedRoundTrip != null &&
            FullHistoryStateEquals(hostedRoundTrip, baseline),
            "hosted material payload round-trips full graph and presentation state");

        return (passed, failed);
    }
}
