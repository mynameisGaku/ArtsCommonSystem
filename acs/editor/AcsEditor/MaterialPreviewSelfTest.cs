// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Threading;

namespace AcsEditor;

internal static class MaterialPreviewSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS: " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL: " + label);
            }
        }

        MaterialPreviewRequest request = EffectRequest(effect: 1);
        int renderCount = 0;
        int callerThread = Environment.CurrentManagedThreadId;
        int rendererThread = callerThread;
        using (var pipeline = new MaterialPreviewPipeline(
            (input, cancellation) =>
            {
                rendererThread = Environment.CurrentManagedThreadId;
                Interlocked.Increment(ref renderCount);
                return MaterialPreview.Render(input, cancellation);
            },
            cacheCapacity: 2))
        {
            MaterialPreviewGenerationResult first =
                pipeline.GenerateLatestAsync(request).GetAwaiter().GetResult();
            Check(first.Image is { IsFrozen: true } &&
                  !first.CacheHit &&
                  rendererThread != callerThread &&
                  pipeline.IsCurrent(first),
                "first request renders asynchronously into a frozen dispatcher-safe image");
            Check(first.Image?.PixelWidth == request.PixelSize &&
                  first.Image?.PixelHeight == request.PixelSize,
                "requested PixelSize controls the live CPU preview output resolution");

            MaterialPreviewGenerationResult cached =
                pipeline.GenerateLatestAsync(request).GetAwaiter().GetResult();
            Check(cached.CacheHit &&
                  ReferenceEquals(first.Image, cached.Image) &&
                  renderCount == 1 &&
                  pipeline.IsCurrent(cached),
                "identical input reuses the bounded preview cache");

            MaterialPreviewGenerationResult differentResolution =
                pipeline.GenerateLatestAsync(EffectRequest(effect: 1, quality: 2))
                    .GetAwaiter().GetResult();
            Check(!differentResolution.CacheHit && renderCount == 2,
                "output-resolution request is part of preview cache identity");

            _ = pipeline.GenerateLatestAsync(EffectRequest(effect: 2))
                .GetAwaiter().GetResult();
            Check(pipeline.CachedItemCount == 2,
                "least-recently-used cache remains bounded");
        }

        using (var started = new ManualResetEventSlim())
        using (var release = new ManualResetEventSlim())
        {
            int sharedRenderCount = 0;
            using var pipeline = new MaterialPreviewPipeline(
                (input, cancellation) =>
                {
                    Interlocked.Increment(ref sharedRenderCount);
                    started.Set();
                    release.Wait(cancellation);
                    return MaterialPreview.Render(input, cancellation);
                });

            var firstTask = pipeline.GenerateLatestAsync(request);
            bool didStart = started.Wait(TimeSpan.FromSeconds(3));
            var secondTask = pipeline.GenerateLatestAsync(request);
            release.Set();
            MaterialPreviewGenerationResult first = firstTask.GetAwaiter().GetResult();
            MaterialPreviewGenerationResult second = secondTask.GetAwaiter().GetResult();

            Check(didStart &&
                  sharedRenderCount == 1 &&
                  second.Coalesced &&
                  !pipeline.IsCurrent(first) &&
                  pipeline.IsCurrent(second),
                "equal in-flight requests coalesce while only the newest generation is accepted");
        }

        using (var slowStarted = new ManualResetEventSlim())
        using (var releaseSlow = new ManualResetEventSlim())
        {
            using var pipeline = new MaterialPreviewPipeline(
                (input, cancellation) =>
                {
                    if (input.Effect == 77)
                    {
                        slowStarted.Set();
                        // Deliberately ignore cancellation while blocked. The scheduler
                        // must still reject this stale result after the renderer returns.
                        releaseSlow.Wait(TimeSpan.FromSeconds(3));
                    }
                    return MaterialPreview.Render(input, CancellationToken.None);
                });

            var staleTask = pipeline.GenerateLatestAsync(EffectRequest(effect: 77));
            bool didStart = slowStarted.Wait(TimeSpan.FromSeconds(3));
            var newestTask = pipeline.GenerateLatestAsync(EffectRequest(effect: 8));
            MaterialPreviewGenerationResult newest = newestTask.GetAwaiter().GetResult();
            releaseSlow.Set();
            MaterialPreviewGenerationResult stale = staleTask.GetAwaiter().GetResult();

            Check(didStart &&
                  newest.Image is not null &&
                  pipeline.IsCurrent(newest) &&
                  !pipeline.IsCurrent(stale) &&
                  stale.Cancelled,
                "superseded render is cancelled and cannot overwrite the newest result");

            slowStarted.Reset();
            releaseSlow.Reset();
            var pendingTask = pipeline.GenerateLatestAsync(EffectRequest(effect: 77));
            bool pendingDidStart = slowStarted.Wait(TimeSpan.FromSeconds(3));
            pipeline.CancelPending();
            releaseSlow.Set();
            MaterialPreviewGenerationResult pending =
                pendingTask.GetAwaiter().GetResult();
            Check(pendingDidStart &&
                  pending.Cancelled &&
                  !pipeline.IsCurrent(pending),
                "input invalidation rejects an old result before debounce starts a replacement");
        }

        using (var pipeline = new MaterialPreviewPipeline(
            (_, _) => throw new InvalidOperationException("synthetic preview failure")))
        {
            MaterialPreviewGenerationResult failedResult =
                pipeline.GenerateLatestAsync(request).GetAwaiter().GetResult();
            Check(failedResult.Image is null &&
                  failedResult.Error == "synthetic preview failure" &&
                  pipeline.IsCurrent(failedResult),
                "render failures are reported without escaping the async UI boundary");
        }

        output.WriteLine(
            $"Material preview self-test: {passed} passed, {failed} failed.");
        return failed;
    }

    private static MaterialPreviewRequest EffectRequest(
        int effect,
        int quality = 1) =>
        MaterialPreviewRequest.CreateEffect(
            model: 0,
            background: 0,
            quality,
            pixelSize: quality == 2 ? 96 : 64,
            effect,
            strength: 0.75f,
            p0: 0.25f,
            p1: 0.5f,
            p2: 0f,
            color: new[] { 0.9f, 0.6f, 0.3f, 1f },
            animated: false);
}
