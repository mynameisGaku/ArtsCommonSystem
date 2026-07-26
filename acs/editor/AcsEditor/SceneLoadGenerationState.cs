// SPDX-License-Identifier: Apache-2.0

using System;

namespace AcsEditor;

internal readonly record struct SceneLoadTicket(int Generation);

/// <summary>
/// Runs the fallible presentation part of scene-load completion while guaranteeing that the
/// load lifetime and editor-input recovery are both settled. Presentation crosses native and
/// WPF boundaries, so an exception there must never leave the otherwise live editor input-locked.
/// </summary>
internal static class SceneLoadCompletionGuard
{
    internal static bool ShouldEnableViewportInput(
        bool sceneInputEnabled,
        bool viewportPublished) =>
        sceneInputEnabled && viewportPublished;

    internal static void Run(
        Action completePresentation,
        IDisposable loadLifetime,
        Action restoreInput)
    {
        ArgumentNullException.ThrowIfNull(completePresentation);
        ArgumentNullException.ThrowIfNull(loadLifetime);
        ArgumentNullException.ThrowIfNull(restoreInput);

        try
        {
            completePresentation();
        }
        finally
        {
            try
            {
                loadLifetime.Dispose();
            }
            finally
            {
                restoreInput();
            }
        }
    }
}

/// <summary>
/// Dispatcher-owned generation gate for asynchronous scene replacement.  A completion may
/// publish native state and restore input only while its ticket is the current active load.
/// </summary>
internal sealed class SceneLoadGenerationState
{
    private int _generation;

    internal bool IsLoading { get; private set; }
    internal int Generation => _generation;

    internal SceneLoadTicket Begin()
    {
        _generation = checked(_generation + 1);
        IsLoading = true;
        return new SceneLoadTicket(_generation);
    }

    internal bool IsCurrent(SceneLoadTicket ticket) =>
        IsLoading && ticket.Generation == _generation;

    internal bool TryComplete(SceneLoadTicket ticket)
    {
        if (!IsCurrent(ticket)) return false;
        IsLoading = false;
        return true;
    }

    internal void Invalidate()
    {
        _generation = checked(_generation + 1);
        IsLoading = false;
    }
}
