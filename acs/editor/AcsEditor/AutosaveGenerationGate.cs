// SPDX-License-Identifier: Apache-2.0

using System;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Owns one autosave mode's active worker generation. Invalidating a generation first suppresses
/// new workers, then cancels and asynchronously joins the old worker. Callers may safely discard
/// recovery only after <see cref="InvalidateAndWaitAsync"/> completes.
/// </summary>
internal sealed class AutosaveGenerationGate : IDisposable
{
    private readonly object _sync = new();
    private CancellationTokenSource _generationCancellation = new();
    private Task _activeTask = Task.CompletedTask;
    private bool _suppressed;
    private bool _disposed;

    internal bool TryStart(
        Func<CancellationToken, Task> operation,
        out Task activeTask)
    {
        ArgumentNullException.ThrowIfNull(operation);
        lock (_sync)
        {
            ThrowIfDisposed();
            if (_suppressed || !_activeTask.IsCompleted)
            {
                activeTask = _activeTask;
                return false;
            }

            CancellationToken token = _generationCancellation.Token;
            try
            {
                // Invocation intentionally begins on the caller (the WPF dispatcher) so native
                // scene capture remains thread-affine. Disk/hash work awaits worker tasks later.
                _activeTask = operation(token) ?? Task.CompletedTask;
            }
            catch (Exception ex)
            {
                _activeTask = Task.FromException(ex);
            }
            activeTask = _activeTask;
            return true;
        }
    }

    internal async Task InvalidateAndWaitAsync()
    {
        CancellationTokenSource invalidated;
        Task active;
        lock (_sync)
        {
            ThrowIfDisposed();
            _suppressed = true;
            invalidated = _generationCancellation;
            _generationCancellation = new CancellationTokenSource();
            active = _activeTask;
            invalidated.Cancel();
        }

        try
        {
            await active.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Cancellation is the expected generation transition.
        }
        finally
        {
            invalidated.Dispose();
        }
    }

    internal void Resume()
    {
        lock (_sync)
        {
            ThrowIfDisposed();
            _suppressed = false;
        }
    }

    internal bool IsSuppressed
    {
        get
        {
            lock (_sync) return _suppressed;
        }
    }

    public void Dispose()
    {
        CancellationTokenSource cancellation;
        lock (_sync)
        {
            if (_disposed) return;
            _disposed = true;
            _suppressed = true;
            cancellation = _generationCancellation;
        }
        cancellation.Cancel();
        cancellation.Dispose();
    }

    private void ThrowIfDisposed() =>
        ObjectDisposedException.ThrowIf(_disposed, this);
}
