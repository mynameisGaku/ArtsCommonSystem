// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
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
