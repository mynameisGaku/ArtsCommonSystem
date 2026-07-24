// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AcsEditor;

internal static class AssetBrowserUiSelfTest
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

        try
        {
            var lifecycleTracker = new AssetOperationLifecycleTracker();
            IDisposable firstLifecycle = lifecycleTracker.Enter();
            IDisposable secondLifecycle = lifecycleTracker.Enter();
            System.Threading.Tasks.Task firstDrain =
                lifecycleTracker.WaitForDrainAsync();
            Check(
                lifecycleTracker.InFlightCount == 2 && !firstDrain.IsCompleted,
                "close-drain lifecycle remains pending through concurrent caller reconciliation");
            firstLifecycle.Dispose();
            Check(
                lifecycleTracker.InFlightCount == 1 && !firstDrain.IsCompleted,
                "close-drain lifecycle does not complete after only one caller exits");
            secondLifecycle.Dispose();
            firstDrain.GetAwaiter().GetResult();
            secondLifecycle.Dispose();
            Check(
                lifecycleTracker.InFlightCount == 0 &&
                lifecycleTracker.WaitForDrainAsync().IsCompletedSuccessfully,
                "close-drain lifecycle completes exactly once after all callers exit");

            var imageCoordinator = new AssetImageDecodeCoordinator(2);
            int imageGeneration = imageCoordinator.Generation;
            using var decodersStarted = new System.Threading.CountdownEvent(2);
            using var releaseDecoders = new System.Threading.ManualResetEventSlim();
            System.Threading.Tasks.Task<int>[] decoderTasks =
                Enumerable.Range(0, 3)
                    .Select(index => imageCoordinator.RunAsync(
                        imageGeneration,
                        System.Threading.CancellationToken.None,
                        _ =>
                        {
                            decodersStarted.Signal();
                            releaseDecoders.Wait();
                            return index;
                        }))
                    .ToArray();
            bool twoDecodersStarted =
                decodersStarted.Wait(TimeSpan.FromSeconds(5));
            AssetImageDecodeDrain imageDrain =
                imageCoordinator.BeginDrain(suspend: false);
            AssetImageDecodeDrain supersedingImageDrain =
                imageCoordinator.BeginDrain(suspend: false);
            bool inputRanDuringDrain = false;
            var imageDrainFrame = new DispatcherFrame();
            _ = Dispatcher.CurrentDispatcher.BeginInvoke(
                DispatcherPriority.Input,
                new Action(() =>
                {
                    inputRanDuringDrain = true;
                    imageDrainFrame.Continue = false;
                }));
            Dispatcher.PushFrame(imageDrainFrame);
            bool drainWaitedForNonCooperativeDecoders =
                !imageDrain.Completion.IsCompleted;
            releaseDecoders.Set();
            foreach (System.Threading.Tasks.Task<int> decoderTask in decoderTasks)
                ObserveCancellation(decoderTask);
            System.Threading.Tasks.Task.WhenAll(
                    imageDrain.Completion,
                    supersedingImageDrain.Completion)
                .GetAwaiter()
                .GetResult();
            bool reopenedBeforeFinalUiDrain =
                imageCoordinator.CompleteDrain();
            bool reopenedAfterFinalUiDrain =
                imageCoordinator.CompleteDrain();
            Check(
                twoDecodersStarted &&
                imageCoordinator.MaximumObservedDecoderCount == 2 &&
                imageCoordinator.ActiveDecoderCount == 0,
                "image decode coordinator enforces two workers and drains every active decoder");
            Check(
                inputRanDuringDrain &&
                drainWaitedForNonCooperativeDecoders &&
                !reopenedBeforeFinalUiDrain &&
                reopenedAfterFinalUiDrain &&
                !imageCoordinator.IsCurrent(imageGeneration) &&
                imageCoordinator.IsCurrent(supersedingImageDrain.Generation),
                "consecutive image drains stay asynchronous and reject every late result from retired generations");
            int resumedDecode = imageCoordinator.RunAsync(
                    imageCoordinator.Generation,
                    System.Threading.CancellationToken.None,
                    static _ => 17)
                .GetAwaiter()
                .GetResult();
            Check(
                resumedDecode == 17,
                "image decoder resumes only after the retired generation has fully drained");

            var boundedImageCoordinator = new AssetImageDecodeCoordinator(
                maximumConcurrency: 1,
                drainDeadline: TimeSpan.FromMilliseconds(75));
            int boundedRetiredGeneration =
                boundedImageCoordinator.Generation;
            using var boundedDecodeStarted =
                new System.Threading.ManualResetEventSlim();
            using var releaseBoundedDecode =
                new System.Threading.ManualResetEventSlim();
            System.Threading.Tasks.Task<int> boundedLateDecode =
                boundedImageCoordinator.RunAsync(
                    boundedRetiredGeneration,
                    System.Threading.CancellationToken.None,
                    _ =>
                    {
                        boundedDecodeStarted.Set();
                        releaseBoundedDecode.Wait();
                        return 23;
                    });
            bool boundedDecodeWasRunning =
                boundedDecodeStarted.Wait(TimeSpan.FromSeconds(5));
            var boundedDrainElapsed =
                System.Diagnostics.Stopwatch.StartNew();
            AssetImageDecodeDrain boundedDrain =
                boundedImageCoordinator.BeginDrain(suspend: false);
            boundedDrain.Completion.GetAwaiter().GetResult();
            boundedDrainElapsed.Stop();
            bool latePublishRan = false;
            bool latePublishAccepted =
                boundedImageCoordinator.TryRunIfCurrent(
                    boundedRetiredGeneration,
                    () => latePublishRan = true);
            bool boundedGenerationReopened =
                boundedImageCoordinator.CompleteDrain();
            bool oldDecodeStillDetached =
                boundedImageCoordinator.ActiveDecoderCount == 1 &&
                !boundedDrain.FullCompletion.IsCompleted;
            releaseBoundedDecode.Set();
            ObserveCancellation(boundedLateDecode);
            boundedDrain.FullCompletion.GetAwaiter().GetResult();
            Check(
                boundedDecodeWasRunning &&
                boundedDrainElapsed.Elapsed < TimeSpan.FromSeconds(2) &&
                oldDecodeStillDetached &&
                boundedGenerationReopened &&
                !latePublishAccepted &&
                !latePublishRan &&
                boundedImageCoordinator.IsCurrent(
                    boundedDrain.Generation) &&
                boundedImageCoordinator.ActiveDecoderCount == 0,
                "image close drain is bounded and discards a cancellation-insensitive late result");

            string imageRoot = Path.Combine(
                Path.GetTempPath(),
                "acs-asset-image-ui-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(imageRoot);
            try
            {
                string widePath = Path.Combine(imageRoot, "wide.png");
                string tallPath = Path.Combine(imageRoot, "tall.png");
                string invalidPath = Path.Combine(imageRoot, "invalid.png");
                string oversizedPath = Path.Combine(imageRoot, "oversized.png");
                WriteTestPng(widePath, width: 2048, height: 128);
                WriteTestPng(tallPath, width: 128, height: 2048);
                File.WriteAllText(invalidPath, "not an image");
                using (var oversized = new FileStream(
                           oversizedPath,
                           FileMode.CreateNew,
                           FileAccess.Write,
                           FileShare.None))
                {
                    oversized.SetLength(
                        AssetImageDecoder.MaximumSourceBytes + 1);
                }

                var wide = AssetImageDecoder.TryDecode(widePath, 96)
                    as BitmapSource;
                var tall = AssetImageDecoder.TryDecode(tallPath, 96)
                    as BitmapSource;
                bool aspectPreserved =
                    wide != null &&
                    tall != null &&
                    wide.PixelWidth == 96 &&
                    wide.PixelHeight == 6 &&
                    tall.PixelWidth == 6 &&
                    tall.PixelHeight == 96;
                Check(
                    aspectPreserved,
                    "bounded WIC decode preserves wide and tall aspect ratios at the requested long edge");

                bool sourceHandleReleased = false;
                using (var exclusive = new FileStream(
                           widePath,
                           FileMode.Open,
                           FileAccess.ReadWrite,
                           FileShare.None))
                {
                    sourceHandleReleased = exclusive.Length > 0;
                }
                Check(
                    sourceHandleReleased &&
                    AssetImageDecoder.TryDecode(invalidPath, 96) == null &&
                    AssetImageDecoder.TryDecode(oversizedPath, 96) == null,
                    "decoded images retain no source handle and malformed/oversized sources fail closed");

                if (wide != null && tall != null)
                {
                    long oneImageBudget =
                        Math.Max(
                            AssetImageDecoder.EstimateDecodedBytes(wide),
                            AssetImageDecoder.EstimateDecodedBytes(tall)) +
                        128;
                    var imageCache = new AssetImageCache(
                        maximumEntries: 8,
                        maximumDecodedBytes: oneImageBudget);
                    var wideInfo = new FileInfo(widePath);
                    var tallInfo = new FileInfo(tallPath);
                    imageCache.Put(
                        "wide",
                        wideInfo.Length,
                        wideInfo.LastWriteTimeUtc.Ticks,
                        wide);
                    imageCache.Put(
                        "tall",
                        tallInfo.Length,
                        tallInfo.LastWriteTimeUtc.Ticks,
                        tall);
                    bool boundedCache =
                        imageCache.Count == 1 &&
                        imageCache.DecodedBytes <= oneImageBudget;
                    bool staleMiss = !imageCache.TryGet(
                        "tall",
                        tallInfo.Length + 1,
                        tallInfo.LastWriteTimeUtc.Ticks,
                        out _);
                    Check(
                        boundedCache && staleMiss && imageCache.Count == 0,
                        "decoded-image LRU obeys its byte budget and removes stale source revisions");
                }
            }
            finally
            {
                try
                {
                    Directory.Delete(imageRoot, recursive: true);
                }
                catch (Exception error) when (
                    error is IOException or UnauthorizedAccessException)
                {
                }
            }

            var incrementalItems =
                new System.Collections.ObjectModel.ObservableCollection<int>();
            var supersededItems =
                new System.Collections.ObjectModel.ObservableCollection<int>();
            int countAtInputTick = -1;
            bool oldGenerationCurrent = true;
            Dispatcher dispatcher = Dispatcher.CurrentDispatcher;
            var incrementalList = new ListBox
            {
                ItemsSource = incrementalItems,
                Width = 320,
                Height = 180,
            };
            ScrollViewer.SetCanContentScroll(incrementalList, true);
            VirtualizingPanel.SetIsVirtualizing(incrementalList, true);
            VirtualizingPanel.SetVirtualizationMode(
                incrementalList,
                VirtualizationMode.Recycling);
            var incrementalHost = new Window
            {
                Width = 320,
                Height = 180,
                Content = incrementalList,
                ShowActivated = false,
                ShowInTaskbar = false,
                WindowStyle = WindowStyle.None,
                Opacity = 0d,
                Left = -32_000d,
                Top = -32_000d,
            };
            ShutdownMode incrementalPreviousShutdownMode =
                Application.Current.ShutdownMode;
            Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            try
            {
                incrementalHost.Show();
                System.Threading.Tasks.Task incremental =
                    AssetViewIncrementalMaterializer.AddAsync(
                        Enumerable.Range(0, 10_240).ToArray(),
                        incrementalItems.Add,
                        static () => true,
                        dispatcher,
                        System.Threading.CancellationToken.None,
                        chunkSize: 64);
                _ = dispatcher.BeginInvoke(
                    DispatcherPriority.Input,
                    new Action(() => countAtInputTick = incrementalItems.Count));
                var materializationFrame = new DispatcherFrame();
                _ = incremental.ContinueWith(
                    _ => dispatcher.BeginInvoke(
                        DispatcherPriority.Send,
                        new Action(() => materializationFrame.Continue = false)),
                    System.Threading.Tasks.TaskScheduler.Default);
                Dispatcher.PushFrame(materializationFrame);
                incremental.GetAwaiter().GetResult();

                System.Threading.Tasks.Task superseded =
                    AssetViewIncrementalMaterializer.AddAsync(
                        Enumerable.Range(0, 10_240).ToArray(),
                        supersededItems.Add,
                        () => oldGenerationCurrent,
                        dispatcher,
                        System.Threading.CancellationToken.None,
                        chunkSize: 64);
                _ = dispatcher.BeginInvoke(
                    DispatcherPriority.Input,
                    new Action(() => oldGenerationCurrent = false));
                var supersededFrame = new DispatcherFrame();
                _ = superseded.ContinueWith(
                    _ => dispatcher.BeginInvoke(
                        DispatcherPriority.Send,
                        new Action(() => supersededFrame.Continue = false)),
                    System.Threading.Tasks.TaskScheduler.Default);
                Dispatcher.PushFrame(supersededFrame);
                superseded.GetAwaiter().GetResult();
            }
            finally
            {
                incrementalHost.Close();
                Application.Current.ShutdownMode =
                    incrementalPreviousShutdownMode;
            }
            Check(
                countAtInputTick > 0 &&
                countAtInputTick < 10_240 &&
                incrementalItems.Count == 10_240,
                "hidden-window 10,000-item materialization yields to an input tick before completion");
            Check(
                supersededItems.Count > 0 &&
                supersededItems.Count < 10_240,
                "a superseded view generation stops at the next dispatcher chunk boundary");

            var panel = new AssetBrowserPanel();
            Check(panel.Tiles.ItemTemplate != null &&
                  panel.Tiles.ItemsPanel != null &&
                  panel.Tiles.ItemContainerStyle != null,
                "Asset View constructs its default tile presentation without opening a window");

            long snapshotWriteTicks =
                new DateTime(2025, 4, 3, 2, 1, 0, DateTimeKind.Utc).Ticks;
            var snapshotOnlyItem = new AssetItem
            {
                AssetId = "snapshot-only",
                FullPath = Path.Combine(
                    Path.GetTempPath(),
                    "missing-asset-" + Guid.NewGuid().ToString("N") + ".txt"),
                Name = "snapshot-only.txt",
                Kind = "text",
                SizeBytes = 1536,
                LastWriteUtcTicks = snapshotWriteTicks,
            };
            panel.Items.Add(snapshotOnlyItem);
            panel.Tiles.SelectedItem = snapshotOnlyItem;
            string expectedLocalWriteTime =
                new DateTime(snapshotWriteTicks, DateTimeKind.Utc)
                    .ToLocalTime()
                    .ToString("yyyy-MM-dd HH:mm");
            Check(
                panel.PreviewMetaText.Text.Contains("1.5 KB", StringComparison.Ordinal) &&
                panel.PreviewMetaText.Text.Contains(
                    expectedLocalWriteTime,
                    StringComparison.Ordinal) &&
                !File.Exists(snapshotOnlyItem.FullPath),
                "selection metadata comes from the indexed snapshot even when the asset path is unavailable");
            panel.Tiles.SelectedItem = null;
            panel.Items.Clear();

            panel.ViewModeBox.SelectedIndex = 1;
            Check(
                ReferenceEquals(
                    panel.Tiles.ItemTemplate,
                    panel.FindResource("AssetListTemplate")) &&
                !panel.ThumbnailSizeSlider.IsEnabled,
                "List mode swaps presentation resources and disables tile sizing");

            panel.ViewModeBox.SelectedIndex = 2;
            Check(
                ReferenceEquals(
                    panel.Tiles.ItemTemplate,
                    panel.FindResource("AssetDetailsTemplate")) &&
                ReferenceEquals(
                    panel.Tiles.ItemsPanel,
                    panel.FindResource("AssetRowsPanel")),
                "Details mode uses the virtualized row panel and details template");

            panel.PreviewToggle.IsChecked = false;
            Check(panel.PreviewPane.Visibility == Visibility.Collapsed &&
                  panel.PreviewSplitter.Visibility == Visibility.Collapsed &&
                  panel.PreviewColumn.Width.Value == 0d,
                "Preview toggle collapses both pane and splitter");

            panel.ShowFoldersMenu.IsChecked = false;
            Check(!panel.ShowEmptyFoldersMenu.IsEnabled &&
                  !panel.ShowEmptyFoldersMenu.IsChecked,
                "hiding folders also disables the empty-folder option");

            panel.ViewModeBox.SelectedIndex = 0;
            panel.ThumbnailSizeSlider.Value = 112d;
            Check(panel.ThumbnailSize == 112d &&
                  panel.ThumbnailSizeSlider.IsEnabled,
                "tile thumbnail size updates the presentation dependency property");

            Check(panel.AssetActionsButton.ContextMenu != null &&
                  panel.ViewOptionsButton.ContextMenu != null,
                "Asset Actions and View Options menus are available from the toolbar");
            string[] newAssetTemplates = panel.NewAssetButton.ContextMenu?.Items
                .OfType<MenuItem>()
                .Select(static item => item.Tag as string)
                .Where(static tag => tag != null)
                .Cast<string>()
                .ToArray() ?? Array.Empty<string>();
            string[] contextNewAssetTemplates = panel.CtxNewAsset.Items
                .OfType<MenuItem>()
                .Select(static item => item.Tag as string)
                .Where(static tag => tag != null)
                .Cast<string>()
                .ToArray();
            string[] expectedNewAssetTemplates =
                { "Folder", "Material", "Scene", "Blueprint", "Prefab" };
            Check(
                newAssetTemplates.SequenceEqual(
                    expectedNewAssetTemplates,
                    StringComparer.OrdinalIgnoreCase) &&
                contextNewAssetTemplates.SequenceEqual(
                    expectedNewAssetTemplates,
                    StringComparer.OrdinalIgnoreCase),
                "toolbar and background New Asset menus expose the same usable templates in canonical order");

            var tilePanelTemplate =
                (ItemsPanelTemplate)panel.FindResource("AssetTilesPanel");
            Check(tilePanelTemplate.LoadContent() is VirtualizingWrapPanel,
                "tile resources use the virtualizing wrap panel");

            var largeList = new ListBox
            {
                Width = 640,
                Height = 420,
                ItemsPanel = tilePanelTemplate,
                ItemsSource = Enumerable.Range(0, 10_000).ToArray(),
            };
            ScrollViewer.SetCanContentScroll(largeList, true);
            VirtualizingPanel.SetIsVirtualizing(largeList, true);
            VirtualizingPanel.SetVirtualizationMode(
                largeList,
                VirtualizationMode.Recycling);
            var layoutHost = new Window
            {
                Width = 640,
                Height = 420,
                Content = largeList,
                ShowActivated = false,
                ShowInTaskbar = false,
                WindowStyle = WindowStyle.None,
                Opacity = 0d,
                Left = -32_000d,
                Top = -32_000d,
            };
            VirtualizingWrapPanel? wrap = null;
            int realized = int.MaxValue;
            ShutdownMode previousShutdownMode =
                Application.Current.ShutdownMode;
            Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            try
            {
                layoutHost.Show();
                largeList.UpdateLayout();
                wrap = FindVisual<VirtualizingWrapPanel>(largeList);
                if (wrap != null)
                    realized = VisualTreeHelper.GetChildrenCount(wrap);
            }
            finally
            {
                layoutHost.Close();
                Application.Current.ShutdownMode = previousShutdownMode;
            }
            bool boundedRealization = wrap != null && realized > 0 && realized < 256;
            Check(
                boundedRealization,
                "10,000 tile items realize only a bounded viewport working set");
            if (!boundedRealization)
            {
                output.WriteLine(
                    $"INFO: virtual tile diagnostic: panel={(wrap == null ? "missing" : "present")}, " +
                    $"realized={realized}");
                output.WriteLine(
                    "INFO: visual tree: " +
                    string.Join(", ", VisualTypes(largeList).Take(24)));
            }
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: Asset View UI construction threw: " + error);
        }

        output.WriteLine(
            $"Asset Browser UI self-test: {passed} PASS / {failed} failures");
        return failed;
    }

    private static T? FindVisual<T>(DependencyObject root)
        where T : DependencyObject
    {
        for (int index = 0;
             index < VisualTreeHelper.GetChildrenCount(root);
             index++)
        {
            DependencyObject child = VisualTreeHelper.GetChild(root, index);
            if (child is T match) return match;
            T? nested = FindVisual<T>(child);
            if (nested != null) return nested;
        }
        return null;
    }

    private static void ObserveCancellation<T>(
        System.Threading.Tasks.Task<T> task)
    {
        try
        {
            _ = task.GetAwaiter().GetResult();
        }
        catch (OperationCanceledException)
        {
        }
    }

    private static void WriteTestPng(
        string path,
        int width,
        int height)
    {
        int stride = checked(width * 4);
        var pixels = new byte[checked(stride * height)];
        for (int index = 0; index < pixels.Length; index += 4)
        {
            pixels[index] = 0x70;
            pixels[index + 1] = 0x98;
            pixels[index + 2] = 0xD0;
            pixels[index + 3] = 0xFF;
        }
        BitmapSource source = BitmapSource.Create(
            width,
            height,
            96,
            96,
            PixelFormats.Bgra32,
            null,
            pixels,
            stride);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(source));
        using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None);
        encoder.Save(stream);
    }

    private static System.Collections.Generic.IEnumerable<string> VisualTypes(
        DependencyObject root)
    {
        yield return root.GetType().Name;
        for (int index = 0;
             index < VisualTreeHelper.GetChildrenCount(root);
             index++)
        {
            foreach (string name in VisualTypes(VisualTreeHelper.GetChild(root, index)))
                yield return name;
        }
    }
}
