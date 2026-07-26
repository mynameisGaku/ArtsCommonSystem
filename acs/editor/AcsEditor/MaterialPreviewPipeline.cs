// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal enum MaterialPreviewKind
{
    Pbr,
    Effect,
}

/// <summary>
/// Immutable, value-equal input for a material preview. Model/background remain in
/// the native-preview contract, while the live CPU window fixes both to its one
/// supported path. Quality currently selects output resolution.
/// </summary>
internal sealed record MaterialPreviewRequest
{
    internal MaterialPreviewKind Kind { get; init; }
    internal int Model { get; init; }
    internal int Background { get; init; }
    internal int Quality { get; init; }
    internal int PixelSize { get; init; }

    internal int Effect { get; init; }
    internal float Strength { get; init; }
    internal float Param0 { get; init; }
    internal float Param1 { get; init; }
    internal float Param2 { get; init; }
    internal float ColorR { get; init; }
    internal float ColorG { get; init; }
    internal float ColorB { get; init; }
    internal float ColorA { get; init; }
    internal bool Animated { get; init; }

    internal float Metallic { get; init; }
    internal float Roughness { get; init; }
    internal float EmissiveR { get; init; }
    internal float EmissiveG { get; init; }
    internal float EmissiveB { get; init; }
    internal float EmissiveStrength { get; init; }
    internal float NormalStrength { get; init; }
    internal float AmbientOcclusion { get; init; }

    internal static MaterialPreviewRequest CreatePbr(
        int model,
        int background,
        int quality,
        int pixelSize,
        float[] baseColor,
        float metallic,
        float roughness,
        float[] emissive,
        float emissiveStrength,
        float normalStrength,
        float ambientOcclusion)
    {
        ArgumentNullException.ThrowIfNull(baseColor);
        ArgumentNullException.ThrowIfNull(emissive);
        return new MaterialPreviewRequest
        {
            Kind = MaterialPreviewKind.Pbr,
            Model = Math.Clamp(model, 0, 2),
            Background = Math.Clamp(background, 0, 2),
            Quality = Math.Clamp(quality, 0, 2),
            PixelSize = Math.Clamp(pixelSize, 8, 1024),
            ColorR = Component(baseColor, 0, 1f),
            ColorG = Component(baseColor, 1, 1f),
            ColorB = Component(baseColor, 2, 1f),
            ColorA = Component(baseColor, 3, 1f),
            Metallic = Finite(metallic),
            Roughness = Finite(roughness, 0.5f),
            EmissiveR = Component(emissive, 0),
            EmissiveG = Component(emissive, 1),
            EmissiveB = Component(emissive, 2),
            EmissiveStrength = Finite(emissiveStrength),
            NormalStrength = Finite(normalStrength, 1f),
            AmbientOcclusion = Finite(ambientOcclusion, 1f),
        };
    }

    internal static MaterialPreviewRequest CreateEffect(
        int model,
        int background,
        int quality,
        int pixelSize,
        int effect,
        float strength,
        float p0,
        float p1,
        float p2,
        float[] color,
        bool animated)
    {
        ArgumentNullException.ThrowIfNull(color);
        return new MaterialPreviewRequest
        {
            Kind = MaterialPreviewKind.Effect,
            Model = Math.Clamp(model, 0, 2),
            Background = Math.Clamp(background, 0, 2),
            Quality = Math.Clamp(quality, 0, 2),
            PixelSize = Math.Clamp(pixelSize, 8, 1024),
            Effect = effect,
            Strength = Finite(strength, 1f),
            Param0 = Finite(p0),
            Param1 = Finite(p1),
            Param2 = Finite(p2),
            ColorR = Component(color, 0),
            ColorG = Component(color, 1),
            ColorB = Component(color, 2),
            ColorA = Component(color, 3, 1f),
            Animated = animated,
        };
    }

    private static float Component(float[] values, int index, float fallback = 0f) =>
        index < values.Length ? Finite(values[index], fallback) : fallback;

    private static float Finite(float value, float fallback = 0f) =>
        float.IsFinite(value) ? value : fallback;
}

internal sealed record MaterialPreviewGenerationResult(
    MaterialPreviewRequest Request,
    long Generation,
    BitmapSource? Image,
    TimeSpan Duration,
    bool CacheHit,
    bool Coalesced,
    bool Cancelled,
    string? Error);

/// <summary>
/// Per-window latest-wins material preview scheduler. Rendering is performed away
/// from the dispatcher, equal requests share/cache a frozen image, and callers
/// must pass the result through <see cref="IsCurrent"/> immediately before display.
/// </summary>
internal sealed class MaterialPreviewPipeline : IDisposable
{
    private sealed class CacheEntry
    {
        internal CacheEntry(MaterialPreviewRequest request, BitmapSource image)
        {
            Request = request;
            Image = image;
        }

        internal MaterialPreviewRequest Request { get; }
        internal BitmapSource Image { get; }
    }

    private sealed class InflightJob
    {
        private readonly object _cancellationGate = new();
        private bool _cancellationDisposed;

        internal InflightJob(MaterialPreviewRequest request, CancellationTokenSource cancellation)
        {
            Request = request;
            Cancellation = cancellation;
        }

        internal MaterialPreviewRequest Request { get; }
        internal CancellationTokenSource Cancellation { get; }
        internal Task<RenderOutcome> Work { get; set; } = null!;

        internal void Cancel()
        {
            lock (_cancellationGate)
            {
                if (_cancellationDisposed) return;
                Cancellation.Cancel();
            }
        }

        internal void DisposeCancellation()
        {
            lock (_cancellationGate)
            {
                if (_cancellationDisposed) return;
                _cancellationDisposed = true;
                Cancellation.Dispose();
            }
        }
    }

    private readonly record struct RenderOutcome(BitmapSource Image, TimeSpan Duration);

    private readonly object _gate = new();
    private readonly Func<MaterialPreviewRequest, CancellationToken, BitmapSource> _renderer;
    private readonly int _cacheCapacity;
    private readonly Dictionary<MaterialPreviewRequest, LinkedListNode<CacheEntry>> _cache = new();
    private readonly LinkedList<CacheEntry> _leastRecentlyUsed = new();
    private readonly CancellationTokenSource _lifetimeCancellation = new();
    private InflightJob? _inflight;
    private long _latestGeneration;
    private bool _disposed;

    internal MaterialPreviewPipeline(
        Func<MaterialPreviewRequest, CancellationToken, BitmapSource>? renderer = null,
        int cacheCapacity = 8)
    {
        if (cacheCapacity < 1)
            throw new ArgumentOutOfRangeException(nameof(cacheCapacity));
        _renderer = renderer ?? MaterialPreview.Render;
        _cacheCapacity = cacheCapacity;
    }

    internal int CachedItemCount
    {
        get
        {
            lock (_gate)
                return _cache.Count;
        }
    }

    internal Task<MaterialPreviewGenerationResult> GenerateLatestAsync(
        MaterialPreviewRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        long generation;
        long lookupStarted = Stopwatch.GetTimestamp();
        BitmapSource? cachedImage = null;
        Task<RenderOutcome>? work = null;
        InflightJob? jobToCancel = null;
        bool coalesced = false;

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            generation = ++_latestGeneration;

            if (TryGetCachedLocked(request, out cachedImage))
            {
                if (_inflight is not null && _inflight.Request != request)
                {
                    jobToCancel = _inflight;
                    _inflight = null;
                }
            }
            else if (_inflight is not null &&
                     _inflight.Request == request &&
                     !_inflight.Work.IsCompleted)
            {
                work = _inflight.Work;
                coalesced = true;
            }
            else
            {
                jobToCancel = _inflight;
                var linkedCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                    _lifetimeCancellation.Token);
                var job = new InflightJob(request, linkedCancellation);
                job.Work = Task.Run(
                    () => RenderAndCache(request, linkedCancellation.Token),
                    CancellationToken.None);
                _inflight = job;
                work = job.Work;
                _ = job.Work.ContinueWith(
                    _ => FinishJob(job),
                    CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default);
            }
        }

        CancelWithoutThrow(jobToCancel);
        if (cachedImage is not null)
        {
            return Task.FromResult(new MaterialPreviewGenerationResult(
                request,
                generation,
                cachedImage,
                Stopwatch.GetElapsedTime(lookupStarted),
                CacheHit: true,
                Coalesced: false,
                Cancelled: false,
                Error: null));
        }

        return AwaitGenerationAsync(
            request,
            generation,
            work!,
            coalesced,
            cancellationToken);
    }

    /// <summary>
    /// Invalidates the current display generation immediately. This is called when
    /// input changes, before debounce elapses, so an older render cannot flash.
    /// </summary>
    internal void CancelPending()
    {
        InflightJob? jobToCancel;
        lock (_gate)
        {
            if (_disposed) return;
            ++_latestGeneration;
            jobToCancel = _inflight;
            _inflight = null;
        }
        CancelWithoutThrow(jobToCancel);
    }

    internal bool IsCurrent(MaterialPreviewGenerationResult result)
    {
        ArgumentNullException.ThrowIfNull(result);
        lock (_gate)
        {
            return !_disposed &&
                   !result.Cancelled &&
                   result.Generation == _latestGeneration;
        }
    }

    private async Task<MaterialPreviewGenerationResult> AwaitGenerationAsync(
        MaterialPreviewRequest request,
        long generation,
        Task<RenderOutcome> work,
        bool coalesced,
        CancellationToken cancellationToken)
    {
        try
        {
            RenderOutcome outcome = await work.WaitAsync(cancellationToken).ConfigureAwait(false);
            return new MaterialPreviewGenerationResult(
                request,
                generation,
                outcome.Image,
                outcome.Duration,
                CacheHit: false,
                Coalesced: coalesced,
                Cancelled: false,
                Error: null);
        }
        catch (OperationCanceledException)
        {
            return new MaterialPreviewGenerationResult(
                request,
                generation,
                null,
                TimeSpan.Zero,
                CacheHit: false,
                Coalesced: coalesced,
                Cancelled: true,
                Error: null);
        }
        catch (Exception error)
        {
            return new MaterialPreviewGenerationResult(
                request,
                generation,
                null,
                TimeSpan.Zero,
                CacheHit: false,
                Coalesced: coalesced,
                Cancelled: false,
                Error: error.Message);
        }
    }

    private RenderOutcome RenderAndCache(
        MaterialPreviewRequest request,
        CancellationToken cancellationToken)
    {
        long started = Stopwatch.GetTimestamp();
        cancellationToken.ThrowIfCancellationRequested();
        BitmapSource image = _renderer(request, cancellationToken) ??
            throw new InvalidOperationException("Material preview renderer returned no image.");
        cancellationToken.ThrowIfCancellationRequested();
        if (image.CanFreeze && !image.IsFrozen)
            image.Freeze();
        if (!image.IsFrozen)
        {
            throw new InvalidOperationException(
                "Material preview renderer must return a frozen image for dispatcher handoff.");
        }

        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!_disposed)
                AddCachedLocked(request, image);
        }
        return new RenderOutcome(image, Stopwatch.GetElapsedTime(started));
    }

    private bool TryGetCachedLocked(
        MaterialPreviewRequest request,
        out BitmapSource? image)
    {
        if (!_cache.TryGetValue(request, out LinkedListNode<CacheEntry>? node))
        {
            image = null;
            return false;
        }

        _leastRecentlyUsed.Remove(node);
        _leastRecentlyUsed.AddFirst(node);
        image = node.Value.Image;
        return true;
    }

    private void AddCachedLocked(MaterialPreviewRequest request, BitmapSource image)
    {
        if (_cache.TryGetValue(request, out LinkedListNode<CacheEntry>? existing))
        {
            _leastRecentlyUsed.Remove(existing);
            _cache.Remove(request);
        }

        var node = new LinkedListNode<CacheEntry>(new CacheEntry(request, image));
        _leastRecentlyUsed.AddFirst(node);
        _cache.Add(request, node);
        while (_cache.Count > _cacheCapacity)
        {
            LinkedListNode<CacheEntry>? oldest = _leastRecentlyUsed.Last;
            if (oldest is null) break;
            _leastRecentlyUsed.RemoveLast();
            _cache.Remove(oldest.Value.Request);
        }
    }

    private void FinishJob(InflightJob job)
    {
        lock (_gate)
        {
            if (ReferenceEquals(_inflight, job))
                _inflight = null;
        }
        job.DisposeCancellation();
    }

    private static void CancelWithoutThrow(InflightJob? job)
    {
        if (job is null) return;
        try { job.Cancel(); }
        catch (ObjectDisposedException) { }
        catch (AggregateException) { }
    }

    public void Dispose()
    {
        InflightJob? jobToCancel;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            ++_latestGeneration;
            jobToCancel = _inflight;
            _inflight = null;
            _cache.Clear();
            _leastRecentlyUsed.Clear();
        }

        try { _lifetimeCancellation.Cancel(); }
        catch (ObjectDisposedException) { }
        catch (AggregateException) { }
        CancelWithoutThrow(jobToCancel);
        _lifetimeCancellation.Dispose();
    }
}
