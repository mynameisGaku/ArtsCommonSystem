using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AcsEditor;

/// <summary>アセットがダブルクリック/ドラッグで起動されたとき。</summary>
public sealed class AssetActivatedEventArgs : EventArgs
{
    public string FullPath { get; }
    public string Kind { get; }   // "image" | "audio" | "mesh" | "text" | "scene" | "file"
    public string AssetId { get; }
    public AssetActivatedEventArgs(string path, string kind, string assetId = "")
    {
        FullPath = path;
        Kind = kind;
        AssetId = assetId;
    }
}

/// <summary>1 アセット (ファイル or フォルダ) のタイル表示用データ。</summary>
public sealed class AssetItem : INotifyPropertyChanged
{
    private bool _isRenaming;
    private bool _isDropTarget;
    private string _editName = "";
    private ImageSource? _thumb;

    public string AssetId { get; init; } = "";
    public string FullPath { get; init; } = "";
    public string Name { get; init; } = "";
    public string Kind { get; init; } = "file";
    public bool IsDirectory { get; init; }
    public string Glyph { get; init; } = "";
    public Brush GlyphBrush { get; init; } = Brushes.Gray;
    public ImageSource? Thumb
    {
        get => _thumb;
        set
        {
            if (ReferenceEquals(_thumb, value)) return;
            _thumb = value;
            OnPropertyChanged();
        }
    }

    public bool IsRenaming
    {
        get => _isRenaming;
        set
        {
            if (_isRenaming == value) return;
            _isRenaming = value;
            OnPropertyChanged();
        }
    }

    public bool IsDropTarget
    {
        get => _isDropTarget;
        set
        {
            if (_isDropTarget == value) return;
            _isDropTarget = value;
            OnPropertyChanged();
        }
    }

    public string EditName
    {
        get => _editName;
        set
        {
            if (_editName == value) return;
            _editName = value;
            OnPropertyChanged();
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

/// <summary>プロジェクトの Assets フォルダを走査・分類して表示し、ノードへの割当 / インポートを行う。</summary>
public partial class AssetBrowserPanel : UserControl
{
    public ObservableCollection<AssetItem> Items { get; } = new();

    /// <summary>エンジンハンドル (.acsmat サムネイルを実シェーダ GPU プレビューで描く。0=CPU フォールバック)。</summary>
    public IntPtr Engine { get; set; } = IntPtr.Zero;

    private Project? _project;
    private AssetDatabase? _assetDatabase;
    private string _currentDir = "";
    private string _filter = "";
    private string _kindFilter = "all";
    private bool _recursiveFilter;
    private AssetBrowserHistory _history = new();
    private FileSystemWatcher? _watcher;
    private readonly DispatcherTimer _debounce;
    private readonly DispatcherTimer _filterDebounce;
    private CancellationTokenSource? _projectRefreshCancellation;
    private CancellationTokenSource? _thumbnailLoadCancellation;
    private CancellationTokenSource? _previewLoadCancellation;
    private int _projectRefreshGeneration;
    private Point _dragStart;
    private bool _maybeDrag;
    private bool _renameCommitInProgress;
    private bool _refreshPendingWhileRenaming;
    private bool _suppressViewFilterRefresh;
    private bool _assetOperationInProgress;
    private bool _assetOperationsSuspended;
    private bool _refreshPendingAfterOperation;
    private readonly SemaphoreSlim _assetOperationGate = new(1, 1);
    private readonly SemaphoreSlim _imageLoadGate = new(2, 2);
    private IReadOnlyList<AssetRecord> _assetSnapshot = Array.Empty<AssetRecord>();
    private readonly List<string> _assetClipboardPaths = new();
    private bool _assetClipboardCut;
    private AssetItem? _dragItem;
    private AssetItem? _dropTargetItem;
    private ContextMenu? _dropActionMenu;
    private readonly object _thumbnailCacheGate = new();
    private readonly Dictionary<string, ThumbnailCacheEntry> _thumbnailCache =
        new(StringComparer.OrdinalIgnoreCase);

    /// <summary>画像/音声などのアセットがアクティブ化されたとき (MainWindow が割当を担う)。</summary>
    public event EventHandler<AssetActivatedEventArgs>? AssetActivated;
    /// <summary>右クリック「シーンに配置」。Blueprint/Prefab をシーンへ実体化する要求。</summary>
    public event EventHandler<AssetActivatedEventArgs>? AssetPlace;
    /// <summary>右クリック「Blueprint に変換」。旧 Prefab を .acsbp へ移行する要求。</summary>
    public event EventHandler<AssetActivatedEventArgs>? AssetConvert;
    /// <summary>
    /// Lets document hosts suspend writes before a path mutation. Delete may be vetoed when a
    /// selected asset is still open in an editor.
    /// </summary>
    internal event EventHandler<AssetPathMutationStartingEventArgs>? AssetPathMutationStarting;
    /// <summary>Signals completion so temporarily suspended document hosts can resume.</summary>
    internal event EventHandler<AssetPathMutationCompletedEventArgs>? AssetPathMutationCompleted;
    /// <summary>Publishes successful root mappings/deletions to open document hosts.</summary>
    internal event EventHandler<AssetPathsChangedEventArgs>? AssetPathsChanged;
    /// <summary>コンソールへのログ出力。</summary>
    public event Action<string>? Log;

    public bool IsOperationInProgress => _assetOperationInProgress;

    public AssetBrowserPanel()
    {
        InitializeComponent();
        Tiles.ItemsSource = Items;
        _debounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
        _debounce.Tick += (_, _) => { _debounce.Stop(); Refresh(); };
        _filterDebounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(140) };
        _filterDebounce.Tick += (_, _) =>
        {
            _filterDebounce.Stop();
            RequestViewRefresh();
        };
        ClearPreview();
    }

    /// <summary>表示対象のプロジェクトを設定し、Assets フォルダの監視と一覧表示を開始する。</summary>
    public void SetProject(Project? project)
    {
        int generation = ++_projectRefreshGeneration;
        ResetAssetDragState(closeActionMenu: true);
        _projectRefreshCancellation?.Cancel();
        _projectRefreshCancellation?.Dispose();
        _projectRefreshCancellation = null;
        CancelImageLoads();
        _project = project;
        _assetDatabase = null;
        _assetSnapshot = Array.Empty<AssetRecord>();
        _refreshPendingWhileRenaming = false;
        _refreshPendingAfterOperation = false;
        _assetClipboardPaths.Clear();
        _assetClipboardCut = false;
        _history = new AssetBrowserHistory();
        DisposeWatcher();
        UpdateAssetOperationUi();
        if (project == null)
        {
            _currentDir = "";
            Items.Clear();
            PathText.Text = "";
            AssetCountText.Text = "0 assets";
            UpdateNavigationControls();
            UpdateAssetOperationUi();
            return;
        }

        Directory.CreateDirectory(project.AssetsDir);   // 念のため
        _currentDir = project.AssetsDir;
        _history.Reset(_currentDir);
        Items.Clear();
        PathText.Text = "Indexing assets...";

        var cancellation = new CancellationTokenSource();
        _projectRefreshCancellation = cancellation;
        _ = InitializeProjectAsync(
            project, generation, cancellation.Token);
    }

    private async Task InitializeProjectAsync(
        Project project,
        int generation,
        CancellationToken cancellationToken)
    {
        AssetDatabase database;
        AssetDatabaseRefreshResult result;
        try
        {
            (database, result) = await RunAssetOperationAsync(
                () =>
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    AssetDatabase candidate = AssetDatabase.ForProject(project);
                    AssetDatabaseRefreshResult refreshed = candidate.Refresh(
                        cancellationToken: cancellationToken);
                    return (candidate, refreshed);
                },
                waitForTurn: true,
                cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        catch (Exception ex)
        {
            if (generation != _projectRefreshGeneration) return;
            _currentDir = "";
            Items.Clear();
            PathText.Text = "";
            Log?.Invoke("Asset database initialization failed: " + ex.Message);
            return;
        }

        if (generation != _projectRefreshGeneration ||
            cancellationToken.IsCancellationRequested)
        {
            return;
        }
        _assetDatabase = database;
        _assetSnapshot = database.Snapshot();
        UpdateAssetOperationUi();
        ReportIndexResult(result);

        try
        {
            _watcher = new FileSystemWatcher(project.AssetsDir)
            {
                IncludeSubdirectories = true,
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite,
                EnableRaisingEvents = true,
            };
            _watcher.Created += OnFsEvent; _watcher.Deleted += OnFsEvent;
            _watcher.Renamed += OnFsEvent; _watcher.Changed += OnFsEvent;
            _watcher.Error += OnFsWatcherError;
        }
        catch { /* 監視できなくても手動更新で動く */ }

        RefreshView();
    }

    private void OnFsEvent(object sender, FileSystemEventArgs e)
    {
        if (e.FullPath.EndsWith(AssetDatabase.MetadataSuffix, StringComparison.OrdinalIgnoreCase) ||
            AssetCreationWorkflow.IsTemporaryPath(e.FullPath) ||
            e.FullPath.Contains(
                AssetDatabase.MetadataSuffix + ".tmp-",
                StringComparison.OrdinalIgnoreCase) ||
            e.FullPath.Contains(
                Path.DirectorySeparatorChar + AssetDatabase.InternalDirectoryName +
                Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase))
            return;
        Dispatcher.BeginInvoke(() => { _debounce.Stop(); _debounce.Start(); });
    }

    private void OnFsWatcherError(object sender, ErrorEventArgs e)
    {
        Dispatcher.BeginInvoke(() =>
        {
            Log?.Invoke("Asset watcher overflowed; rebuilding the asset index.");
            _debounce.Stop();
            _debounce.Start();
        });
    }

    private void DisposeWatcher()
    {
        if (_watcher != null) { _watcher.EnableRaisingEvents = false; _watcher.Dispose(); _watcher = null; }
    }

    public void Refresh() => _ = RefreshAsync();

    private async Task RefreshAsync()
    {
        if (_assetOperationsSuspended)
        {
            _refreshPendingAfterOperation = true;
            return;
        }
        if (Items.Any(item => item.IsRenaming))
        {
            _refreshPendingWhileRenaming = true;
            return;
        }
        _refreshPendingWhileRenaming = false;
        AssetDatabase? database = _assetDatabase;
        if (database == null)
        {
            RefreshView();
            return;
        }

        if (_assetOperationInProgress)
        {
            _refreshPendingAfterOperation = true;
            return;
        }

        int generation = _projectRefreshGeneration;
        try
        {
            AssetDatabaseRefreshResult result = await RunAssetOperationAsync(
                () => database.Refresh());
            if (generation != _projectRefreshGeneration ||
                !ReferenceEquals(database, _assetDatabase))
                return;
            _assetSnapshot = database.Snapshot();
            ReportIndexResult(result);
        }
        catch (InvalidOperationException) when (_assetOperationInProgress)
        {
            _refreshPendingAfterOperation = true;
            return;
        }
        catch (Exception ex)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            Log?.Invoke("Asset database refresh failed: " + ex.Message);
        }
        RefreshView();
    }

    private async Task<T> RunAssetOperationAsync<T>(
        Func<T> operation,
        bool waitForTurn = false,
        CancellationToken cancellationToken = default)
    {
        if (_assetOperationsSuspended)
            throw new InvalidOperationException("Asset operations are suspended while the editor is closing.");
        if (waitForTurn)
        {
            await _assetOperationGate.WaitAsync(cancellationToken);
        }
        else if (!await _assetOperationGate.WaitAsync(0))
        {
            throw new InvalidOperationException("Another asset operation is already running.");
        }
        if (_assetOperationsSuspended)
        {
            _assetOperationGate.Release();
            throw new OperationCanceledException(
                "Asset operations were suspended before the queued operation started.",
                cancellationToken);
        }

        _assetOperationInProgress = true;
        UpdateAssetOperationUi();
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            return await Task.Run(operation, cancellationToken);
        }
        finally
        {
            _assetOperationInProgress = false;
            _assetOperationGate.Release();
            UpdateAssetOperationUi();
            if (_refreshPendingAfterOperation)
            {
                _refreshPendingAfterOperation = false;
                _ = Dispatcher.BeginInvoke(
                    DispatcherPriority.Background,
                    new Action(Refresh));
            }
        }
    }

    private bool CanStartAssetOperation(string action)
    {
        if (!_assetOperationInProgress && !_assetOperationsSuspended) return true;
        Log?.Invoke(_assetOperationsSuspended
            ? $"{action}: asset operations are suspended while the editor is closing."
            : $"{action}: another asset operation is still running.");
        return false;
    }

    private AssetPathMutationStartingEventArgs? BeginAssetPathMutation(
        AssetPathMutationKind kind,
        IEnumerable<string> affectedRoots)
    {
        var operation = new AssetPathMutationStartingEventArgs(
            Guid.NewGuid(),
            kind,
            affectedRoots);
        if (AssetPathMutationStarting != null)
        {
            foreach (EventHandler<AssetPathMutationStartingEventArgs> handler in
                     AssetPathMutationStarting.GetInvocationList())
            {
                try
                {
                    handler(this, operation);
                }
                catch (Exception error)
                {
                    operation.Cancel = true;
                    operation.CancellationReason =
                        "The open document state could not be prepared safely: " + error.Message;
                }
                if (operation.Cancel) break;
            }
        }
        if (!operation.Cancel) return operation;

        CompleteAssetPathMutation(operation, succeeded: false);
        string reason = string.IsNullOrWhiteSpace(operation.CancellationReason)
            ? "The asset operation was cancelled by an open editor."
            : operation.CancellationReason;
        ReportAssetOperationFailure(reason);
        return null;
    }

    private void CompleteAssetPathMutation(
        AssetPathMutationStartingEventArgs? operation,
        bool succeeded)
    {
        if (operation == null || AssetPathMutationCompleted == null) return;
        var completed = new AssetPathMutationCompletedEventArgs(operation, succeeded);
        foreach (EventHandler<AssetPathMutationCompletedEventArgs> handler in
                 AssetPathMutationCompleted.GetInvocationList())
        {
            try { handler(this, completed); }
            catch (Exception error)
            {
                Log?.Invoke("Open asset editor resume failed: " + error.Message);
            }
        }
    }

    private void PublishAssetPathsChanged(AssetPathsChangedEventArgs change)
    {
        if (AssetPathsChanged == null) return;
        foreach (EventHandler<AssetPathsChangedEventArgs> handler in
                 AssetPathsChanged.GetInvocationList())
        {
            try { handler(this, change); }
            catch (Exception error)
            {
                Log?.Invoke("Open asset editor path update failed: " + error.Message);
            }
        }
    }

    private bool IsCurrentOperationContext(AssetDatabase database, int generation) =>
        !_assetOperationsSuspended &&
        generation == _projectRefreshGeneration &&
        ReferenceEquals(database, _assetDatabase);

    public async Task SuspendOperationsAndWaitAsync()
    {
        _assetOperationsSuspended = true;
        _debounce.Stop();
        _filterDebounce.Stop();
        CancelImageLoads();
        ResetAssetDragState(closeActionMenu: true);
        UpdateAssetOperationUi();
        await _assetOperationGate.WaitAsync();
        _assetOperationGate.Release();
    }

    public void ResumeOperations()
    {
        if (!_assetOperationsSuspended) return;
        _assetOperationsSuspended = false;
        UpdateAssetOperationUi();
        if (_project != null && _assetDatabase == null)
        {
            _projectRefreshCancellation?.Cancel();
            _projectRefreshCancellation?.Dispose();
            var cancellation = new CancellationTokenSource();
            _projectRefreshCancellation = cancellation;
            _ = InitializeProjectAsync(
                _project,
                _projectRefreshGeneration,
                cancellation.Token);
            return;
        }
        if (_refreshPendingAfterOperation)
        {
            _refreshPendingAfterOperation = false;
            Refresh();
            return;
        }
        if (_project != null) RefreshView();
    }

    private void UpdateAssetOperationUi()
    {
        BusyText.Visibility = _assetOperationInProgress
            ? Visibility.Visible
            : Visibility.Collapsed;
        bool acceptsCommands = !_assetOperationInProgress && !_assetOperationsSuspended;
        NewAssetButton.IsEnabled = acceptsCommands &&
                                   _project != null &&
                                   _assetDatabase != null;
        ImportButton.IsEnabled = acceptsCommands && _project != null;
        RefreshButton.IsEnabled = acceptsCommands && _assetDatabase != null;
    }

    private void RefreshView()
    {
        ResetAssetDragState(closeActionMenu: true);
        string[] selectedPaths = Tiles.SelectedItems
            .Cast<AssetItem>()
            .Select(static item => item.FullPath)
            .ToArray();
        _thumbnailLoadCancellation?.Cancel();
        Items.Clear();
        if (_project == null)
        {
            AssetCountText.Text = "0 assets";
            UpdateNavigationControls();
            return;
        }
        if (string.IsNullOrEmpty(_currentDir) || !Directory.Exists(_currentDir))
        {
            _currentDir = _project.AssetsDir;
            _history.Navigate(_currentDir);
        }

        PathText.Text = RelDisplay(_currentDir);
        UpdateNavigationControls();

        try
        {
            foreach (string dir in Directory.GetDirectories(_currentDir)
                         .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase))
            {
                var di = new DirectoryInfo(dir);
                if (di.Name.Equals(AssetDatabase.InternalDirectoryName, StringComparison.OrdinalIgnoreCase) ||
                    (di.Attributes & FileAttributes.ReparsePoint) != 0)
                    continue;
                string relative = Path.GetRelativePath(_project.AssetsDir, dir)
                    .Replace('\\', '/');
                // Type chips filter assets; folders stay navigable unless the query itself
                // explicitly excludes them (for example type:material).
                if (!AssetBrowserQuery.Matches(
                        _filter, "all", di.Name, relative, "folder", "", true))
                    continue;
                Items.Add(new AssetItem
                {
                    FullPath = dir, Name = di.Name, Kind = "dir", IsDirectory = true,
                    Glyph = "📁", GlyphBrush = MakeBrush(0xC9, 0xA2, 0x5A),
                });
            }
            var indexedFiles = _assetSnapshot
                .Where(record => _recursiveFilter
                    ? IsUnder(record.FullPath, _currentDir)
                    : PathEquals(
                        Path.GetDirectoryName(record.FullPath) ?? "",
                        _currentDir))
                .OrderBy(static record => record.RelativePath, StringComparer.OrdinalIgnoreCase);
            foreach (AssetRecord record in indexedFiles)
            {
                string file = record.FullPath;
                if (!AssetBrowserQuery.Matches(
                        _filter,
                        _kindFilter,
                        Path.GetFileName(file),
                        record.RelativePath,
                        record.Kind,
                        record.AssetId,
                        isDirectory: false))
                    continue;
                string kind = record.Kind;
                Items.Add(new AssetItem
                {
                    AssetId = record.AssetId,
                    FullPath = file, Name = Path.GetFileName(file), Kind = kind, IsDirectory = false,
                    Glyph = GlyphFor(kind), GlyphBrush = BrushFor(kind),
                });
            }
        }
        catch (Exception ex) { Log?.Invoke("Asset enumerate error: " + ex.Message); }
        int folderCount = Items.Count(static item => item.IsDirectory);
        int assetCount = Items.Count - folderCount;
        AssetCountText.Text = $"{assetCount} assets  |  {folderCount} folders";
        RestoreSelection(selectedPaths);
        StartThumbnailLoading();
    }

    private void RestoreSelection(IEnumerable<string> paths)
    {
        var selected = new HashSet<string>(
            paths.Select(Path.GetFullPath),
            StringComparer.OrdinalIgnoreCase);
        if (selected.Count == 0) return;
        foreach (AssetItem item in Items)
        {
            if (selected.Contains(Path.GetFullPath(item.FullPath)))
                Tiles.SelectedItems.Add(item);
        }
    }

    private void ReportIndexResult(AssetDatabaseRefreshResult result)
    {
        if (result.CreatedMetadataCount != 0 || result.RecoveredIdentityCount != 0)
        {
            Log?.Invoke(
                $"Asset database: {result.AssetCount} indexed, " +
                $"{result.CreatedMetadataCount} metadata created, " +
                $"{result.RecoveredIdentityCount} identities recovered.");
        }
        foreach (string warning in result.Warnings)
            Log?.Invoke("Asset database warning: " + warning);
    }

    // ===== ナビゲーション =====
    private void OnBack(object sender, RoutedEventArgs e) =>
        NavigateHistory(backward: true);

    private void OnForward(object sender, RoutedEventArgs e) =>
        NavigateHistory(backward: false);

    private void OnUp(object sender, RoutedEventArgs e)
    {
        if (_project == null || PathEquals(_currentDir, _project.AssetsDir)) return;
        string? parent = Path.GetDirectoryName(_currentDir);
        if (parent != null) NavigateToDirectory(parent, addHistory: true);
    }

    private void NavigateHistory(bool backward)
    {
        while (backward ? _history.CanGoBack : _history.CanGoForward)
        {
            string? path = backward ? _history.Back() : _history.Forward();
            if (path != null && NavigateToDirectory(path, addHistory: false)) return;
        }
        UpdateNavigationControls();
    }

    private bool NavigateToDirectory(string path, bool addHistory)
    {
        if (_project == null) return false;
        string full;
        try
        {
            full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
            if (!IsUnderOrEqual(full, _project.AssetsDir) || !Directory.Exists(full))
                return false;
            FileAttributes attributes = File.GetAttributes(full);
            if ((attributes & FileAttributes.Directory) == 0 ||
                (attributes & FileAttributes.ReparsePoint) != 0)
                return false;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            Log?.Invoke("Asset folder is no longer available: " + error.Message);
            return false;
        }
        _currentDir = full;
        if (addHistory) _history.Navigate(full);
        RefreshView();
        return true;
    }

    private void UpdateNavigationControls()
    {
        BackBtn.IsEnabled = _project != null && _history.CanGoBack;
        ForwardBtn.IsEnabled = _project != null && _history.CanGoForward;
        UpBtn.IsEnabled = _project != null &&
                          !string.IsNullOrEmpty(_currentDir) &&
                          !PathEquals(_currentDir, _project.AssetsDir);
    }

    private void OnRefresh(object sender, RoutedEventArgs e) => Refresh();

    private void OnOpenNewAssetMenu(object sender, RoutedEventArgs e)
    {
        if (NewAssetButton.ContextMenu is not ContextMenu menu) return;
        menu.PlacementTarget = NewAssetButton;
        menu.Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom;
        menu.IsOpen = true;
    }

    private async void OnCreateAsset(object sender, RoutedEventArgs e)
    {
        if (_project == null || string.IsNullOrWhiteSpace(_currentDir))
        {
            ReportCreationFailure("アセット作成には開いているプロジェクトが必要です。");
            return;
        }
        if (_assetDatabase == null)
        {
            ReportCreationFailure("アセット索引を準備中です。完了後にもう一度作成してください。");
            return;
        }
        if (!CanStartAssetOperation("Create asset")) return;
        if (sender is not MenuItem menuItem ||
            menuItem.Tag is not string templateName ||
            !Enum.TryParse(templateName, ignoreCase: true, out AcsAssetTemplate template))
        {
            ReportCreationFailure("不明なアセットテンプレートです。");
            return;
        }
        Project project = _project;
        AssetDatabase database = _assetDatabase;
        string currentDirectory = _currentDir;
        int generation = _projectRefreshGeneration;
        AcsAssetCreationResult created;
        AssetDatabaseRefreshResult indexResult;
        try
        {
            (created, indexResult) = await RunAssetOperationAsync(() =>
            {
                AcsAssetCreationResult result = AssetCreationWorkflow.Create(
                    project.AssetsDir,
                    currentDirectory,
                    template,
                    template == AcsAssetTemplate.Material
                        ? static (path, name) =>
                            EngineInterop.acs_editor_material_create(path, name) != 0
                        : null);
                return (result, database.Refresh());
            });
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            ReportCreationFailure("アセット作成に失敗しました: " + error.Message);
            return;
        }

        if (generation != _projectRefreshGeneration ||
            !ReferenceEquals(database, _assetDatabase))
            return;
        try
        {
            // Full-content verification is for cook/CI. A normal refresh still hashes the new
            // cache miss while reusing metadata for unchanged assets, keeping New responsive.
            _assetSnapshot = database.Snapshot();
            ReportIndexResult(indexResult);
            RefreshView();
            SelectCreatedAsset(created.FullPath);
            Log?.Invoke(
                $"Created {created.Definition.DisplayName}: {RelDisplay(created.FullPath)}" +
                (created.Definition.IsDirectory ? "" : " (double-click to open)"));
        }
        catch (Exception error)
        {
            Log?.Invoke(
                "アセットは作成されましたが索引の更新に失敗しました: " + error.Message);
        }
    }

    private void ReportCreationFailure(string message)
    {
        Log?.Invoke(message);
        MessageBox.Show(
            Window.GetWindow(this),
            message,
            "アセット作成",
            MessageBoxButton.OK,
            MessageBoxImage.Warning);
    }

    private void SelectCreatedAsset(string path, bool beginRename = true)
    {
        AssetItem? created = Items.FirstOrDefault(item => PathEquals(item.FullPath, path));
        if (created == null &&
            (_filter.Length != 0 ||
             !_kindFilter.Equals("all", StringComparison.OrdinalIgnoreCase)))
        {
            ClearViewFilters();
            RefreshView();
            created = Items.FirstOrDefault(item => PathEquals(item.FullPath, path));
        }
        if (created == null) return;
        Tiles.SelectedItem = created;
        Tiles.ScrollIntoView(created);
        Tiles.Focus();
        if (beginRename)
            BeginRename(created);
    }

    private void SelectAssets(IEnumerable<string> paths)
    {
        var requested = new HashSet<string>(
            paths.Select(Path.GetFullPath),
            StringComparer.OrdinalIgnoreCase);
        if (requested.Count == 0)
        {
            Tiles.UnselectAll();
            return;
        }

        List<AssetItem> matches = Items
            .Where(item => requested.Contains(Path.GetFullPath(item.FullPath)))
            .ToList();
        if (matches.Count != requested.Count &&
            (_filter.Length != 0 ||
             !_kindFilter.Equals("all", StringComparison.OrdinalIgnoreCase)))
        {
            ClearViewFilters();
            RefreshView();
            matches = Items
                .Where(item => requested.Contains(Path.GetFullPath(item.FullPath)))
                .ToList();
        }

        Tiles.UnselectAll();
        foreach (AssetItem item in matches)
            Tiles.SelectedItems.Add(item);
        if (matches.Count != 0)
        {
            Tiles.ScrollIntoView(matches[^1]);
            Tiles.Focus();
        }
    }

    private void ClearViewFilters()
    {
        _suppressViewFilterRefresh = true;
        try
        {
            _filter = "";
            _kindFilter = "all";
            SearchBox.Text = "";
            KindFilterBox.SelectedIndex = 0;
        }
        finally
        {
            _suppressViewFilterRefresh = false;
        }
    }

    private void OnSearchChanged(object sender, TextChangedEventArgs e)
    {
        _filter = SearchBox.Text?.Trim() ?? "";
        if (_suppressViewFilterRefresh) return;
        _filterDebounce.Stop();
        _filterDebounce.Start();
    }

    private void OnKindFilterChanged(object sender, SelectionChangedEventArgs e)
    {
        if (KindFilterBox.SelectedItem is ComboBoxItem { Tag: string kind })
            _kindFilter = kind;
        if (IsInitialized && !_suppressViewFilterRefresh) RequestViewRefresh();
    }

    private void OnRecursiveFilterChanged(object sender, RoutedEventArgs e)
    {
        _recursiveFilter = RecursiveToggle.IsChecked == true;
        if (IsInitialized && !_suppressViewFilterRefresh) RequestViewRefresh();
    }

    private void RequestViewRefresh()
    {
        if (Items.Any(static item => item.IsRenaming))
        {
            _refreshPendingWhileRenaming = true;
            return;
        }
        RefreshView();
    }

    // タイル選択でプレビュー + 情報を更新 (参考エンジン風)。
    private void OnTileSelected(object sender, SelectionChangedEventArgs e)
    {
        _previewLoadCancellation?.Cancel();
        if (Tiles.SelectedItems.Count > 1)
        {
            PreviewNameText.Text = $"{Tiles.SelectedItems.Count} assets selected";
            PreviewMetaText.Text = "複製・削除・パスのコピーをまとめて実行できます。";
            PreviewImage.Source = null;
            PreviewImage.Visibility = Visibility.Collapsed;
            PreviewGlyphBox.Visibility = Visibility.Visible;
            PreviewGlyph.Text = "MULTI";
            return;
        }
        if (Tiles.SelectedItem is not AssetItem item) { ClearPreview(); return; }

        PreviewNameText.Text = item.Name;
        string meta = item.IsDirectory ? "フォルダ" : KindLabel(item.Kind);
        if (!item.IsDirectory)
        {
            try
            {
                var fi = new FileInfo(item.FullPath);
                meta += "   " + FormatSize(fi.Length);
                meta += "   " + fi.LastWriteTime.ToString("yyyy-MM-dd HH:mm");   // 更新日時
            }
            catch { }
        }
        meta += "\n" + RelDisplay(item.FullPath);
        if (!item.IsDirectory && item.AssetId.Length != 0)
            meta += "\nAsset ID  " + item.AssetId;
        PreviewMetaText.Text = meta;

        PreviewImage.Source = null;
        PreviewImage.Visibility = Visibility.Collapsed;
        PreviewGlyphBox.Visibility = Visibility.Visible;
        PreviewGlyph.Text = item.IsDirectory ? "📁" : item.Glyph;
        if (!item.IsDirectory && item.Kind is "material" or "image")
            StartPreviewLoading(item);
    }

    private void ClearPreview()
    {
        PreviewNameText.Text = "";
        PreviewMetaText.Text = "アセットを選択";
        PreviewImage.Source = null;
        PreviewImage.Visibility = Visibility.Collapsed;
        PreviewGlyphBox.Visibility = Visibility.Visible;
        PreviewGlyph.Text = "";
    }

    private static string KindLabel(string kind) => kind switch
    {
        "image" => "画像", "audio" => "音声", "mesh" => "メッシュ", "text" => "テキスト",
        "scene" => "シーン", "project" => "プロジェクト", "material" => "マテリアル",
        "prefab" => "プレハブ", "blueprint" => "Blueprint", "dir" => "フォルダ", _ => "ファイル",
    };

    private static string FormatSize(long bytes)
    {
        if (bytes < 1024) return $"{bytes} B";
        double kb = bytes / 1024.0;
        if (kb < 1024) return $"{kb:0.#} KB";
        double mb = kb / 1024.0;
        return mb < 1024 ? $"{mb:0.#} MB" : $"{mb / 1024.0:0.#} GB";
    }

    private void OnTileDoubleClick(object sender, MouseButtonEventArgs e)
    {
        DependencyObject? source = e.OriginalSource as DependencyObject;
        if (FindVisualAncestor<TextBox>(
                source,
                static editor => Equals(editor.Tag, "AssetRenameEditor")) != null)
            return;
        AssetItem? clicked = GetItemFromEventSource(source);
        if (clicked == null) return;
        if (Tiles.SelectedItems.Count != 1 ||
            !ReferenceEquals(Tiles.SelectedItem, clicked))
        {
            Tiles.UnselectAll();
            Tiles.SelectedItem = clicked;
        }
        ActivateSelectedAsset();
    }

    private void OnTileKeyDown(object sender, KeyEventArgs e)
    {
        if (e.OriginalSource is TextBox) return;
        ModifierKeys modifiers = Keyboard.Modifiers;
        if (e.Key == Key.Enter && modifiers == ModifierKeys.None)
        {
            if (_assetOperationInProgress || _assetOperationsSuspended)
            {
                CanStartAssetOperation("Open asset");
                e.Handled = true;
                return;
            }
            if (ActivateSelectedAsset()) e.Handled = true;
            return;
        }
        if (e.Key == Key.F2 && modifiers == ModifierKeys.None)
        {
            if (BeginRenameSelected()) e.Handled = true;
            return;
        }
        if (e.Key == Key.D && modifiers == ModifierKeys.Control)
        {
            if (!_assetOperationInProgress && !_assetOperationsSuspended &&
                SelectedAssets().Count != 0 &&
                _assetDatabase != null)
            {
                OnCtxDuplicate(sender, new RoutedEventArgs());
                e.Handled = true;
            }
            return;
        }
        if (e.Key == Key.C && modifiers == ModifierKeys.Control)
        {
            OnCtxCopyAsset(sender, new RoutedEventArgs());
            e.Handled = true;
            return;
        }
        if (e.Key == Key.X && modifiers == ModifierKeys.Control)
        {
            OnCtxCutAsset(sender, new RoutedEventArgs());
            e.Handled = true;
            return;
        }
        if (e.Key == Key.V && modifiers == ModifierKeys.Control)
        {
            if (!_assetOperationInProgress && !_assetOperationsSuspended &&
                ValidateAssetClipboardForPaste())
            {
                OnCtxPasteAsset(sender, new RoutedEventArgs());
                e.Handled = true;
            }
            return;
        }
        if (e.Key == Key.Delete && modifiers == ModifierKeys.None)
        {
            if (!_assetOperationInProgress && !_assetOperationsSuspended &&
                SelectedAssets().Count != 0 &&
                _assetDatabase != null)
            {
                OnCtxDelete(sender, new RoutedEventArgs());
                e.Handled = true;
            }
        }
    }

    private bool ActivateSelectedAsset()
    {
        if (!CanStartAssetOperation("Open asset")) return false;
        if (Tiles.SelectedItems.Count != 1) return false;
        if (Tiles.SelectedItem is not AssetItem item) return false;
        if (item.IsDirectory)
        {
            return NavigateToDirectory(item.FullPath, addHistory: true);
        }
        AssetActivated?.Invoke(
            this,
            new AssetActivatedEventArgs(item.FullPath, item.Kind, item.AssetId));
        return true;
    }

    // 右クリックで対象タイルを選択する (コンテキストメニューが選択アイテムに作用するように)。
    private void OnTileRightDown(object sender, MouseButtonEventArgs e)
    {
        AssetItem? clicked = GetItemFromEventSource(e.OriginalSource as DependencyObject);
        if (clicked == null)
        {
            Tiles.UnselectAll();
            return;
        }
        if (!Tiles.SelectedItems.Contains(clicked))
        {
            Tiles.UnselectAll();
            Tiles.SelectedItem = clicked;
        }
    }

    private List<AssetItem> SelectedAssets() =>
        Tiles.SelectedItems.Cast<AssetItem>().ToList();

    private void OnAssetContextOpened(object sender, RoutedEventArgs e)
    {
        List<AssetItem> selection = SelectedAssets();
        AssetItem? single = selection.Count == 1 ? selection[0] : null;
        bool indexed = _assetDatabase != null;
        bool idle = !_assetOperationInProgress && !_assetOperationsSuspended;
        bool clipboardValid = idle && indexed && ValidateAssetClipboardForPaste();
        CtxNewAsset.IsEnabled = idle && indexed && _project != null &&
                                !string.IsNullOrWhiteSpace(_currentDir);
        CtxOpen.IsEnabled = idle && single != null;
        CtxPlace.IsEnabled = idle && single is { IsDirectory: false } &&
                             single.Kind is "blueprint" or "prefab";
        CtxReferences.IsEnabled = idle && indexed &&
                                  single is { IsDirectory: false } &&
                                  single.AssetId.Length != 0;
        CtxRename.IsEnabled = idle && indexed && single != null;
        CtxDuplicate.IsEnabled = idle && indexed && selection.Count != 0;
        CtxCopyAsset.IsEnabled = idle && indexed && selection.Count != 0;
        CtxCutAsset.IsEnabled = idle && indexed && selection.Count != 0;
        CtxPasteAsset.IsEnabled = clipboardValid;
        CtxDelete.IsEnabled = idle && indexed && selection.Count != 0;
        CtxReveal.IsEnabled = idle && selection.Count != 0;
        CtxCopyPath.IsEnabled = idle && selection.Count != 0;
        CtxConvert.IsEnabled = idle &&
                               single is { IsDirectory: false, Kind: "prefab" };
    }

    private bool ValidateAssetClipboardForPaste()
    {
        if (_assetDatabase == null || _assetClipboardPaths.Count == 0) return false;
        try
        {
            var workflow = new AssetManagementWorkflow(_assetDatabase);
            foreach (string path in _assetClipboardPaths)
                workflow.ValidateExternalPath(path);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private void OnCtxRename(object sender, RoutedEventArgs e) =>
        BeginRenameSelected();

    private bool BeginRenameSelected()
    {
        List<AssetItem> selection = SelectedAssets();
        return selection.Count == 1 && BeginRename(selection[0]);
    }

    private bool BeginRename(AssetItem item)
    {
        if (_assetOperationInProgress || _assetOperationsSuspended ||
            _assetDatabase == null || !Items.Contains(item))
            return false;
        foreach (AssetItem candidate in Items)
            candidate.IsRenaming = false;
        string extension = item.IsDirectory ? "" : Path.GetExtension(item.Name);
        item.EditName = extension.Length == 0
            ? item.Name
            : item.Name[..^extension.Length];
        item.IsRenaming = true;
        _debounce.Stop();
        Tiles.ScrollIntoView(item);
        QueueRenameFocus(item, selectAll: true);
        return true;
    }

    private void QueueRenameFocus(AssetItem item, bool selectAll)
    {
        Dispatcher.BeginInvoke(
            DispatcherPriority.Input,
            new Action(() =>
            {
                if (!item.IsRenaming) return;
                if (Tiles.ItemContainerGenerator.ContainerFromItem(item) is not
                    DependencyObject container)
                    return;
                TextBox? editor = FindVisualChild<TextBox>(
                    container,
                    candidate => Equals(candidate.Tag, "AssetRenameEditor"));
                if (editor == null) return;
                editor.Focus();
                if (selectAll) editor.SelectAll();
            }));
    }

    private void OnRenameEditorKeyDown(object sender, KeyEventArgs e)
    {
        if (sender is not TextBox { DataContext: AssetItem item }) return;
        if (e.Key == Key.Escape)
        {
            item.IsRenaming = false;
            e.Handled = true;
            Tiles.Focus();
            FlushDeferredRefresh();
            return;
        }
        if (e.Key != Key.Enter) return;
        e.Handled = true;
        CommitRename(item);
    }

    private void OnRenameEditorLostFocus(object sender, KeyboardFocusChangedEventArgs e)
    {
        if (_renameCommitInProgress ||
            sender is not TextBox { DataContext: AssetItem item } ||
            !item.IsRenaming)
            return;
        CommitRename(item);
    }

    private async void CommitRename(AssetItem item)
    {
        if (_renameCommitInProgress || !item.IsRenaming || _assetDatabase == null)
            return;
        if (!CanStartAssetOperation("Rename asset")) return;
        _renameCommitInProgress = true;
        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        string originalPath = item.FullPath;
        AssetPathMutationStartingEventArgs? pathMutation = BeginAssetPathMutation(
            AssetPathMutationKind.Rename,
            new[] { originalPath });
        if (pathMutation == null)
        {
            _renameCommitInProgress = false;
            item.IsRenaming = false;
            return;
        }
        bool pathMutationSucceeded = false;
        string? renamedPath = null;
        try
        {
            renamedPath = await RunAssetOperationAsync(() =>
            {
                var workflow = new AssetManagementWorkflow(database);
                return workflow.Rename(
                    item.FullPath,
                    item.AssetId,
                    item.IsDirectory,
                    item.EditName);
            });
            PublishAssetPathsChanged(new AssetPathsChangedEventArgs(
                mappings: new[] { new AssetPathMapping(originalPath, renamedPath) }));
            pathMutationSucceeded = true;
            Log?.Invoke($"Renamed asset: {RelDisplay(renamedPath)}");
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            ReportAssetOperationFailure(FormatAssetOperationError(
                "名前を変更できませんでした", error));
        }
        finally
        {
            CompleteAssetPathMutation(pathMutation, pathMutationSucceeded);
            _renameCommitInProgress = false;
        }

        if (renamedPath == null)
        {
            QueueRenameFocus(item, selectAll: true);
            return;
        }
        item.IsRenaming = false;
        _refreshPendingWhileRenaming = false;
        if (generation != _projectRefreshGeneration ||
            !ReferenceEquals(database, _assetDatabase))
            return;
        _assetSnapshot = database.Snapshot();
        RefreshView();
        SelectAssets(new[] { renamedPath });
        Tiles.Focus();
    }

    private static T? FindVisualChild<T>(
        DependencyObject root,
        Func<T, bool> predicate)
        where T : DependencyObject
    {
        for (int index = 0; index < VisualTreeHelper.GetChildrenCount(root); index++)
        {
            DependencyObject child = VisualTreeHelper.GetChild(root, index);
            if (child is T match && predicate(match)) return match;
            T? nested = FindVisualChild(child, predicate);
            if (nested != null) return nested;
        }
        return null;
    }

    private static T? FindVisualAncestor<T>(
        DependencyObject? source,
        Func<T, bool> predicate)
        where T : DependencyObject
    {
        for (DependencyObject? current = source;
             current != null;
             current = VisualTreeHelper.GetParent(current))
        {
            if (current is T match && predicate(match)) return match;
        }
        return null;
    }

    private AssetItem? GetItemFromEventSource(DependencyObject? source) =>
        source == null
            ? null
            : (ItemsControl.ContainerFromElement(Tiles, source) as ListBoxItem)
                ?.DataContext as AssetItem;

    private void FlushDeferredRefresh()
    {
        if (!_refreshPendingWhileRenaming) return;
        _refreshPendingWhileRenaming = false;
        Refresh();
    }

    private void OnCtxOpen(object sender, RoutedEventArgs e)
    {
        ActivateSelectedAsset();
    }

    private void OnCtxCopyPath(object sender, RoutedEventArgs e)
    {
        if (!CanStartAssetOperation("Copy asset paths")) return;
        List<AssetItem> selection = SelectedAssets();
        if (selection.Count == 0) return;
        try
        {
            if (_assetDatabase == null) return;
            var workflow = new AssetManagementWorkflow(_assetDatabase);
            foreach (AssetItem item in selection)
                workflow.ValidateExternalPath(item.FullPath);
            Clipboard.SetText(string.Join(
                Environment.NewLine,
                selection.Select(item => item.FullPath)));
            Log?.Invoke(selection.Count == 1
                ? "Asset path copied: " + selection[0].FullPath
                : $"Copied {selection.Count} asset paths.");
        }
        catch (Exception error)
        {
            ReportAssetOperationFailure("パスをコピーできませんでした: " + error.Message);
        }
    }

    private void OnCtxReveal(object sender, RoutedEventArgs e)
    {
        if (!CanStartAssetOperation("Reveal assets")) return;
        List<AssetItem> selection = SelectedAssets();
        if (selection.Count == 0) return;
        try
        {
            if (_assetDatabase == null) return;
            var workflow = new AssetManagementWorkflow(_assetDatabase);
            foreach (AssetItem item in selection)
                workflow.ValidateExternalPath(item.FullPath);
            var start = new ProcessStartInfo
            {
                FileName = "explorer.exe",
                UseShellExecute = true,
            };
            if (selection.Count == 1)
                start.ArgumentList.Add("/select," + selection[0].FullPath);
            else
                start.ArgumentList.Add(_currentDir);
            Process.Start(start);
            if (selection.Count > 1)
            {
                Log?.Invoke(
                    $"Opened the containing folder for {selection.Count} selected assets.");
            }
        }
        catch (Exception error)
        {
            ReportAssetOperationFailure(
                "エクスプローラーで表示できませんでした: " + error.Message);
        }
    }

    private async void OnCtxDuplicate(object sender, RoutedEventArgs e)
    {
        List<AssetItem> selection = SelectedAssets();
        if (selection.Count == 0 || _assetDatabase == null) return;
        if (!CanStartAssetOperation("Duplicate assets")) return;
        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        string[] paths = selection.Select(static item => item.FullPath).ToArray();
        try
        {
            IReadOnlyList<string> created = await RunAssetOperationAsync(() =>
            {
                var workflow = new AssetManagementWorkflow(database);
                return workflow.Duplicate(paths);
            });
            if (generation != _projectRefreshGeneration ||
                !ReferenceEquals(database, _assetDatabase))
                return;
            _assetSnapshot = database.Snapshot();
            RefreshView();
            SelectAssets(created);
            Log?.Invoke(created.Count == 1
                ? $"Duplicated asset: {RelDisplay(created[0])}"
                : $"Duplicated {created.Count} assets.");
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            TryRefreshAfterOperationFailure();
            ReportAssetOperationFailure(FormatAssetOperationError(
                "アセットを複製できませんでした", error));
        }
    }

    private void OnCtxCopyAsset(object sender, RoutedEventArgs e) =>
        CaptureAssetClipboard(cut: false);

    private void OnCtxCutAsset(object sender, RoutedEventArgs e) =>
        CaptureAssetClipboard(cut: true);

    private void CaptureAssetClipboard(bool cut)
    {
        if (!CanStartAssetOperation(cut ? "Cut assets" : "Copy assets")) return;
        List<AssetItem> selection = SelectedAssets();
        if (selection.Count == 0 || _assetDatabase == null) return;
        try
        {
            var workflow = new AssetManagementWorkflow(_assetDatabase);
            string[] paths = selection
                .Select(item => workflow.ValidateExternalPath(item.FullPath))
                .ToArray();
            _assetClipboardPaths.Clear();
            _assetClipboardPaths.AddRange(paths);
            _assetClipboardCut = cut;
            Log?.Invoke(cut
                ? $"Cut {paths.Length} assets; navigate to a folder and paste to move."
                : $"Copied {paths.Length} assets; navigate to a folder and paste to duplicate.");
        }
        catch (Exception error)
        {
            ReportAssetOperationFailure(FormatAssetOperationError(
                cut ? "アセットを切り取れませんでした" : "アセットをコピーできませんでした",
                error));
        }
    }

    private async void OnCtxPasteAsset(object sender, RoutedEventArgs e)
    {
        if (_assetClipboardPaths.Count == 0 || _assetDatabase == null ||
            string.IsNullOrWhiteSpace(_currentDir))
            return;
        if (!CanStartAssetOperation("Paste assets")) return;
        bool moving = _assetClipboardCut;
        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        string destination = _currentDir;
        string[] sourcePaths = _assetClipboardPaths.ToArray();
        AssetPathMutationStartingEventArgs? pathMutation = moving
            ? BeginAssetPathMutation(AssetPathMutationKind.Move, sourcePaths)
            : null;
        if (moving && pathMutation == null) return;
        bool pathMutationSucceeded = false;
        AssetMoveResult? moveResult = null;
        try
        {
            IReadOnlyList<string> published = await RunAssetOperationAsync(() =>
            {
                var workflow = new AssetManagementWorkflow(database);
                if (!moving) return workflow.Duplicate(sourcePaths, destination);
                moveResult = workflow.MoveWithMappings(sourcePaths, destination);
                return moveResult.PublishedPaths;
            });
            if (moving && moveResult != null)
            {
                PublishAssetPathsChanged(new AssetPathsChangedEventArgs(
                    mappings: moveResult.Mappings.Select(static mapping =>
                        new AssetPathMapping(
                            mapping.OriginalPath,
                            mapping.DestinationPath))));
                pathMutationSucceeded = true;
            }
            if (generation != _projectRefreshGeneration ||
                !ReferenceEquals(database, _assetDatabase))
                return;
            if (moving)
            {
                _assetClipboardPaths.Clear();
                _assetClipboardCut = false;
            }
            _assetSnapshot = database.Snapshot();
            RefreshView();
            SelectAssets(published);
            Log?.Invoke(moving
                ? $"Moved {published.Count} assets."
                : $"Pasted {published.Count} asset copies.");
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            TryRefreshAfterOperationFailure();
            ReportAssetOperationFailure(FormatAssetOperationError(
                moving
                    ? "アセットを移動できませんでした"
                    : "アセットを貼り付けできませんでした",
                error));
        }
        finally
        {
            CompleteAssetPathMutation(pathMutation, pathMutationSucceeded);
        }
    }

    private async void OnCtxDelete(object sender, RoutedEventArgs e)
    {
        List<AssetItem> selection = SelectedAssets();
        if (selection.Count == 0 || _assetDatabase == null) return;
        if (!CanStartAssetOperation("Inspect asset delete")) return;
        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        string[] paths = selection.Select(static item => item.FullPath).ToArray();
        try
        {
            AssetDeleteInspection inspection = await RunAssetOperationAsync(() =>
            {
                var workflow = new AssetManagementWorkflow(database);
                return workflow.InspectDelete(paths);
            });
            if (generation != _projectRefreshGeneration ||
                !ReferenceEquals(database, _assetDatabase))
                return;
            if (!SelectionStillMatches(paths))
            {
                Log?.Invoke("Delete cancelled because the Asset View selection changed during inspection.");
                return;
            }
            if (!inspection.CanDelete)
            {
                throw new AssetOperationBlockedException(
                    "Referenced assets cannot be deleted safely.",
                    inspection.Blockers);
            }

            string itemNames = string.Join(
                "\n",
                paths.Take(8).Select(static path => "• " + Path.GetFileName(path)));
            if (paths.Length > 8)
                itemNames += $"\n• …ほか {paths.Length - 8} 件";
            string summary =
                $"{paths.Length} 個の選択項目を削除しますか？\n\n" +
                itemNames + "\n\n" +
                $"Assets: {inspection.AssetCount}\n" +
                $"Folders: {inspection.FolderCount}\n" +
                $"Size: {FormatSize(inspection.TotalBytes)}\n\n" +
                "この操作は元に戻せません。";
            if (MessageBox.Show(
                    Window.GetWindow(this),
                    summary,
                    "アセットを削除",
                    MessageBoxButton.OKCancel,
                    MessageBoxImage.Warning,
                    MessageBoxResult.Cancel) != MessageBoxResult.OK)
            {
                return;
            }

            if (!CanStartAssetOperation("Delete assets")) return;
            AssetPathMutationStartingEventArgs? pathMutation = BeginAssetPathMutation(
                AssetPathMutationKind.Delete,
                paths);
            if (pathMutation == null) return;
            bool pathMutationSucceeded = false;
            AssetDeleteResult deleted;
            try
            {
                deleted = await RunAssetOperationAsync(() =>
                {
                    var workflow = new AssetManagementWorkflow(database);
                    return workflow.Delete(paths);
                });
                PublishAssetPathsChanged(new AssetPathsChangedEventArgs(
                    deletedRoots: paths));
                pathMutationSucceeded = true;
            }
            finally
            {
                CompleteAssetPathMutation(pathMutation, pathMutationSucceeded);
            }
            if (generation != _projectRefreshGeneration ||
                !ReferenceEquals(database, _assetDatabase))
                return;
            _assetSnapshot = database.Snapshot();
            RefreshView();
            if (deleted.DeferredCleanupPath != null)
            {
                Log?.Invoke(
                    "Asset delete completed; quarantined data will be cleaned later: " +
                    deleted.DeferredCleanupPath);
            }
            Log?.Invoke(
                $"Deleted {deleted.AssetCount} assets and {deleted.FolderCount} folders.");
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            TryRefreshAfterOperationFailure();
            ReportAssetOperationFailure(FormatAssetOperationError(
                "アセットを削除できませんでした", error));
        }
    }

    private bool SelectionStillMatches(IReadOnlyCollection<string> expectedPaths)
    {
        string[] current = SelectedAssets()
            .Select(static item => Path.GetFullPath(item.FullPath))
            .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        string[] expected = expectedPaths
            .Select(Path.GetFullPath)
            .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        return current.SequenceEqual(expected, StringComparer.OrdinalIgnoreCase);
    }

    private void TryRefreshAfterOperationFailure()
    {
        Refresh();
    }

    private static string FormatAssetOperationError(string prefix, Exception error)
    {
        if (error is not AssetOperationBlockedException blocked ||
            blocked.Blockers.Count == 0)
        {
            return prefix + ": " + error.Message;
        }
        string details = string.Join(
            Environment.NewLine,
            blocked.Blockers.Take(8).Select(static blocker => "• " + blocker));
        if (blocked.Blockers.Count > 8)
            details += Environment.NewLine + $"• …ほか {blocked.Blockers.Count - 8} 件";
        return prefix + ": 参照中のアセットがあります。\n\n" + details;
    }

    private void ReportAssetOperationFailure(string message)
    {
        Log?.Invoke(message);
        MessageBox.Show(
            Window.GetWindow(this),
            message,
            "アセット操作",
            MessageBoxButton.OK,
            MessageBoxImage.Warning);
    }

    private void OnCtxReferences(object sender, RoutedEventArgs e)
    {
        if (Tiles.SelectedItem is not AssetItem item ||
            item.IsDirectory ||
            item.AssetId.Length == 0 ||
            _assetDatabase == null)
            return;

        var viewer = new AssetReferenceViewerWindow(_assetDatabase, item.AssetId)
        {
            Owner = Window.GetWindow(this),
        };
        viewer.Show();
    }

    private void OnCtxPlace(object sender, RoutedEventArgs e)
    {
        if (!CanStartAssetOperation("Place asset")) return;
        if (Tiles.SelectedItem is AssetItem item && !item.IsDirectory)
            AssetPlace?.Invoke(this, new AssetActivatedEventArgs(item.FullPath, item.Kind, item.AssetId));
    }

    private void OnCtxConvert(object sender, RoutedEventArgs e)
    {
        if (!CanStartAssetOperation("Convert asset")) return;
        if (Tiles.SelectedItem is AssetItem item && !item.IsDirectory)
            AssetConvert?.Invoke(this, new AssetActivatedEventArgs(item.FullPath, item.Kind, item.AssetId));
    }

    // ===== インポート (Assets/現在フォルダへコピー) =====
    private async void OnImport(object sender, RoutedEventArgs e)
    {
        if (_project == null) { Log?.Invoke("インポートにはプロジェクトが必要です。"); return; }
        if (_assetDatabase == null)
        {
            ReportAssetOperationFailure("アセット索引の準備完了後にインポートしてください。");
            return;
        }
        if (!CanStartAssetOperation("Import assets")) return;
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "アセットをインポート",
            Multiselect = true,
            Filter = "All assets|*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.wav;*.ogg;*.mp3;*.flac;*.fbx;*.gltf;*.glb;*.obj;*.txt;*.json|All files (*.*)|*.*",
        };
        Window? owner = Window.GetWindow(this);
        bool? accepted = owner != null
            ? dlg.ShowDialog(owner)
            : dlg.ShowDialog();
        if (accepted != true) return;
        if (!CanStartAssetOperation("Import assets")) return;
        AssetDatabase database = _assetDatabase;
        string destinationDirectory = _currentDir;
        string[] sources = dlg.FileNames.ToArray();
        int generation = _projectRefreshGeneration;
        try
        {
            ImportOperationResult operation = await RunAssetOperationAsync(() =>
            {
                var imported = new List<(string Source, string Destination)>();
                var failures = new List<string>();
                foreach (string source in sources)
                {
                    try
                    {
                        string destination = UniquePath(Path.Combine(
                            destinationDirectory,
                            Path.GetFileName(source)));
                        File.Copy(source, destination);
                        imported.Add((source, destination));
                    }
                    catch (Exception error)
                    {
                        failures.Add($"{Path.GetFileName(source)}: {error.Message}");
                    }
                }

                AssetDatabaseRefreshResult index = database.Refresh();
                foreach ((string source, string destination) in imported)
                {
                    if (!database.TryGetByPath(destination, out AssetRecord? record) ||
                        record == null)
                        continue;
                    try
                    {
                        database.UpdateImportMetadata(
                            record.AssetId,
                            source,
                            ImporterForKind(record.Kind),
                            importerVersion: 1);
                    }
                    catch (Exception error)
                    {
                        failures.Add(
                            $"{Path.GetFileName(destination)} metadata: {error.Message}");
                    }
                }
                return new ImportOperationResult(imported, failures, index);
            });

            if (generation != _projectRefreshGeneration ||
                !ReferenceEquals(database, _assetDatabase))
                return;
            _assetSnapshot = database.Snapshot();
            ReportIndexResult(operation.IndexResult);
            foreach (string failure in operation.Failures)
                Log?.Invoke("Import failed: " + failure);
            RefreshView();
            SelectAssets(operation.Imported.Select(static item => item.Destination));
            Log?.Invoke(
                $"{operation.Imported.Count} 個のアセットをインポートしました → " +
                RelDisplay(destinationDirectory));
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            TryRefreshAfterOperationFailure();
            ReportAssetOperationFailure("アセットをインポートできませんでした: " + error.Message);
        }
    }

    private sealed record ImportOperationResult(
        IReadOnlyList<(string Source, string Destination)> Imported,
        IReadOnlyList<string> Failures,
        AssetDatabaseRefreshResult IndexResult);

    // ===== ドラッグ (Asset View 内の整理 / ノード・インスペクタへの配置) =====
    private void OnTileMouseDown(object sender, MouseButtonEventArgs e)
    {
        _maybeDrag = false;
        _dragItem = null;
        if (_assetOperationInProgress || _assetOperationsSuspended ||
            _project == null || _assetDatabase == null)
            return;
        DependencyObject? source = e.OriginalSource as DependencyObject;
        if (FindVisualAncestor<TextBox>(
                source,
                static editor => Equals(editor.Tag, "AssetRenameEditor")) != null)
            return;
        _dragStart = e.GetPosition(this);
        _dragItem = GetItemFromEventSource(source);
        _maybeDrag = _dragItem != null;
    }

    private void OnTilePreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (!_maybeDrag || e.LeftButton != MouseButtonState.Pressed)
        {
            if (e.LeftButton != MouseButtonState.Pressed)
            {
                _maybeDrag = false;
                _dragItem = null;
            }
            return;
        }
        Point p = e.GetPosition(this);
        if (Math.Abs(p.X - _dragStart.X) < 5 && Math.Abs(p.Y - _dragStart.Y) < 5) return;
        AssetItem? item = _dragItem;
        _maybeDrag = false;
        _dragItem = null;
        if (item == null || !Items.Contains(item)) return;
        AssetItem[] dragged = Tiles.SelectedItems.Contains(item)
            ? Tiles.SelectedItems.Cast<AssetItem>().ToArray()
            : new[] { item };
        if (!TryCreateAssetBrowserDragPayload(dragged, out AssetBrowserDragPayload? payload) ||
            payload == null)
            return;

        var data = new DataObject();
        data.SetData(typeof(AssetBrowserDragPayload), payload);

        // Existing viewport/inspector drops remain byte-for-byte compatible for
        // file-only selections. Folders and mixed selections are deliberately
        // internal-only so they cannot be mistaken for placeable scene assets.
        if (payload.Entries.All(static entry => !entry.IsDirectory))
        {
            string[] paths = payload.Entries
                .Select(static entry => entry.FullPath)
                .ToArray();
            data.SetData("ASSET_PATH", paths[0]);
            data.SetData("ASSET_PATHS", paths);
            if (dragged[0].AssetId.Length != 0)
                data.SetData("ASSET_ID", dragged[0].AssetId);
            data.SetData(DataFormats.FileDrop, paths);
        }

        try
        {
            DragDrop.DoDragDrop(
                Tiles,
                data,
                // The actual Move/Copy choice is made by our post-drop menu.
                // Advertising Copy only preserves the old viewport/Explorer
                // contract and prevents an external target from moving files
                // out of the authoritative Assets root.
                DragDropEffects.Copy);
        }
        catch (Exception error)
        {
            Log?.Invoke("Asset drag failed: " + error.Message);
        }
        finally
        {
            ResetAssetDragState(closeActionMenu: false);
        }
    }

    private bool TryCreateAssetBrowserDragPayload(
        IReadOnlyList<AssetItem> dragged,
        out AssetBrowserDragPayload? payload)
    {
        payload = null;
        if (_assetOperationInProgress || _assetOperationsSuspended ||
            _project == null || _assetDatabase == null ||
            dragged.Count == 0)
            return false;

        var currentItems = new HashSet<AssetItem>(Items);
        if (dragged.Any(item => !currentItems.Contains(item)))
            return false;

        try
        {
            var workflow = new AssetManagementWorkflow(_assetDatabase);
            var entries = new AssetBrowserDragEntry[dragged.Count];
            for (int index = 0; index < dragged.Count; index++)
            {
                AssetItem item = dragged[index];
                string path = workflow.ValidateExternalPath(item.FullPath);
                bool physicalDirectory = Directory.Exists(path);
                if (physicalDirectory != item.IsDirectory ||
                    (!physicalDirectory && !File.Exists(path)))
                {
                    Log?.Invoke("Asset drag cancelled because the item changed on disk.");
                    return false;
                }
                entries[index] = new AssetBrowserDragEntry(path, physicalDirectory);
            }
            payload = new AssetBrowserDragPayload(
                _projectRefreshGeneration,
                _project.AssetsDir,
                entries);
            return true;
        }
        catch (Exception error)
        {
            Log?.Invoke("Asset drag cancelled: " + error.Message);
            return false;
        }
    }

    private void OnTileDragOver(object sender, DragEventArgs e)
    {
        e.Handled = true;
        if (!TryGetAssetBrowserDrop(
                e,
                out _,
                out AssetItem? target,
                out AssetBrowserDropPlan? plan) ||
            plan == null)
        {
            SetDropTarget(null);
            e.Effects = DragDropEffects.None;
            return;
        }

        SetDropTarget(target);
        e.Effects = DragDropEffects.Copy;
    }

    private void OnTileDragLeave(object sender, DragEventArgs e)
    {
        SetDropTarget(null);
    }

    private void OnTileDrop(object sender, DragEventArgs e)
    {
        e.Handled = true;
        bool valid = TryGetAssetBrowserDrop(
            e,
            out AssetBrowserDragPayload? payload,
            out _,
            out AssetBrowserDropPlan? plan);
        SetDropTarget(null);
        if (!valid || payload == null || plan == null)
        {
            e.Effects = DragDropEffects.None;
            return;
        }

        e.Effects = DragDropEffects.Copy;
        ShowAssetDropActionMenu(payload, plan);
    }

    private bool TryGetAssetBrowserDrop(
        DragEventArgs e,
        out AssetBrowserDragPayload? payload,
        out AssetItem? target,
        out AssetBrowserDropPlan? plan)
    {
        payload = null;
        target = GetItemFromEventSource(e.OriginalSource as DependencyObject);
        plan = null;
        if (_assetOperationInProgress || _assetOperationsSuspended ||
            _project == null || _assetDatabase == null)
            return false;
        try
        {
            if (!e.Data.GetDataPresent(typeof(AssetBrowserDragPayload)))
                return false;
            payload = e.Data.GetData(typeof(AssetBrowserDragPayload)) as AssetBrowserDragPayload;
            plan = AssetBrowserDropPolicy.Evaluate(
                payload,
                _projectRefreshGeneration,
                _project.AssetsDir,
                target?.FullPath ?? "",
                target is { IsDirectory: true } &&
                Items.Contains(target) &&
                Directory.Exists(target.FullPath));
            return plan.IsValid;
        }
        catch
        {
            payload = null;
            plan = null;
            return false;
        }
    }

    private void SetDropTarget(AssetItem? target)
    {
        if (ReferenceEquals(_dropTargetItem, target)) return;
        if (_dropTargetItem != null)
            _dropTargetItem.IsDropTarget = false;
        _dropTargetItem = target;
        if (_dropTargetItem != null)
            _dropTargetItem.IsDropTarget = true;
    }

    private void ResetAssetDragState(bool closeActionMenu)
    {
        _maybeDrag = false;
        _dragItem = null;
        SetDropTarget(null);
        if (!closeActionMenu || _dropActionMenu == null) return;
        ContextMenu menu = _dropActionMenu;
        _dropActionMenu = null;
        menu.IsOpen = false;
    }

    private void ShowAssetDropActionMenu(
        AssetBrowserDragPayload payload,
        AssetBrowserDropPlan plan)
    {
        ResetAssetDragState(closeActionMenu: true);
        var menu = new ContextMenu
        {
            Placement = PlacementMode.MousePoint,
            PlacementTarget = Tiles,
        };
        menu.Items.Add(new MenuItem
        {
            Header = $"{plan.SourcePaths.Count} 個の項目 → {RelDisplay(plan.DestinationDirectory)}",
            IsEnabled = false,
        });
        menu.Items.Add(new Separator());

        var move = new MenuItem
        {
            Header = "ここへ移動 (Move Here)",
            IsEnabled = plan.CanMove,
        };
        move.Click += async (_, _) =>
            await ExecuteAssetDropAsync(payload, plan, moveAssets: true);
        menu.Items.Add(move);

        var copy = new MenuItem
        {
            Header = "ここへコピー (Copy Here)",
            IsEnabled = plan.CanCopy,
        };
        copy.Click += async (_, _) =>
            await ExecuteAssetDropAsync(payload, plan, moveAssets: false);
        menu.Items.Add(copy);
        menu.Items.Add(new Separator());
        menu.Items.Add(new MenuItem { Header = "キャンセル" });
        menu.Closed += (_, _) =>
        {
            if (ReferenceEquals(_dropActionMenu, menu))
                _dropActionMenu = null;
        };
        _dropActionMenu = menu;
        menu.IsOpen = true;
    }

    private async Task ExecuteAssetDropAsync(
        AssetBrowserDragPayload payload,
        AssetBrowserDropPlan expectedPlan,
        bool moveAssets)
    {
        if (!CanStartAssetOperation(moveAssets ? "Move assets" : "Copy assets") ||
            _project == null || _assetDatabase == null)
            return;

        AssetBrowserDropPlan plan = AssetBrowserDropPolicy.Evaluate(
            payload,
            _projectRefreshGeneration,
            _project.AssetsDir,
            expectedPlan.DestinationDirectory,
            destinationIsDirectory: Directory.Exists(expectedPlan.DestinationDirectory));
        if (!plan.IsValid || (moveAssets ? !plan.CanMove : !plan.CanCopy))
        {
            Log?.Invoke("Asset drop cancelled: " + plan.RejectionReason);
            return;
        }

        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        string[] sourcePaths = plan.SourcePaths.ToArray();
        AssetPathMutationStartingEventArgs? pathMutation = moveAssets
            ? BeginAssetPathMutation(AssetPathMutationKind.Move, sourcePaths)
            : null;
        if (moveAssets && pathMutation == null) return;
        bool pathMutationSucceeded = false;
        AssetMoveResult? moveResult = null;
        try
        {
            IReadOnlyList<string> published = await RunAssetOperationAsync(() =>
            {
                var workflow = new AssetManagementWorkflow(database);
                if (!moveAssets)
                    return workflow.Duplicate(sourcePaths, plan.DestinationDirectory);
                moveResult = workflow.MoveWithMappings(
                    sourcePaths,
                    plan.DestinationDirectory);
                return moveResult.PublishedPaths;
            });
            if (moveAssets && moveResult != null)
            {
                PublishAssetPathsChanged(new AssetPathsChangedEventArgs(
                    mappings: moveResult.Mappings.Select(static mapping =>
                        new AssetPathMapping(
                            mapping.OriginalPath,
                            mapping.DestinationPath))));
                pathMutationSucceeded = true;
            }
            if (generation != _projectRefreshGeneration ||
                !ReferenceEquals(database, _assetDatabase))
                return;

            _assetSnapshot = database.Snapshot();
            RefreshView();
            SelectAssets(published);
            Log?.Invoke(moveAssets
                ? $"Moved {published.Count} assets to {RelDisplay(plan.DestinationDirectory)}."
                : $"Copied {published.Count} assets to {RelDisplay(plan.DestinationDirectory)}.");
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            TryRefreshAfterOperationFailure();
            ReportAssetOperationFailure(FormatAssetOperationError(
                moveAssets
                    ? "アセットを移動できませんでした"
                    : "アセットをコピーできませんでした",
                error));
        }
        finally
        {
            CompleteAssetPathMutation(pathMutation, pathMutationSucceeded);
        }
    }

    private void CancelImageLoads()
    {
        _thumbnailLoadCancellation?.Cancel();
        _thumbnailLoadCancellation?.Dispose();
        _thumbnailLoadCancellation = null;
        _previewLoadCancellation?.Cancel();
        _previewLoadCancellation?.Dispose();
        _previewLoadCancellation = null;
    }

    private void StartThumbnailLoading()
    {
        _thumbnailLoadCancellation?.Cancel();
        _thumbnailLoadCancellation?.Dispose();
        var cancellation = new CancellationTokenSource();
        _thumbnailLoadCancellation = cancellation;
        AssetItem[] candidates = Items
            .Where(static item => !item.IsDirectory && item.Kind is "image" or "material")
            .ToArray();
        if (candidates.Length == 0) return;
        _ = LoadThumbnailsAsync(candidates, cancellation.Token);
    }

    private async Task LoadThumbnailsAsync(
        IReadOnlyList<AssetItem> candidates,
        CancellationToken cancellationToken)
    {
        List<(AssetItem Item, ImageSource? Image)> results;
        bool enteredGate = false;
        try
        {
            await _imageLoadGate.WaitAsync(cancellationToken);
            enteredGate = true;
            cancellationToken.ThrowIfCancellationRequested();
            results = await Task.Run(() =>
            {
                var loaded = new List<(AssetItem, ImageSource?)>(candidates.Count);
                foreach (AssetItem item in candidates)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    loaded.Add((item, LoadCachedImage(item.FullPath, item.Kind, 64)));
                }
                return loaded;
            }, cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        finally
        {
            if (enteredGate) _imageLoadGate.Release();
        }

        if (cancellationToken.IsCancellationRequested) return;
        foreach ((AssetItem item, ImageSource? image) in results)
            item.Thumb = image;
    }

    private void StartPreviewLoading(AssetItem item)
    {
        _previewLoadCancellation?.Cancel();
        _previewLoadCancellation?.Dispose();
        var cancellation = new CancellationTokenSource();
        _previewLoadCancellation = cancellation;
        _ = LoadPreviewAsync(item, cancellation.Token);
    }

    private async Task LoadPreviewAsync(AssetItem item, CancellationToken cancellationToken)
    {
        ImageSource? image;
        bool enteredGate = false;
        try
        {
            await _imageLoadGate.WaitAsync(cancellationToken);
            enteredGate = true;
            cancellationToken.ThrowIfCancellationRequested();
            image = await Task.Run(
                () => LoadCachedImage(
                    item.FullPath,
                    item.Kind,
                    item.Kind == "image" ? 320 : 256),
                cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        finally
        {
            if (enteredGate) _imageLoadGate.Release();
        }
        if (cancellationToken.IsCancellationRequested ||
            Tiles.SelectedItems.Count != 1 ||
            !ReferenceEquals(Tiles.SelectedItem, item) ||
            image == null)
            return;

        PreviewImage.Source = image;
        PreviewImage.Visibility = Visibility.Visible;
        PreviewGlyphBox.Visibility = Visibility.Collapsed;
    }

    private ImageSource? LoadCachedImage(string path, string kind, int decodeWidth)
    {
        try
        {
            string fullPath = Path.GetFullPath(path);
            var info = new FileInfo(fullPath);
            info.Refresh();
            if (!info.Exists) return null;
            string key = $"{kind}|{decodeWidth}|{fullPath}";
            lock (_thumbnailCacheGate)
            {
                if (_thumbnailCache.TryGetValue(key, out ThumbnailCacheEntry? cached) &&
                    cached.Length == info.Length &&
                    cached.LastWriteTicks == info.LastWriteTimeUtc.Ticks)
                    return cached.Image;
            }

            ImageSource? image = kind == "material"
                ? TryMaterialPreview(fullPath, decodeWidth)
                : TryThumb(fullPath, decodeWidth);
            if (image != null)
            {
                lock (_thumbnailCacheGate)
                {
                    if (_thumbnailCache.Count >= 768) _thumbnailCache.Clear();
                    _thumbnailCache[key] = new ThumbnailCacheEntry(
                        info.Length,
                        info.LastWriteTimeUtc.Ticks,
                        image);
                }
            }
            return image;
        }
        catch
        {
            return null;
        }
    }

    private sealed record ThumbnailCacheEntry(
        long Length,
        long LastWriteTicks,
        ImageSource? Image);

    // ===== 分類・グリフ・サムネイル =====
    private static string ImporterForKind(string kind) => kind switch
    {
        "image" => "texture",
        "audio" => "audio",
        "mesh" => "mesh",
        "scene" => "scene",
        "material" => "material",
        "blueprint" => "blueprint",
        "prefab" => "prefab",
        _ => "passthrough",
    };

    private static string GlyphFor(string kind) => kind switch
    {
        "image" => "IMG", "audio" => "AUD", "mesh" => "MESH", "text" => "TXT",
        "scene" => "SCN", "project" => "PROJ", "material" => "MAT", "prefab" => "PREF",
        "blueprint" => "BP", _ => "FILE",
    };

    private static Brush BrushFor(string kind) => kind switch
    {
        "image" => MakeBrush(0x3C, 0x9A, 0xE8),
        "audio" => MakeBrush(0x4C, 0xB0, 0x6B),
        "mesh"  => MakeBrush(0xC2, 0x6A, 0xD6),
        "text"  => MakeBrush(0x8A, 0x93, 0xA0),
        "scene" => MakeBrush(0xD8, 0x8A, 0x3C),
        "project" => MakeBrush(0xC9, 0x55, 0x55),
        "material" => MakeBrush(0x5C, 0xC2, 0xC9),
        "blueprint" => MakeBrush(0x6B, 0xD0, 0x8A),
        "prefab" => MakeBrush(0xC2, 0x9A, 0x5A),
        _ => MakeBrush(0x60, 0x68, 0x74),
    };

    private static SolidColorBrush MakeBrush(byte r, byte g, byte b)
    {
        var br = new SolidColorBrush(Color.FromRgb(r, g, b));
        br.Freeze();
        return br;
    }

    // .acsmat のサムネイル (一覧=56)。
    private ImageSource? TryMaterialThumb(string path) => TryMaterialPreview(path, 56);

    // .acsmat を N×N にレンダリング (一覧サムネ=56 / プレビュー=大)。エンジンがあれば実シェーダ GPU、無ければ CPU。
    private ImageSource? TryMaterialPreview(string path, int N)
    {
        try
        {
            // 重要(安定性): GPU プレビュー (acs_editor_render_preview_material = preview_cl の Submit+WaitIdle) が
            // メインレンダーループと競合し、«操作不要で起動後数秒に約70%» エディタを間欠クラッシュさせていた
            // (実測: GPU 経路あり 0〜1/3 安定 → 切ると 4/4 安定)。これがメッシュプレビュー/SSAO/アセット選択の
            // 不安定の正体。よってサムネ/プレビューは «CPU 数式 (MaterialPreview)» で生成する (十分な品質・完全安定)。
            // GPU プレビュー再導入は «preview の submit をメインフレームと安全に直列化» してから (描画オーバーホール)。
            // CPU で生成:
            int kind = EngineInterop.acs_editor_material_kind(path);
            if (kind == 0)
            {
                var bc = new float[4] { 1, 1, 1, 1 };
                var em = new float[3] { 0, 0, 0 };
                var ab = new byte[260]; var nb = new byte[260];
                if (EngineInterop.acs_editor_material_load_pbr(path, bc, out float metallic, out float roughness,
                        em, out float emStr, out float normalStr, out float ao, ab, ab.Length, nb, nb.Length) == 0)
                    return null;
                return MaterialPreview.RenderPbr(bc, metallic, roughness, em, emStr, normalStr, ao, N);
            }
            var color = new float[4] { 1, 1, 1, 1 };
            var nameBuf = new byte[64];
            if (EngineInterop.acs_editor_material_load(path, out int effect, out float strength,
                    out float p0, out float p1, out float p2, color, out int animated, nameBuf, nameBuf.Length) == 0)
                return null;
            return MaterialPreview.Render(effect, strength, p0, p1, p2, color, animated != 0, N);
        }
        catch { return null; }   // DLL 未ロード等は glyph 表示にフォールバック
    }

    private static ImageSource GpuBmp(byte[] bgra, int n)
    {
        var b = BitmapSource.Create(n, n, 96, 96, PixelFormats.Bgra32, null, bgra, n * 4);
        b.Freeze();
        return b;
    }

    private static ImageSource? TryThumb(string path, int decodeW = 64)
    {
        try
        {
            using var stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete,
                bufferSize: 64 * 1024,
                FileOptions.SequentialScan);
            var bmp = new BitmapImage();
            bmp.BeginInit();
            bmp.CacheOption = BitmapCacheOption.OnLoad;   // 即デコードしてストリーム依存を断つ
            bmp.DecodePixelWidth = decodeW;               // サムネ=64 / プレビュー=大
            bmp.StreamSource = stream;
            bmp.EndInit();
            bmp.Freeze();
            return bmp;
        }
        catch { return null; }
    }

    // ===== パスユーティリティ =====
    private string RelDisplay(string fullDir)
    {
        if (_project == null) return fullDir;
        string root = _project.AssetsDir;
        if (PathEquals(fullDir, root)) return "Assets";
        if (fullDir.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            return "Assets" + fullDir.Substring(root.Length).Replace('\\', '/');
        return fullDir;
    }

    private static bool PathEquals(string a, string b) =>
        string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(a)),
                      Path.TrimEndingDirectorySeparator(Path.GetFullPath(b)),
                      StringComparison.OrdinalIgnoreCase);

    private static bool IsUnder(string candidate, string root)
    {
        string relative = Path.GetRelativePath(root, candidate);
        return relative != "." && !Path.IsPathRooted(relative) && relative != ".." &&
               !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
               !relative.StartsWith(".." + Path.AltDirectorySeparatorChar, StringComparison.Ordinal);
    }

    private static bool IsUnderOrEqual(string candidate, string root) =>
        PathEquals(candidate, root) || IsUnder(candidate, root);

    private static string UniquePath(string dst)
    {
        if (!File.Exists(dst)) return dst;
        string dir = Path.GetDirectoryName(dst)!;
        string name = Path.GetFileNameWithoutExtension(dst);
        string ext = Path.GetExtension(dst);
        for (int i = 1; i < 1000; ++i)
        {
            string cand = Path.Combine(dir, $"{name} ({i}){ext}");
            if (!File.Exists(cand)) return cand;
        }
        return dst;
    }
}
