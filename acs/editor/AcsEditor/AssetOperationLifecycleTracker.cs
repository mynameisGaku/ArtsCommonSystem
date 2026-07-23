// SPDX-License-Identifier: Apache-2.0

using System;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Tracks fire-and-forget async UI callers through their post-I/O reconciliation.
///
/// A filesystem-operation semaphore only proves that the worker released its turn. The
/// awaiting caller can still need to publish path mappings, rebind open documents, refresh the
/// snapshot, or install a watcher. Close drains this tracker as well, so those continuations
/// cannot run after close preparation has started saving documents or tearing down the window.
/// </summary>
internal sealed class AssetOperationLifecycleTracker
{
    private readonly object _gate = new();
    private int _inFlight;
    private TaskCompletionSource<object?>? _drained;

    internal int InFlightCount
    {
        get
        {
            lock (_gate) return _inFlight;
        }
    }

    internal IDisposable Enter()
    {
        lock (_gate)
        {
            if (_inFlight == 0)
            {
                _drained = new TaskCompletionSource<object?>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
            }
            checked { _inFlight++; }
        }
        return new Lease(this);
    }

    internal Task WaitForDrainAsync()
    {
        lock (_gate)
            return _drained?.Task ?? Task.CompletedTask;
    }

    private void Exit()
    {
        TaskCompletionSource<object?>? drained = null;
        lock (_gate)
        {
            if (_inFlight <= 0)
                throw new InvalidOperationException(
                    "Asset operation lifecycle tracking is unbalanced.");
            _inFlight--;
            if (_inFlight == 0)
            {
                drained = _drained;
                _drained = null;
            }
        }
        drained?.TrySetResult(null);
    }

    private sealed class Lease : IDisposable
    {
        private AssetOperationLifecycleTracker? _owner;

        internal Lease(AssetOperationLifecycleTracker owner) =>
            _owner = owner;

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.Exit();
    }
}
