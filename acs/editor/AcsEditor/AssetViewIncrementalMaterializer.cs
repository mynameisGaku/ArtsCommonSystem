// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Threading;

namespace AcsEditor;

/// <summary>
/// Adds a prepared Asset View result in bounded UI-thread chunks. Yielding through the
/// dispatcher lets input, paint, close, and a newer filter generation preempt a large result.
/// </summary>
internal static class AssetViewIncrementalMaterializer
{
    internal const int DefaultChunkSize = 96;

    internal static async Task AddAsync<T>(
        IReadOnlyList<T> source,
        Action<T> add,
        Func<bool> isCurrent,
        Dispatcher dispatcher,
        CancellationToken cancellationToken,
        Action<int>? chunkCompleted = null,
        int chunkSize = DefaultChunkSize)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(add);
        ArgumentNullException.ThrowIfNull(isCurrent);
        ArgumentNullException.ThrowIfNull(dispatcher);
        if (chunkSize <= 0)
            throw new ArgumentOutOfRangeException(nameof(chunkSize));
        if (!dispatcher.CheckAccess())
            throw new InvalidOperationException(
                "Asset View materialization must run on its owning dispatcher.");

        int index = 0;
        while (index < source.Count)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!isCurrent()) return;

            int end = Math.Min(source.Count, index + chunkSize);
            for (; index < end; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!isCurrent()) return;
                add(source[index]);
            }
            chunkCompleted?.Invoke(index);
            if (index < source.Count)
                await Dispatcher.Yield(DispatcherPriority.Background);
        }
    }
}
