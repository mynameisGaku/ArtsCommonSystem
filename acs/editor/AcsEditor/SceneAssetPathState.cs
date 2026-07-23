// SPDX-License-Identifier: Apache-2.0

using System;

namespace AcsEditor;

/// <summary>
/// The three runtime aliases that identify the one open scene document while the native
/// 2D/3D compatibility serializers still have separate source slots.
/// </summary>
internal sealed record SceneAssetPathState(
    string? CurrentPath,
    string? TwoDPath,
    string? ThreeDPath)
{
    internal bool IsAffectedBy(AssetPathMutationStartingEventArgs operation)
    {
        ArgumentNullException.ThrowIfNull(operation);
        return IsAffected(CurrentPath, operation) ||
               IsAffected(TwoDPath, operation) ||
               IsAffected(ThreeDPath, operation);
    }

    internal bool ShouldVeto(AssetPathMutationStartingEventArgs operation) =>
        (operation.Kind is AssetPathMutationKind.Delete or
            AssetPathMutationKind.ContentRewrite) &&
        IsAffectedBy(operation);

    /// <summary>
    /// Applies committed mappings to every alias as one state transition. A deletion notification
    /// is defensive only (the MainWindow preflight vetoes it) and detaches the affected alias so a
    /// later save cannot recreate a path that no longer belongs to the open document.
    /// </summary>
    internal SceneAssetPathState Apply(
        AssetPathsChangedEventArgs change,
        out bool remapped,
        out bool detached)
    {
        ArgumentNullException.ThrowIfNull(change);
        remapped = false;
        detached = false;
        return new SceneAssetPathState(
            ApplyOne(CurrentPath, change, ref remapped, ref detached),
            ApplyOne(TwoDPath, change, ref remapped, ref detached),
            ApplyOne(ThreeDPath, change, ref remapped, ref detached));
    }

    /// <summary>
    /// Fail-closed fallback for a mutation which reports success without publishing its committed
    /// destination mapping. Detaching only aliases below the original roots prevents a later save
    /// from recreating the source path while preserving unrelated compatibility aliases.
    /// </summary>
    internal SceneAssetPathState DetachAffected(
        AssetPathMutationStartingEventArgs operation,
        out bool detached)
    {
        ArgumentNullException.ThrowIfNull(operation);
        detached = false;
        return new SceneAssetPathState(
            DetachOne(CurrentPath, operation, ref detached),
            DetachOne(TwoDPath, operation, ref detached),
            DetachOne(ThreeDPath, operation, ref detached));
    }

    private static bool IsAffected(
        string? path,
        AssetPathMutationStartingEventArgs operation) =>
        !string.IsNullOrWhiteSpace(path) && operation.AffectsPath(path);

    private static string? ApplyOne(
        string? path,
        AssetPathsChangedEventArgs change,
        ref bool remapped,
        ref bool detached)
    {
        if (string.IsNullOrWhiteSpace(path)) return path;
        if (change.TryRemapPath(path, out string destination))
        {
            remapped = true;
            return destination;
        }
        if (!change.IsDeletedPath(path)) return path;
        detached = true;
        return null;
    }

    private static string? DetachOne(
        string? path,
        AssetPathMutationStartingEventArgs operation,
        ref bool detached)
    {
        if (string.IsNullOrWhiteSpace(path) || !operation.AffectsPath(path))
            return path;
        detached = true;
        return null;
    }
}

/// <summary>
/// Pure lifecycle state for one open-scene Rename/Move. MainWindow owns the actual document and
/// autosave suspension; this object makes the path transition explicit and testable.
/// </summary>
internal sealed class SceneAssetPathMutationGuard
{
    internal SceneAssetPathMutationGuard(
        SceneAssetPathState startingPaths,
        AssetPathMutationStartingEventArgs operation)
    {
        StartingPaths = startingPaths ??
            throw new ArgumentNullException(nameof(startingPaths));
        Operation = operation ??
            throw new ArgumentNullException(nameof(operation));
        CurrentPaths = startingPaths;
    }

    internal SceneAssetPathState StartingPaths { get; }
    internal SceneAssetPathState CurrentPaths { get; private set; }
    internal AssetPathMutationStartingEventArgs Operation { get; }
    internal bool IsActive { get; private set; } = true;
    internal bool PathChangePublished { get; private set; }

    internal SceneAssetPathState Publish(
        AssetPathsChangedEventArgs change,
        out bool remapped,
        out bool detached)
    {
        if (!IsActive)
            throw new InvalidOperationException(
                "A completed scene path mutation cannot accept another publication.");

        CurrentPaths = CurrentPaths.Apply(change, out remapped, out detached);
        // A partial/malformed publication is not enough: every alias which began below the
        // mutation roots must stop pointing at those roots before saving can resume.
        if ((remapped || detached) && !CurrentPaths.IsAffectedBy(Operation))
            PathChangePublished = true;
        return CurrentPaths;
    }

    internal SceneAssetPathState Complete(
        bool succeeded,
        out bool detachedForSafety)
    {
        if (!IsActive)
            throw new InvalidOperationException(
                "The scene path mutation was already completed.");

        detachedForSafety = false;
        if (succeeded && !PathChangePublished)
        {
            CurrentPaths = CurrentPaths.DetachAffected(
                Operation,
                out detachedForSafety);
        }
        IsActive = false;
        return CurrentPaths;
    }
}

/// <summary>
/// Reference-counted input block used while an open scene path is between mutation preflight and
/// completion reconciliation. Leases make nested callers and exceptional exits explicit; the
/// transition callback only runs for the outermost enter and final exit.
/// </summary>
internal sealed class SceneEditingBlockState
{
    private readonly Action<bool> _onBlockedChanged;
    private int _depth;

    internal SceneEditingBlockState(Action<bool> onBlockedChanged) =>
        _onBlockedChanged = onBlockedChanged ??
            throw new ArgumentNullException(nameof(onBlockedChanged));

    internal bool IsBlocked => _depth != 0;
    internal int Depth => _depth;

    internal IDisposable Enter()
    {
        checked { _depth++; }
        if (_depth == 1)
        {
            try
            {
                _onBlockedChanged(true);
            }
            catch
            {
                _depth = 0;
                throw;
            }
        }
        return new Lease(this);
    }

    private void Exit()
    {
        if (_depth <= 0)
            throw new InvalidOperationException("Scene editing input block is unbalanced.");
        _depth--;
        if (_depth == 0)
            _onBlockedChanged(false);
    }

    private sealed class Lease : IDisposable
    {
        private SceneEditingBlockState? _owner;

        internal Lease(SceneEditingBlockState owner) => _owner = owner;

        public void Dispose() =>
            System.Threading.Interlocked.Exchange(ref _owner, null)?.Exit();
    }
}
