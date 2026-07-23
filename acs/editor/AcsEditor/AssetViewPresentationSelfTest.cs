// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal static class AssetViewPresentationSelfTest
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

        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-view-presentation-" + Guid.NewGuid().ToString("N"));
        string assets = Path.Combine(root, "Assets");
        try
        {
            Directory.CreateDirectory(assets);
            var store = new AssetViewPresentationStore();
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "missing preferences use deterministic defaults");

            var requested = new AssetViewPresentationState(
                AssetViewMode.Details,
                ThumbnailSize: 500,
                ShowPreview: false,
                ShowFolders: false,
                ShowEmptyFolders: true);
            store.Save(assets, requested);
            AssetViewPresentationState loaded = store.Load(assets);
            Check(loaded.ViewMode == AssetViewMode.Details &&
                  loaded.ThumbnailSize ==
                      AssetViewPresentationState.MaximumThumbnailSize &&
                  !loaded.ShowPreview &&
                  !loaded.ShowFolders &&
                  !loaded.ShowEmptyFolders,
                "round trip normalizes mode, thumbnail size, and folder invariants");

            string path = AssetViewPresentationStore.GetStorePath(assets);
            Check(File.Exists(path) &&
                  !Directory.EnumerateFiles(
                          Path.GetDirectoryName(path)!,
                          "*.tmp-*",
                          SearchOption.TopDirectoryOnly)
                      .Any(),
                "atomic save leaves no temporary files");

            File.WriteAllText(path, "{not json", new UTF8Encoding(false));
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "corrupt preferences fail closed to defaults");

            File.WriteAllText(
                path,
                "{\"schemaVersion\":99,\"viewMode\":\"List\"," +
                "\"thumbnailSize\":48,\"showPreview\":true," +
                "\"showFolders\":true,\"showEmptyFolders\":true}",
                new UTF8Encoding(false));
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "unknown schema versions do not partially apply");

            File.WriteAllText(path, new string('x', 33 * 1024), new UTF8Encoding(false));
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "oversized preference files are bounded");

            var invalidMode = AssetViewPresentationState.Default with
            {
                ViewMode = (AssetViewMode)999,
                ThumbnailSize = 1,
            };
            AssetViewPresentationState normalized = invalidMode.Normalize();
            Check(normalized.ViewMode == AssetViewMode.Tiles &&
                  normalized.ThumbnailSize ==
                      AssetViewPresentationState.MinimumThumbnailSize,
                "invalid in-memory values normalize before persistence");

            AssetViewPresentationState projectAState =
                AssetViewPresentationState.Default with
                {
                    ViewMode = AssetViewMode.List,
                    ThumbnailSize = 48,
                };
            AssetViewPresentationState projectBState =
                AssetViewPresentationState.Default with
                {
                    ViewMode = AssetViewMode.Details,
                    ThumbnailSize = 96,
                };
            string projectA = Path.Combine(root, "ProjectA", "Assets");
            string projectB = Path.Combine(root, "ProjectB", "Assets");
            using (var projectALoadEntered = new ManualResetEventSlim())
            using (var releaseProjectALoad = new ManualResetEventSlim())
            using (var releaseFallback = new Timer(
                       _ => releaseProjectALoad.Set(),
                       null,
                       TimeSpan.FromSeconds(5),
                       Timeout.InfiniteTimeSpan))
            {
                var loadCoordinator = new AssetViewPresentationIoCoordinator(
                    assetsRoot =>
                    {
                        if (string.Equals(
                                assetsRoot,
                                Path.GetFullPath(projectA),
                                StringComparison.OrdinalIgnoreCase))
                        {
                            projectALoadEntered.Set();
                            releaseProjectALoad.Wait();
                            return projectAState;
                        }
                        return projectBState;
                    },
                    static (_, _) => { });

                Stopwatch nonBlockingStart = Stopwatch.StartNew();
                AssetViewPresentationLoadOperation oldLoad =
                    loadCoordinator.StartLoad(projectA);
                nonBlockingStart.Stop();
                bool oldWorkerEntered =
                    projectALoadEntered.Wait(TimeSpan.FromSeconds(2));
                Check(
                    nonBlockingStart.Elapsed < TimeSpan.FromMilliseconds(250) &&
                    oldWorkerEntered &&
                    !oldLoad.Completion.IsCompleted,
                    "preference load starts on a worker without blocking the caller");

                AssetViewPresentationLoadOperation currentLoad =
                    loadCoordinator.StartLoad(projectB);
                AssetViewPresentationLoadResult currentResult =
                    currentLoad.Completion
                        .WaitAsync(TimeSpan.FromSeconds(2))
                        .GetAwaiter()
                        .GetResult();
                releaseProjectALoad.Set();
                AssetViewPresentationLoadResult oldResult =
                    oldLoad.Completion
                        .WaitAsync(TimeSpan.FromSeconds(2))
                        .GetAwaiter()
                        .GetResult();
                Check(
                    currentResult.State == projectBState &&
                    currentResult.Error == null &&
                    !currentResult.Canceled &&
                    loadCoordinator.IsCurrentLoad(
                        currentLoad.Generation,
                        currentLoad.AssetsRoot) &&
                    oldResult.Canceled &&
                    !loadCoordinator.IsCurrentLoad(
                        oldLoad.Generation,
                        oldLoad.AssetsRoot),
                    "project switch rejects a late preference load from the retired generation");
            }

            var saveOrder = new List<int>();
            object saveOrderGate = new();
            AssetViewPresentationState persistedState =
                AssetViewPresentationState.Default;
            using (var firstSaveEntered = new ManualResetEventSlim())
            using (var secondSaveEntered = new ManualResetEventSlim())
            using (var sameRootLoadEntered = new ManualResetEventSlim())
            using (var releaseFirstSave = new ManualResetEventSlim())
            using (var releaseFallback = new Timer(
                       _ => releaseFirstSave.Set(),
                       null,
                       TimeSpan.FromSeconds(5),
                       Timeout.InfiniteTimeSpan))
            {
                var saveCoordinator = new AssetViewPresentationIoCoordinator(
                    _ =>
                    {
                        sameRootLoadEntered.Set();
                        lock (saveOrderGate) return persistedState;
                    },
                    (_, state) =>
                    {
                        if (state.ThumbnailSize == 48)
                        {
                            firstSaveEntered.Set();
                            releaseFirstSave.Wait();
                        }
                        else
                        {
                            secondSaveEntered.Set();
                        }
                        lock (saveOrderGate)
                        {
                            saveOrder.Add(state.ThumbnailSize);
                            persistedState = state;
                        }
                    });
                AssetViewPresentationSaveOperation firstSave =
                    saveCoordinator.EnqueueSave(
                        projectA,
                        projectAState,
                        generation: 7);
                bool firstStarted =
                    firstSaveEntered.Wait(TimeSpan.FromSeconds(2));
                AssetViewPresentationSaveOperation secondSave =
                    saveCoordinator.EnqueueSave(
                        projectA,
                        projectBState,
                        generation: 7);
                AssetViewPresentationLoadOperation sameRootLoad =
                    saveCoordinator.StartLoad(projectA);
                Stopwatch boundedWait = Stopwatch.StartNew();
                bool completedWithinGrace =
                    AssetViewPresentationIoCoordinator.WaitForCompletionAsync(
                            secondSave.Completion,
                            TimeSpan.FromMilliseconds(40))
                        .GetAwaiter()
                        .GetResult();
                boundedWait.Stop();
                bool laterWorkStayedQueued =
                    !secondSaveEntered.IsSet &&
                    !sameRootLoadEntered.IsSet &&
                    !sameRootLoad.Completion.IsCompleted;
                releaseFirstSave.Set();
                Task.WhenAll(firstSave.Completion, secondSave.Completion)
                    .WaitAsync(TimeSpan.FromSeconds(2))
                    .GetAwaiter()
                    .GetResult();
                AssetViewPresentationLoadResult postSaveLoad =
                    sameRootLoad.Completion
                        .WaitAsync(TimeSpan.FromSeconds(2))
                        .GetAwaiter()
                        .GetResult();
                lock (saveOrderGate)
                {
                    Check(
                        firstStarted &&
                        !completedWithinGrace &&
                        boundedWait.Elapsed < TimeSpan.FromSeconds(1) &&
                        laterWorkStayedQueued &&
                        saveOrder.SequenceEqual(new[] { 48, 96 }) &&
                        firstSave.Sequence < secondSave.Sequence &&
                        sameRootLoadEntered.IsSet &&
                        postSaveLoad.State == projectBState,
                        "preference flush is bounded while same-project saves and reloads retain enqueue order");
                }
            }

            int staleLoadAttempts = 0;
            var failingCoordinator = new AssetViewPresentationIoCoordinator(
                _ =>
                {
                    Interlocked.Increment(ref staleLoadAttempts);
                    return AssetViewPresentationState.Default;
                },
                static (_, _) => throw new IOException("synthetic save failure"));
            AssetViewPresentationSaveResult failedSave =
                failingCoordinator.EnqueueSave(
                        projectA,
                        projectAState,
                        generation: 11)
                    .Completion
                    .WaitAsync(TimeSpan.FromSeconds(2))
                    .GetAwaiter()
                    .GetResult();
            AssetViewPresentationLoadResult loadAfterFailedSave =
                failingCoordinator.StartLoad(projectA)
                    .Completion
                    .WaitAsync(TimeSpan.FromSeconds(2))
                    .GetAwaiter()
                    .GetResult();
            Check(
                failedSave.Error is IOException &&
                failingCoordinator.LatestSave.IsCompletedSuccessfully &&
                loadAfterFailedSave.Error is IOException &&
                staleLoadAttempts == 0,
                "background save faults are logged safely and prevent a stale same-project reload");
        }
        finally
        {
            try
            {
                if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException)
            {
            }
        }

        output.WriteLine(
            $"Asset View presentation self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
