// SPDX-License-Identifier: Apache-2.0

using System;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Coalesces repeated close requests and provides a bounded, non-blocking drain
/// for the package workflow.  This type deliberately has no WPF dependency so
/// the shutdown contract can be exercised by the command-line self-test.
/// </summary>
internal sealed class PackageShutdownCoordinator
{
    private readonly object _gate = new();
    private Task<bool>? _inFlight;

    internal Task<bool> RunOnceAsync(Func<Task<bool>> operation)
    {
        ArgumentNullException.ThrowIfNull(operation);
        lock (_gate)
        {
            if (_inFlight != null)
                return _inFlight;

            var completion = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _inFlight = completion.Task;
            _ = ExecuteAsync(operation, completion);
            return completion.Task;
        }
    }

    internal static async Task<bool> CancelAndDrainAsync(
        Task? activeOperation,
        Action requestCancellation,
        TimeSpan timeout)
    {
        ArgumentNullException.ThrowIfNull(requestCancellation);
        if (timeout <= TimeSpan.Zero ||
            timeout == Timeout.InfiniteTimeSpan)
        {
            throw new ArgumentOutOfRangeException(
                nameof(timeout),
                "Package shutdown requires a finite positive timeout.");
        }

        try
        {
            requestCancellation();
        }
        catch (ObjectDisposedException)
        {
            // The operation won the race and disposed its cancellation source.
            // Its task is still the authoritative drain boundary below.
        }

        if (activeOperation == null)
            return true;

        try
        {
            await activeOperation.WaitAsync(timeout).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            return false;
        }
        catch (OperationCanceledException) when (activeOperation.IsCanceled)
        {
            // A cancelled task is terminal and therefore fully drained.
        }
        catch
        {
            // Faulted is also terminal.  The package workflow owns user-facing
            // error reporting; shutdown only needs proof that no work remains.
        }
        return true;
    }

    private async Task ExecuteAsync(
        Func<Task<bool>> operation,
        TaskCompletionSource<bool> completion)
    {
        try
        {
            completion.TrySetResult(
                await operation().ConfigureAwait(false));
        }
        catch (Exception error)
        {
            completion.TrySetException(error);
        }
        finally
        {
            lock (_gate)
            {
                if (ReferenceEquals(_inFlight, completion.Task))
                    _inFlight = null;
            }
        }
    }
}
