// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Threading;

namespace AcsEditor;

public partial class AssetBrowserPanel
{
    public static readonly DependencyProperty ThumbnailSizeProperty =
        DependencyProperty.Register(
            nameof(ThumbnailSize),
            typeof(double),
            typeof(AssetBrowserPanel),
            new FrameworkPropertyMetadata(64d));

    private readonly AssetViewPresentationStore _presentationStore = new();
    private readonly DispatcherTimer _presentationSaveDebounce = new()
    {
        Interval = TimeSpan.FromMilliseconds(250),
    };
    private readonly DispatcherTimer _thumbnailViewportDebounce = new()
    {
        Interval = TimeSpan.FromMilliseconds(90),
    };
    private ScrollViewer? _assetScrollViewer;
    private int _thumbnailGeneration = 1;
    private bool _thumbnailReloadPending;
    private AssetViewPresentationState _presentationState =
        AssetViewPresentationState.Default;
    private bool _presentationControlsReady;
    private bool _applyingPresentation;

    public double ThumbnailSize
    {
        get => (double)GetValue(ThumbnailSizeProperty);
        set => SetValue(ThumbnailSizeProperty, value);
    }

    private void InitializeAssetViewPresentation()
    {
        _presentationSaveDebounce.Tick += (_, _) =>
        {
            _presentationSaveDebounce.Stop();
            PersistAssetViewPresentation();
            if (_thumbnailReloadPending)
            {
                _thumbnailReloadPending = false;
                InvalidateThumbnailDecode();
                QueueThumbnailViewportRefresh();
            }
        };
        _thumbnailViewportDebounce.Tick += (_, _) =>
        {
            _thumbnailViewportDebounce.Stop();
            StartThumbnailLoading();
        };
        Loaded += OnAssetBrowserLoaded;
        Unloaded += OnAssetBrowserUnloaded;
        _presentationControlsReady = true;
        ApplyAssetViewPresentation(
            AssetViewPresentationState.Default,
            refreshFolders: false,
            reloadThumbnails: false);
    }

    private void LoadAssetViewPresentation(Project? project)
    {
        _presentationSaveDebounce.Stop();
        AssetViewPresentationState state = AssetViewPresentationState.Default;
        if (project != null)
        {
            try
            {
                state = _presentationStore.Load(project.AssetsDir);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                ArgumentException or InvalidDataException)
            {
                Log?.Invoke(
                    "Asset View preferences could not be loaded; defaults were used: " +
                    error.Message);
            }
        }
        ApplyAssetViewPresentation(
            state,
            refreshFolders: false,
            reloadThumbnails: false);
    }

    private void ScheduleAssetViewPresentationSave()
    {
        if (_applyingPresentation || _project == null) return;
        _presentationSaveDebounce.Stop();
        _presentationSaveDebounce.Start();
    }

    private void FlushAssetViewPresentation()
    {
        if (!_presentationControlsReady) return;
        bool pending = _presentationSaveDebounce.IsEnabled;
        _presentationSaveDebounce.Stop();
        if (pending) PersistAssetViewPresentation();
    }

    private void PersistAssetViewPresentation()
    {
        Project? project = _project;
        if (project == null) return;
        try
        {
            _presentationStore.Save(project.AssetsDir, _presentationState);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
            ArgumentException or InvalidDataException)
        {
            Log?.Invoke("Asset View preferences could not be saved: " + error.Message);
        }
    }

    private void UpdateAssetViewPresentation(
        AssetViewPresentationState requested,
        bool refreshFolders,
        bool reloadThumbnails)
    {
        ApplyAssetViewPresentation(
            requested,
            refreshFolders,
            reloadThumbnails);
        ScheduleAssetViewPresentationSave();
    }

    private void ApplyAssetViewPresentation(
        AssetViewPresentationState requested,
        bool refreshFolders,
        bool reloadThumbnails)
    {
        if (!_presentationControlsReady) return;
        AssetViewPresentationState state = requested.Normalize();
        _presentationState = state;
        _applyingPresentation = true;
        try
        {
            ViewModeBox.SelectedIndex = state.ViewMode switch
            {
                AssetViewMode.List => 1,
                AssetViewMode.Details => 2,
                _ => 0,
            };
            ThumbnailSizeSlider.Value = state.ThumbnailSize;
            ThumbnailSizeSlider.IsEnabled = state.ViewMode == AssetViewMode.Tiles;
            PreviewToggle.IsChecked = state.ShowPreview;
            ShowFoldersMenu.IsChecked = state.ShowFolders;
            ShowEmptyFoldersMenu.IsChecked = state.ShowEmptyFolders;
            ShowEmptyFoldersMenu.IsEnabled = state.ShowFolders;
            ThumbnailSize = state.ThumbnailSize;

            string itemTemplateKey = state.ViewMode switch
            {
                AssetViewMode.List => "AssetListTemplate",
                AssetViewMode.Details => "AssetDetailsTemplate",
                _ => "AssetTileTemplate",
            };
            string itemsPanelKey = state.ViewMode == AssetViewMode.Tiles
                ? "AssetTilesPanel"
                : "AssetRowsPanel";
            string containerStyleKey = state.ViewMode == AssetViewMode.Tiles
                ? "AssetTileContainerStyle"
                : "AssetRowContainerStyle";
            Tiles.ItemTemplate = (DataTemplate)FindResource(itemTemplateKey);
            Tiles.ItemsPanel = (ItemsPanelTemplate)FindResource(itemsPanelKey);
            Tiles.ItemContainerStyle = (Style)FindResource(containerStyleKey);

            PreviewPane.Visibility = state.ShowPreview
                ? Visibility.Visible
                : Visibility.Collapsed;
            PreviewSplitter.Visibility = state.ShowPreview
                ? Visibility.Visible
                : Visibility.Collapsed;
            PreviewColumn.MinWidth = state.ShowPreview ? 180d : 0d;
            PreviewColumn.MaxWidth = state.ShowPreview ? 296d : 0d;
            PreviewColumn.Width = state.ShowPreview
                ? new GridLength(1d, GridUnitType.Star)
                : new GridLength(0d);
            if (!state.ShowPreview)
                _previewLoadCancellation?.Cancel();
        }
        finally
        {
            _applyingPresentation = false;
        }

        if (reloadThumbnails)
        {
            InvalidateThumbnailDecode();
        }
        if (refreshFolders && _project != null)
            RequestViewRefresh();
        else if (reloadThumbnails && _project != null)
            QueueThumbnailViewportRefresh();
    }

    private int PreferredThumbnailDecodeWidth() =>
        _presentationState.ViewMode switch
        {
            AssetViewMode.List => 40,
            AssetViewMode.Details => 24,
            _ => _presentationState.ThumbnailSize,
        };

    private void InvalidateThumbnailDecode()
    {
        _thumbnailLoadCancellation?.Cancel();
        if (_thumbnailGeneration == int.MaxValue)
        {
            _thumbnailGeneration = 1;
            foreach (AssetItem item in Items)
                item.ThumbnailGeneration = 0;
            return;
        }
        _thumbnailGeneration++;
    }

    private void OnAssetBrowserLoaded(object sender, RoutedEventArgs e)
    {
        ScrollViewer? viewer = FindVisualChild<ScrollViewer>(
            Tiles,
            static _ => true);
        if (ReferenceEquals(viewer, _assetScrollViewer))
        {
            QueueThumbnailViewportRefresh();
            return;
        }
        if (_assetScrollViewer != null)
            _assetScrollViewer.ScrollChanged -= OnAssetScrollChanged;
        _assetScrollViewer = viewer;
        if (_assetScrollViewer != null)
            _assetScrollViewer.ScrollChanged += OnAssetScrollChanged;
        QueueThumbnailViewportRefresh();
    }

    private void OnAssetBrowserUnloaded(object sender, RoutedEventArgs e)
    {
        _thumbnailViewportDebounce.Stop();
        if (_assetScrollViewer != null)
            _assetScrollViewer.ScrollChanged -= OnAssetScrollChanged;
        _assetScrollViewer = null;
    }

    private void OnAssetScrollChanged(object sender, ScrollChangedEventArgs e) =>
        QueueThumbnailViewportRefresh();

    private void QueueThumbnailViewportRefresh()
    {
        if (!_presentationControlsReady ||
            _project == null ||
            _assetOperationsSuspended ||
            !IsLoaded)
        {
            return;
        }
        _thumbnailViewportDebounce.Stop();
        _thumbnailViewportDebounce.Start();
    }

    private IReadOnlyList<AssetItem> RealizedThumbnailCandidates()
    {
        var result = new List<AssetItem>();
        CollectRealizedThumbnailCandidates(Tiles, result);
        return result;
    }

    private void CollectRealizedThumbnailCandidates(
        DependencyObject root,
        ICollection<AssetItem> result)
    {
        for (int index = 0;
             index < VisualTreeHelper.GetChildrenCount(root);
             index++)
        {
            DependencyObject child = VisualTreeHelper.GetChild(root, index);
            if (child is ListBoxItem container)
            {
                if (container.DataContext is AssetItem item &&
                    item.ThumbnailGeneration != _thumbnailGeneration &&
                    !item.IsDirectory &&
                    (item.Kind is "image" or "material") &&
                    IsVisualNearThumbnailViewport(container))
                {
                    result.Add(item);
                }
                continue;
            }
            CollectRealizedThumbnailCandidates(child, result);
        }
    }

    private bool IsVisualNearThumbnailViewport(FrameworkElement itemVisual)
    {
        if (!itemVisual.IsVisible || Tiles.ActualHeight <= 0) return false;
        try
        {
            Point position = itemVisual.TranslatePoint(new Point(0, 0), Tiles);
            double viewportHeight = Tiles.ActualHeight;
            return position.Y + itemVisual.ActualHeight >= -viewportHeight &&
                   position.Y <= viewportHeight * 2d;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    private void OnAssetViewModeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_presentationControlsReady || _applyingPresentation ||
            ViewModeBox.SelectedItem is not ComboBoxItem selected ||
            selected.Tag is not string tag ||
            !Enum.TryParse(tag, ignoreCase: true, out AssetViewMode mode))
        {
            return;
        }
        UpdateAssetViewPresentation(
            _presentationState with { ViewMode = mode },
            refreshFolders: false,
            reloadThumbnails: true);
    }

    private void OnThumbnailSizeChanged(
        object sender,
        RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_presentationControlsReady || _applyingPresentation) return;
        int size = (int)Math.Round(
            e.NewValue,
            MidpointRounding.AwayFromZero);
        _thumbnailReloadPending = true;
        UpdateAssetViewPresentation(
            _presentationState with { ThumbnailSize = size },
            refreshFolders: false,
            reloadThumbnails: false);
    }

    private void OnPreviewVisibilityChanged(object sender, RoutedEventArgs e)
    {
        if (!_presentationControlsReady || _applyingPresentation) return;
        UpdateAssetViewPresentation(
            _presentationState with
            {
                ShowPreview = PreviewToggle.IsChecked == true,
            },
            refreshFolders: false,
            reloadThumbnails: false);
    }

    private void OnOpenViewOptions(object sender, RoutedEventArgs e)
    {
        if (ViewOptionsButton.ContextMenu is not ContextMenu menu) return;
        menu.PlacementTarget = ViewOptionsButton;
        menu.Placement = PlacementMode.Bottom;
        menu.IsOpen = true;
    }

    private void OnFolderVisibilityChanged(object sender, RoutedEventArgs e)
    {
        if (!_presentationControlsReady || _applyingPresentation) return;
        UpdateAssetViewPresentation(
            _presentationState with
            {
                ShowFolders = ShowFoldersMenu.IsChecked,
            },
            refreshFolders: true,
            reloadThumbnails: false);
    }

    private void OnEmptyFolderVisibilityChanged(object sender, RoutedEventArgs e)
    {
        if (!_presentationControlsReady || _applyingPresentation) return;
        UpdateAssetViewPresentation(
            _presentationState with
            {
                ShowEmptyFolders = ShowEmptyFoldersMenu.IsChecked,
            },
            refreshFolders: true,
            reloadThumbnails: false);
    }
}
