// SPDX-License-Identifier: Apache-2.0

using System;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Owns the lifetime of package preflight work. Every newly-started operation
/// cancels the previous one, while the semaphore guarantees that even a slow
/// cancellation cannot turn repeated UI validation into parallel filesystem
/// scans.
/// </summary>
internal sealed class PackageValidationCoordinator : IDisposable
{
    private readonly object _sync = new();
    private readonly SemaphoreSlim _serial = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private CancellationTokenSource? _latest;
    private int _generation;
    private bool _disposed;

    internal PackageValidationOperation BeginLatest(
        CancellationToken cancellationToken = default)
    {
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            _latest?.Cancel();
            CancellationTokenSource source =
                CancellationTokenSource.CreateLinkedTokenSource(
                    _lifetime.Token,
                    cancellationToken);
            _latest = source;
            int generation = checked(++_generation);
            return new PackageValidationOperation(
                this,
                source,
                generation);
        }
    }

    internal void CancelLatest()
    {
        lock (_sync)
            _latest?.Cancel();
    }

    internal bool IsCurrent(PackageValidationOperation operation)
    {
        ArgumentNullException.ThrowIfNull(operation);
        lock (_sync)
        {
            return !_disposed &&
                   ReferenceEquals(_latest, operation.Source) &&
                   operation.Generation == _generation &&
                   !operation.Token.IsCancellationRequested;
        }
    }

    internal async Task<T> RunAsync<T>(
        PackageValidationOperation operation,
        Func<CancellationToken, T> work)
    {
        ArgumentNullException.ThrowIfNull(operation);
        ArgumentNullException.ThrowIfNull(work);
        CancellationToken token = operation.Token;
        await _serial.WaitAsync(token).ConfigureAwait(false);
        try
        {
            token.ThrowIfCancellationRequested();
            return await Task.Run(
                    () =>
                    {
                        token.ThrowIfCancellationRequested();
                        T result = work(token);
                        token.ThrowIfCancellationRequested();
                        return result;
                    },
                    token)
                .ConfigureAwait(false);
        }
        finally
        {
            _serial.Release();
        }
    }

    internal void Complete(PackageValidationOperation operation)
    {
        lock (_sync)
        {
            if (ReferenceEquals(_latest, operation.Source))
                _latest = null;
        }
    }

    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed)
                return;
            _disposed = true;
            _lifetime.Cancel();
            _latest?.Cancel();
            _latest = null;
        }
        _lifetime.Dispose();
    }
}

internal sealed class PackageValidationOperation : IDisposable
{
    private PackageValidationCoordinator? _owner;

    internal PackageValidationOperation(
        PackageValidationCoordinator owner,
        CancellationTokenSource source,
        int generation)
    {
        _owner = owner;
        Source = source;
        Generation = generation;
    }

    internal CancellationTokenSource Source { get; }
    internal CancellationToken Token => Source.Token;
    internal int Generation { get; }

    public void Dispose()
    {
        PackageValidationCoordinator? owner =
            Interlocked.Exchange(ref _owner, null);
        if (owner == null)
            return;
        owner.Complete(this);
        Source.Dispose();
    }
}
