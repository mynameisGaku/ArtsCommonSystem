// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

/// <summary>
/// 上限付き画像デコーダーを実行し、プロジェクト切替／終了に確実な排出境界を提供する。
/// 排出開始と同時に世代を無効化するため、WIC 内部で中断できないデコーダーも、
/// 遅れて完了した結果を次のプロジェクトへ公開できない。
/// </summary>
internal sealed class AssetImageDecodeCoordinator
{
    internal static readonly TimeSpan DefaultDrainDeadline =
        TimeSpan.FromSeconds(3);

    private readonly object _gate = new();
    private readonly SemaphoreSlim _decodeSlots;
    private readonly AssetOperationLifecycleTracker _lifecycles = new();
    private readonly TimeSpan _drainDeadline;
    private CancellationTokenSource _generationCancellation = new();
    private int _generation = 1;
    private bool _accepting = true;
    private bool _suspended;
    private int _drainsInFlight;
    private int _activeDecoders;
    private int _maximumObservedDecoders;

    internal AssetImageDecodeCoordinator(
        int maximumConcurrency = 2,
        TimeSpan? drainDeadline = null)
    {
        if (maximumConcurrency <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumConcurrency));
        TimeSpan effectiveDrainDeadline =
            drainDeadline ?? DefaultDrainDeadline;
        if (effectiveDrainDeadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(drainDeadline));
        _decodeSlots = new SemaphoreSlim(
            maximumConcurrency,
            maximumConcurrency);
        _drainDeadline = effectiveDrainDeadline;
    }

    internal int Generation
    {
        get
        {
            lock (_gate) return _generation;
        }
    }

    internal bool IsAccepting
    {
        get
        {
            lock (_gate) return _accepting && !_suspended;
        }
    }

    internal int ActiveDecoderCount => Volatile.Read(ref _activeDecoders);

    internal int MaximumObservedDecoderCount =>
        Volatile.Read(ref _maximumObservedDecoders);

    internal bool IsCurrent(int generation)
    {
        lock (_gate)
        {
            return generation == _generation &&
                   _accepting &&
                   !_suspended;
        }
    }

    internal bool TryRunIfCurrent(
        int generation,
        Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        lock (_gate)
        {
            if (generation != _generation ||
                !_accepting ||
                _suspended)
            {
                return false;
            }
            action();
            return true;
        }
    }

    internal async Task<T> RunAsync<T>(
        int generation,
        CancellationToken requestCancellation,
        Func<CancellationToken, T> decode)
    {
        ArgumentNullException.ThrowIfNull(decode);
        using IDisposable lifecycle = _lifecycles.Enter();

        CancellationToken generationCancellation;
        lock (_gate)
        {
            if (!_accepting || _suspended || generation != _generation)
                throw new OperationCanceledException(
                    "The image decode generation is no longer current.",
                    requestCancellation);
            generationCancellation = _generationCancellation.Token;
        }

        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            requestCancellation,
            generationCancellation);
        CancellationToken cancellationToken = linked.Token;
        bool enteredSlot = false;
        try
        {
            await _decodeSlots.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            enteredSlot = true;
            cancellationToken.ThrowIfCancellationRequested();
            int active = Interlocked.Increment(ref _activeDecoders);
            UpdateMaximumObserved(active);
            try
            {
                T result = await Task.Run(
                        () =>
                        {
                            cancellationToken.ThrowIfCancellationRequested();
                            T decoded = decode(cancellationToken);
                            cancellationToken.ThrowIfCancellationRequested();
                            return decoded;
                        },
                        cancellationToken)
                    .ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                return result;
            }
            finally
            {
                Interlocked.Decrement(ref _activeDecoders);
            }
        }
        finally
        {
            if (enteredSlot) _decodeSlots.Release();
        }
    }

    /// <summary>
    /// 現在の世代を同期的に無効化し、実行中／キュー中の全デコーダーが終了した後にのみ
    /// 完了する非同期処理を返す。要求の継続処理を排出してデコード済みサーフェスを
    /// 切り離した後、所有者が <see cref="CompleteDrain"/> も呼び出すまで、
    /// 新しい処理の受付を拒否し続ける。
    /// </summary>
    internal AssetImageDecodeDrain BeginDrain(bool suspend)
    {
        CancellationTokenSource retired;
        int generation;
        lock (_gate)
        {
            _accepting = false;
            if (suspend) _suspended = true;
            checked { _drainsInFlight++; }
            retired = _generationCancellation;
            _generationCancellation = new CancellationTokenSource();
            _generation = _generation == int.MaxValue
                ? 1
                : _generation + 1;
            generation = _generation;
        }
        retired.Cancel();
        Task fullCompletion = CompleteRetiredGenerationAsync(retired);
        return new AssetImageDecodeDrain(
            generation,
            CompleteDrainAsync(fullCompletion),
            fullCompletion);
    }

    internal void Resume()
    {
        lock (_gate)
        {
            _suspended = false;
            if (_drainsInFlight == 0)
                _accepting = true;
        }
    }

    /// <summary>
    /// UI 所有者が要求の継続処理を排出し、デコード済みサーフェスを切り離したことを確認する。
    /// 最新のパイプラインを要求受付状態へ戻す呼び出し元にのみ true を返す。
    /// 連続するプロジェクト切替は順不同で完了する可能性がある。
    /// </summary>
    internal bool CompleteDrain()
    {
        lock (_gate)
        {
            if (_drainsInFlight <= 0)
                throw new InvalidOperationException(
                    "Image decode drain completion is unbalanced.");
            _drainsInFlight--;
            bool reopened = _drainsInFlight == 0 && !_suspended;
            if (reopened) _accepting = true;
            return reopened;
        }
    }

    private async Task CompleteDrainAsync(Task fullCompletion)
    {
        _ = await AssetImageDrainDeadline.WaitAsync(
                fullCompletion,
                _drainDeadline)
            .ConfigureAwait(false);
    }

    private async Task CompleteRetiredGenerationAsync(
        CancellationTokenSource retired)
    {
        try
        {
            await _lifecycles.WaitForDrainAsync().ConfigureAwait(false);
        }
        finally
        {
            retired.Dispose();
        }
    }

    private void UpdateMaximumObserved(int active)
    {
        while (true)
        {
            int observed = Volatile.Read(ref _maximumObservedDecoders);
            if (observed >= active) return;
            if (Interlocked.CompareExchange(
                    ref _maximumObservedDecoders,
                    active,
                    observed) == observed)
            {
                return;
            }
        }
    }
}

internal readonly record struct AssetImageDecodeDrain(
    int Generation,
    Task Completion,
    Task FullCompletion);

internal static class AssetImageDrainDeadline
{
    internal static async Task<bool> WaitAsync(
        Task completion,
        TimeSpan deadline)
    {
        ArgumentNullException.ThrowIfNull(completion);
        if (deadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(deadline));
        try
        {
            await completion.WaitAsync(deadline).ConfigureAwait(false);
            return true;
        }
        catch (TimeoutException)
        {
            _ = completion.ContinueWith(
                static task => _ = task.Exception,
                CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted |
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
            return false;
        }
    }
}

/// <summary>
/// ソースのアスペクト比を維持しながら、長辺を上限内に収めてデコードする。
/// メタデータのみを読む WIC 処理でデコード寸法を一方だけ指定する。
/// 両方を指定すると、一部のコーデックでは画像が変形する可能性がある。
/// </summary>
internal static class AssetImageDecoder
{
    internal const int MaximumDecodeEdge = 512;
    internal const long MaximumSourceBytes = 512L * 1024L * 1024L;
    internal const int MaximumSourceDimension = 1_000_000;
    internal const long MaximumSourcePixels = 268_435_456L;
    internal const int MaximumFrameCount = 256;
    internal const long MaximumWorkerOutputBytes = 8L * 1024L * 1024L;

    internal static ImageSource? TryDecode(
        string path,
        int requestedEdge,
        CancellationToken cancellationToken = default) =>
        AssetImageWorkerClient.TryDecode(
            path,
            requestedEdge,
            cancellationToken);

    internal static long EstimateDecodedBytes(ImageSource image)
    {
        if (image is not BitmapSource bitmap)
            return 1024L * 1024L;
        int bitsPerPixel = Math.Max(1, bitmap.Format.BitsPerPixel);
        long bytesPerPixel = Math.Max(4L, (bitsPerPixel + 7L) / 8L);
        try
        {
            return Math.Max(
                1L,
                checked(
                    (long)bitmap.PixelWidth *
                    bitmap.PixelHeight *
                    bytesPerPixel));
        }
        catch (OverflowException)
        {
            return long.MaxValue;
        }
    }
}

/// <summary>
/// エントリー数とデコード済みバイト数の両方に上限を持つ、スレッドセーフな画像 LRU。
/// 追い出しが変えるのは再利用の可否だけで、デコード寸法や表示品質には影響しない。
/// </summary>
internal sealed class AssetImageCache
{
    internal const int DefaultMaximumEntries = 768;
    internal const long DefaultMaximumDecodedBytes = 96L * 1024L * 1024L;

    private readonly object _gate = new();
    private readonly int _maximumEntries;
    private readonly long _maximumDecodedBytes;
    private readonly Dictionary<string, Entry> _entries =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly LinkedList<string> _recency = new();
    private long _decodedBytes;

    internal AssetImageCache(
        int maximumEntries = DefaultMaximumEntries,
        long maximumDecodedBytes = DefaultMaximumDecodedBytes)
    {
        if (maximumEntries <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumEntries));
        if (maximumDecodedBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumDecodedBytes));
        _maximumEntries = maximumEntries;
        _maximumDecodedBytes = maximumDecodedBytes;
    }

    internal int Count
    {
        get
        {
            lock (_gate) return _entries.Count;
        }
    }

    internal long DecodedBytes
    {
        get
        {
            lock (_gate) return _decodedBytes;
        }
    }

    internal bool TryGet(
        string key,
        long sourceLength,
        long sourceWriteTicks,
        out ImageSource? image)
    {
        lock (_gate)
        {
            if (!_entries.TryGetValue(key, out Entry? entry))
            {
                image = null;
                return false;
            }
            if (entry.SourceLength != sourceLength ||
                entry.SourceWriteTicks != sourceWriteTicks)
            {
                RemoveCore(key);
                image = null;
                return false;
            }
            Touch(entry);
            image = entry.Image;
            return true;
        }
    }

    internal void Put(
        string key,
        long sourceLength,
        long sourceWriteTicks,
        ImageSource image)
    {
        ArgumentNullException.ThrowIfNull(image);
        long bytes = AssetImageDecoder.EstimateDecodedBytes(image);
        lock (_gate)
        {
            RemoveCore(key);
            if (bytes > _maximumDecodedBytes) return;

            var node = _recency.AddFirst(key);
            _entries.Add(
                key,
                new Entry(
                    sourceLength,
                    sourceWriteTicks,
                    image,
                    bytes,
                    node));
            _decodedBytes += bytes;
            while (_entries.Count > _maximumEntries ||
                   _decodedBytes > _maximumDecodedBytes)
            {
                LinkedListNode<string>? last = _recency.Last;
                if (last == null) break;
                RemoveCore(last.Value);
            }
        }
    }

    internal void Clear()
    {
        lock (_gate)
        {
            _entries.Clear();
            _recency.Clear();
            _decodedBytes = 0;
        }
    }

    private void Touch(Entry entry)
    {
        _recency.Remove(entry.Node);
        _recency.AddFirst(entry.Node);
    }

    private void RemoveCore(string key)
    {
        if (!_entries.Remove(key, out Entry? entry)) return;
        _recency.Remove(entry.Node);
        _decodedBytes -= entry.DecodedBytes;
    }

    private sealed record Entry(
        long SourceLength,
        long SourceWriteTicks,
        ImageSource Image,
        long DecodedBytes,
        LinkedListNode<string> Node);
}
