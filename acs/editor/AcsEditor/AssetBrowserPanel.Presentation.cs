// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
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

    private readonly AssetViewPresentationIoCoordinator _presentationIo =
        new(new AssetViewPresentationStore());
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
    private Task _presentationLoadTask = Task.CompletedTask;
    private bool _presentationLoadPendingForCurrentProject;
    private bool _restartPresentationLoadAfterResume;

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
            _ = PersistAssetViewPresentation();
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
        if (project == null)
        {
            _presentationIo.CancelLoad();
            _presentationLoadTask = Task.CompletedTask;
            _presentationLoadPendingForCurrentProject = false;
            ApplyAssetViewPresentation(
                AssetViewPresentationState.Default,
                refreshFolders: false,
                reloadThumbnails: false);
            return;
        }

        // 小さな表示設定文書をワーカーで読み込んでいる間も、前のプロジェクトの
        // 表示設定を新しいプロジェクトへ引き継がない。
        ApplyAssetViewPresentation(
            AssetViewPresentationState.Default,
            refreshFolders: false,
            reloadThumbnails: false);
        AssetViewPresentationLoadOperation operation =
            _presentationIo.StartLoad(project.AssetsDir);
        _presentationLoadPendingForCurrentProject = true;
        _presentationLoadTask =
            ObserveAssetViewPresentationLoadAsync(project, operation);
    }

    private void ScheduleAssetViewPresentationSave()
    {
        if (_applyingPresentation || _project == null) return;
        // この編集より前に開始したディスク読み込みで、新しい UI 入力を上書きしない。
        _presentationLoadPendingForCurrentProject = false;
        _presentationIo.CancelLoad();
        _presentationSaveDebounce.Stop();
        _presentationSaveDebounce.Start();
    }

    private Task FlushAssetViewPresentation()
    {
        if (!_presentationControlsReady)
            return _presentationIo.AllPendingSaves;
        bool pending = _presentationSaveDebounce.IsEnabled;
        _presentationSaveDebounce.Stop();
        if (pending)
            _ = PersistAssetViewPresentation();
        return _presentationIo.AllPendingSaves;
    }

    private Task PersistAssetViewPresentation()
    {
        Project? project = _project;
        if (project == null) return Task.CompletedTask;
        AssetViewPresentationSaveOperation operation =
            _presentationIo.EnqueueSave(
                project.AssetsDir,
                _presentationState,
                _presentationIo.Generation);
        _ = ObserveAssetViewPresentationSaveAsync(operation);
        return operation.Completion;
    }

    private async Task ObserveAssetViewPresentationLoadAsync(
        Project project,
        AssetViewPresentationLoadOperation operation)
    {
        AssetViewPresentationLoadResult result =
            await operation.Completion.ConfigureAwait(false);
        if (result.Canceled) return;

        try
        {
            if (Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
                return;
            await Dispatcher.InvokeAsync(
                () =>
                {
                    if (!_presentationIo.IsCurrentLoad(
                            operation.Generation,
                            operation.AssetsRoot) ||
                        _assetOperationsSuspended ||
                        !ReferenceEquals(_project, project))
                    {
                        return;
                    }
                    if (result.Error != null)
                    {
                        _presentationLoadPendingForCurrentProject = false;
                        Log?.Invoke(
                            "Asset View preferences could not be loaded; defaults were used: " +
                            result.Error.Message);
                        return;
                    }
                    _presentationLoadPendingForCurrentProject = false;
                    ApplyAssetViewPresentation(
                        result.State ?? AssetViewPresentationState.Default,
                        refreshFolders: false,
                        reloadThumbnails: false);
                },
                DispatcherPriority.Background);
        }
        catch (Exception error) when (
            error is TaskCanceledException or InvalidOperationException)
        {
            // ディスパッチャーは終了処理中。表示設定 I/O によってクローズ処理を待たせない。
        }
    }

    private async Task ObserveAssetViewPresentationSaveAsync(
        AssetViewPresentationSaveOperation operation)
    {
        AssetViewPresentationSaveResult result =
            await operation.Completion.ConfigureAwait(false);
        if (result.Error == null ||
            Dispatcher.HasShutdownStarted ||
            Dispatcher.HasShutdownFinished)
        {
            return;
        }
        try
        {
            await Dispatcher.InvokeAsync(
                () => Log?.Invoke(
                    "Asset View preferences could not be saved: " +
                    result.Error.Message),
                DispatcherPriority.Background);
        }
        catch (Exception error) when (
            error is TaskCanceledException or InvalidOperationException)
        {
            // 所有元ディスパッチャーの終了開始後は、ログ記録を可能な範囲でのみ行う。
        }
    }

    private void SuspendAssetViewPresentationIo()
    {
        _restartPresentationLoadAfterResume =
            _presentationLoadPendingForCurrentProject &&
            !_presentationLoadTask.IsCompleted;
        _presentationLoadPendingForCurrentProject = false;
        _presentationIo.CancelLoad();
    }

    private void ResumeAssetViewPresentationIo()
    {
        if (!_restartPresentationLoadAfterResume) return;
        _restartPresentationLoadAfterResume = false;
        if (_project != null)
            LoadAssetViewPresentation(_project);
    }

    private static Task<bool> WaitForAssetViewPresentationSaveAsync(
        Task save,
        TimeSpan timeout) =>
        AssetViewPresentationIoCoordinator.WaitForCompletionAsync(save, timeout);

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
            {
                CancelImageLoad(ref _previewLoadCancellation);
                _previewRefreshPendingAfterImageDrain = false;
                PreviewImage.Source = null;
            }
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
        CancelImageLoad(ref _thumbnailLoadCancellation);
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
        if (_presentationState.ShowPreview &&
            Tiles.SelectedItem is AssetItem selectedItem &&
            !selectedItem.IsDirectory &&
            selectedItem.Kind is "material" or "image")
        {
            StartPreviewLoading(selectedItem);
        }
    }

    private void OnAssetBrowserUnloaded(object sender, RoutedEventArgs e)
    {
        _thumbnailViewportDebounce.Stop();
        CancelImageLoads();
        ReleasePublishedThumbnails();
        PreviewImage.Source = null;
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

internal sealed record AssetViewPresentationLoadResult(
    AssetViewPresentationState? State,
    Exception? Error,
    bool Canceled);

internal sealed record AssetViewPresentationLoadOperation(
    long Generation,
    string AssetsRoot,
    Task<AssetViewPresentationLoadResult> Completion);

internal sealed record AssetViewPresentationSaveResult(
    long Generation,
    long Sequence,
    string AssetsRoot,
    Exception? Error);

internal sealed record AssetViewPresentationSaveOperation(
    long Generation,
    long Sequence,
    string AssetsRoot,
    Task<AssetViewPresentationSaveResult> Completion);

/// <summary>
/// Asset View の表示設定文書に対する I/O をディスパッチャーから完全に分離する。
/// 読み込みは世代で有効性を判定し、公開時にキャンセルできる。保存は直列化し、
/// 古いスナップショットが新しいものより後に永続化されないようにする。
/// </summary>
internal sealed class AssetViewPresentationIoCoordinator
{
    private readonly Func<string, AssetViewPresentationState> _load;
    private readonly Action<string, AssetViewPresentationState> _save;
    private readonly object _gate = new();
    private long _generation;
    private long _saveSequence;
    private string? _currentLoadRoot;
    private CancellationTokenSource? _loadCancellation;
    private readonly Dictionary<string, Task<AssetViewPresentationSaveResult>>
        _saveTailsByRoot =
        new(StringComparer.OrdinalIgnoreCase);
    private Task _saveTail = Task.CompletedTask;

    internal AssetViewPresentationIoCoordinator(AssetViewPresentationStore store)
        : this(store.Load, store.Save)
    {
    }

    internal AssetViewPresentationIoCoordinator(
        Func<string, AssetViewPresentationState> load,
        Action<string, AssetViewPresentationState> save)
    {
        ArgumentNullException.ThrowIfNull(load);
        ArgumentNullException.ThrowIfNull(save);
        _load = load;
        _save = save;
    }

    internal long Generation
    {
        get
        {
            lock (_gate) return _generation;
        }
    }

    internal Task LatestSave
    {
        get
        {
            lock (_gate) return _saveTail;
        }
    }

    internal Task AllPendingSaves
    {
        get
        {
            lock (_gate)
            {
                return _saveTailsByRoot.Count == 0
                    ? Task.CompletedTask
                    : Task.WhenAll(_saveTailsByRoot.Values);
            }
        }
    }

    internal Task LatestSaveFor(string assetsRoot)
    {
        string root = NormalizeRoot(assetsRoot);
        lock (_gate)
        {
            return _saveTailsByRoot.TryGetValue(
                    root,
                    out Task<AssetViewPresentationSaveResult>? pending)
                ? pending
                : Task.CompletedTask;
        }
    }

    internal AssetViewPresentationLoadOperation StartLoad(string assetsRoot)
    {
        string root = NormalizeRoot(assetsRoot);
        CancellationTokenSource? retired;
        CancellationTokenSource cancellation = new();
        Task<AssetViewPresentationSaveResult>? saveBarrier;
        long generation;
        lock (_gate)
        {
            retired = _loadCancellation;
            _loadCancellation = cancellation;
            _currentLoadRoot = root;
            generation = ++_generation;
            saveBarrier = _saveTailsByRoot.TryGetValue(
                    root,
                    out Task<AssetViewPresentationSaveResult>? pendingSave)
                ? pendingSave
                : null;
        }
        CancelAndDispose(retired);

        CancellationToken token = cancellation.Token;
        Task<AssetViewPresentationLoadResult> completion =
            RunLoadAfterAsync(saveBarrier, root, token);
        return new AssetViewPresentationLoadOperation(
            generation,
            root,
            completion);
    }

    internal void CancelLoad()
    {
        CancellationTokenSource? retired;
        lock (_gate)
        {
            retired = _loadCancellation;
            _loadCancellation = null;
            _currentLoadRoot = null;
            _generation++;
        }
        CancelAndDispose(retired);
    }

    internal bool IsCurrentLoad(long generation, string assetsRoot)
    {
        string root;
        try
        {
            root = NormalizeRoot(assetsRoot);
        }
        catch (Exception error) when (
            error is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
        lock (_gate)
        {
            return generation == _generation &&
                   string.Equals(
                       root,
                       _currentLoadRoot,
                       StringComparison.OrdinalIgnoreCase);
        }
    }

    internal AssetViewPresentationSaveOperation EnqueueSave(
        string assetsRoot,
        AssetViewPresentationState state,
        long generation)
    {
        ArgumentNullException.ThrowIfNull(state);
        string root = NormalizeRoot(assetsRoot);
        AssetViewPresentationState snapshot = state.Normalize();
        long sequence;
        Task<AssetViewPresentationSaveResult> completion;
        lock (_gate)
        {
            sequence = ++_saveSequence;
            Task predecessor =
                _saveTailsByRoot.TryGetValue(
                    root,
                    out Task<AssetViewPresentationSaveResult>? rootTail)
                    ? rootTail
                    : Task.CompletedTask;
            completion = RunSaveAfterAsync(
                predecessor,
                root,
                snapshot,
                generation,
                sequence);
            _saveTailsByRoot[root] = completion;
            _saveTail = completion;
        }
        return new AssetViewPresentationSaveOperation(
            generation,
            sequence,
            root,
            completion);
    }

    internal static async Task<bool> WaitForCompletionAsync(
        Task operation,
        TimeSpan timeout)
    {
        ArgumentNullException.ThrowIfNull(operation);
        if (operation.IsCompleted) return true;
        if (timeout <= TimeSpan.Zero) return false;
        Task winner = await Task.WhenAny(
                operation,
                Task.Delay(timeout))
            .ConfigureAwait(false);
        return ReferenceEquals(winner, operation);
    }

    private async Task<AssetViewPresentationLoadResult> RunLoadAfterAsync(
        Task<AssetViewPresentationSaveResult>? saveBarrier,
        string assetsRoot,
        CancellationToken token)
    {
        if (saveBarrier != null)
        {
            try
            {
                AssetViewPresentationSaveResult saveResult =
                    await saveBarrier.ConfigureAwait(false);
                if (saveResult.Error != null)
                {
                    return new AssetViewPresentationLoadResult(
                        null,
                        new IOException(
                            "The preceding Asset View preference save failed; " +
                            "a stale preference document was not loaded.",
                            saveResult.Error),
                        Canceled: false);
                }
            }
            catch (Exception error)
            {
                return new AssetViewPresentationLoadResult(
                    null,
                    new IOException(
                        "The preceding Asset View preference save did not complete cleanly; " +
                        "a stale preference document was not loaded.",
                        error),
                    Canceled: false);
            }
        }
        if (token.IsCancellationRequested)
            return new AssetViewPresentationLoadResult(null, null, Canceled: true);

        return await Task.Run(
                () =>
                {
                    if (token.IsCancellationRequested)
                    {
                        return new AssetViewPresentationLoadResult(
                            null,
                            null,
                            Canceled: true);
                    }
                    try
                    {
                        AssetViewPresentationState state = _load(assetsRoot);
                        return token.IsCancellationRequested
                            ? new AssetViewPresentationLoadResult(
                                null,
                                null,
                                Canceled: true)
                            : new AssetViewPresentationLoadResult(
                                state,
                                null,
                                Canceled: false);
                    }
                    catch (Exception error)
                    {
                        return token.IsCancellationRequested
                            ? new AssetViewPresentationLoadResult(
                                null,
                                null,
                                Canceled: true)
                            : new AssetViewPresentationLoadResult(
                                null,
                                error,
                                Canceled: false);
                    }
                })
            .ConfigureAwait(false);
    }

    private async Task<AssetViewPresentationSaveResult> RunSaveAfterAsync(
        Task predecessor,
        string assetsRoot,
        AssetViewPresentationState state,
        long generation,
        long sequence)
    {
        try
        {
            await predecessor.ConfigureAwait(false);
        }
        catch (Exception)
        {
            // コーディネーターによる保存は必ずエラー結果を返す。この不変条件が崩れた場合も、
            // 後続の保存を進行させるための防護とする。
        }

        return await Task.Run(
                () =>
                {
                    try
                    {
                        _save(assetsRoot, state);
                        return new AssetViewPresentationSaveResult(
                            generation,
                            sequence,
                            assetsRoot,
                            null);
                    }
                    catch (Exception error)
                    {
                        return new AssetViewPresentationSaveResult(
                            generation,
                            sequence,
                            assetsRoot,
                            error);
                    }
                })
            .ConfigureAwait(false);
    }

    private static string NormalizeRoot(string assetsRoot)
    {
        if (string.IsNullOrWhiteSpace(assetsRoot))
            throw new ArgumentException(
                "Assets root cannot be empty.",
                nameof(assetsRoot));
        return Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
    }

    private static void CancelAndDispose(CancellationTokenSource? cancellation)
    {
        if (cancellation == null) return;
        try
        {
            cancellation.Cancel();
        }
        finally
        {
            cancellation.Dispose();
        }
    }
}
