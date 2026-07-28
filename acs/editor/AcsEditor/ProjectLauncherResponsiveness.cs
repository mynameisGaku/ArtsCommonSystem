// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal readonly record struct ProjectLauncherAsyncTicket(long Generation);

/// <summary>
/// Owns one generation of launcher work.  The exclusive entry point rejects
/// double-click/Enter races, while <see cref="BeginLatest"/> supports
/// replaceable background presentation such as the recent-project snapshot.
/// Disposing the gate makes every outstanding ticket stale.
/// </summary>
internal sealed class ProjectLauncherAsyncGate : IDisposable
{
    private long _generation;
    private int _exclusiveActive;
    private int _disposed;

    internal bool IsExclusiveActive =>
        Volatile.Read(ref _exclusiveActive) != 0;

    internal ProjectLauncherAsyncTicket BeginLatest()
    {
        if (Volatile.Read(ref _disposed) != 0)
            return default;
        long generation = Interlocked.Increment(ref _generation);
        return Volatile.Read(ref _disposed) == 0
            ? new ProjectLauncherAsyncTicket(generation)
            : default;
    }

    internal bool TryBeginExclusive(out ProjectLauncherAsyncTicket ticket)
    {
        ticket = default;
        if (Volatile.Read(ref _disposed) != 0 ||
            Interlocked.CompareExchange(ref _exclusiveActive, 1, 0) != 0)
        {
            return false;
        }

        long generation = Interlocked.Increment(ref _generation);
        if (Volatile.Read(ref _disposed) != 0)
        {
            Volatile.Write(ref _exclusiveActive, 0);
            return false;
        }
        ticket = new ProjectLauncherAsyncTicket(generation);
        return true;
    }

    internal bool IsCurrent(ProjectLauncherAsyncTicket ticket) =>
        ticket.Generation > 0 &&
        Volatile.Read(ref _disposed) == 0 &&
        Volatile.Read(ref _generation) == ticket.Generation;

    internal bool TryCompleteExclusive(ProjectLauncherAsyncTicket ticket)
    {
        if (!IsCurrent(ticket))
            return false;
        return Interlocked.CompareExchange(ref _exclusiveActive, 0, 1) == 1;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        Interlocked.Increment(ref _generation);
        Volatile.Write(ref _exclusiveActive, 0);
    }
}

internal sealed record ProjectLauncherRecentEntry(
    string Name,
    string Path,
    string Status);

internal static class ProjectLauncherClosePolicy
{
    internal static bool ShouldDeferClose(
        bool exclusiveOperationActive,
        bool closeApproved) =>
        exclusiveOperationActive && !closeApproved;
}

/// <summary>
/// Keeps launcher filesystem work off WPF's Dispatcher.  Cancellation is
/// checked both before and after a non-cancellable legacy operation: the work
/// may have to drain, but its result can never be published after lifetime
/// cancellation.
/// </summary>
internal static class ProjectLauncherBackgroundOperations
{
    internal static Task<T> RunAsync<T>(
        Func<T> operation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(operation);
        return Task.Run(
            () =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                T result = operation();
                cancellationToken.ThrowIfCancellationRequested();
                return result;
            },
            CancellationToken.None);
    }

    internal static Task<IReadOnlyList<ProjectLauncherRecentEntry>>
        LoadRecentEntriesAsync(
            Func<IReadOnlyList<string>> loadPaths,
            Func<string, bool>? pathExists = null,
            CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(loadPaths);
        pathExists ??= File.Exists;
        return RunAsync<IReadOnlyList<ProjectLauncherRecentEntry>>(
            () => loadPaths()
                .Select(path =>
                {
                    bool exists = pathExists(path);
                    string name = System.IO.Path.GetFileNameWithoutExtension(
                        path);
                    return new ProjectLauncherRecentEntry(
                        string.IsNullOrWhiteSpace(name)
                            ? "Unnamed Project"
                            : name,
                        path,
                        exists ? "" : "MISSING");
                })
                .ToArray(),
            cancellationToken);
    }
}
