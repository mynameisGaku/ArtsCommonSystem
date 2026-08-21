using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace AcsEditor;

internal enum EditorEngineStartupState
{
    WaitingForAttach,
    WarmingRenderer,
    FinalizingEditor,
    Ready,
    Failed,
    Closed,
}

internal readonly record struct EditorViewSwitchPlan(
    bool StartPlay,
    bool StopPlay,
    bool MutateEditorNavigationCamera);

internal static class EditorViewSwitchPolicy
{
    internal static EditorViewSwitchPlan Plan(bool gameView, int playState)
    {
        _ = gameView;
        _ = playState;
        // Scene/Game tabs choose a presentation camera only. Simulation
        // lifetime and the editor navigation pose belong to Play/Stop.
        return new EditorViewSwitchPlan(
            StartPlay: false,
            StopPlay: false,
            MutateEditorNavigationCamera: false);
    }
}

public partial class MainWindow : Window
{
    private EngineViewport? _viewport;
    private EditorEngineStartupState _engineStartupState =
        EditorEngineStartupState.WaitingForAttach;
    private bool _engineStartupInternalAccess;
    private int _engineStartupGeneration;
    private int _engineStartupCompletionStage;
    private readonly ProjectSettingsLoadGenerationGate
        _projectSettingsLoadGeneration = new();
    private bool _showProfilerAtStartup;
    private Project? _project;        // 開いているプロジェクト (null = プロジェクト無しの素の起動)
    private string? _currentScenePath;   // 現在のシーンファイル (Save 先)。プロジェクトの初期シーン等。
    private bool _building;           // ビルド実行中フラグ
    private System.Diagnostics.Process? _gameProcess;          // Run で起動したゲームプロセス
    private System.IO.FileSystemWatcher? _srcWatcher;          // Source/ 監視 (ホットリロード)
    private System.Windows.Threading.DispatcherTimer? _reloadTimer;  // 再ビルドのデバウンス
    private bool _hotReload;          // ホットリロード ON/OFF
    private bool _pendingReconfigure; // ソース追加/削除あり → 次ビルドで CMake 再 configure
    private int  _contextNodeId = -1; // 右クリック対象ノード (-1 = ルート/空白)
    private HierarchyDropAdorner? _dropAdorner;   // D&D 中のドロップ位置インジケータ
    private int  _dropMode = -1;      // 0=before, 1=after, 2=child, -1=root/none
    private int  _dropTargetId = -1;  // ドロップ対象ノード
    private int  _selectedId = -1;   // primary (active) ノード id。複数選択の集合は ABI 側が保持。
    private bool _populating;        // populate 中は編集ハンドラを無視
    private bool _refreshingMaterialBox; // MaterialBox 再構築中の SelectionChanged を抑止
    private bool _syncingSelection;  // 選択同期中は OnHierarchySelect の単一選択化を抑止
    private string? _clipboard;      // コピーした subtree のシリアライズ文字列 (2D)
    private bool    _hasClip3d;      // 3D クリップボードに内容があるか (Paste の CanExecute 用)
    private (float Yaw, float Pitch, float Distance,
             float TargetX, float TargetY, float TargetZ)? _startupCamera3D;
    private bool? _startupGridVisible;
    private Point _dragStart;        // Hierarchy ドラッグ開始座標 (しきい値判定用)
    private int  _dragNodeId = -1;   // ドラッグ中のノード id (-1 = ドラッグなし)
    private readonly List<MaterialEditorWindow> _materialEditorWindows = new();
    private readonly Dictionary<Guid, AssetDocumentMutationState> _assetDocumentMutations = new();
    private readonly AssetOperationLifecycleTracker _assetDocumentMutationLifecycles = new();
    private SceneEditingBlockState _sceneEditingBlock = null!;
    private int _sceneSourceSaveDepth;

    private sealed class AssetDocumentMutationState
    {
        internal AssetDocumentMutationState(AssetPathMutationStartingEventArgs operation) =>
            Operation = operation;

        internal AssetPathMutationStartingEventArgs Operation { get; }
        internal BlueprintWindow? SuspendedBlueprintWindow { get; set; }
        internal List<MaterialEditorWindow> SuspendedMaterialWindows { get; } = new();
        internal List<EditorDocument> SuspendedMaterialDocuments { get; } = new();
        internal SceneAssetPathMutationGuard? SceneMutation { get; set; }
        internal EditorDocument? SuspendedSceneDocument { get; set; }
        internal Task SceneAutosaveDrainTask { get; set; } = Task.CompletedTask;
        internal bool SceneAutosaveMaintenanceLockHeld { get; set; }
        internal bool ResumeSceneAutosave2D { get; set; }
        internal bool ResumeSceneAutosave3D { get; set; }
        internal bool CompletionStarted { get; set; }
        internal IDisposable? CompletionLifecycle { get; set; }
        internal IDisposable? SceneEditingBlockLease { get; set; }
        internal ProjectSceneReferenceMoveIntent? InitialSceneMoveIntent { get; set; }
        internal bool InitialSceneReferencesCommitted { get; set; }
    }

    private sealed class SceneSourceSaveScope : IDisposable
    {
        private MainWindow? _owner;
        private AssetMutationLock? _assetMutationLock;

        internal SceneSourceSaveScope(MainWindow owner) => _owner = owner;

        internal bool TryAcquireProjectAssetMutationLock(out string reason)
        {
            reason = "";
            MainWindow owner = _owner ??
                throw new ObjectDisposedException(nameof(SceneSourceSaveScope));
            if (_assetMutationLock != null || owner._project == null)
                return true;
            if (!owner.Dispatcher.CheckAccess())
            {
                reason = "scene source mutation lock must be acquired on the editor dispatcher";
                return false;
            }

            try
            {
                _assetMutationLock = AssetMutationLock.AcquireFailFast(
                    owner._project.AssetsDir,
                    "Save scene source");
                return true;
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    ArgumentException or InvalidDataException)
            {
                reason = error.Message;
                return false;
            }
        }

        public void Dispose()
        {
            MainWindow? owner =
                System.Threading.Interlocked.Exchange(ref _owner, null);
            if (owner == null) return;

            void ReleaseOnDispatcher()
            {
                try
                {
                    _assetMutationLock?.Dispose();
                    _assetMutationLock = null;
                }
                finally
                {
                    if (owner._sceneSourceSaveDepth <= 0)
                        throw new InvalidOperationException(
                            "Scene source save scope depth became unbalanced.");
                    owner._sceneSourceSaveDepth--;
                }
            }

            // AssetMutationLock deliberately uses a thread-owned Monitor for safe nesting. WPF
            // save awaits normally resume on this Dispatcher; this fallback also makes release
            // correct if a future await stops capturing the synchronization context.
            if (owner.Dispatcher.CheckAccess())
                ReleaseOnDispatcher();
            else
                owner.Dispatcher.Invoke(ReleaseOnDispatcher);
        }
    }

    private bool TryBeginSceneSourceSave(
        out SceneSourceSaveScope? scope,
        out string reason)
    {
        scope = null;
        reason = "";
        if (!Dispatcher.CheckAccess())
        {
            reason = "scene source saves must start on the editor dispatcher";
            return false;
        }
        foreach (AssetDocumentMutationState mutation in _assetDocumentMutations.Values)
        {
            if (mutation.SceneMutation == null) continue;
            reason = "the open scene source is being renamed or moved";
            return false;
        }

        _sceneSourceSaveDepth++;
        scope = new SceneSourceSaveScope(this);
        return true;
    }

    public MainWindow()
    {
        InitializeComponent();
        _sceneEditingBlock = new SceneEditingBlockState(_ => UpdateEditorInputEnabled());
        InitializeWindowInteraction();
        InitLog();              // ConsoleList をタグ付きログビューに束縛
        InitializeDockableToolPanels();
        Loaded += OnLoaded;
        Loaded += (_, _) => RestoreEditorLayout();
        Loaded += (_, _) => RestoreFloatingToolPanels();
        Loaded += (_, _) =>
        {
            if (_showProfilerAtStartup)
                ShowBottomTab("profiler");
        };
        InitializeInteractionHealthDiagnostics();

        // Inspector フィールドの編集 → エンジンへ反映 (Enter / フォーカス喪失で確定)。
        foreach (var tb in new[] { PosX, PosY, RotDeg, ScaleX, ScaleY })
        {
            tb.LostKeyboardFocus += (_, _) => ApplyInspector();
            tb.KeyDown += (_, e) => { if (e.Key == Key.Enter) { ApplyInspector(); Keyboard.ClearFocus(); } };
        }
        // Name 欄の編集 → リネーム。
        NameBox.LostKeyboardFocus += (_, _) => ApplyRename();
        NameBox.KeyDown += (_, e) => { if (e.Key == Key.Enter) { ApplyRename(); Keyboard.ClearFocus(); } };

        // 階層ツリーの Ctrl+click → 選択トグル (通常クリックは WPF 既定 → OnHierarchySelect)。
        HierarchyTree.PreviewMouseLeftButtonDown += OnHierarchyPreviewMouseDown;
        // 階層ツリーのドラッグ&ドロップで親子付け替え (acs_editor_node_reparent)。
        HierarchyTree.AllowDrop = true;
        HierarchyTree.PreviewMouseMove          += OnHierarchyPreviewMouseMove;
        HierarchyTree.PreviewMouseLeftButtonUp  += (_, _) => _dragNodeId = -1;   // ドラッグ未成立で離した
        HierarchyTree.DragOver                  += OnHierarchyDragOver;
        HierarchyTree.Drop                      += OnHierarchyDrop;
        HierarchyTree.DragLeave                 += (_, _) => ClearDropAdorner();
        HierarchyTree.DragEnter                 += OnHierarchyDragOver;

        // Display 数値欄: フォーカス喪失で適用 (Enter は ClearFocus → 同じく適用、二重発火なし)。
        foreach (var tb in new[] { DispLayer, DispBase, ColR, ColG, ColB, ColA })
        {
            tb.LostKeyboardFocus += (_, _) => ApplyDisplay();
            tb.KeyDown += (_, e) => { if (e.Key == Key.Enter) Keyboard.ClearFocus(); };
        }

        // ドラッグスクラブ: 数値欄を左右ドラッグで増減 (キー入力不要)。クリックは従来どおり編集。
        EnableScrub(PosX,   0.5,   ApplyInspector);
        EnableScrub(PosY,   0.5,   ApplyInspector);
        EnableScrub(RotDeg, 0.5,   ApplyInspector);
        EnableScrub(ScaleX, 0.01,  ApplyInspector);
        EnableScrub(ScaleY, 0.01,  ApplyInspector);
        EnableScrub(DispLayer, 0.1, ApplyDisplay, integer: true);
        EnableScrub(DispBase,  0.5, ApplyDisplay);
        EnableScrub(ColR, 0.005, ApplyDisplay);
        EnableScrub(ColG, 0.005, ApplyDisplay);
        EnableScrub(ColB, 0.005, ApplyDisplay);
        EnableScrub(ColA, 0.005, ApplyDisplay);

        // 階層ツリーのダブルクリックで選択ノードへカメラフォーカス。
        HierarchyTree.MouseDoubleClick += OnHierarchyDoubleClick;
        // 右クリックで対象ノードを確定 (コンテキストメニューの生成/削除の親/対象に使う)。
        HierarchyTree.PreviewMouseRightButtonDown += OnHierarchyRightDown;
        // ポリゴン描画中の Enter/Esc を拾う。+ Play 中はゲーム入力を DLL へフィードする。
        PreviewKeyDown += OnGlobalKeyDown;
        PreviewKeyUp   += OnGlobalKeyUp;
        Deactivated += (_, _) => ResetGameInput();

        // アセットブラウザ: ダブルクリックで編集し、ドラッグで明示的に割り当てる。
        AssetBrowser.AssetActivated += OnAssetActivated;
        AssetBrowser.AssetPlace += OnAssetPlace;   // 右クリック「シーンに配置」
        AssetBrowser.AssetConvert += OnAssetConvert;   // 右クリック「Blueprint に変換」
        AssetBrowser.Log += Log;
        AssetBrowser.AssetPathMutationStarting += OnAssetPathMutationStarting;
        AssetBrowser.AssetPathsChanged += OnAssetPathsChanged;
        AssetBrowser.AssetPathMutationCompleted += OnAssetPathMutationCompleted;

        // 終了時: ソース監視を止め、起動中のゲームプロセスを終了させる。
        Closed += (_, _) =>
        {
            ProfilerView.Stop();
            _engineStartupTimer?.Stop();
            _engineStartupTimer = null;
            _engineLogTimer?.Stop();
            _engineLogTimer = null;
            _engineLogDrainQueued = false;
            _engineStartupGeneration++;
            _projectSettingsLoadGeneration.Invalidate();
            _sceneLoadCancellation?.Cancel();
            _sceneLoadCancellation = null;
            _sceneLoadGeneration.Invalidate();
            _engineStartupInternalAccess = false;
            _engineStartupState = EditorEngineStartupState.Closed;
            _sceneStateTimer.Stop();
            StopAutosave();
            StopSourceWatch();
            if (_gameProcess != null && !_gameProcess.HasExited) { try { _gameProcess.Kill(); } catch { } }
        };
        Closing += OnEditorClosing;
        // Registered after the asynchronous close gate. The first close attempt
        // is cancelled while documents drain. The approved second attempt
        // returns auxiliary surfaces, then remains cancelled while recovery
        // workers stop; the final attempt bypasses these handlers. Tool panels
        // run first so a tool re-dock failure cannot occur after the Camera View
        // has already been closed.
        Closing += OnDockableToolPanelsOwnerClosing;
        Closing += OnCameraViewOwnerClosing;
    }

    /// <summary>プロジェクトを開いた状態で起動する。初期シーンは attach 後にロードする。</summary>
    public MainWindow(Project project) : this()
    {
        _project = project;
        Title = $"ACS Editor — {project.Name}";
        AssetBrowser.SetProject(project);   // Assets フォルダを走査・監視
    }

    /// <summary>
    /// Configure a deterministic, input-free startup camera for visual
    /// automation. Applied only after the project scene has finished loading.
    /// </summary>
    internal void SetStartupCamera3D(
        float yaw, float pitch, float distance,
        float targetX, float targetY, float targetZ)
    {
        _startupCamera3D =
            (yaw, pitch, distance, targetX, targetY, targetZ);
    }

    /// <summary>
    /// Configure grid visibility before the first rendered validation frame.
    /// This avoids UI automation (and therefore foreground activation) in
    /// unattended image-comparison runs.
    /// </summary>
    internal void SetStartupGridVisible(bool visible) =>
        _startupGridVisible = visible;

    /// <summary>Open the docked profiler after persisted layout restoration.</summary>
    internal void ShowProfilerAtStartup() => _showProfilerAtStartup = true;

    // 下部パネルのタブ切替 (Console / Build / Assets)。
    private void OnBottomTab(object sender, RoutedEventArgs e)
    {
        string tab = (sender as System.Windows.Controls.Primitives.ToggleButton)?.Tag as string ?? "console";
        _ = ExecuteToolPanelUserMutation(() =>
        {
            bool activated = TryShowBottomTool(tab, redockFloating: true);
            if (!activated)
            {
                Log(
                    $"Bottom tool '{tab}' could not be activated safely.",
                    "Editor",
                    LogLevel.Warn);
            }
            return activated;
        });
    }

    private void ShowBottomTab(string tab)
    {
        if (!TryShowBottomTool(tab, redockFloating: false))
        {
            Log(
                $"Bottom tool '{tab}' could not be activated safely.",
                "Editor",
                LogLevel.Warn);
        }
    }

    private bool TryShowBottomTool(
        string tab,
        bool redockFloating)
    {
        // The profiler has a graph, headline counters and pass details. Give it
        // enough room to be useful on first open while preserving any larger
        // height the user already chose with the splitter.
        string panelId = EditorWorkspaceStore.NormalizeBottomTab(tab);
        if (panelId == ToolPanelDockingContract.ProfilerPanelId)
            _bottomDockHeight = Math.Max(_bottomDockHeight, 300);
        DockableToolHost? host = GetToolPanelHost(panelId);
        if (host == null)
            return false;
        if (host.IsFloating && !redockFloating)
        {
            UpdateToolPanelPresentation(
                panelId,
                ToolPanelDockState.Floating);
            return true;
        }
        // Panel height stays user-controlled; switching to Assets must not race the hosted
        // swap-chain by forcing a resize from inside a selection event.
        return ActivateBottomTool(panelId);
    }

    // アセットがダブルクリック/ドラッグで起動された: 画像は選択ノードのスプライトに割り当てる。
    private void OnAssetPathMutationStarting(
        object? sender,
        AssetPathMutationStartingEventArgs e)
    {
        var scenePaths = new SceneAssetPathState(
            _currentScenePath,
            _scene2DPath,
            _scene3DDocumentPath);
        string? initialSceneDestination = null;
        bool sceneAffected = scenePaths.IsAffectedBy(e);
        if (scenePaths.ShouldVeto(e))
        {
            e.Cancel = true;
            e.CancellationReason =
                "The operation would delete or rewrite the open scene. " +
                "Open a different scene before retrying.";
            return;
        }
        if (_project != null && e.AffectsPath(_project.InitialScenePath))
        {
            if (_building)
            {
                e.Cancel = true;
                e.CancellationReason =
                    "The project's initial scene cannot be changed while Build, Run, or " +
                    "Package is in progress.";
                return;
            }
            if (e.Kind == AssetPathMutationKind.Delete)
            {
                e.Cancel = true;
                e.CancellationReason =
                    "The project's initial scene cannot be deleted. Assign another initial " +
                    "scene before retrying.";
                return;
            }
            if (e.Kind is AssetPathMutationKind.Rename or AssetPathMutationKind.Move)
            {
                try
                {
                    if (!e.TryRemapPath(
                            _project.InitialScenePath,
                            out initialSceneDestination) ||
                        SceneSourceFile.PathsEqual(
                            _project.InitialScenePath,
                            initialSceneDestination))
                    {
                        throw new InvalidDataException(
                            "The asset command did not publish a distinct proposed destination.");
                    }
                    ProjectManager.ValidateInitialSceneReferenceFollow(_project);
                }
                catch (Exception error)
                {
                    e.Cancel = true;
                    e.CancellationReason =
                        "The initial-scene references cannot safely follow this path change: " +
                        error.Message;
                    return;
                }
            }
        }

        BlueprintWindow? blueprintWindow = _bpWindow;
        string? blueprintPath = blueprintWindow?.Editor.CurrentPath;
        bool blueprintAffected = blueprintPath != null && e.AffectsPath(blueprintPath);
        var affectedMaterials = new List<MaterialEditorWindow>();
        foreach (MaterialEditorWindow materialWindow in _materialEditorWindows.ToArray())
        {
            string? materialPath = materialWindow.CurrentAssetPath;
            if (materialPath != null && e.AffectsPath(materialPath))
                affectedMaterials.Add(materialWindow);
        }

        if (e.Kind is AssetPathMutationKind.Delete or AssetPathMutationKind.ContentRewrite &&
            (blueprintAffected || affectedMaterials.Count != 0))
        {
            e.Cancel = true;
            e.CancellationReason =
                "変更対象が Blueprint または Material Editor で開かれています。" +
                "変更を保存して対象エディタを閉じてから、操作を再実行してください。";
            return;
        }

        bool suspendOpenScene =
            sceneAffected &&
            e.Kind is AssetPathMutationKind.Rename or AssetPathMutationKind.Move;
        if (suspendOpenScene && _sceneSourceSaveDepth != 0)
        {
            e.Cancel = true;
            e.CancellationReason =
                "The open scene is still being saved. Wait for the save to finish before " +
                "renaming or moving its source.";
            return;
        }
        if (suspendOpenScene && _autosaveStopping)
        {
            e.Cancel = true;
            e.CancellationReason =
                "The editor is stopping scene autosave. Retry the path change after the " +
                "current editor operation finishes.";
            return;
        }

        bool autosaveMaintenanceLockHeld = false;
        if (suspendOpenScene)
        {
            // A save/recovery cleanup can also suppress and resume the generation gates. Holding
            // its serialization lock for the whole asset mutation prevents another owner from
            // resuming autosave while the old scene path is still in flight.
            autosaveMaintenanceLockHeld = _autosaveMaintenanceLock.Wait(0);
            if (!autosaveMaintenanceLockHeld)
            {
                e.Cancel = true;
                e.CancellationReason =
                    "Scene autosave maintenance is still running. Retry the path change when it " +
                    "finishes.";
                return;
            }
        }

        var state = new AssetDocumentMutationState(e);
        state.SceneAutosaveMaintenanceLockHeld = autosaveMaintenanceLockHeld;
        if (suspendOpenScene)
            state.SceneMutation = new SceneAssetPathMutationGuard(scenePaths, e);
        try
        {
            state.CompletionLifecycle = _assetDocumentMutationLifecycles.Enter();
            _assetDocumentMutations[e.OperationId] = state;
            if (_project != null && initialSceneDestination != null)
            {
                state.InitialSceneMoveIntent =
                    ProjectManager.PrepareInitialScenePathFollow(
                        _project,
                        e.OperationId,
                        initialSceneDestination);
            }
            if (suspendOpenScene)
            {
                // Commit any already-focused Inspector edit before the document is suspended.
                // The input block then prevents every later scene interaction until path and
                // autosave reconciliation have both completed.
                Keyboard.ClearFocus();
                state.SceneEditingBlockLease = _sceneEditingBlock.Enter();
                if (_documentHostInitialized &&
                    _documentHost.TryGet(SceneDocumentId(), out EditorDocument sceneDocument))
                {
                    sceneDocument.Suspend(synchronize: true);
                    state.SuspendedSceneDocument = sceneDocument;
                }

                // InvalidateAndWaitAsync marks each gate suppressed before returning its Task.
                // The asset filesystem operation may therefore continue immediately, while its
                // completion keeps the scene suspended until any old worker has drained.
                state.ResumeSceneAutosave2D = !_autosave2D.Gate.IsSuppressed;
                state.ResumeSceneAutosave3D = !_autosave3D.Gate.IsSuppressed;
                state.SceneAutosaveDrainTask = Task.WhenAll(
                    _autosave2D.Gate.InvalidateAndWaitAsync(),
                    _autosave3D.Gate.InvalidateAndWaitAsync());
            }
            if (blueprintAffected && blueprintWindow != null && blueprintWindow.IsEnabled &&
                blueprintWindow.Editor.SuspendForAssetPathMutation())
            {
                blueprintWindow.IsEnabled = false;
                state.SuspendedBlueprintWindow = blueprintWindow;
            }
            foreach (MaterialEditorWindow materialWindow in affectedMaterials)
            {
                if (materialWindow.SuspendForAssetPathMutation())
                {
                    try
                    {
                        SuspendHostedMaterialDocument(materialWindow, state);
                    }
                    catch
                    {
                        materialWindow.ResumeAfterAssetPathMutation();
                        throw;
                    }
                    state.SuspendedMaterialWindows.Add(materialWindow);
                }
            }
        }
        catch (Exception error)
        {
            e.Cancel = true;
            e.CancellationReason =
                "The open document state could not be suspended safely: " + error.Message;
            if (!_assetDocumentMutations.ContainsKey(e.OperationId) &&
                autosaveMaintenanceLockHeld)
            {
                _autosaveMaintenanceLock.Release();
                state.SceneAutosaveMaintenanceLockHeld = false;
            }
            if (!_assetDocumentMutations.ContainsKey(e.OperationId))
            {
                try
                {
                    try { state.SceneEditingBlockLease?.Dispose(); }
                    catch (Exception resumeError)
                    {
                        try
                        {
                            Log(
                                "Scene editing input could not be restored after mutation " +
                                "preflight failed: " + resumeError.Message,
                                "Asset",
                                LogLevel.Error);
                        }
                        catch { }
                    }
                    state.SceneEditingBlockLease = null;
                }
                finally
                {
                    state.CompletionLifecycle?.Dispose();
                    state.CompletionLifecycle = null;
                }
            }
        }
    }

    private void OnAssetPathsChanged(object? sender, AssetPathsChangedEventArgs e)
    {
        void LogDeliveryFailure(string message)
        {
            // Diagnostics must not prevent the remaining open documents from being
            // rebound or detached after the filesystem mutation has committed.
            try { Log(message, "Asset", LogLevel.Error); }
            catch { }
        }

        int updatedEditors = 0;
        if (_project != null &&
            e.TryRemapPath(_project.InitialScenePath, out string remappedInitialScene))
        {
            try
            {
                ProjectSceneReferenceUpdate update =
                    ProjectManager.FollowInitialScenePath(
                        _project,
                        remappedInitialScene);
                if (!string.Equals(
                        update.PreviousReference,
                        update.CurrentReference,
                        StringComparison.OrdinalIgnoreCase))
                {
                    // The durable manifest and INI transaction commits before the native
                    // settings object is refreshed. A later Build/Run therefore cannot observe
                    // mismatched persistent startup-scene references.
                    if (Engine != IntPtr.Zero)
                        ApplyPersistedInitialSceneReference(
                            update.CurrentReference,
                            update.DurableSettingsSource);
                    Log(
                        $"Initial scene references followed asset path: " +
                        $"{update.PreviousReference} -> {update.CurrentReference}");
                }
                foreach (AssetDocumentMutationState mutation in
                         _assetDocumentMutations.Values)
                {
                    ProjectSceneReferenceMoveIntent? intent =
                        mutation.InitialSceneMoveIntent;
                    if (intent == null) continue;
                    string proposed = Path.Combine(
                        _project.RootDir,
                        intent.DestinationReference.Replace(
                            '/',
                            Path.DirectorySeparatorChar));
                    if (!SceneSourceFile.PathsEqual(proposed, remappedInitialScene))
                        continue;
                    mutation.InitialSceneReferencesCommitted = true;
                    break;
                }
            }
            catch (Exception error)
            {
                // The asset move is already committed. ProjectManager leaves both persistent
                // references at their old value (or reports incomplete rollback), while
                // Build/Run's existing mismatch/missing-source gates remain fail-closed.
                LogDeliveryFailure(
                    "The asset path changed, but its startup-scene references could not be " +
                    "updated safely. Build/Run is blocked until they are repaired. " +
                    error.Message);
            }
        }
        else if (_project != null && e.IsDeletedPath(_project.InitialScenePath))
        {
            LogDeliveryFailure(
                "The configured initial scene was deleted without passing mutation preflight. " +
                "Build/Run is blocked until another initial scene is assigned.");
        }

        var previousScenePaths = new SceneAssetPathState(
            _currentScenePath,
            _scene2DPath,
            _scene3DDocumentPath);
        SceneAssetPathMutationGuard? activeSceneMutation = null;
        foreach (AssetDocumentMutationState mutation in _assetDocumentMutations.Values)
        {
            if (mutation.SceneMutation is not { IsActive: true } candidate)
                continue;
            activeSceneMutation = candidate;
            break;
        }
        bool sceneRemapped;
        bool sceneDetached;
        SceneAssetPathState updatedScenePaths = activeSceneMutation != null
            ? activeSceneMutation.Publish(
                e,
                out sceneRemapped,
                out sceneDetached)
            : previousScenePaths.Apply(
                e,
                out sceneRemapped,
                out sceneDetached);
        if (sceneRemapped || sceneDetached)
        {
            // Commit all compatibility aliases together. Save, autosave and the managed document
            // host must never retain a mixture of the pre-move and post-move scene identities.
            ApplySceneAssetPathState(updatedScenePaths);
            if (sceneRemapped)
                updatedEditors++;
            if (sceneDetached)
            {
                LogDeliveryFailure(
                    "An open scene source was deleted without passing mutation preflight. " +
                    "Its save path was detached to prevent a stale-path save.");
            }
        }

        BlueprintEditor? blueprintEditor = _bpWindow?.Editor;
        string? blueprintPath = blueprintEditor?.CurrentPath;
        if (blueprintEditor != null && blueprintPath != null)
        {
            try
            {
                if (e.AffectsPath(blueprintPath))
                {
                    if (!blueprintEditor.ApplyAssetPathsChanged(e))
                        throw new InvalidOperationException(
                            "The affected Blueprint did not accept its new asset path.");
                    updatedEditors++;
                }
            }
            catch (Exception error)
            {
                blueprintEditor.DetachFromAssetPath(
                    "Blueprint path update failed; detached to require Save As.");
                LogDeliveryFailure(
                    "Blueprint path update failed; the editor was detached to prevent " +
                    $"stale-path saves. {error.Message}");
            }
        }
        foreach (MaterialEditorWindow materialWindow in _materialEditorWindows.ToArray())
        {
            string? materialPath = materialWindow.CurrentAssetPath;
            if (materialPath == null) continue;
            try
            {
                if (!e.AffectsPath(materialPath)) continue;
                if (!materialWindow.ApplyAssetPathsChanged(e))
                    throw new InvalidOperationException(
                        "The affected Material did not accept its new asset path.");
                RefreshHostedMaterialPresentation(materialWindow);
                updatedEditors++;
            }
            catch (Exception error)
            {
                materialWindow.DetachFromAssetPath(
                    "Asset path update failed - editor detached");
                LogDeliveryFailure(
                    "Material path update failed; the editor was closed to prevent " +
                    $"stale-path saves. {error.Message}");
            }
        }
        if (updatedEditors != 0)
            Log($"Updated {updatedEditors} open asset editor path(s).");
    }

    private void ApplySceneAssetPathState(SceneAssetPathState state)
    {
        _currentScenePath = state.CurrentPath;
        _scene2DPath = state.TwoDPath;
        _scene3DDocumentPath = state.ThreeDPath;
        UpdateSceneName();
        if (_documentHostInitialized)
            EnsureSceneDocumentRegistered(_view3d);
    }

    private async void OnAssetPathMutationCompleted(
        object? sender,
        AssetPathMutationCompletedEventArgs e)
    {
        if (!_assetDocumentMutations.TryGetValue(
                e.Operation.OperationId,
                out AssetDocumentMutationState? state) ||
            state.CompletionStarted)
        {
            return;
        }
        state.CompletionStarted = true;

        try
        {
            if (state.SceneMutation != null)
            {
                try
                {
                    await state.SceneAutosaveDrainTask;
                }
                catch (Exception error)
                {
                    // The generation gates remain suppressed even when an old worker faults.
                    // Resume in finally, but surface the failure because recovery durability for
                    // that old generation could not be proven.
                    Log(
                        "Scene autosave drain failed during asset path mutation: " +
                        error.Message,
                        "Asset",
                        LogLevel.Error);
                }

                SceneAssetPathState completedPaths = state.SceneMutation.Complete(
                    e.Succeeded,
                    out bool detachedForSafety);
                var livePaths = new SceneAssetPathState(
                    _currentScenePath,
                    _scene2DPath,
                    _scene3DDocumentPath);
                if (livePaths != completedPaths)
                    ApplySceneAssetPathState(completedPaths);
                if (detachedForSafety)
                {
                    Log(
                        "The scene asset operation reported success without publishing its " +
                        "destination. The old save path was detached to prevent it from being " +
                        "recreated; use Save As after verifying the asset.",
                        "Asset",
                        LogLevel.Error);
                }
            }
        }
        catch (Exception error)
        {
            Log(
                "Open scene path mutation completion failed: " + error.Message,
                "Asset",
                LogLevel.Error);
        }
        finally
        {
            try
            {
                if (state.InitialSceneMoveIntent != null && _project != null)
                {
                    ProjectSceneReferenceRecoveryResult settlement =
                        ProjectManager.SettleInitialScenePathFollow(
                            _project,
                            state.InitialSceneMoveIntent,
                            e.Succeeded,
                            state.InitialSceneReferencesCommitted);
                    state.InitialSceneMoveIntent = null;
                    if (settlement.Status ==
                        ProjectSceneReferenceRecoveryStatus.Deferred)
                    {
                        Log(
                            "Initial-scene move recovery remains pending: " +
                            settlement.Message,
                            "Asset",
                            LogLevel.Error);
                    }
                }
                if (state.SuspendedSceneDocument != null)
                {
                    try
                    {
                        state.SuspendedSceneDocument.Resume(
                            acceptCurrentWithoutTransaction: true);
                    }
                    catch (Exception error)
                    {
                        Log(
                            "Open scene document resume failed: " + error.Message,
                            "Asset",
                            LogLevel.Error);
                    }
                }

                if (state.SceneAutosaveMaintenanceLockHeld)
                {
                    // A new identity must not inherit the hash/debounce bookkeeping of the old path.
                    _autosave2D.ResetState();
                    _autosave3D.ResetState();
                    if (!_autosaveStopping)
                    {
                        if (state.ResumeSceneAutosave2D)
                        {
                            try { _autosave2D.Gate.Resume(); }
                            catch (Exception error)
                            {
                                Log(
                                    "Scene autosave compatibility-channel resume failed " +
                                    "(legacy orthographic serializer): " + error.Message,
                                    "Asset",
                                    LogLevel.Error);
                            }
                        }
                        if (state.ResumeSceneAutosave3D)
                        {
                            try { _autosave3D.Gate.Resume(); }
                            catch (Exception error)
                            {
                                Log(
                                    "Scene autosave compatibility-channel resume failed " +
                                    "(legacy spatial serializer): " + error.Message,
                                    "Asset",
                                    LogLevel.Error);
                            }
                        }
                    }
                    try { _autosaveMaintenanceLock.Release(); }
                    catch (Exception error)
                    {
                        Log(
                            "Scene autosave maintenance lock release failed: " + error.Message,
                            "Asset",
                            LogLevel.Error);
                    }
                    state.SceneAutosaveMaintenanceLockHeld = false;
                }

                if (state.SuspendedBlueprintWindow != null)
                {
                    try
                    {
                        BlueprintWindow window = state.SuspendedBlueprintWindow;
                        window.Editor.ResumeAfterAssetPathMutation();
                        if (window.IsLoaded)
                            window.IsEnabled = true;
                    }
                    catch (Exception error)
                    {
                        Log(
                            "Blueprint editor resume failed: " + error.Message,
                            "Asset",
                            LogLevel.Error);
                    }
                }
                foreach (MaterialEditorWindow materialWindow in state.SuspendedMaterialWindows)
                {
                    try { materialWindow.ResumeAfterAssetPathMutation(); }
                    catch (Exception error)
                    {
                        try
                        {
                            Log(
                                "Material editor window resume failed: " +
                                error.Message,
                                "Asset",
                                LogLevel.Error);
                        }
                        catch { }
                    }
                    try { ResumeHostedMaterialDocument(materialWindow, state); }
                    catch (Exception error)
                    {
                        try
                        {
                            Log(
                                "Hosted material document resume failed: " +
                                error.Message,
                                "Asset",
                                LogLevel.Error);
                        }
                        catch { }
                    }
                }
            }
            finally
            {
                try
                {
                    _assetDocumentMutations.Remove(e.Operation.OperationId);
                }
                finally
                {
                    try
                    {
                        try { state.SceneEditingBlockLease?.Dispose(); }
                        catch (Exception resumeError)
                        {
                            try
                            {
                                Log(
                                    "Scene editing input could not be restored after the asset " +
                                    "mutation: " + resumeError.Message,
                                    "Asset",
                                    LogLevel.Error);
                            }
                            catch { }
                        }
                        state.SceneEditingBlockLease = null;
                    }
                    finally
                    {
                        state.CompletionLifecycle?.Dispose();
                        state.CompletionLifecycle = null;
                    }
                }
            }
        }
    }

    private Task WaitForAssetDocumentMutationsAsync() =>
        _assetDocumentMutationLifecycles.WaitForDrainAsync();

    private async void OnAssetActivated(object? sender, AssetActivatedEventArgs e)
    {
        // Blueprint and Material editors are managed document surfaces and remain available while
        // the native scene engine is still starting. A new Content Browser asset must therefore
        // never become temporarily unopenable just because the viewport has not attached yet.
        if (e.Kind == "blueprint")
        {
            foreach (AssetDocumentMutationState mutation in _assetDocumentMutations.Values)
            {
                if (!mutation.Operation.AffectsPath(e.FullPath)) continue;
                Log("Blueprint Editor cannot open this asset while its path is changing. Retry when the Asset View operation finishes.");
                return;
            }
            OnBlueprintTab(this, new RoutedEventArgs());
            BlueprintHost.LoadFromFile(e.FullPath);
            Log($"Blueprint ← {System.IO.Path.GetFileName(e.FullPath)}");
            return;
        }
        if (e.Kind == "material")
        {
            OpenMaterialEditor(e.FullPath, e.AssetId);
            return;
        }
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        switch (e.Kind)
        {
            case "image":
                if (_selectedId < 0) { Log("画像を割り当てるノードを先に選択してください。"); return; }
                if (EngineInterop.acs_editor_node_set_sprite(Engine, _selectedId, e.FullPath) != 0)
                {
                    RefreshSpriteLabel(_selectedId);
                    RecordSceneDocumentChange("Assign Sprite");
                    Log($"Sprite ← {System.IO.Path.GetFileName(e.FullPath)} (node {_selectedId})");
                }
                else Log("スプライト割当に失敗: " + e.FullPath);
                break;
            case "scene":
                try
                {
                    await OpenScenePathAsync(e.FullPath);
                }
                catch (Exception error)
                {
                    // Routed/event callbacks are async void. Keep every fault
                    // from the shared scene-open task inside the dispatcher
                    // boundary instead of escalating it as an unhandled WPF
                    // exception.
                    Log(
                        "Open failed: " + error.Message,
                        "Scene",
                        LogLevel.Error);
                }
                break;
            case "prefab":
                InstantiatePrefab(e.FullPath, _selectedId);   // 選択ノード配下へ (無ければ root)
                break;
            default:
                Log($"{e.Kind}: {System.IO.Path.GetFileName(e.FullPath)}");
                break;
        }
    }

    // 選択ノード (3D/2D) へ .acsmat を割り当てる (エディタは開かない)。ドロップ・ダブルクリック共用。
    private void AssignMaterialToSelection(string matPath)
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        if (_view3d)
        {
            int s3 = EngineInterop.acs_editor_selected3d(Engine);
            if (s3 >= 0)
            {
                if (MaterialAssetWorkflow.SamePath(
                        EngineInterop.NodeMaterial3D(Engine, s3),
                        matPath))
                {
                    return;
                }

                if (EngineInterop.acs_editor_node3d_set_material(Engine, s3, matPath) == 0)
                {
                    Populate3DInspector(s3); // native state is authoritative after failure.
                    Log($"Material assignment failed (3D node {s3}): {matPath}");
                    return;
                }

                Populate3DInspector(s3);   // インスペクタの .acsmat ドロップダウンを更新
                RecordSceneDocumentChange("Assign Material");
                Log($"Material ← {System.IO.Path.GetFileName(matPath)} (3D node {s3})");
            }
        }
        else if (_selectedId >= 0)
        {
            int id = _selectedId;
            if (MaterialAssetWorkflow.SamePath(
                    EngineInterop.NodeMaterial(Engine, id),
                    matPath))
            {
                return;
            }

            if (EngineInterop.acs_editor_node_set_material(Engine, id, matPath) == 0)
            {
                RefreshMaterialBox(id); // rollback any optimistic Inspector selection.
                Log($"Material assignment failed (node {id}): {matPath}");
                return;
            }

            RefreshMaterialBox(id);
            RecordSceneDocumentChange("Assign Material");
            Log($"Material ← {System.IO.Path.GetFileName(matPath)} (node {id})");
        }
    }

    // アセットがビューポートへドロップされた (アセットブラウザ or Explorer)。ドロップ点のノードを
    // pick して選択し、マテリアル以外は種別ごとの既存ロジックへ委譲する:
    //   .acsmat → ドロップ先ノードへマテリアル割当 / 画像 → スプライト割当 / prefab・bp → 実体化。
    private void OnViewportAssetDropped(string path, int x, int y)
    {
        if (Engine == IntPtr.Zero ||
            IsSceneEditingBlocked ||
            string.IsNullOrEmpty(path))
        {
            return;
        }
        try
        {
            int hit = _view3d ? EngineInterop.acs_editor_pick3d(Engine, x, y)
                              : EngineInterop.acs_editor_pick(Engine, x, y);
            if (hit >= 0) OnViewportPicked(hit);   // ドロップ点のノードを選択
        }
        catch { /* pick 失敗時は選択ノードへフォールバック */ }
        string kind = ClassifyDroppedKind(path);
        if (kind == "material") AssignMaterialToSelection(path);   // ドロップは «割当» のみ (エディタは開かない)
        else OnAssetActivated(this, new AssetActivatedEventArgs(path, kind));
    }

    private static string ClassifyDroppedKind(string path)
    {
        string ext = System.IO.Path.GetExtension(path).ToLowerInvariant();
        return ext switch
        {
            ".png" or ".jpg" or ".jpeg" or ".bmp" or ".tga" or ".dds" or ".gif" => "image",
            ".acsmat" => "material",
            ".acscene" => "scene",
            ".acsprefab" => "prefab",
            ".acsbp" => "blueprint",
            _ => "file",
        };
    }

    // 数値 TextBox を「ドラッグでスクラブ」可能にする。未フォーカス時の左ドラッグで値を
    // step×移動px ぶん増減し、ドラッグせず離したら通常のクリック (フォーカス→全選択で編集) 扱い。
    private void EnableScrub(TextBox tb, double step, Action apply, bool integer = false)
    {
        bool armed = false, scrubbing = false;
        Point start = default; double startVal = 0;
        IDisposable? documentTransaction = null;

        void CompleteScrub()
        {
            Mouse.OverrideCursor = null;
            if (!scrubbing) return;
            scrubbing = false;
            if (Engine != IntPtr.Zero)
                EngineInterop.acs_editor_end_continuous(Engine);
            documentTransaction?.Dispose();
            documentTransaction = null;
        }

        tb.PreviewMouseLeftButtonDown += (_, e) =>
        {
            if (tb.IsKeyboardFocused || !tb.IsEnabled) return;   // 編集中は通常のキャレット操作
            armed = true; scrubbing = false;
            start = e.GetPosition(tb);
            startVal = ParseF(tb.Text, 0f);
            tb.CaptureMouse();
            e.Handled = true;                                    // 既定のフォーカス/キャレットを抑止
        };
        tb.PreviewMouseMove += (_, e) =>
        {
            if (!armed) return;
            double dx = e.GetPosition(tb).X - start.X;
            if (!scrubbing && Math.Abs(dx) < 3) return;          // しきい値未満は click
            if (!scrubbing)
            {
                scrubbing = true;
                documentTransaction = BeginSceneDocumentTransaction(
                    $"Edit {tb.Name}",
                    mergeKey: $"inspector.scrub.{tb.Name}",
                    mergeWindow: TimeSpan.FromSeconds(1),
                    nodeId: _selectedId);
                if (Engine != IntPtr.Zero) EngineInterop.acs_editor_begin_continuous(Engine);   // ドラッグ全体を 1 undo に束ねる
                Mouse.OverrideCursor = Cursors.SizeWE;
            }
            double v = startVal + dx * step;
            tb.Text = integer ? Math.Round(v).ToString(CultureInfo.InvariantCulture)
                              : v.ToString("0.###", CultureInfo.InvariantCulture);
            tb.CaretIndex = tb.Text.Length;
            apply();
        };
        tb.PreviewMouseLeftButtonUp += (_, e) =>
        {
            if (!armed) return;
            bool wasScrubbing = scrubbing;
            armed = false;
            if (tb.IsMouseCaptured) tb.ReleaseMouseCapture();
            CompleteScrub();
            if (wasScrubbing)
            {
                e.Handled = true;                                      // ドラッグだった → click 扱いしない
            }
            else { tb.Focus(); tb.SelectAll(); }                       // クリックだった → 編集開始
        };
        tb.LostMouseCapture += (_, _) =>
        {
            if (!armed) return;
            armed = false;
            CompleteScrub();
        };
    }

    // 階層ツリーのダブルクリック → 選択ノードへフォーカス (展開トグル上は無視)。
    private void OnHierarchyDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (Engine == IntPtr.Zero || _selectedId < 0) return;
        EngineInterop.acs_editor_camera_focus(Engine);
        Log($"Focus on node {_selectedId}.");
    }

    private IntPtr RawEngine => _viewport?.Engine ?? IntPtr.Zero;

    internal static bool IsEngineCommandReady(
        EditorEngineStartupState state,
        bool handleReady) =>
        state == EditorEngineStartupState.Ready && handleReady;

    private bool EngineCommandsReady => IsEngineCommandReady(
        _engineStartupState,
        RawEngine != IntPtr.Zero);

    // All ordinary editor handlers see a null handle until finalization is
    // complete. Individual startup stages temporarily opt in on the dispatcher
    // so partially initialized native state cannot be edited from menus/keys.
    private IntPtr Engine =>
        (_engineStartupInternalAccess || EngineCommandsReady)
            ? RawEngine
            : IntPtr.Zero;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        _dispatcherWatchdog?.Beat("attaching renderer host");
        _engineStartupState = EditorEngineStartupState.WaitingForAttach;
        StatusText.Text = "Attaching renderer...";
        ViewportLoadingTitle.Text = "Loading scene…";
        ViewportLoadingDetail.Text = "Attaching the renderer";
        ViewportLoadingOverlay.Visibility = Visibility.Visible;

        _viewport = new EngineViewport();
        // This configuration must precede both hiding the host and publishing
        // the HwndHost child. BuildWindowCore preserves it, so the first pump
        // can attach even when WPF has already made the native surface hidden.
        _viewport.SetHiddenStartupRenderingAllowed(true);

        // HwndHost has its own child HWND and cannot be covered by a WPF
        // overlay. Keep it physically absent from composition until the
        // generation-gated scene transaction publishes a complete scene.
        ViewportHost.Visibility = Visibility.Hidden;
        UpdateEditorInputEnabled();
        Log("ACS Editor started.");

        _viewport.Attached += OnEngineAttached;   // アタッチ後にシーンを取り込む
        _viewport.AttachmentFailed += OnEngineAttachmentFailed;
        _viewport.RenderingFailed += OnEngineRenderingFailed;
        _viewport.Picked += OnViewportPicked;     // ビューポート左クリックで選択
        _viewport.TransformChanged += OnViewportTransformChanged;   // ギズモ移動後に Inspector 更新
        _viewport.PolyKeyFinalize += FinalizePoly;                  // 描画中の Enter/Esc でポリゴン確定
        _viewport.AssetDropped += OnViewportAssetDropped;           // アセットをビューポートへドロップ → 配置/割当
        _viewport.SizeChanged += (_, args) =>
            ViewportInfo.Text = $"Viewport {(int)args.NewSize.Width} x {(int)args.NewSize.Height}";
        ViewportHost.IsHitTestVisible = false;
        ViewportHost.Child = _viewport;
        SynchronizeWindowInteractionWithViewport();
        ProfilerView.EngineProvider = () => Engine;
        ProfilerView.AbiCapabilitiesProvider =
            () => _viewport?.AbiCapabilities ??
                  EditorAbiCapability.None;
        ProfilerView.OptionalServiceUiStateProvider =
            service =>
                _viewport?.GetOptionalServiceUiState(service) ??
                EditorOptionalServiceUiState.Legacy(
                    service,
                    hostAvailable: false);
        ProfilerView.LogPumpProvider = GetEditorLogPumpSnapshot;
        ProfilerView.NativeRenderProvider =
            () => _viewport?.GetNativeRenderDiagnostic() ?? default;
        ProfilerView.DispatcherWatchdogProvider =
            GetDispatcherWatchdogSnapshot;
        ProfilerView.ResetEditorPeaks = () =>
        {
            ResetEditorLogPumpPeaks();
            _viewport?.ResetNativeRenderDiagnostics();
            ResetDispatcherWatchdogPeaks();
        };
        ProfilerView.SummaryChanged += summary => ProfilerStatusText.Text = summary;
        ProfilerView.Start();
    }

    // ===== Hierarchy: エンジンのシーングラフから構築 =====
    private System.Windows.Threading.DispatcherTimer? _engineStartupTimer;
    private const int EngineStartupCompletionStageCount = 9;

    private void OnEngineAttached()
    {
        if (_engineStartupState == EditorEngineStartupState.Closed) return;
        _dispatcherWatchdog?.Beat("renderer attached");

        // Pause hidden submissions while the bounded settings snapshot and parse run on a
        // worker. Re-enabling this flag queues the first warm-up frame after native settings
        // application, so quality-dependent resources cannot race the source load.
        _viewport?.SetHiddenStartupRenderingAllowed(false);
        int generation = ++_engineStartupGeneration;
        _engineStartupCompletionStage = 0;
        _engineStartupState = EditorEngineStartupState.WarmingRenderer;
        Log("Viewport attached — loading project settings.");
        StartEngineLogPump();
        ViewportHost.IsHitTestVisible = false;
        StatusText.Text = _project == null
            ? "Initializing renderer..."
            : "Loading project settings...";
        ViewportLoadingDetail.Text = StatusText.Text;

        if (_project == null)
        {
            _projectSettingsLoadGeneration.Invalidate();
            BeginRendererWarmup(generation);
            return;
        }
        _ = ContinueEngineStartupAfterProjectSettingsLoadAsync(generation);
    }

    private async Task ContinueEngineStartupAfterProjectSettingsLoadAsync(
        int startupGeneration)
    {
        Dispatcher.VerifyAccess();
        Project? project = _project;
        if (project == null)
        {
            BeginRendererWarmup(startupGeneration);
            return;
        }

        string projectRoot = project.RootDir;
        string settingsPath =
            Path.Combine(projectRoot, "Config", "ProjectSettings.ini");
        ProjectSettingsLoadTicket ticket =
            _projectSettingsLoadGeneration.Begin();
        string source = "";
        Exception? sourceError = null;
        try
        {
            source = await ProjectSettingsDocumentContract.ReadSourceAsync(
                projectRoot,
                settingsPath,
                ticket.CancellationToken);
        }
        catch (OperationCanceledException)
            when (ticket.CancellationToken.IsCancellationRequested)
        {
            return;
        }
        catch (Exception error)
        {
            sourceError = error;
        }

        Dispatcher.VerifyAccess();
        if (!IsCurrentProjectSettingsStartupLoad(
                ticket,
                startupGeneration,
                project,
                projectRoot))
        {
            return;
        }

        try
        {
            RunWithStartupEngineAccess(
                () => ApplyProjectSettingsSourceOrDefaults(
                    source,
                    sourceError));
        }
        catch (Exception error)
        {
            FailEngineStartup(
                "Project settings defaults recovery failed during renderer startup.",
                error);
            return;
        }

        if (!IsCurrentProjectSettingsStartupLoad(
                ticket,
                startupGeneration,
                project,
                projectRoot))
        {
            return;
        }
        _projectSettingsLoadGeneration.Invalidate();
        BeginRendererWarmup(startupGeneration);
    }

    private bool IsCurrentProjectSettingsStartupLoad(
        ProjectSettingsLoadTicket ticket,
        int startupGeneration,
        Project project,
        string projectRoot)
    {
        if (!_projectSettingsLoadGeneration.IsCurrent(ticket) ||
            startupGeneration != _engineStartupGeneration ||
            _engineStartupState != EditorEngineStartupState.WarmingRenderer ||
            RawEngine == IntPtr.Zero ||
            !ReferenceEquals(_project, project))
        {
            return false;
        }
        try
        {
            return string.Equals(
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(project.RootDir)),
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(projectRoot)),
                StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private void BeginRendererWarmup(int startupGeneration)
    {
        Dispatcher.VerifyAccess();
        if (startupGeneration != _engineStartupGeneration ||
            _engineStartupState != EditorEngineStartupState.WarmingRenderer ||
            RawEngine == IntPtr.Zero)
        {
            return;
        }

        // The HwndHost stays hidden behind the WPF loading surface until a complete scene is
        // published. Cooperative hidden submissions now begin with project settings applied.
        _viewport?.SetHiddenStartupRenderingAllowed(true);
        Log("Project settings stage complete — renderer warm-up started.");
        StatusText.Text = "Initializing renderer...";
        ViewportLoadingDetail.Text = "Initializing renderer…";
        _engineStartupTimer?.Stop();
        _engineStartupTimer = new System.Windows.Threading.DispatcherTimer(
            TimeSpan.FromMilliseconds(50),
            System.Windows.Threading.DispatcherPriority.Normal,
            (_, _) => PollEngineStartup(),
            Dispatcher);
        _engineStartupTimer.Start();
        PollEngineStartup();
    }

    private void PollEngineStartup()
    {
        if (_engineStartupState != EditorEngineStartupState.WarmingRenderer)
            return;
        IntPtr engine = RawEngine;
        if (engine == IntPtr.Zero)
        {
            FailEngineStartup("Renderer handle was lost during initialization.");
            return;
        }

        int state = EngineInterop.acs_editor_startup_status(
            engine, out uint completed, out uint total);
        uint percent = total == 0 ? 0 : Math.Min(100u, completed * 100u / total);
        StatusText.Text = $"Initializing renderer... {percent}%  ({completed}/{total})";
        ViewportLoadingDetail.Text =
            $"Initializing renderer… {percent}%  ({completed}/{total})";
        if (state < 0)
        {
            FailEngineStartup("Renderer startup failed.");
            return;
        }
        if (state == 0) return;

        _engineStartupTimer?.Stop();
        _engineStartupTimer = null;
        CompleteEngineStartup();
    }

    private void CompleteEngineStartup()
    {
        if (_engineStartupState != EditorEngineStartupState.WarmingRenderer)
            return;
        if (RawEngine == IntPtr.Zero)
        {
            FailEngineStartup("Renderer handle was lost before editor initialization.");
            return;
        }

        _engineStartupState = EditorEngineStartupState.FinalizingEditor;
        _engineStartupCompletionStage = 0;
        int generation = ++_engineStartupGeneration;
        AssetBrowser.Engine = RawEngine;
        Log("Renderer warm-up complete — finalizing editor workspace.");
        QueueEngineStartupCompletionStage(generation);
    }

    private void QueueEngineStartupCompletionStage(int generation)
    {
        _ = Dispatcher.BeginInvoke(
            System.Windows.Threading.DispatcherPriority.Background,
            new Action(() => RunEngineStartupCompletionStage(generation)));
    }

    private void RunEngineStartupCompletionStage(int generation)
    {
        if (generation != _engineStartupGeneration ||
            _engineStartupState != EditorEngineStartupState.FinalizingEditor)
        {
            return;
        }
        if (RawEngine == IntPtr.Zero)
        {
            FailEngineStartup("Renderer handle was lost while finalizing the editor.");
            return;
        }

        int stage = _engineStartupCompletionStage;
        _dispatcherWatchdog?.Beat(
            "startup / " + EngineStartupCompletionStageName(stage));
        StatusText.Text =
            $"Initializing editor... {stage + 1}/{EngineStartupCompletionStageCount}  " +
            EngineStartupCompletionStageName(stage);

        if (stage == 1)
        {
            _ = ContinueEngineStartupAfterSceneLoadAsync(generation);
            return;
        }

        try
        {
            RunWithStartupEngineAccess(() =>
            {
                switch (stage)
                {
                    case 0:
                        // AssetBrowser.SetProject performs its index refresh asynchronously;
                        // do not synchronously rescan the project at GPU attach time.
                        if (_project != null) LoadUserTypes();
                        break;
                    case 2:
                        ApplyStartupCamera();
                        ApplyStartupGridVisibility();
                        break;
                    case 3:
                        UpdateGizmoToggles(
                            EngineInterop.acs_editor_gizmo_get_mode(Engine));
                        PopulateComponentCombo();
                        RefreshCreateMenus();
                        // Document source and camera projection are independent.
                        ApplySceneDocumentModePresentation();
                        break;
                    case 4:
                        BuildHierarchy();
                        break;
                    case 5:
                        SyncSelectionUi();
                        break;
                    case 6:
                        InitializeWorkspaceState();
                        break;
                    case 7:
                        InitializeDocumentHost();
                        break;
                    case 8:
                        InitializeAutosaveAndRecovery();
                        break;
                    default:
                        throw new InvalidOperationException(
                            $"Unknown editor startup stage {stage}.");
                }
            });
        }
        catch (Exception ex)
        {
            FailEngineStartup(
                $"Editor initialization failed during " +
                $"'{EngineStartupCompletionStageName(stage)}'.",
                ex);
            return;
        }

        _engineStartupCompletionStage++;
        if (_engineStartupCompletionStage < EngineStartupCompletionStageCount)
        {
            QueueEngineStartupCompletionStage(generation);
            return;
        }

        _engineStartupState = EditorEngineStartupState.Ready;
        _dispatcherWatchdog?.Beat("ready");
        _viewport?.SetHiddenStartupRenderingAllowed(false);
        StatusText.Text = "Ready";
        UpdateEditorInputEnabled();
        // Input-free validation is enforced at both HWND message boundaries;
        // leave the HwndHost enabled for normal DXGI composition.
        ViewportHost.IsHitTestVisible = !IsSceneEditingBlocked;
        CommandManager.InvalidateRequerySuggested();
        Log("Editor startup complete — viewport is ready.");
    }

    private async Task ContinueEngineStartupAfterSceneLoadAsync(int generation)
    {
        try
        {
            bool completed = await InitializeProjectSceneDocument(generation);
            if (!completed)
            {
                if (generation == _engineStartupGeneration &&
                    _engineStartupState == EditorEngineStartupState.FinalizingEditor)
                {
                    FailEngineStartup(
                        "The initial scene load was cancelled before a scene could be published.");
                }
                return;
            }
        }
        catch (Exception ex)
        {
            if (generation == _engineStartupGeneration &&
                _engineStartupState == EditorEngineStartupState.FinalizingEditor)
            {
                FailEngineStartup(
                    "Editor initialization failed during 'project scene'.",
                    ex);
            }
            return;
        }

        if (generation != _engineStartupGeneration ||
            _engineStartupState != EditorEngineStartupState.FinalizingEditor)
        {
            return;
        }
        _engineStartupCompletionStage++;
        QueueEngineStartupCompletionStage(generation);
    }

    private void ApplyStartupCamera()
    {
        if (_startupCamera3D is { } camera)
        {
            _startupCamera3D = null;
            if (!_view3d)
            {
                Log(
                    "Startup --camera3d was ignored because the initial scene is not 3D.",
                    "Camera",
                    LogLevel.Warn);
            }
            else if (EngineInterop.acs_editor_camera3d_set(
                         Engine, camera.Yaw, camera.Pitch, camera.Distance,
                         camera.TargetX, camera.TargetY, camera.TargetZ) == 0)
            {
                Log("Startup --camera3d values were rejected.", "Camera", LogLevel.Warn);
            }
        }
    }

    private void ApplyStartupGridVisibility()
    {
        if (_startupGridVisible is not { } visible)
            return;

        _startupGridVisible = null;
        ShowGridItem.IsChecked = visible;
        EngineInterop.acs_editor_set_show_grid3d(Engine, visible ? 1 : 0);
    }

    private static string EngineStartupCompletionStageName(int stage) =>
        stage switch
        {
            0 => "project types",
            1 => "project scene",
            2 => "startup camera",
            3 => "editor menus",
            4 => "scene hierarchy",
            5 => "selection state",
            6 => "workspace state",
            7 => "document baseline",
            8 => "autosave",
            _ => "unknown stage",
        };

    private void RunWithStartupEngineAccess(Action action)
    {
        bool previous = _engineStartupInternalAccess;
        _engineStartupInternalAccess = true;
        try { action(); }
        finally { _engineStartupInternalAccess = previous; }
    }

    private void OnEngineAttachmentFailed()
    {
        FailEngineStartup(
            _viewport?.AttachmentFailureDetail ??
            "Renderer attachment failed; automatic retry was stopped.");
    }

    private void OnEngineRenderingFailed(string detail)
    {
        FailEngineStartup(detail);
    }

    private void FailEngineStartup(string detail, Exception? exception = null)
    {
        if (_engineStartupState == EditorEngineStartupState.Closed) return;

        _engineStartupTimer?.Stop();
        _engineStartupTimer = null;
        _engineStartupGeneration++;
        _projectSettingsLoadGeneration.Invalidate();
        _engineStartupInternalAccess = false;
        _engineStartupState = EditorEngineStartupState.Failed;
        _viewport?.SetHiddenStartupRenderingAllowed(false);
        AssetBrowser.Engine = IntPtr.Zero;
        InvalidateSceneLoad(detail);
        ViewportHost.IsHitTestVisible = false;
        _viewport?.SuspendRenderPumpForStartupFailure();
        StatusText.Text = "Renderer initialization failed — see Console";
        Log(
            exception == null ? detail : $"{detail} {exception.Message}",
            "Engine",
            LogLevel.Error);
        CommandManager.InvalidateRequerySuggested();
    }

    /// <summary>Explicit retry hook for a failed HWND attach; no automatic retry is allowed.</summary>
    internal bool RetryEngineAttachment()
    {
        if (_engineStartupState != EditorEngineStartupState.Failed ||
            _viewport == null)
        {
            return false;
        }

        // Failure completion disables hidden rendering. Re-enable it before
        // RetryAttach queues the first pump, otherwise a still-hidden HwndHost
        // can circularly wait for an Attached event that can never be raised.
        _viewport.SetHiddenStartupRenderingAllowed(true);
        if (!_viewport.RetryAttach())
        {
            _viewport.SetHiddenStartupRenderingAllowed(false);
            return false;
        }

        _engineStartupGeneration++;
        _engineStartupCompletionStage = 0;
        _engineStartupState = EditorEngineStartupState.WaitingForAttach;
        AssetBrowser.Engine = IntPtr.Zero;
        ViewportHost.IsHitTestVisible = false;
        StatusText.Text = "Retrying renderer attachment...";
        CommandManager.InvalidateRequerySuggested();
        return true;
    }

    private System.Windows.Threading.DispatcherTimer? _engineLogTimer;
    private bool _engineLogDrainQueued;
    internal const int EngineLogPumpMaximumBatchEntries = 64;
    internal const double EngineLogPumpMaximumDrainMilliseconds = 2.0;

    /// <summary>エンジン側 ACS_LOG をキューから定期的に引いてコンソールへ流す。</summary>
    private void StartEngineLogPump()
    {
        if (_engineLogTimer != null) return;
        _engineLogTimer = new System.Windows.Threading.DispatcherTimer(
            System.Windows.Threading.DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(100),
        };
        _engineLogTimer.Tick += (_, _) =>
        {
            if (!_engineLogDrainQueued)
                DrainEngineLogBatch();
        };
        _engineLogTimer.Start();
    }

    private void DrainEngineLogBatch()
    {
        Dispatcher.VerifyAccess();
        _engineLogDrainQueued = false;
        if (_engineStartupState == EditorEngineStartupState.Closed)
            return;

        var messages =
            new List<(int Severity, string Message)>(
                EngineLogPumpMaximumBatchEntries);
        long begin = System.Diagnostics.Stopwatch.GetTimestamp();
        bool budgetReached = false;
        while (messages.Count < EngineLogPumpMaximumBatchEntries &&
               EngineInterop.LogPoll(out int severity, out string message))
        {
            messages.Add((severity, message));
            double elapsedMilliseconds =
                (System.Diagnostics.Stopwatch.GetTimestamp() - begin) *
                1000.0 / System.Diagnostics.Stopwatch.Frequency;
            if (elapsedMilliseconds >=
                EngineLogPumpMaximumDrainMilliseconds)
            {
                budgetReached = true;
                break;
            }
        }

        double drainMilliseconds =
            (System.Diagnostics.Stopwatch.GetTimestamp() - begin) *
            1000.0 / System.Diagnostics.Stopwatch.Frequency;
        AppendEngineLogBatch(messages, drainMilliseconds);

        // Saturation means the native queue may still contain messages.
        // Continue at Background priority in another bounded slice instead of
        // either waiting 100 ms or monopolizing this Dispatcher turn.
        if ((budgetReached ||
             messages.Count == EngineLogPumpMaximumBatchEntries) &&
            !_engineLogDrainQueued &&
            !Dispatcher.HasShutdownStarted &&
            !Dispatcher.HasShutdownFinished)
        {
            _engineLogDrainQueued = true;
            _ = Dispatcher.BeginInvoke(
                System.Windows.Threading.DispatcherPriority.Background,
                new Action(DrainEngineLogBatch));
        }
    }

    // ===== プロジェクト設定 (Config/ProjectSettings.ini) =====
    private string SettingsIniPath =>
        System.IO.Path.Combine(_project!.RootDir, "Config", "ProjectSettings.ini");

    /// <summary>
    /// Applies a worker-preflighted ProjectSettings.ini snapshot on the Dispatcher. Because the
    /// native load ABI returns no status, every source entry must survive serialization.
    /// </summary>
    private void ApplyProjectSettingsSourceOrDefaults(
        string source,
        Exception? sourceError)
    {
        if (sourceError == null)
        {
            try
            {
                ProjectSettingsDocumentContract.Parse(source);
                EngineInterop.acs_editor_settings_load_text(Engine, "");
                if (source.Length != 0)
                    EngineInterop.acs_editor_settings_load_text(Engine, source);
                EditorDocumentState loaded =
                    CaptureProjectSettingsDocumentState();
                ProjectSettingsDocumentContract.EnsureSourceEntriesPreserved(
                    source,
                    loaded.Payload);

                _projectSettingsPersistenceGate.ClearAfterVerifiedLoad();
                EditorDocumentState canonical =
                    ProjectSettingsDocumentContract.CreateState(loaded.Payload);
                _initialProjectSettingsDocumentState = canonical;
                _initialProjectSettingsDocumentInitiallySaved = true;
                _projectSettingsDocument?.ResetHistory(
                    markSaved: true,
                    canonical);
                SynchronizeProjectSettingsChrome();
                if (source.Length > 0)
                    Log($"Project settings ← {SettingsIniPath}");
                return;
            }
            catch (Exception error)
            {
                sourceError = error;
            }
        }

        string reason =
            "ProjectSettings.ini was rejected and will not be overwritten: " +
            sourceError.Message;
        try
        {
            EngineInterop.acs_editor_settings_load_text(Engine, "");
            EditorDocumentState defaults =
                CaptureProjectSettingsDocumentState();
            _projectSettingsPersistenceGate.Latch(reason);
            _initialProjectSettingsDocumentState = defaults;
            _initialProjectSettingsDocumentInitiallySaved = false;
            _projectSettingsDocument?.ResetHistory(
                markSaved: false,
                defaults);
            SynchronizeProjectSettingsChrome();
        }
        catch (Exception recoveryError)
        {
            throw new InvalidOperationException(
                reason + " Defaults recovery also failed.",
                new AggregateException(sourceError, recoveryError));
        }
        Log(reason, "Settings", LogLevel.Error);
    }

    /// <summary>Rendering.MsaaSamples の現在値をツールバーの AA コンボへ反映する (再発火させない)。</summary>
    private bool _suppressAa;
    private void SyncAaCombo()
    {
        var buf = new byte[32];
        if (EngineInterop.acs_editor_settings_get_value(Engine, "Rendering", "MsaaSamples", buf, buf.Length) == 0) return;
        int samples = EngineInterop.Utf8Z(buf) switch { "2" => 2, "4" => 4, "8" => 8, _ => 1 };
        ApplyAaMenuCheck(samples);
    }

    // View → Anti-aliasing の排他チェック状態を samples に合わせる (発火抑止つき)。
    private void ApplyAaMenuCheck(int samples)
    {
        if (AaFxaa == null) return;
        _suppressAa = true;
        AaFxaa.IsChecked = samples == 1; AaMsaa2.IsChecked = samples == 2;
        AaMsaa4.IsChecked = samples == 4; AaMsaa8.IsChecked = samples == 8;
        _suppressAa = false;
    }

    /// <summary>Rendering.QualityLevel の現在値を View → Graphics Quality メニューへ反映する。</summary>
    private bool _suppressQuality;
    private void SyncQualityMenu()
    {
        var buf = new byte[32];
        string level = (EngineInterop.acs_editor_settings_get_value(Engine, "Rendering", "QualityLevel", buf, buf.Length) != 0)
            ? EngineInterop.Utf8Z(buf) : "High";
        ApplyQualityMenuCheck(level);
    }

    // View → Graphics Quality の排他チェックを level に合わせる (発火抑止つき)。
    private void ApplyQualityMenuCheck(string level)
    {
        if (QualHigh == null) return;
        _suppressQuality = true;
        QualUltra.IsChecked = level == "Ultra"; QualHighest.IsChecked = level == "Highest";
        QualHigh.IsChecked  = level == "High";  QualMedium.IsChecked  = level == "Medium";
        QualLow.IsChecked   = level == "Low";   QualLowest.IsChecked  = level == "Lowest";
        _suppressQuality = false;
    }

    /// <summary>Graphics Quality メニュー: 品質レベルを Rendering.QualityLevel 経由で即適用 (影/bloom 等が連動)。</summary>
    private void OnQualityMenuClick(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _suppressQuality) return;
        if (sender is not MenuItem mi || mi.Tag is not string level) return;
        bool applied = _project == null
            ? EngineInterop.acs_editor_settings_set(
                Engine,
                "Rendering",
                "QualityLevel",
                level) != 0
            : TryApplyProjectSettingsMutation(
                "Change graphics quality",
                "settings.Rendering.QualityLevel",
                () => EngineInterop.acs_editor_settings_set(
                    Engine,
                    "Rendering",
                    "QualityLevel",
                    level) != 0);
        if (applied)
        {
            SyncQualityMenu();
            int ss = EngineInterop.acs_editor_quality_shadow_size(Engine);
            string shadowDesc = ss > 0 ? ss + "px" : "オフ";
            Log($"Graphics Quality: {level}  (影 {shadowDesc})", "General", LogLevel.Info);
        }
        else
        {
            SyncQualityMenu();
            Log($"品質設定の適用に失敗: {level}");
        }
    }

    /// <summary>Lighting メニュー: 時間帯プリセット (太陽方向/色/強度 + 空の色 + 露出) を一括適用。</summary>
    private void OnLightingPreset(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (sender is not MenuItem mi || mi.Tag is not string preset) return;
        bool applied = _project == null
            ? EngineInterop.acs_editor_apply_lighting_preset(
                Engine,
                preset) != 0
            : TryApplyProjectSettingsMutation(
                "Apply lighting preset",
                "settings.Rendering.LightingPreset",
                () => EngineInterop.acs_editor_apply_lighting_preset(
                    Engine,
                    preset) != 0);
        if (applied)
        {
            Log($"Lighting preset: {preset} (太陽/空/露出を一括設定)", "General", LogLevel.Info);
        }
        else Log($"照明プリセットの適用に失敗: {preset}");
    }

    /// <summary>View → Show Grid: 3D ビューポートのグリッド表示を切替える (清書/スクショ用)。</summary>
    private void OnToggleGrid(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        bool on = ShowGridItem.IsChecked;
        EngineInterop.acs_editor_set_show_grid3d(Engine, on ? 1 : 0);
        Log(on ? "グリッド表示 ON" : "グリッド表示 OFF");
    }

    private void OnProjectSettings(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero ||
            _project == null ||
            !_documentHostInitialized ||
            !CanEditProjectSettings())
        {
            return;
        }
        EditorDocument settings =
            EnsureProjectSettingsDocumentRegistered();
        _documentHost.Activate(settings.Id);
        var win = new ProjectSettingsWindow(
            this,
            Engine,
            _project.RootDir,
            TryApplyProjectSettingsMutation);
        win.ShowDialog();
        SynchronizeSnapSettingsFromProject();
    }

    /// <summary>
    /// Guards commands that require the legacy .acs3d source adapter. This never changes the
    /// current Perspective/2D view preset or selects a hidden compatibility payload.
    /// </summary>
    private bool EnsureView3D()
    {
        if (_legacySceneSourceMode == SceneDocumentMode.ThreeD && _view3d)
            return true;
        Log(
            "This command requires an explicit .acscene to .acs3d source conversion. " +
            "No hidden 3D payload was modified.",
            "Scene",
            LogLevel.Warn);
        return false;
    }

    // GameObject メニュー: 空ノードを生成 (root 直下 / 選択ノードの子)。
    private void OnCreateEmpty(object sender, RoutedEventArgs e) => CreateEmptyNode(-1);
    private void OnCreateChild(object sender, RoutedEventArgs e) =>
        CreateEmptyNode(_view3d ? EngineInterop.acs_editor_selected3d(Engine) : _selectedId);

    private void CreateEmptyNode(int parentId)
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        if (_view3d)   // .acs3d: 描画しない «空ノード» (グループ用トランスフォーム)
        {
            int id3 = EngineInterop.acs_editor_add_empty3d(Engine, "Empty");
            if (id3 < 0) return;
            if (parentId >= 0 && parentId != id3) EngineInterop.acs_editor_reparent3d(Engine, id3, parentId);
            RefreshAfterSceneChange();   // 階層再構築 + 選択 UI 同期 (add_empty3d が新ノードを選択済み)
            Log(parentId >= 0 ? $"空ノード {id3} を {parentId} の子に作成。" : $"空ノード {id3} を作成。");
            return;
        }
        int id = EngineInterop.acs_editor_add_node(Engine, "Empty", parentId);
        if (id < 0) return;
        BuildHierarchy();
        SyncSelectionUi();   // ABI が新ノードを選択済み → UI を同期
        RecordSceneDocumentChange("Create Node");
        Log(parentId >= 0 ? $"Created empty node {id} under {parentId}." : $"Created empty node {id}.");
    }

    // ===== ヒエラルキー右クリック → 生成メニュー =====

    // 右クリックされたノードを確定する (空白なら -1 = ルート直下に作る)。
    private void OnHierarchyRightDown(object sender, MouseButtonEventArgs e)
    {
        var tvi = FindAncestorTreeViewItem(e.OriginalSource as DependencyObject);
        if (tvi != null && tvi.Tag is int id) { _contextNodeId = id; tvi.IsSelected = true; }
        else _contextNodeId = -1;
    }

    // 型名を表示用に整える (F接頭辞 / Component接尾辞を落とす)。
    private static string Friendly(string typeName)
    {
        string s = typeName;
        if (s.Length > 1 && s[0] == 'F' && char.IsUpper(s[1])) s = s.Substring(1);
        if (s.EndsWith("Component", StringComparison.Ordinal)) s = s.Substring(0, s.Length - 9);
        return s.Length == 0 ? typeName : s;
    }

    // ビルトイン / ユーザー定義 の生成サブメニューを構築する (attach 後・ロード後に呼ぶ)。
    private void RefreshCreateMenus()
    {
        if (Engine == IntPtr.Zero) return;

        // ビルトイン: リフレクションの Component カテゴリ型。
        CtxBuiltin.Items.Clear();
        int count = EngineInterop.acs_editor_type_count();
        int builtins = 0;
        for (int i = 0; i < count; i++)
        {
            if (EngineInterop.CategoryLabel(EngineInterop.acs_editor_type_category_at(i)) != "Component") continue;
            string tn = EngineInterop.TypeName(i);
            var mi = new MenuItem { Header = Friendly(tn), Tag = tn };
            mi.Click += OnCreateTypedObject;
            CtxBuiltin.Items.Add(mi);
            builtins++;
        }
        CtxBuiltin.IsEnabled = builtins > 0;

        RefreshUserMenu();
    }

    // ユーザー定義型 (ゲーム DLL から取り込んだもの) の生成サブメニュー。
    private void RefreshUserMenu()
    {
        CtxUser.Items.Clear();
        int uc = Engine != IntPtr.Zero ? EngineInterop.acs_editor_user_type_count(Engine) : 0;
        for (int i = 0; i < uc; i++)
        {
            string tn = EngineInterop.UserTypeName(Engine, i);
            var mi = new MenuItem { Header = Friendly(tn), Tag = tn };
            mi.Click += OnCreateTypedObject;
            CtxUser.Items.Add(mi);
        }
        if (uc == 0)
            CtxUser.Items.Add(new MenuItem { Header = "(Build / Hot Reload で読み込み)", IsEnabled = false });
        CtxUser.IsEnabled = true;
    }

    private void OnCtxCreateEmpty(object sender, RoutedEventArgs e) => CreateEmptyNode(_contextNodeId);

    // ビルトイン/ユーザー型のオブジェクト = 空ノード + そのコンポーネント。
    private void OnCreateTypedObject(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || sender is not MenuItem mi || mi.Tag is not string typeName) return;
        int id = EngineInterop.acs_editor_add_node(Engine, Friendly(typeName), _contextNodeId);
        if (id < 0) return;
        if (EngineInterop.acs_editor_node_add_component(Engine, id, typeName) == 0)
            Log($"'{typeName}' をアタッチできませんでした (未登録の可能性)。");
        BuildHierarchy();
        SyncSelectionUi();
        RecordSceneDocumentChange("Create Node");
        Log($"Created '{Friendly(typeName)}' (node {id}) with {typeName}.");
    }

    private void OnCtxRename(object sender, RoutedEventArgs e)
    {
        if (_contextNodeId < 0) return;
        NameBox.Focus(); NameBox.SelectAll();   // インスペクタの Name 欄で編集
    }

    private void OnCtxDelete(object sender, RoutedEventArgs e) => DeleteSelected();

    // 畳んだノード id を覚えておき、ヒエラルキー再構築でも展開状態を維持する
    // (再構築のたびに IsExpanded=true だと畳んでもすぐ開いてしまうため)。
    private readonly HashSet<int> _collapsedNodes = new();
    private void WireCollapseTracking(TreeViewItem tvi)
    {
        tvi.Expanded  += (s, e) => { if (s is TreeViewItem t && t.Tag is int id) { _collapsedNodes.Remove(id); } e.Handled = true; };
        tvi.Collapsed += (s, e) => { if (s is TreeViewItem t && t.Tag is int id) { _collapsedNodes.Add(id);    } e.Handled = true; };
    }

    private void BuildHierarchy()
    {
        if (Engine == IntPtr.Zero) return;
        if (_view3d) { Build3DHierarchy(); return; }    // 3D モードは 3D ノードを並べる
        HierarchyTree.Items.Clear();

        int count = EngineInterop.acs_editor_node_count(Engine);
        var items = new Dictionary<int, TreeViewItem>();
        var ids = new List<int>();
        for (int i = 0; i < count; ++i)
        {
            int id = EngineInterop.acs_editor_node_id_at(Engine, i);
            ids.Add(id);
            var visible = new CheckBox
            {
                IsChecked = EngineInterop.acs_editor_node_get_visible(Engine, id) != 0,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 6, 0),
                Focusable = false,
                ToolTip = "Visible in viewport",
            };
            int visibleId = id;
            visible.Checked += (_, __) =>
            {
                if (Engine != IntPtr.Zero)
                {
                    EngineInterop.acs_editor_node_set_visible(Engine, visibleId, 1);
                    RecordSceneDocumentChange("Visibility");
                }
            };
            visible.Unchecked += (_, __) =>
            {
                if (Engine != IntPtr.Zero)
                {
                    EngineInterop.acs_editor_node_set_visible(Engine, visibleId, 0);
                    RecordSceneDocumentChange("Visibility");
                }
            };
            var header = new StackPanel { Orientation = Orientation.Horizontal };
            header.Children.Add(visible);
            header.Children.Add(new TextBlock
            {
                Text = "◇",
                FontSize = 11,
                Margin = new Thickness(0, 0, 5, 0),
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = (System.Windows.Media.Brush)FindResource("InfoFg"),
            });
            header.Children.Add(new TextBlock
            {
                Text = EngineInterop.NodeName(Engine, id),
                VerticalAlignment = VerticalAlignment.Center,
            });
            var tvi = new TreeViewItem
            {
                Header = header,
                Tag = id,
                IsExpanded = !_collapsedNodes.Contains(id),   // 畳み状態を再構築でも維持
                Foreground = System.Windows.Media.Brushes.Gainsboro,
            };
            WireCollapseTracking(tvi);
            items[id] = tvi;
        }
        // 親子をつなぐ。
        foreach (int id in ids)
        {
            int parent = EngineInterop.acs_editor_node_parent(Engine, id);
            if (parent >= 0 && items.TryGetValue(parent, out var pItem))
                pItem.Items.Add(items[id]);
            else
                HierarchyTree.Items.Add(items[id]);
        }
        Log($"Hierarchy: {count} nodes from engine scene.");

        // ABI の選択集合をツリーへ反映 (primary も含め全メンバをハイライト)。
        RefreshHierarchyHighlight();
        ApplyHierarchyFilter();   // 検索フィルタを再適用 (再構築でも維持)
        UpdateStatusBar();        // ノード数をステータスバーへ
    }

    // ===== ヒエラルキー検索/フィルタ: 名前部分一致でノードを絞り込み (一致 + その祖先を表示) =====
    private string _hierFilter = "";

    private void OnHierSearchChanged(object sender, TextChangedEventArgs e)
    {
        _hierFilter = HierSearchBox.Text?.Trim() ?? "";
        ApplyHierarchyFilter();
    }

    /// <summary>検索フィルタを TreeView に適用する (一致ノードとその祖先を表示、他を Collapsed)。</summary>
    private void ApplyHierarchyFilter()
    {
        foreach (var obj in HierarchyTree.Items)
            if (obj is TreeViewItem tvi) ApplyHierFilterRec(tvi, _hierFilter);
    }

    private bool ApplyHierFilterRec(TreeViewItem tvi, string filter)
    {
        bool selfMatch = filter.Length == 0 ||
            TviName(tvi).Contains(filter, StringComparison.OrdinalIgnoreCase);
        bool childMatch = false;
        foreach (var obj in tvi.Items)
            if (obj is TreeViewItem child && ApplyHierFilterRec(child, filter)) childMatch = true;
        bool visible = selfMatch || childMatch;
        tvi.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        if (filter.Length == 0)
            tvi.IsExpanded = !(tvi.Tag is int id && _collapsedNodes.Contains(id));   // 元の畳み状態へ復元
        else if (childMatch)
            tvi.IsExpanded = true;                                                   // 子に一致 → 道筋を展開
        return visible;
    }

    /// <summary>TreeViewItem の表示名を取り出す (2D=string ヘッダ / 3D=StackPanel 内 TextBlock)。</summary>
    private static string TviName(TreeViewItem tvi)
    {
        if (tvi.Header is string s) return s;
        if (tvi.Header is StackPanel sp)
        {
            string name = "";
            foreach (var c in sp.Children)
                if (c is TextBlock tb && !string.IsNullOrWhiteSpace(tb.Text))
                    name = tb.Text;
            // 3D headers are [visibility checkbox, primitive glyph, node name].
            // The last non-empty TextBlock is therefore the editable node name,
            // while returning the first one made filters search "●"/"▣" instead.
            return name;
        }
        return tvi.Header?.ToString() ?? "";
    }

    // ===== Viewport picking: ビューポートのクリック選択を Hierarchy/Inspector に反映 =====
    // ビューポート側で既に ABI の選択集合を更新済み (single/toggle/none)。ここでは ABI を
    // 真実点として読み直し、ツリーのハイライトと Inspector を同期するだけ (id は無視可)。
    private void OnViewportPicked(int id)
    {
        if (_view3d) { Select3DInHierarchy(id); if (id >= 0) Populate3DInspector(id); else Clear3DInspector(); return; }
        SyncSelectionUi();
    }

    /// <summary>
    /// ABI の選択集合を各 TreeViewItem へ写す (IsInSet/IsPrimary 添付プロパティ + native 単一
    /// 選択を primary にミラー)。programmatic な IsSelected 変更が OnHierarchySelect を経由して
    /// 単一選択へ巻き戻さないよう _syncingSelection で抑止する。try/finally で例外時もフラグを
    /// 必ず戻す (戻し損ねると以降の選択が無反応になるため)。
    /// </summary>
    private void SyncHighlightAndNativeSelection()
    {
        if (Engine == IntPtr.Zero) return;
        _syncingSelection = true;
        try
        {
            foreach (var tvi in AllTreeItems(HierarchyTree.Items))
            {
                if (tvi.Tag is int id)
                {
                    SelectionHighlight.SetIsInSet(tvi, EngineInterop.acs_editor_selection_contains(Engine, id) != 0);
                    SelectionHighlight.SetIsPrimary(tvi, id == _selectedId);
                    bool wantNative = (id == _selectedId);
                    if (tvi.IsSelected != wantNative) tvi.IsSelected = wantNative;
                }
            }
        }
        finally { _syncingSelection = false; }
    }

    /// <summary>ABI の選択集合を読み直し、primary・ツリーハイライト・Inspector を更新する。</summary>
    private void SyncSelectionUi()
    {
        if (Engine == IntPtr.Zero) return;
        UpdateStatusBar();   // 2D/3D 共通: ノード数・選択数をステータスバーへ反映 (選択ファネル)
        if (_view3d)   // 3D モード: 3D 選択 + 3D インスペクター (undo/redo/シーン変更後の再同期)
        {
            int s3 = EngineInterop.acs_editor_selected3d(Engine);
            ObserveSceneSelectionForMerge(
                use3D: true,
                nodeId: s3,
                selectionCount: EngineInterop.acs_editor_selected3d_count(Engine));
            Apply3DSelectionHighlight();   // multi-select の primary/集合ハイライトを反映
            if (s3 >= 0) Populate3DInspector(s3);
            else Clear3DInspector();
            return;
        }
        _selectedId = EngineInterop.acs_editor_selected(Engine);
        ObserveSceneSelectionForMerge(
            use3D: false,
            nodeId: _selectedId,
            selectionCount: EngineInterop.acs_editor_selection_count(Engine));
        SyncHighlightAndNativeSelection();
        if (_selectedId >= 0) PopulateInspector(_selectedId);
        else                  ClearSelectionUi();
    }

    /// <summary>ステータスバーにシーンのノード数と選択数を表示する (2D/3D 双方)。</summary>
    private void UpdateStatusBar()
    {
        if (Engine == IntPtr.Zero) { StatusText.Text = "Ready"; return; }
        int total, sel;
        if (_view3d)
        {
            total = EngineInterop.acs_editor_node3d_count(Engine);
            sel   = EngineInterop.acs_editor_selected3d_count(Engine);
        }
        else
        {
            total = EngineInterop.acs_editor_node_count(Engine);
            sel   = EngineInterop.acs_editor_selection_count(Engine);
        }
        StatusText.Text = $"Nodes: {total}  |  Selected: {sel}";
        HierarchyCountText.Text = total.ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>ABI の選択集合をツリーのハイライトへ反映する (primary も ABI から読み直す)。</summary>
    private void RefreshHierarchyHighlight()
    {
        if (Engine == IntPtr.Zero) return;
        _selectedId = EngineInterop.acs_editor_selected(Engine);   // ABI を真実点に (stale primary 防止)
        SyncHighlightAndNativeSelection();
    }

    // Ctrl+click でトグル、複数選択中の通常クリックで単一へ畳む。単一/無選択の通常クリックは
    // WPF 既定の選択 (→ OnHierarchySelect) に任せる。展開トグル (▸) のクリックは常に既定へ渡す。
    private void OnHierarchyPreviewMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        var src = e.OriginalSource as DependencyObject;
        if (ClickedExpander(src)) return;                       // 展開/折りたたみは WPF に任せる
        var tvi = FindAncestorTreeViewItem(src);
        if (tvi?.Tag is not int id) return;

        _dragStart  = e.GetPosition(null);   // ドラッグ&ドロップ (付け替え) の起点を記録
        _dragNodeId = id;

        bool ctrl = (Keyboard.Modifiers & ModifierKeys.Control) != 0;
        int selCount = _view3d ? EngineInterop.acs_editor_selected3d_count(Engine)
                               : EngineInterop.acs_editor_selection_count(Engine);
        if (ctrl)
        {
            if (_view3d) EngineInterop.acs_editor_select3d_toggle(Engine, id);
            else         EngineInterop.acs_editor_select_toggle(Engine, id);
            SyncSelectionUi();
            e.Handled = true;
        }
        else if (selCount > 1)
        {
            // 複数選択中のメンバを通常クリック → {id} に畳む。既に primary だと
            // SelectedItemChanged が発火しないので、ここで明示的に単一選択する。
            if (_view3d) EngineInterop.acs_editor_select3d(Engine, id);
            else         EngineInterop.acs_editor_select(Engine, id);
            SyncSelectionUi();
            e.Handled = true;
        }
        // それ以外 (単一/無選択) は WPF 既定 → OnHierarchySelect に委ねる。
    }

    // しきい値を超えて動いたらドラッグ開始 (1 ジェスチャにつき 1 回 DoDragDrop)。
    private void OnHierarchyPreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (e.LeftButton != MouseButtonState.Pressed || _dragNodeId < 0) return;
        Point pos = e.GetPosition(null);
        if (Math.Abs(pos.X - _dragStart.X) < SystemParameters.MinimumHorizontalDragDistance &&
            Math.Abs(pos.Y - _dragStart.Y) < SystemParameters.MinimumVerticalDragDistance) return;
        int dragged = _dragNodeId;
        _dragNodeId = -1;   // 多重発火を防ぐ (DoDragDrop はドロップまでブロック)
        DragDrop.DoDragDrop(HierarchyTree, dragged, DragDropEffects.Move);
    }

    private const double HierRowH = 22.0;   // ヒエラルキー 1 行の概算高さ (ドロップ位置判定用)

    // ドラッグ中: 対象行のどこにいるか (上端/中央/下端) で before/child/after を決め、インジケータを更新。
    private void OnHierarchyDragOver(object sender, DragEventArgs e)
    {
        e.Handled = true;
        if (!e.Data.GetDataPresent(typeof(int))) { e.Effects = DragDropEffects.None; ClearDropAdorner(); return; }
        e.Effects = DragDropEffects.Move;

        var tvi = FindAncestorTreeViewItem(e.OriginalSource as DependencyObject);
        int dragged = e.Data.GetData(typeof(int)) is int d ? d : -1;
        if (tvi == null || tvi.Tag is not int tid || tid == dragged)
        {
            _dropMode = -1; _dropTargetId = -1; ClearDropAdorner();   // 空白/自分 = ルート直下扱い
            return;
        }
        double y = e.GetPosition(tvi).Y;
        // 上 35% = 前に挿入(兄弟), 下 35% = 後に挿入(兄弟), 中央 30% = 子にする。
        // これにより「隙間に挿す」が容易になり、子化の誤爆を防ぐ。
        _dropMode = y < HierRowH * 0.35 ? 0 : (y > HierRowH * 0.65 ? 1 : 2);
        _dropTargetId = tid;
        ShowDropAdorner(tvi, _dropMode);
    }

    private void ShowDropAdorner(TreeViewItem tvi, int mode)
    {
        var layer = System.Windows.Documents.AdornerLayer.GetAdornerLayer(HierarchyTree);
        if (layer == null) return;
        if (_dropAdorner == null) { _dropAdorner = new HierarchyDropAdorner(HierarchyTree); layer.Add(_dropAdorner); }
        try
        {
            System.Windows.Media.GeneralTransform gt = tvi.TransformToAncestor(HierarchyTree);
            Point tl = gt.Transform(new Point(0, 0));
            _dropAdorner.TargetRect = new Rect(tl.X, tl.Y, Math.Max(tvi.ActualWidth, 40), HierRowH);
            _dropAdorner.Mode = mode;
            _dropAdorner.InvalidateVisual();
        }
        catch { }
    }

    private void ClearDropAdorner()
    {
        if (_dropAdorner == null) return;
        System.Windows.Documents.AdornerLayer.GetAdornerLayer(HierarchyTree)?.Remove(_dropAdorner);
        _dropAdorner = null;
    }

    // ドロップ確定: 前/後ろ = 兄弟挿入 (acs_editor_node_move), 子 = 子化, 空白 = root。
    private void OnHierarchyDrop(object sender, DragEventArgs e)
    {
        _dragNodeId = -1;
        int mode = _dropMode, target = _dropTargetId;
        ClearDropAdorner(); _dropMode = -1; _dropTargetId = -1;
        if (Engine == IntPtr.Zero || !e.Data.GetDataPresent(typeof(int))) return;
        int dragged = (int)e.Data.GetData(typeof(int));
        e.Handled = true;
        if (dragged == target) return;

        if (_view3d)   // 3D: 中央=子化 / 前後=兄弟(target の親へ) / 空白=root。順序付けは無し。
        {
            int newParent = target < 0 ? -1
                          : (mode == 2 ? target : EngineInterop.acs_editor_node3d_parent(Engine, target));
            if (EngineInterop.acs_editor_reparent3d(Engine, dragged, newParent) != 0)
            {
                Build3DHierarchy();
                EngineInterop.acs_editor_select3d(Engine, dragged);
                Select3DInHierarchy(dragged);
                Populate3DInspector(dragged);
                RecordSceneDocumentChange("Reparent Node");
                Log($"3D: reparented {dragged} → {(newParent < 0 ? "root" : newParent.ToString())}");
            }
            return;
        }

        int rc; string what;
        if (target < 0) { rc = EngineInterop.acs_editor_node_reparent(Engine, dragged, -1); what = "→ root"; }
        else
        {
            rc = EngineInterop.acs_editor_node_move(Engine, dragged, target, mode);
            what = mode == 0 ? $"→ {target} の前" : mode == 1 ? $"→ {target} の後" : $"→ {target} の子";
        }
        if (rc != 0)
        {
            BuildHierarchy();
            EngineInterop.acs_editor_select(Engine, dragged);   // 動かしたノードを選択して見せる
            SyncSelectionUi();
            RecordSceneDocumentChange("Reparent Node");
            Log($"Moved node {dragged} {what}");
        }
    }

    private static TreeViewItem? FindAncestorTreeViewItem(DependencyObject? o)
    {
        while (o != null && o is not TreeViewItem) o = System.Windows.Media.VisualTreeHelper.GetParent(o);
        return o as TreeViewItem;
    }

    // クリックが展開トグル (ToggleButton) 上か (TreeViewItem に達する前に見つかれば true)。
    private static bool ClickedExpander(DependencyObject? o)
    {
        while (o != null && o is not TreeViewItem)
        {
            if (o is System.Windows.Controls.Primitives.ToggleButton) return true;
            o = System.Windows.Media.VisualTreeHelper.GetParent(o);
        }
        return false;
    }

    private void OnSelectAll(object sender, ExecutedRoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        EngineInterop.acs_editor_select_all(Engine);
        SyncSelectionUi();
        Log("Selected all nodes.");
    }

    private void OnViewportTransformChanged()
    {
        if (Engine == IntPtr.Zero) return;
        if (_view3d)
        {
            int s3 = EngineInterop.acs_editor_selected3d(Engine);
            if (s3 >= 0) Populate3DInspector(s3);
            RecordSceneDocumentChange(
                "Transform",
                mergeKey: "viewport.gizmo",
                mergeWindow: TimeSpan.FromSeconds(1),
                nodeId: s3);
            return;
        }
        int sel = EngineInterop.acs_editor_selected(Engine);
        if (sel >= 0) { _selectedId = sel; PopulateInspector(sel); }
        RecordSceneDocumentChange(
            "Transform",
            mergeKey: "viewport.gizmo",
            mergeWindow: TimeSpan.FromSeconds(1),
            nodeId: sel);
    }

    private bool SelectHierarchyItem(int id)
    {
        foreach (var tvi in AllTreeItems(HierarchyTree.Items))
        {
            if (tvi.Tag is int tid && tid == id)
            {
                tvi.IsSelected = true;
                tvi.BringIntoView();
                return true;
            }
        }
        return false;
    }

    private static IEnumerable<TreeViewItem> AllTreeItems(ItemCollection items)
    {
        foreach (var o in items)
        {
            if (o is TreeViewItem tvi)
            {
                yield return tvi;
                foreach (var c in AllTreeItems(tvi.Items)) yield return c;
            }
        }
    }

    private void OnResetView(object sender, RoutedEventArgs e)
    {
        if (Engine != IntPtr.Zero) { EngineInterop.acs_editor_camera_reset(Engine); Log("View reset (pan 0, zoom 1)."); }
    }

    /// <summary>AA コンボ変更: MSAA サンプル数 (FXAA/2x/4x/8x) をエンジンへ反映する。</summary>
    // View → Anti-aliasing メニュー: 選んだ MSAA を適用 (Rendering.MsaaSamples 経由で永続化、共有)。
    private void OnAaMenuClick(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _suppressAa) return;
        if (sender is not MenuItem mi || mi.Tag is not string tag || !int.TryParse(tag, out int samples)) return;
        if (_project != null)
        {
            if (!TryApplyProjectSettingsMutation(
                    "Change anti-aliasing",
                    "settings.Rendering.MsaaSamples",
                    () => EngineInterop.acs_editor_settings_set(
                        Engine,
                        "Rendering",
                        "MsaaSamples",
                        samples.ToString()) != 0))
            {
                SyncAaCombo();
                Log("AA 設定の適用に失敗しました。", "Settings", LogLevel.Error);
                return;
            }
        }
        else
        {
            EngineInterop.acs_editor_set_msaa(Engine, samples);
            ApplyAaMenuCheck(samples);
        }
        Log(samples == 1 ? "AA: FXAA" : $"AA: MSAA {samples}x");
    }

    // ===== ギズモモード切替 (Move / Rotate / Scale) =====
    private void SetGizmoMode(int mode, string name)
    {
        if (Engine != IntPtr.Zero) EngineInterop.acs_editor_gizmo_set_mode(Engine, mode);
        UpdateGizmoToggles(mode);
        Log($"Gizmo mode: {name}");
    }
    // 3 つのトグルを排他に。クリックで一旦反転した状態を正しい active 状態へ上書きする。
    private void UpdateGizmoToggles(int mode)
    {
        GizMove.IsChecked   = mode == 0;
        GizRotate.IsChecked = mode == 1;
        GizScale.IsChecked  = mode == 2;
    }
    private void OnGizmoMove(object sender, RoutedEventArgs e)   => SetGizmoMode(0, "Move");
    private void OnGizmoRotate(object sender, RoutedEventArgs e) => SetGizmoMode(1, "Rotate");
    private void OnGizmoScale(object sender, RoutedEventArgs e)  => SetGizmoMode(2, "Scale");

    // ===== Play / Pause / Step (物理プレビュー) =====
    // 物理ボディ (Inspector の Physics で動的/静的) を持つノードを Play で落下・衝突させ、
    // Stop で開始状態へ復元する。ステップは ABI の render(dt) 内で進む。
    private bool StartPlayMode()
    {
        if (Engine == IntPtr.Zero ||
            EngineInterop.acs_editor_play_state(Engine) != 0)
        {
            return false;
        }

        SuspendSceneDocumentHistoryForSimulation();
        if (EngineInterop.acs_editor_play_start(Engine) == 0)
        {
            ResumeSceneDocumentHistoryAfterSimulation();
            Log("Play could not start because a complete restore snapshot was unavailable.",
                "Play", LogLevel.Error);
            return false;
        }

        // プロジェクトの reflect DLL があれば、インプロセス Play で «ユーザーコンポーネント» も実行する。
        string? dll = _project != null ? BuildService.ReflectDllPath(_project) : null;
        if (dll != null && System.IO.File.Exists(dll))
        {
            int r = EngineInterop.acs_editor_logic_play_start(Engine, dll);
            Log(r == 1 ? "▶ Play — 物理 + ユーザーロジック (インプロセス)。"
                       : $"▶ Play — 物理プレビュー (logic 起動失敗 {r})。");
        }
        else Log("▶ Play — 物理プレビュー開始。");
        return true;
    }

    private bool StopPlayMode()
    {
        if (Engine == IntPtr.Zero ||
            EngineInterop.acs_editor_play_state(Engine) == 0)
        {
            return false;
        }

        ResetGameInput();
        if (EngineInterop.acs_editor_logic_play_active(Engine) != 0)
            EngineInterop.acs_editor_logic_play_stop(Engine);
        EngineInterop.acs_editor_play_stop(Engine);    // 開始状態へ復元
        RefreshAfterSceneChange();                     // 復元後の位置/選択を UI に反映 (3D Inspector も再同期)
        ResumeSceneDocumentHistoryAfterSimulation();
        Log("⏹ Stop — 開始状態へ復元。");
        return true;
    }

    private void OnPlay(object sender, RoutedEventArgs e)   // Play / Stop トグル
    {
        if (Engine == IntPtr.Zero) return;
        if (EngineInterop.acs_editor_play_state(Engine) == 0)
            StartPlayMode();
        else
            StopPlayMode();
        UpdatePlayButtons();
    }

    private void OnPause(object sender, RoutedEventArgs e)  // Pause / Resume トグル
    {
        if (Engine == IntPtr.Zero) return;
        int st = EngineInterop.acs_editor_play_state(Engine);
        if (st == 1) { EngineInterop.acs_editor_play_set_paused(Engine, 1); Log("❚❚ Pause。"); }
        else if (st == 2) { EngineInterop.acs_editor_play_set_paused(Engine, 0); Log("▶ Resume。"); }
        UpdatePlayButtons();
    }

    private void OnStep(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        EngineInterop.acs_editor_play_step(Engine);        // 一時停止中のみ 1 フレーム進む
    }

    /// <summary>Preview: DLL ビルド不要でエンジンコンポーネントを editor 内ライブ実行(参照解決込み)。</summary>
    private void OnTogglePreview(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) { PreviewBtn.IsChecked = false; return; }
        if (PreviewBtn.IsChecked == true)
        {
            SuspendSceneDocumentHistoryForSimulation();
            int n = EngineInterop.acs_editor_preview_start(Engine);
            Log($"Preview 開始 (実コンポーネント {n} 個をライブ実行)", "Play", LogLevel.Success);
        }
        else
        {
            EngineInterop.acs_editor_preview_stop(Engine);
            BuildHierarchy();
            ResumeSceneDocumentHistoryAfterSimulation();
            Log("Preview 停止 (位置を復元)", "Play", LogLevel.Info);
        }
        ApplySceneViewModePresentation();
    }

    // ===== Scene / Game ビュータブ =====
    // Scene = 編集ビュー (グリッド/ギズモ等)。Game = ゲーム画面のみ (chrome 無し) で Play を再生。
    private void OnSceneTab(object sender, RoutedEventArgs e) => SetGameView(false);
    private void OnGameTab(object sender, RoutedEventArgs e)  => SetGameView(true);

    private BlueprintWindow? _bpWindow;

    /// <summary>Blueprint の独立ウィンドウを (必要なら作って) 返す。中央 HWND ビューポートとは別ウィンドウ。</summary>
    private BlueprintWindow EnsureBlueprintWindow()
    {
        if (_bpWindow == null)
        {
            _bpWindow = new BlueprintWindow { Owner = this };
            _bpWindow.Closed += (_, __) => _bpWindow = null;
        }
        return _bpWindow;
    }

    /// <summary>BlueprintWindow 内のグラフエディタ (配線/読込先)。アクセス時にウィンドウを用意する。</summary>
    private BlueprintEditor BlueprintHost => EnsureBlueprintWindow().Editor;

    /// <summary>「⛓ Blueprint」: 独立ウィンドウでノードグラフ + ブラックボードを開く。</summary>
    private void OnBlueprintTab(object sender, RoutedEventArgs e)
    {
        BlueprintTabBtn.IsChecked = false;   // モーメンタリ (中央タブではなく別ウィンドウ)
        var win = EnsureBlueprintWindow();
        BuildBlueprintPalette();             // リフレクションからパレット構築 + 配線 (ホットリロード後も最新)
        win.Show();
        if (win.WindowState == WindowState.Minimized) win.WindowState = WindowState.Normal;
        win.Activate();
        Log("⛓ Blueprint ウィンドウを開きました (左=ブラックボード / 中央=ノードグラフ)。");
    }

    /// <summary>
    /// Blueprint のノードパレットを構築して BlueprintHost へ渡す。
    /// ビルトインのイベント/フロー/サブシステムに加え、リフレクトされた BlueprintCallable
    /// メソッド (エンジン型 + ロード済みユーザー型) を «関数» ノードとして列挙する。
    /// </summary>
    private void BuildBlueprintPalette()
    {
        var ev   = System.Windows.Media.Color.FromRgb(0xB0, 0x3A, 0x46);   // イベント = 赤
        var flow = System.Windows.Media.Color.FromRgb(0x5A, 0x64, 0x72);   // フロー   = 灰
        var bus  = System.Windows.Media.Color.FromRgb(0x35, 0x7A, 0x55);   // サブシステム = 緑
        var fn   = System.Windows.Media.Color.FromRgb(0x2E, 0x5C, 0x8A);   // 関数     = 青
        var scn  = System.Windows.Media.Color.FromRgb(0x8A, 0x5C, 0x2E);   // シーン操作 = 茶
        var vbl  = System.Windows.Media.Color.FromRgb(0x6A, 0x4C, 0x8C);   // 変数     = 紫
        var mth  = System.Windows.Media.Color.FromRgb(0x3E, 0x6E, 0x5E);   // 演算/ベクトル = 暗緑
        var lgc  = System.Windows.Media.Color.FromRgb(0x70, 0x50, 0x3E);   // 論理     = 橙茶
        var cvt  = System.Windows.Media.Color.FromRgb(0x4A, 0x55, 0x6E);   // 変換     = 青灰
        var str  = System.Windows.Media.Color.FromRgb(0x8C, 0x3E, 0x74);   // 文字列   = 暗マゼンタ
        var cmp  = System.Windows.Media.Color.FromRgb(0x4E, 0x6E, 0x4A);   // 比較     = 緑
        var tim  = System.Windows.Media.Color.FromRgb(0x35, 0x6E, 0x7A);   // 時間/乱数 = 青緑

        static BlueprintEditor.BpPinSpec Ex(string n) => new(n, true);
        static BlueprintEditor.BpPinSpec Da(string n, string ty = "") => new(n, false, ty);   // ty="" = ワイルドカード
        var none = Array.Empty<BlueprintEditor.BpPinSpec>();

        var pal = new List<BlueprintEditor.BpNodeTemplate>
        {
            // イベント (実行の起点)。
            new("イベント", "On BeginPlay", ev, none, new[] { Ex("▶") }),
            new("イベント", "On Tick",      ev, none, new[] { Ex("▶"), Da("dt", "Float") }),
            new("イベント", "On Destroy",   ev, none, new[] { Ex("▶") }),
            new("イベント", "On Event",     ev, new[] { Da("channel", "String") }, new[] { Ex("▶") }),
            // フロー制御。
            new("フロー", "Branch",       flow, new[] { Ex("▶"), Da("cond", "Bool") }, new[] { Ex("True"), Ex("False") }),
            new("フロー", "Sequence",     flow, new[] { Ex("▶") },             new[] { Ex("0"), Ex("1"), Ex("2") }),
            new("フロー", "Print String", flow, new[] { Ex("▶"), Da("text", "String") }, new[] { Ex("▶") }),
            // サブシステム。
            new("サブシステム", "Publish Event", bus, new[] { Ex("▶"), Da("channel", "String") },                  new[] { Ex("▶") }),
            new("サブシステム", "Spawn Prefab",  bus, new[] { Ex("▶"), Da("path", "String"), Da("pos", "Vector") }, new[] { Ex("▶"), Da("spawned", "Object") }),
            new("サブシステム", "Spawn from Class", bus, new[] { Ex("▶"), Da("class", "Class"), Da("pos", "Vector") }, new[] { Ex("▶"), Da("spawned", "Object") }),   // 右クリックで生成クラス選択
            // シーン操作 (実ノードを編集=永続)。target はノード ID。
            new("シーン操作", "Set Position", scn, new[] { Ex("▶"), Da("target", "Object"), Da("x", "Float"), Da("y", "Float") }, new[] { Ex("▶") }),
            new("シーン操作", "Get Position", scn, new[] { Ex("▶"), Da("target", "Object") },                 new[] { Ex("▶"), Da("pos", "Vector") }),
            new("シーン操作", "Set Color",    scn, new[] { Ex("▶"), Da("target", "Object"), Da("r", "Float"), Da("g", "Float"), Da("b", "Float") }, new[] { Ex("▶") }),
            new("シーン操作", "Set Visible",  scn, new[] { Ex("▶"), Da("target", "Object"), Da("visible", "Bool") },  new[] { Ex("▶") }),
            new("シーン操作", "Set Scale",    scn, new[] { Ex("▶"), Da("target", "Object"), Da("sx", "Float"), Da("sy", "Float") }, new[] { Ex("▶") }),
            new("シーン操作", "Set Rotation", scn, new[] { Ex("▶"), Da("target", "Object"), Da("deg", "Float") },      new[] { Ex("▶") }),
            new("シーン操作", "Destroy",      scn, new[] { Ex("▶"), Da("target", "Object") },                 new[] { Ex("▶") }),
            new("シーン操作", "Reparent",     scn, new[] { Ex("▶"), Da("target", "Object"), Da("parent", "Object") },   new[] { Ex("▶") }),
            // 変数: 名前付きの値を保持 (Set) / 参照 (Get、pure)。実行ごとにクリア。
            new("変数", "Set Variable", vbl, new[] { Ex("▶"), Da("name", "String"), Da("value") }, new[] { Ex("▶") }),
            new("変数", "Get Variable", vbl, new[] { Da("name", "String") }, new[] { Da("value") }),
            new("変数", "Get Self",     vbl, none, new[] { Da("self", "Object") }),   // self = この BP の配置インスタンス (実行時に解決)
            // 演算 (pure: exec なし。要求時に実評価)。
            new("演算", "Math Expression", mth, none, new[] { Da("result", "Float") }),   // 右クリック「式を編集」で a*2+b 等 → 変数が入力ピンに
            new("演算", "Add",      mth, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Subtract", mth, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Multiply", mth, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Divide",   mth, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Modulo",   mth, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Min",      mth, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Max",      mth, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Power",    mth, new[] { Da("base", "Float"), Da("exp", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Clamp",    mth, new[] { Da("value", "Float"), Da("min", "Float"), Da("max", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Lerp",     mth, new[] { Da("a", "Float"), Da("b", "Float"), Da("t", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Abs",      mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Negate",   mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Sqrt",     mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Floor",    mth, new[] { Da("value", "Float") }, new[] { Da("result", "Int") }),
            new("演算", "Ceil",     mth, new[] { Da("value", "Float") }, new[] { Da("result", "Int") }),
            new("演算", "Round",    mth, new[] { Da("value", "Float") }, new[] { Da("result", "Int") }),
            new("演算", "Sign",     mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Sin",      mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Cos",      mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Tan",      mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Atan2",    mth, new[] { Da("y", "Float"), Da("x", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Exp",      mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Log",      mth, new[] { Da("value", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Deg To Rad", mth, new[] { Da("deg", "Float") }, new[] { Da("rad", "Float") }),
            new("演算", "Rad To Deg", mth, new[] { Da("rad", "Float") }, new[] { Da("deg", "Float") }),
            new("演算", "Map Range",  mth, new[] { Da("value", "Float"), Da("inMin", "Float"), Da("inMax", "Float"), Da("outMin", "Float"), Da("outMax", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Move Towards", mth, new[] { Da("current", "Float"), Da("target", "Float"), Da("step", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "Wrap",     mth, new[] { Da("value", "Float"), Da("min", "Float"), Da("max", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "PingPong", mth, new[] { Da("t", "Float"), Da("length", "Float") }, new[] { Da("result", "Float") }),
            new("演算", "SmoothStep", mth, new[] { Da("a", "Float"), Da("b", "Float"), Da("t", "Float") }, new[] { Da("result", "Float") }),
            // 文字列 (pure)。
            new("文字列", "Append",        str, new[] { Da("a", "String"), Da("b", "String") }, new[] { Da("result", "String") }),
            new("文字列", "String Length", str, new[] { Da("in", "String") },                   new[] { Da("length", "Int") }),
            new("文字列", "To Upper",      str, new[] { Da("in", "String") },                   new[] { Da("result", "String") }),
            new("文字列", "To Lower",      str, new[] { Da("in", "String") },                   new[] { Da("result", "String") }),
            new("文字列", "Contains",      str, new[] { Da("in", "String"), Da("sub", "String") }, new[] { Da("result", "Bool") }),
            new("文字列", "Replace",       str, new[] { Da("in", "String"), Da("from", "String"), Da("to", "String") }, new[] { Da("result", "String") }),
            new("文字列", "Substring",     str, new[] { Da("in", "String"), Da("start", "Int"), Da("count", "Int") }, new[] { Da("result", "String") }),
            new("文字列", "Format Text",   str, new[] { Da("format", "String"), Da("arg0"), Da("arg1"), Da("arg2") }, new[] { Da("result", "String") }),   // UE: {0}{1}{2} 置換
            new("文字列", "To Text",          str, new[] { Da("in", "String") },     new[] { Da("result", "Text") }),       // FString → FText (表示/ローカライズ用)
            new("文字列", "Make Literal Text", str, new[] { Da("value", "String") }, new[] { Da("result", "Text") }),       // 定数 FText
            // 乱数 (pure)。
            new("乱数", "Random Float", tim, new[] { Da("min", "Float"), Da("max", "Float") }, new[] { Da("result", "Float") }),
            new("乱数", "Random Int",   tim, new[] { Da("min", "Int"), Da("max", "Int") },     new[] { Da("result", "Int") }),
            new("乱数", "Random Bool",  tim, none,                                             new[] { Da("result", "Bool") }),
            // 時間 (pure)。
            new("時間", "Get Delta Time", tim, none, new[] { Da("dt", "Float") }),
            new("時間", "Get Time",       tim, none, new[] { Da("time", "Float") }),
            // 論理 (pure)。
            new("論理", "And", lgc, new[] { Da("a", "Bool"), Da("b", "Bool") }, new[] { Da("result", "Bool") }),
            new("論理", "Or",  lgc, new[] { Da("a", "Bool"), Da("b", "Bool") }, new[] { Da("result", "Bool") }),
            new("論理", "Not", lgc, new[] { Da("in", "Bool") },                 new[] { Da("result", "Bool") }),
            new("論理", "Is Valid", lgc, new[] { Da("object", "Object") },       new[] { Da("result", "Bool") }),   // UE: null チェック
            // 比較 (pure: 各演算子ごとに独立ノード。a,b は Float)。
            new("比較", "Greater",       cmp, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Bool") }),
            new("比較", "Less",          cmp, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Bool") }),
            new("比較", "Greater Equal", cmp, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Bool") }),
            new("比較", "Less Equal",    cmp, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Bool") }),
            new("比較", "Equal",         cmp, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Bool") }),
            new("比較", "Not Equal",     cmp, new[] { Da("a", "Float"), Da("b", "Float") }, new[] { Da("result", "Bool") }),
            new("比較", "In Range",      cmp, new[] { Da("value", "Float"), Da("min", "Float"), Da("max", "Float") }, new[] { Da("result", "Bool") }),   // UE: min<=x<=max
            new("比較", "In Range Exclusive", cmp, new[] { Da("value", "Float"), Da("min", "Float"), Da("max", "Float") }, new[] { Da("result", "Bool") }),
            // ベクトル (pure)。
            new("ベクトル", "Make Vector",  mth, new[] { Da("x", "Float"), Da("y", "Float") }, new[] { Da("vector", "Vector") }),
            new("ベクトル", "Break Vector", mth, new[] { Da("in", "Vector") },                 new[] { Da("x", "Float"), Da("y", "Float") }),
            new("ベクトル", "Make Vector3",  mth, new[] { Da("x", "Float"), Da("y", "Float"), Da("z", "Float") }, new[] { Da("vector", "Vector3") }),
            new("ベクトル", "Break Vector3", mth, new[] { Da("in", "Vector3") }, new[] { Da("x", "Float"), Da("y", "Float"), Da("z", "Float") }),
            new("ベクトル", "Vector Add",      mth, new[] { Da("a", "Vector"), Da("b", "Vector") }, new[] { Da("result", "Vector") }),
            new("ベクトル", "Vector Subtract", mth, new[] { Da("a", "Vector"), Da("b", "Vector") }, new[] { Da("result", "Vector") }),
            new("ベクトル", "Vector Scale",    mth, new[] { Da("v", "Vector"), Da("s", "Float") },   new[] { Da("result", "Vector") }),
            new("ベクトル", "Vector Length",   mth, new[] { Da("v", "Vector") },                     new[] { Da("length", "Float") }),
            new("ベクトル", "Vector Distance", mth, new[] { Da("a", "Vector"), Da("b", "Vector") }, new[] { Da("distance", "Float") }),
            new("ベクトル", "Vector Dot",      mth, new[] { Da("a", "Vector"), Da("b", "Vector") }, new[] { Da("result", "Float") }),
            new("ベクトル", "Vector Normalize",mth, new[] { Da("v", "Vector") },                     new[] { Da("result", "Vector") }),
            // 型変換 (pure)。
            new("変換", "To String", cvt, new[] { Da("in") },             new[] { Da("out", "String") }),
            new("変換", "To Float",  cvt, new[] { Da("in") },             new[] { Da("out", "Float") }),
            new("変換", "To Int",    cvt, new[] { Da("in") },             new[] { Da("out", "Int") }),
            new("変換", "To Bool",   cvt, new[] { Da("in") },             new[] { Da("out", "Bool") }),
            new("変換", "To Vector3",cvt, new[] { Da("in", "Vector") },   new[] { Da("out", "Vector3") }),
            new("変換", "To Vector2",cvt, new[] { Da("in", "Vector3") },  new[] { Da("out", "Vector") }),
            // ループ。
            new("ループ", "For Loop", flow, new[] { Ex("▶"), Da("first", "Int"), Da("last", "Int"), Ex("Break") }, new[] { Ex("Loop Body"), Da("index", "Int"), Ex("Completed") }),
            new("ループ", "While",    flow, new[] { Ex("▶"), Da("cond", "Bool"), Ex("Break") },                    new[] { Ex("Loop Body"), Ex("Completed") }),
            // 関数 (Custom Event = 直接呼び出し専用の入口 / Call Function = 同名を同期呼び出し)。
            new("関数", "Custom Event",   ev,  new[] { Da("name", "String") },                  new[] { Ex("▶") }),
            new("関数", "Call Function",  fn,  new[] { Ex("▶"), Da("name", "String") },         new[] { Ex("▶") }),
            new("関数", "Function Entry", bus, none,                                            new[] { Ex("▶") }),   // 関数グラフの入口 (右クリックで入力ピン追加)
            new("関数", "Return",         bus, new[] { Ex("▶") },                               none),                // 関数グラフの戻り (右クリックで出力ピン追加)
            new("マクロ", "Tunnel Entry",  bus, none,                          new[] { Ex("▶") }),   // マクロの入口 (複数 exec/data 出力可)
            new("マクロ", "Tunnel Exit",   bus, new[] { Ex("▶") },             none),                // マクロの出口 (複数 exec/data 入力可)
            new("マクロ", "Call Macro",    fn,  new[] { Ex("▶") },             new[] { Ex("▶") }),   // 右クリックでマクロを選択 → インライン展開
            // フロー制御 (状態付き)。
            new("フロー", "Gate",          flow, new[] { Ex("▶"), Ex("Open"), Ex("Close"), Ex("Toggle"), Da("Start Closed", "Bool") }, new[] { Ex("Exit") }),
            new("フロー", "DoOnce",        flow, new[] { Ex("▶"), Ex("Reset") },               new[] { Ex("Completed") }),
            new("フロー", "Do N Times",    flow, new[] { Ex("▶"), Da("n", "Int"), Ex("Reset") }, new[] { Ex("Exit"), Da("count", "Int") }),   // UE: N 回まで発火
            new("フロー", "FlipFlop",      flow, new[] { Ex("▶") },                            new[] { Ex("A"), Ex("B"), Da("is A", "Bool") }),
            new("フロー", "Switch on Int", flow, new[] { Ex("▶"), Da("selector", "Int") },     new[] { Ex("0"), Ex("1"), Ex("2"), Ex("Default") }),
            new("フロー", "Switch on String", flow, new[] { Ex("▶"), Da("selector", "String") }, new[] { Ex("Default") }),   // Case ピンは右クリックで追加 (ピン名=照合文字列)
            new("フロー", "Switch on Enum",   flow, new[] { Ex("▶"), Da("selector", "") },        new[] { Ex("Default") }),   // 右クリック「列挙型を選択」でエントリ別 exec 出力を生成
            new("列挙", "Make Literal Enum", str, none,                  new[] { Da("value", "") }),    // 右クリックで列挙型を選択
            new("列挙", "Enum to String",   str, new[] { Da("in", "") }, new[] { Da("result", "String") }),
            new("フロー", "MultiGate",     flow, new[] { Ex("▶"), Ex("Reset"), Da("Is Random", "Bool"), Da("Loop", "Bool") }, new[] { Ex("Out 0"), Ex("Out 1"), Ex("Out 2") }),   // 入るたび 1 本ずつ
            new("フロー", "ForEach",       flow, new[] { Ex("▶"), Da("array"), Ex("Break") },  new[] { Ex("Loop Body"), Da("element"), Da("index", "Int"), Ex("Completed") }),
            new("フロー", "Delay",         flow, new[] { Ex("▶"), Da("duration", "Float") },   new[] { Ex("Completed") }),
            new("フロー", "Retriggerable Delay", flow, new[] { Ex("▶"), Da("duration", "Float") }, new[] { Ex("Completed") }),   // 再入でタイマーをリセット (デバウンス)
            new("フロー", "Timeline",      flow, new[] { Ex("Play"), Ex("Stop"), Da("duration", "Float") }, new[] { Ex("Update"), Ex("Finished"), Da("value", "Float"), Ex("Event") }),   // 値を時間で動かす + イベント時刻で Event 発火 (右クリック編集)
            new("フロー", "Cast",          cvt,  new[] { Ex("▶"), Da("object", "Object") },    new[] { Ex("Success"), Ex("Failed"), Da("As", "Object") }),
            new("クラス", "Get Class",        cvt, new[] { Da("object", "Object") },                       new[] { Da("class", "Class") }),     // オブジェクトの実行時クラス (pure)
            new("クラス", "Class is Child Of", cvt, new[] { Da("class", "Class"), Da("parent", "Class") }, new[] { Da("result", "Bool") }),     // 型判定 (pure)
            // 配列 (カンマ区切り文字列で表現。pure)。
            new("配列", "Make Array",   mth, new[] { Da("a"), Da("b"), Da("c") },           new[] { Da("array") }),
            new("配列", "Array Add",    mth, new[] { Da("array"), Da("element") },          new[] { Da("array") }),
            new("配列", "Array Get",    mth, new[] { Da("array"), Da("index", "Int") },     new[] { Da("element") }),
            new("配列", "Array Length", mth, new[] { Da("array") },                         new[] { Da("length", "Int") }),
            new("論理", "Select",       lgc, new[] { Da("a"), Da("b"), Da("pick", "Bool") }, new[] { Da("result") }),
            // 入力イベント (実行は real input。シミュレーションでは自動起動しない)。
            new("入力", "On Key",     ev, new[] { Da("key", "String") }, new[] { Ex("▶") }),
            new("入力", "On Overlap", ev, new[] { Da("with", "String") }, new[] { Ex("▶"), Da("other", "Object") }),
            // 整理: Reroute (配線中継。pure passthrough)。
            new("整理", "Reroute", flow, new[] { Da("in") }, new[] { Da("out") }),
        };

        // リフレクトされた BlueprintCallable メソッド (古い ABI だと EntryPointNotFound → ビルトインのみ)。
        try
        {
            int mc = EngineInterop.acs_editor_method_count();
            for (int i = 0; i < mc; i++)
            {
                if ((EngineInterop.acs_editor_method_flags_at(i) & 1) == 0) continue;   // bit0 = BlueprintCallable
                string name  = EngineInterop.MethodName(i);
                if (string.IsNullOrEmpty(name)) continue;
                string owner = EngineInterop.MethodOwner(i);
                string title = string.IsNullOrEmpty(owner) ? name : $"{owner}.{name}";
                var ins = EngineInterop.acs_editor_method_argkind_at(i) != 0   // 引数ありなら arg ピンを足す
                    ? new[] { Ex("▶"), Da("target", "Object"), Da("arg") }
                    : new[] { Ex("▶"), Da("target", "Object") };
                var outs = EngineInterop.acs_editor_method_retkind_at(i) != 0   // 戻り値ありなら ret ピンを足す
                    ? new[] { Ex("▶"), Da("ret") }
                    : new[] { Ex("▶") };
                pal.Add(new("関数", title, fn, ins, outs));
            }
        }
        catch (Exception ex) { Log("Blueprint パレット: 反射メソッド列挙をスキップ (" + ex.GetType().Name + ")"); }

        BlueprintHost.SetPalette(pal);
        BlueprintHost.DefaultDir = _project != null
            ? System.IO.Path.Combine(_project.RootDir, "Assets") : null;   // 保存/開くダイアログの初期位置
        BlueprintHost.SourceDir = _project?.SourceDir;                     // C++ 生成先 (エンジン組み込み)
        BlueprintHost.BuildRequested = () => OnBuildProject(this, new RoutedEventArgs());   // 生成→エンジンビルド
        BlueprintHost.LogSink = Log;   // 実行トレースをコンソールへ
        BlueprintHost.InvokeMethod = InvokeBound;   // 関数ノード→target (無指定なら選択) ノードへ実呼出
        BlueprintHost.SpawnPrefab = SpawnPrefabFromGraph;   // Spawn Prefab ノード→実シーンへ生成
        BlueprintHost.BuiltinOp = BuiltinSceneOp;           // Set Position / Get Position / Destroy 等
    }

    /// <summary>
    /// Blueprint の組込シーン操作ノード実行時に呼ばれる束縛。反射 invoke と違い «実ノード» を
    /// 直接編集するので変更が永続する (Spawn Prefab と同系)。args[0] は対象ノード ID 文字列。
    /// </summary>
    private string? BuiltinSceneOp(string op, string[] args)
    {
        int id = ResolveTarget(args.Length >= 1 ? args[0] : "");   // 空/無効 target は self(_bpSelfId) へ
        if (Engine == IntPtr.Zero || id < 0) return null;
        switch (op)
        {
            case "SetPosition":
                if (args.Length < 3
                    || !float.TryParse(args[1].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out float px)
                    || !float.TryParse(args[2].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out float py)) return null;
                EngineInterop.acs_editor_node_get_transform(Engine, id, out _, out _, out float rot, out float sx, out float sy);
                EngineInterop.acs_editor_node_set_transform(Engine, id, px, py, rot, sx, sy);   // 回転/スケールは維持
                if (_selectedId == id) PopulateInspector(id);
                NotifySceneMutationPending();
                return "ok";
            case "GetPosition":
                EngineInterop.acs_editor_node_get_transform(Engine, id, out float gx, out float gy, out _, out _, out _);
                return $"{gx.ToString("0.##", CultureInfo.InvariantCulture)},{gy.ToString("0.##", CultureInfo.InvariantCulture)}";
            case "Destroy":
                EngineInterop.acs_editor_node_delete(Engine, id);
                BuildHierarchy();
                NotifySceneMutationPending();
                return "ok";
            case "SetColor":
                if (args.Length < 4 || !ParseF(args[1], out float cr) || !ParseF(args[2], out float cg) || !ParseF(args[3], out float cb)) return null;
                EngineInterop.acs_editor_node_get_color(Engine, id, out _, out _, out _, out float ca);   // alpha は維持
                EngineInterop.acs_editor_node_set_color(Engine, id, cr, cg, cb, ca);
                if (_selectedId == id) PopulateInspector(id);
                NotifySceneMutationPending();
                return "ok";
            case "SetVisible":
                if (args.Length < 2) return null;
                string vs = args[1].Trim().ToLowerInvariant();
                EngineInterop.acs_editor_node_set_visible(Engine, id, (vs == "1" || vs == "true" || vs == "on" || vs == "yes") ? 1 : 0);
                if (_selectedId == id) PopulateInspector(id);
                NotifySceneMutationPending();
                return "ok";
            case "SetScale":
                if (args.Length < 3 || !ParseF(args[1], out float ssx) || !ParseF(args[2], out float ssy)) return null;
                EngineInterop.acs_editor_node_get_transform(Engine, id, out float kx, out float ky, out float krot, out _, out _);
                EngineInterop.acs_editor_node_set_transform(Engine, id, kx, ky, krot, ssx, ssy);   // 位置/回転は維持
                if (_selectedId == id) PopulateInspector(id);
                NotifySceneMutationPending();
                return "ok";
            case "SetRotation":
                if (args.Length < 2 || !ParseF(args[1], out float deg)) return null;
                EngineInterop.acs_editor_node_get_transform(Engine, id, out float rx, out float ry, out _, out float rsx, out float rsy);
                EngineInterop.acs_editor_node_set_transform(Engine, id, rx, ry, deg, rsx, rsy);   // 位置/スケールは維持
                if (_selectedId == id) PopulateInspector(id);
                NotifySceneMutationPending();
                return "ok";
            case "Reparent":
                int parentId = -1;
                if (args.Length >= 2) int.TryParse(args[1].Trim(), out parentId);   // 無効/空は root(-1)
                EngineInterop.acs_editor_node_reparent(Engine, id, parentId);
                BuildHierarchy();
                NotifySceneMutationPending();
                return "ok";
        }
        return null;
    }

    /// <summary>BP の «self» (実行中の配置インスタンス)。-1 のときは通常実行 (self バインドなし)。</summary>
    private int _bpSelfId = -1;

    /// <summary>target 文字列を解決する: 有効な id ならそれ、空/無効なら self(_bpSelfId)。
    /// 通常実行では _bpSelfId=-1 のため «明示 target 必須» の従来挙動になる。</summary>
    private int ResolveTarget(string target) =>
        (int.TryParse((target ?? "").Trim(), out var v) && v >= 0) ? v : _bpSelfId;

    private void OnRunBlueprints(object sender, RoutedEventArgs e) => RunPlacedBlueprints();

    /// <summary>配置された全 Blueprint インスタンス (.acsbp リンクを持つノード) のイベントグラフを、
    /// «自分自身を self» にして実行する (UE の BeginPlay 相当)。グラフを持たない BP は無視。</summary>
    private void RunPlacedBlueprints()
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        // インスタンスを先に集める (実行中に Spawn/Destroy で構造が変わっても走査を壊さない)。
        var insts = new System.Collections.Generic.List<(int id, string src)>();
        int cnt = EngineInterop.acs_editor_node_count(Engine);
        for (int i = 0; i < cnt; i++)
        {
            int nid = EngineInterop.acs_editor_node_id_at(Engine, i);
            string src = EngineInterop.NodePrefabSrc(Engine, nid);
            if (!string.IsNullOrEmpty(src) && IsBlueprint(src) && System.IO.File.Exists(src))
                insts.Add((nid, src));
        }
        if (insts.Count == 0)
        {
            Log("実行できる Blueprint インスタンスがありません (.acsbp をシーンに配置してください)。", "Play", LogLevel.Info);
            return;
        }

        // self バインドで各 .acsbp グラフを実行する transient インタプリタ (開いているグラフは触らない)。
        var bp = new BlueprintEditor { LogSink = Log, BuiltinOp = BuiltinSceneOp, InvokeMethod = InvokeBound, SpawnPrefab = SpawnPrefabFromGraph };
        int ran = 0;
        foreach (var (id, src) in insts)
        {
            string text;
            try { text = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8); }
            catch { continue; }
            bp.Deserialize(text);
            if (!bp.HasGraph) continue;   // コンポーネントのみの BP はスキップ
            _bpSelfId = id;
            try
            {
                Log($"▶ BP: {System.IO.Path.GetFileName(src)} (self=node {id})", "Play", LogLevel.Info);
                bp.RunGraph();
                ran++;
            }
            finally { _bpSelfId = -1; }
        }
        BuildHierarchy();
        if (_selectedId >= 0) PopulateInspector(_selectedId);
        Log($"▶ Blueprint 実行完了 — {ran}/{insts.Count} 個のインスタンスがグラフを実行。", "Play", LogLevel.Success);
    }

    private static bool ParseF(string s, out float v) =>
        float.TryParse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out v);

    /// <summary>
    /// Blueprint の «Spawn Prefab» ノード実行時に呼ばれる束縛。path のプレハブ (絶対 or
    /// プロジェクトの Assets 相対) を実シーンの root 配下へ生成し、pos "x,y" を適用して
    /// 生成ノードの id 文字列を返す (spawned 出力に流れる)。失敗は null。
    /// </summary>
    private string? SpawnPrefabFromGraph(string path, string pos)
    {
        if (Engine == IntPtr.Zero || string.IsNullOrWhiteSpace(path) || path.StartsWith("(")) return null;
        string full = path;
        if (!System.IO.File.Exists(full) && _project != null)
            full = System.IO.Path.Combine(_project.RootDir, "Assets", path);
        string text;
        try { text = System.IO.File.ReadAllText(full, System.Text.Encoding.UTF8); }
        catch { return null; }
        int id = EngineInterop.acs_editor_paste_subtree(Engine, text, -1);   // root 配下
        if (id < 0) return null;
        EngineInterop.acs_editor_node_set_prefab_src(Engine, id, full);
        var xy = pos.Split(',');
        if (xy.Length >= 2
            && float.TryParse(xy[0].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out float px)
            && float.TryParse(xy[1].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out float py))
            EngineInterop.acs_editor_node_set_transform(Engine, id, px, py, 0f, 1f, 1f);
        BuildHierarchy();
        NotifySceneMutationPending();
        return id.ToString();
    }

    /// <summary>
    /// Blueprint の «関数» ノード実行時に呼ばれる束縛。target ピンにノード ID があればその
    /// ノード、無ければ現在の選択ノード (=self) のコンポーネントから owner 型名が一致する
    /// slot を探し、その反射メソッドを arg 付きで実呼出する。戻り値文字列を返す (void は "")。
    /// 束縛できなければ null。
    /// </summary>
    private string? InvokeBound(string ownerType, string method, string target, string arg)
    {
        int nodeId = ResolveTarget(target);            // target → self(_bpSelfId)
        if (nodeId < 0) nodeId = _selectedId;          // self も無ければ選択ノード (従来挙動)
        if (Engine == IntPtr.Zero || nodeId < 0) return null;
        int cc = EngineInterop.acs_editor_node_component_count(Engine, nodeId);
        for (int s = 0; s < cc; s++)
        {
            if (EngineInterop.ComponentName(Engine, nodeId, s) == ownerType)
            {
                if (!EngineInterop.InvokeMethodRet(
                        Engine,
                        nodeId,
                        s,
                        method,
                        arg ?? "",
                        out string ret))
                {
                    return null;
                }

                NotifySceneMutationPending(
                    $"Invoke {ownerType}.{method}",
                    $"component.{s}.method.{ownerType}.{method}",
                    nodeId);
                return ret;
            }
        }
        return null;
    }

    private void SetGameView(bool game)
    {
        // Blueprint は独立ウィンドウなので中央は常にビューポート。
        BlueprintTabBtn.IsChecked = false;
        SceneTools.Visibility     = Visibility.Visible;
        if (!game && DetachedCameraViewOwnsLiveSurface)
        {
            SceneTabBtn.IsChecked = false;
            GameTabBtn.IsChecked = true;
            Log(
                "Re-dock Camera View before switching the one live renderer " +
                "surface back to Scene View.",
                "Camera",
                LogLevel.Info);
            return;
        }
        if (Engine == IntPtr.Zero) { SceneTabBtn.IsChecked = true; GameTabBtn.IsChecked = false; return; }
        EditorViewSwitchPlan plan = EditorViewSwitchPolicy.Plan(
            game,
            EngineInterop.acs_editor_play_state(Engine));
        System.Diagnostics.Debug.Assert(
            !plan.StartPlay &&
            !plan.StopPlay &&
            !plan.MutateEditorNavigationCamera);
        ResetGameInput();
        SceneTabBtn.IsChecked = !game;
        GameTabBtn.IsChecked  = game;
        EngineInterop.acs_editor_set_game_view(Engine, game ? 1 : 0);
        if (game)
        {
            Log(EngineInterop.acs_editor_play_state(Engine) == 0
                ? "▶ Game View — authored game camera preview (Play is stopped)."
                : "▶ Game View — simulation continues through the authored game camera.");
        }
        else
        {
            Log(EngineInterop.acs_editor_play_state(Engine) == 0
                ? "◳ Scene View — editor navigation camera."
                : "◳ Scene View — editor navigation camera; simulation continues.");
        }
        UpdatePlayButtons();
    }

    private void UpdatePlayButtons()
    {
        int st = Engine != IntPtr.Zero ? EngineInterop.acs_editor_play_state(Engine) : 0;
        PlayBtn.Content    = st == 0 ? "Play" : "Stop";
        PauseBtn.Content   = st == 2 ? "Resume" : "Pause";
        PauseBtn.IsEnabled = st != 0;
        StepBtn.IsEnabled  = st == 2;
        UpdatePlayStatePresentation();
    }

    private void OnSnapToggle(object sender, RoutedEventArgs e)
    {
        ApplySnapSettings(logChange: true);
    }

    private void OnFocus(object sender, RoutedEventArgs e)
    {
        if (Engine != IntPtr.Zero) { EngineInterop.acs_editor_camera_focus(Engine); Log("Focus on selection."); }
    }

    // ===== ポリゴン描画ツール: クリックで頂点 → Enter/Esc で閉じてポリゴン化 =====
    private void OnPolyToggle(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _viewport == null) return;
        if (PolyBtn.IsChecked == true)
        {
            if (_view3d) EngineInterop.acs_editor_poly3d_begin(Engine);
            else         EngineInterop.acs_editor_poly_begin(Engine);
            _viewport.PolyMode = true;
            Log(_view3d
                ? "ポリゴン描画 (Ortho 推奨): クリックで z=0 平面に頂点、Enter/Esc で確定。"
                : "ポリゴン描画: ビューポートをクリックで頂点を置き、Enter か Esc で閉じる。");
        }
        else
        {
            if (_view3d) EngineInterop.acs_editor_poly3d_cancel(Engine);
            else         EngineInterop.acs_editor_poly_cancel(Engine);
            _viewport.PolyMode = false;
            Log("ポリゴン描画をキャンセル。");
        }
    }

    private void FinalizePoly()
    {
        if (Engine == IntPtr.Zero || _viewport == null || !_viewport.PolyMode) return;
        int id = _view3d ? EngineInterop.acs_editor_poly3d_finalize(Engine)
                         : EngineInterop.acs_editor_poly_finalize(Engine);
        _viewport.PolyMode = false;
        PolyBtn.IsChecked = false;
        if (id >= 0)
        {
            BuildHierarchy();
            if (_view3d) { Select3DInHierarchy(id); Populate3DInspector(id); }
            else SyncSelectionUi();
            RecordSceneDocumentChange("Create Polygon");
            Log($"ポリゴンを作成しました (node {id})。");
        }
        else Log("ポリゴンには頂点が 3 つ以上必要です。");
    }

    // 描画中の Enter/Esc でポリゴンを確定する。
    private void OnGlobalKeyDown(object sender, KeyEventArgs e)
    {
        // A visible unattended validation window must never react to keys
        // intended for the user's foreground application.  The active/focus
        // checks also prevent ordinary editor shortcuts from firing after a
        // transient focus hand-off.
        if (EditorInputGate.ShouldSuppressShortcuts(
                App.IsNonInteractiveLaunch, IsActive, IsKeyboardFocusWithin))
            return;

        const ModifierKeys shortcutModifiers =
            ModifierKeys.Control | ModifierKeys.Shift | ModifierKeys.Alt | ModifierKeys.Windows;
        ModifierKeys modifiers = Keyboard.Modifiers & shortcutModifiers;
        if (e.Key == Key.P &&
            modifiers ==
            (ModifierKeys.Control | ModifierKeys.Shift))
        {
            ShowCommandPalette();
            e.Handled = true;
            return;
        }
        if (e.Key == Key.W &&
            modifiers ==
            (ModifierKeys.Control | ModifierKeys.Alt))
        {
            OnManageWorkspaces(this, new RoutedEventArgs());
            e.Handled = true;
            return;
        }
        if (IsSceneEditingBlocked)
        {
            // The native child HWND and routed command bindings have independent input paths.
            // Swallow the Window-level route as a second boundary while the scene workspace is
            // disabled so no shortcut can mutate a suspended document.
            e.Handled = true;
            return;
        }
        if (_viewport != null && _viewport.PolyMode && (e.Key == Key.Enter || e.Key == Key.Escape))
        {
            FinalizePoly();
            e.Handled = true;
            return;
        }
        // Exact routing matters: Ctrl+F5 is Standalone Run and must be resolved before plain F5.
        // Build shortcuts are rejected while a text editor owns focus; mouse/menu activation first
        // commits LostKeyboardFocus handlers, but a preview-key shortcut would otherwise serialize
        // stale inspector values.
        BuildShortcutAction buildShortcut =
            EditorShortcutRouting.ResolveBuildShortcut(e.Key, modifiers);
        if (buildShortcut == BuildShortcutAction.StandaloneRun)
        {
            if (CanRunBuildShortcut("Ctrl+F5"))
                OnRunProject(this, new RoutedEventArgs());
            e.Handled = true;
        }
        else if (buildShortcut == BuildShortcutAction.BuildAndRun)
        {
            if (CanRunBuildShortcut("F5"))
                OnBuildAndRun(this, new RoutedEventArgs());
            e.Handled = true;
        }
        else if (buildShortcut == BuildShortcutAction.Build)
        {
            if (CanRunBuildShortcut("F7"))
                OnBuildProject(this, new RoutedEventArgs());
            e.Handled = true;
        }
        // Ctrl+D = 選択ノードを複製 (テキスト編集中は除く)。Duplicate は selection ベースで 2D/3D 両対応。
        else if (e.Key == Key.D && (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control
                 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            OnDuplicateNode(this, new RoutedEventArgs());
            e.Handled = true;
        }
        // Ctrl+Shift+S = 初期化済みの全シーン保存。Ctrl+S はアクティブ文書だけを保存する。
        // 修飾キーを厳密に分け、Ctrl+Shift+S が先に Ctrl+S として処理されないようにする。
        else if (e.Key == Key.S &&
                 (Keyboard.Modifiers & (ModifierKeys.Control | ModifierKeys.Shift | ModifierKeys.Alt)) ==
                 (ModifierKeys.Control | ModifierKeys.Shift)
                 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            OnSaveAllScenes(this, new RoutedEventArgs());
            e.Handled = true;
        }
        // Ctrl+S = シーン保存 / Ctrl+N = 新規シーン (テキスト編集中は除く)。
        else if (e.Key == Key.S &&
                 (Keyboard.Modifiers & (ModifierKeys.Control | ModifierKeys.Shift | ModifierKeys.Alt)) ==
                 ModifierKeys.Control
                 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            OnSaveScene(this, new RoutedEventArgs());
            e.Handled = true;
        }
        else if (e.Key == Key.N && (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control
                 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            OnNewScene(this, new RoutedEventArgs());
            e.Handled = true;
        }
        // Ctrl+J = presentation-only bottom dock collapse/restore. Individual
        // tool Docked/Floating/Hidden states remain unchanged.
        else if (e.Key == Key.J && (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control
                 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            ToggleBottomDockPresentationFromUser();
            e.Handled = true;
        }
        // Ctrl+X = カット (テキスト編集中は除く)。Copy は CommandBinding だが Cut は Click ハンドラのため手動配線。
        else if (e.Key == Key.X && (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control
                 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            OnCut(this, new RoutedEventArgs());
            e.Handled = true;
        }
        // F2 = 選択ノードを Name 欄でリネーム (テキスト編集中は除く)。3D は 3D インスペクタの Name 欄。
        else if (e.Key == Key.F2 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            if (_view3d) { _name3dBox?.Focus(); _name3dBox?.SelectAll(); }
            else if (CurSelCount() > 0) { NameBox.Focus(); NameBox.SelectAll(); }
            e.Handled = true;
        }
        // Esc = 選択解除 (テキスト編集中は除く。PolyMode の Esc は上で処理済み)。2D=select_none / 3D=select3d(-1)。
        else if (e.Key == Key.Escape && Keyboard.FocusedElement is not System.Windows.Controls.TextBox)
        {
            if (_view3d) EngineInterop.acs_editor_select3d(Engine, -1);
            else         EngineInterop.acs_editor_select_none(Engine);
            SyncSelectionUi();
            e.Handled = true;
        }
        // Scene View のギズモ/ビュー ショートカット (UE5/Unity 流:
        // W=移動 / E=回転 / R=拡縮 / F=選択にフォーカス)。Play 中でも
        // editor navigation remains independent; Game View only routes gameplay input.
        else if (Engine != IntPtr.Zero
                 && EngineInterop.acs_editor_is_game_view(Engine) == 0
                 && Keyboard.FocusedElement is not System.Windows.Controls.TextBox
                 && (e.Key == Key.W || e.Key == Key.E || e.Key == Key.R || e.Key == Key.F))
        {
            if      (e.Key == Key.W) OnGizmoMove(this, new RoutedEventArgs());
            else if (e.Key == Key.E) OnGizmoRotate(this, new RoutedEventArgs());
            else if (e.Key == Key.R) OnGizmoScale(this, new RoutedEventArgs());
            else if (e.Key == Key.F) OnFocus(this, new RoutedEventArgs());
            e.Handled = true;
        }
        // インプロセス Play 中はゲーム入力を DLL の acs::Input へフィードする (オートリピートは無視)。
        else if (!e.IsRepeat) FeedGameKey(e.Key, true);
    }

    private bool CanRunBuildShortcut(string shortcut)
    {
        if (Keyboard.FocusedElement is not System.Windows.Controls.Primitives.TextBoxBase &&
            Keyboard.FocusedElement is not System.Windows.Controls.PasswordBox)
            return true;

        string message =
            $"{shortcut} ignored: finish the current text edit with Enter or move focus first.";
        StatusText.Text = message;
        Log(message, "Build", LogLevel.Warn);
        return false;
    }

    // Play 中のキー解放を DLL へフィードする。
    private void OnGlobalKeyUp(object sender, KeyEventArgs e)
    {
        if (EditorInputGate.ShouldSuppressShortcuts(
                App.IsNonInteractiveLaunch, IsActive, IsKeyboardFocusWithin))
            return;
        if (IsSceneEditingBlocked) return;
        FeedGameKey(e.Key, false);
    }

    // WPF Key を acs::EKey 整数へマップし、Play 中なら DLL へフィードする。テキスト編集中
    // (TextBox にフォーカス) は誤爆を避けてスキップする。
    private void FeedGameKey(Key key, bool down)
    {
        if (Engine == IntPtr.Zero) return;
        if (!EngineViewport.ShouldRouteGameplayInput(
                EngineInterop.acs_editor_is_game_view(Engine) != 0,
                EngineInterop.acs_editor_logic_play_active(Engine) != 0))
            return;
        if (Keyboard.FocusedElement is System.Windows.Controls.TextBox) return;
        int ek = KeyFromWpf(key);
        if (ek != 0) EngineInterop.acs_editor_logic_input_key(Engine, ek, down ? 1 : 0);
    }

    private void ResetGameInput()
    {
        if (Engine != IntPtr.Zero)
            EngineInterop.acs_editor_logic_input_reset(Engine);
    }

    // WPF System.Windows.Input.Key → acs::EKey の整数値 (enum 順: Unknown=0, A=1..Z=26,
    // Num0=27.., F1=37.., LeftShift=49.., Up=57/Down=58/Left=59/Right=60, Space=61, Enter=62,
    // Tab=63, Backspace=64, Escape=65)。未対応は 0。
    private static int KeyFromWpf(Key k)
    {
        if (k >= Key.A && k <= Key.Z) return (int)(k - Key.A) + 1;        // A..Z → 1..26
        if (k >= Key.D0 && k <= Key.D9) return (int)(k - Key.D0) + 27;    // 0..9 → 27..36
        switch (k)
        {
            case Key.LeftShift:  return 49;
            case Key.RightShift: return 50;
            case Key.LeftCtrl:   return 51;
            case Key.RightCtrl:  return 52;
            case Key.LeftAlt:    return 53;
            case Key.RightAlt:   return 54;
            case Key.Up:         return 57;
            case Key.Down:       return 58;
            case Key.Left:       return 59;
            case Key.Right:      return 60;
            case Key.Space:      return 61;
            case Key.Enter:      return 62;
            case Key.Tab:        return 63;
            case Key.Back:       return 64;
            case Key.Escape:     return 65;
            default:             return 0;
        }
    }

    // ===== プロジェクトのビルド / 実行 (スタンドアロン) =====
    // 新規クラス/ソースを生成する (基底選択。Empty=空クラス、それ以外は <IDENT>_API エクスポート)。
    private async void OnNewClass(object sender, RoutedEventArgs e)
    {
        if (_project == null) { Log("プロジェクトがありません。"); return; }
        var dlg = new NewClassDialog(_project) { Owner = this };
        if (dlg.ShowDialog() != true) return;
        try
        {
            var made = ProjectManager.GenerateClass(_project, dlg.ClassName, dlg.BaseClass);
            _pendingReconfigure = true;   // 新ファイルを CMake に拾わせる (次ビルドで再 configure)
            ShowBottomTab("build");
            BuildLog($"生成: {string.Join(", ", made.ConvertAll(System.IO.Path.GetFileName))}");
            if (dlg.BuildAfter)
            {
                // VS を開かずに «生成 → ビルド → 型反映» を 1 アクションで完結 (codegen + reflect DLL + LoadUserTypes)。
                BuildLog("生成後ビルドを開始します…");
                await DoBuild(run: false);
            }
            else if (dlg.BaseClass == "AComponent")
                BuildLog("Build または Hot Reload で『ユーザー定義のオブジェクト』に追加されます。");
        }
        catch (Exception ex)
        {
            BuildLog("クラス生成に失敗: " + ex.Message);
            MessageBox.Show(this, ex.Message, "クラス生成に失敗", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private async void OnBuildProject(object sender, RoutedEventArgs e) => await DoBuild(run: false);
    private async void OnBuildAndRun(object sender, RoutedEventArgs e) => await DoBuild(run: true);

    // メニュー Run (Ctrl+F5): スタンドアロン exe をフルビルドして «別ウィンドウ» で起動する
    // (= 出荷ビルドの確認用)。通常のイテレーションは Build & Run (F5) → Game View タブを使う。
    private async void OnRunProject(object sender, RoutedEventArgs e)
    {
        if (IsSceneEditingBlocked) return;
        if (_project == null) { Log("プロジェクトがありません。"); return; }
        if (_building) { BuildLog("ビルド実行中です。"); return; }
        if (!EnsureBuildSceneCompatibility("Standalone Run")) return;
        ShowBottomTab("build");
        BuildLog($"==== Build Standalone: {_project.Name} ====");
        using BuildWorkflowLease workflow = BeginBuildWorkflow();
        using EditorOperationSession operation = BeginEditorOperation(
            EditorOperationService.Build,
            EditorOperationCodes.BuildStarted,
            $"Standalone build started for {_project.Name}.");
        _building = true;
        SetBuildUiEnabled(false);
        try
        {
            // Autosave cleanup is asynchronous; reserve the build slot before awaiting it.
            if (!await SaveDocumentsForBuildAsync(workflow.Token))
            {
                if (workflow.IsCancellationRequested)
                {
                    operation.Cancel(
                        EditorOperationCodes.BuildCancelled,
                        "Standalone build was cancelled while saving required editor documents.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
                else
                {
                    operation.Fail(
                        EditorOperationCodes.BuildSceneSaveFailed,
                        "Standalone build stopped because required editor documents could not be saved.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
                return;
            }
            workflow.Token.ThrowIfCancellationRequested();
            bool force = _pendingReconfigure;
            _pendingReconfigure = false;
            string? exe = await BuildService.BuildAsync(
                _project,
                BuildLog,
                force,
                standalone: true,
                cancellationToken: workflow.Token);
            workflow.Token.ThrowIfCancellationRequested();
            if (exe != null)
            {
                LoadUserTypes();
                workflow.Token.ThrowIfCancellationRequested();
                await StopGameProcessForReplacementAsync(
                    _gameProcess,
                    workflow.Token);
                workflow.Token.ThrowIfCancellationRequested();
                _gameProcess = BuildService.Run(_project, BuildLog);
                if (_gameProcess == null)
                {
                    operation.Fail(
                        EditorOperationCodes.BuildLaunchFailed,
                        "Standalone build completed but the game process could not be launched.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: exe);
                }
                else
                {
                    operation.Succeed(
                        EditorOperationCodes.BuildSucceeded,
                        "Standalone build completed and the game process was launched.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: exe);
                }
            }
            else
            {
                operation.Fail(
                    EditorOperationCodes.BuildFailed,
                    "Standalone build did not produce an executable.",
                    assetId: _project.CanonicalSceneAssetId,
                    path: _project.ProjectFilePath);
            }
        }
        catch (OperationCanceledException)
            when (workflow.IsCancellationRequested)
        {
            operation.Cancel(
                EditorOperationCodes.BuildCancelled,
                "Standalone build/run was cancelled during editor shutdown.",
                assetId: _project.CanonicalSceneAssetId,
                path: _project.ProjectFilePath);
            BuildLog("Standalone Build/Run cancelled during editor shutdown.");
        }
        catch (Exception ex)
        {
            if (workflow.IsCancellationRequested)
            {
                operation.Cancel(
                    EditorOperationCodes.BuildCancelled,
                    "Standalone build/run stopped during cancellation: " +
                    ex.Message,
                    assetId: _project.CanonicalSceneAssetId,
                    path: _project.ProjectFilePath);
            }
            else
            {
                operation.Fail(
                    EditorOperationCodes.BuildFailed,
                    ex.Message,
                    assetId: _project.CanonicalSceneAssetId,
                    path: _project.ProjectFilePath);
            }
            BuildLog("Build エラー: " + ex.Message);
        }
        finally
        {
            if (!operation.IsCompleted)
            {
                if (workflow.IsCancellationRequested)
                {
                    operation.Cancel(
                        EditorOperationCodes.BuildCancelled,
                        "Standalone build ended during cancellation.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
                else
                {
                    operation.Fail(
                        EditorOperationCodes.BuildFailed,
                        "Standalone build ended without a terminal result.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
            }
            _building = false;
            SetBuildUiEnabled(true);
        }
    }

    private async System.Threading.Tasks.Task DoBuild(bool run)
    {
        if (IsSceneEditingBlocked) return;
        if (_project == null) { BuildLog("プロジェクトがありません。"); return; }
        if (_building) { BuildLog("ビルド実行中です。"); return; }
        if (!EnsureBuildSceneCompatibility(run ? "Build & Run" : "Build")) return;
        ShowBottomTab("build");
        BuildLog($"==== Build: {_project.Name} ====");
        using BuildWorkflowLease workflow = BeginBuildWorkflow();
        using EditorOperationSession operation = BeginEditorOperation(
            EditorOperationService.Build,
            EditorOperationCodes.BuildStarted,
            $"{(run ? "Build and Run" : "Build")} started for {_project.Name}.");
        _building = true;
        SetBuildUiEnabled(false);
        try
        {
            // Source save and recovery cleanup are part of the build transaction.
            if (!await SaveDocumentsForBuildAsync(workflow.Token))
            {
                if (workflow.IsCancellationRequested)
                {
                    operation.Cancel(
                        EditorOperationCodes.BuildCancelled,
                        "Build was cancelled while saving required editor documents.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
                else
                {
                    operation.Fail(
                        EditorOperationCodes.BuildSceneSaveFailed,
                        "Build stopped because required editor documents could not be saved.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
                return;
            }
            workflow.Token.ThrowIfCancellationRequested();
            bool force = _pendingReconfigure;
            _pendingReconfigure = false;
            string? exe = await BuildService.BuildAsync(
                _project,
                BuildLog,
                force,
                cancellationToken: workflow.Token);
            workflow.Token.ThrowIfCancellationRequested();
            if (exe != null)
            {
                LoadUserTypes();          // リフレクション DLL からユーザー定義型を取り込む
                workflow.Token.ThrowIfCancellationRequested();
                if (run)
                {
                    workflow.Token.ThrowIfCancellationRequested();
                    await StopGameProcessForReplacementAsync(
                        _gameProcess,
                        workflow.Token);
                    workflow.Token.ThrowIfCancellationRequested();
                    RunGame();
                }
                operation.Succeed(
                    EditorOperationCodes.BuildSucceeded,
                    run
                        ? "Build completed and Game View was started."
                        : "Build completed successfully.",
                    assetId: _project.CanonicalSceneAssetId,
                    path: exe);
            }
            else
            {
                operation.Fail(
                    EditorOperationCodes.BuildFailed,
                    "Build did not produce the requested artifact.",
                    assetId: _project.CanonicalSceneAssetId,
                    path: _project.ProjectFilePath);
            }
        }
        catch (OperationCanceledException)
            when (workflow.IsCancellationRequested)
        {
            operation.Cancel(
                EditorOperationCodes.BuildCancelled,
                "Build was cancelled during editor shutdown.",
                assetId: _project.CanonicalSceneAssetId,
                path: _project.ProjectFilePath);
            BuildLog("Build cancelled during editor shutdown.");
        }
        catch (Exception ex)
        {
            if (workflow.IsCancellationRequested)
            {
                operation.Cancel(
                    EditorOperationCodes.BuildCancelled,
                    "Build stopped during cancellation: " + ex.Message,
                    assetId: _project.CanonicalSceneAssetId,
                    path: _project.ProjectFilePath);
            }
            else
            {
                operation.Fail(
                    EditorOperationCodes.BuildFailed,
                    ex.Message,
                    assetId: _project.CanonicalSceneAssetId,
                    path: _project.ProjectFilePath);
            }
            BuildLog("Build エラー: " + ex.Message);
        }
        finally
        {
            if (!operation.IsCompleted)
            {
                if (workflow.IsCancellationRequested)
                {
                    operation.Cancel(
                        EditorOperationCodes.BuildCancelled,
                        "Build ended during cancellation.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
                else
                {
                    operation.Fail(
                        EditorOperationCodes.BuildFailed,
                        "Build ended without a terminal result.",
                        assetId: _project.CanonicalSceneAssetId,
                        path: _project.ProjectFilePath);
                }
            }
            _building = false;
            SetBuildUiEnabled(true);
        }
    }

    private async System.Threading.Tasks.Task<bool> SaveDocumentsForBuildAsync(
        System.Threading.CancellationToken cancellationToken)
    {
        if (!await SaveProjectSettingsForBuildAsync(cancellationToken))
            return false;
        cancellationToken.ThrowIfCancellationRequested();
        return await SaveSceneForBuildAsync();
    }

    // ビルド前に現在のシーンをプロジェクトの初期シーンへ保存する。
    // これでスタンドアロン (Build & Run) が編集中と同じシーンを読み込む。
    private async System.Threading.Tasks.Task<bool> SaveSceneForBuildAsync()
    {
        if (Engine == IntPtr.Zero || _project == null)
        {
            BuildLog("Scene save failed: the editor engine or project is unavailable. Build aborted.");
            return false;
        }
        if (ProjectManager.HasPendingInitialScenePathFollow(_project))
        {
            BuildLog(
                "Scene save failed: [INITIAL_SCENE_MOVE_PENDING] an interrupted initial-scene " +
                "move still requires recovery. Build/Run/Package was aborted.");
            return false;
        }
        if (!TryBeginSceneSourceSave(
                out SceneSourceSaveScope? saveScope,
                out string saveBlockedReason))
        {
            BuildLog(
                "Scene save failed: " + saveBlockedReason + ". " +
                "Build/Run/Package was aborted.");
            return false;
        }
        using SceneSourceSaveScope saveLease = saveScope!;

        // Keep this durability boundary fail-closed even if a future caller forgets the outer
        // compatibility guard. Runtime simulation data must never replace editable source either.
        if (!EnsureBuildSceneCompatibility("Scene save for build"))
            return false;
        bool use3D = _legacySceneSourceMode == SceneDocumentMode.ThreeD;
        SceneDocumentMode sourceMode =
            use3D ? SceneDocumentMode.ThreeD : SceneDocumentMode.TwoD;
        if (use3D ? !_scene3DInitialized : !_scene2DInitialized)
        {
            BuildLog(
                $"Scene save failed: the {(use3D ? ".acs3d" : ".acscene")} " +
                "source is not initialized. Build aborted.");
            return false;
        }
        if (EngineInterop.acs_editor_play_state(Engine) != 0 || PreviewBtn.IsChecked == true)
        {
            BuildLog(
                "Scene save failed: stop Play/Preview before Build, Run or Package. " +
                "Runtime simulation was not written to source.");
            return false;
        }
        try
        {
            string target = SceneSourceFile.ResolveProjectSceneReference(
                _project.RootDir,
                _project.AssetsDir,
                _project.InitialScene,
                sourceMode);
            var configuredBuffer = new byte[1024];
            if (EngineInterop.acs_editor_settings_get_value(
                    Engine, "Game", "DefaultScene", configuredBuffer, configuredBuffer.Length) != 0)
            {
                string configured = EngineInterop.Utf8Z(configuredBuffer).Trim();
                if (configured.Length != 0)
                {
                    string configuredPath;
                    try
                    {
                        configuredPath = SceneSourceFile.ResolveProjectSceneReference(
                            _project.RootDir,
                            _project.AssetsDir,
                            configured,
                            sourceMode);
                    }
                    catch (Exception ex)
                    {
                        BuildLog(
                            "Scene save failed: [DEFAULT_SCENE_INVALID] Game.DefaultScene must " +
                            $"be a relative {(use3D ? ".acs3d" : ".acscene")} under Assets. " +
                            "Build/Run/Package was aborted.");
                        BuildLog($"Game.DefaultScene: {configured}");
                        BuildLog(ex.Message);
                        return false;
                    }
                    if (!SceneSourceFile.PathsEqual(configuredPath, target))
                    {
                        BuildLog(
                            "Scene save failed: [DEFAULT_SCENE_MISMATCH] Game.DefaultScene and " +
                            ".acsproject InitialScene resolve to different files. " +
                            "Build/Run/Package was aborted.");
                        BuildLog($"Game.DefaultScene: {configuredPath}");
                        BuildLog($".acsproject InitialScene: {target}");
                        return false;
                    }
                }
            }
            if (!string.IsNullOrWhiteSpace(_currentScenePath) &&
                !SceneSourceFile.PathsEqual(_currentScenePath, target))
            {
                BuildLog(
                    "Scene save failed: [ACS-BUILD-SCENE-TARGET-002] The active scene is not the " +
                    "project InitialScene. Build/Run/Package was aborted instead of shipping stale data.");
                BuildLog($"Active scene: {_currentScenePath}");
                BuildLog($"Configured InitialScene: {target}");
                return false;
            }

            if (!saveLease.TryAcquireProjectAssetMutationLock(
                    out string mutationBlockedReason))
            {
                BuildLog(
                    "Scene save failed: " + mutationBlockedReason + ". " +
                    "Build/Run/Package was aborted.");
                return false;
            }
            string? previousPath = _currentScenePath;
            string text = use3D
                ? EngineInterop.Scene3DText(Engine)
                : EngineInterop.SceneText(Engine);
            SceneSourceFile.WriteProjectSceneAtomicText(
                target,
                text,
                _project.RootDir,
                _project.AssetsDir,
                sourceMode);
            SetCurrentScenePath(target);
            MarkSceneClean(text);
            NotifySceneDocumentSaved(use3D, target);
            await OnSceneSourceSavedAsync(use3D, previousPath, target);
            BuildLog($"シーンを保存: {target}");
            return true;
        }
        catch (Exception ex)
        {
            BuildLog("Scene save failed; Build/Run/Package was aborted: " + ex.Message);
            return false;
        }
    }

    // リフレクション DLL からユーザー定義型を取り込み、生成メニュー / +Add 候補を更新する。
    private void LoadUserTypes()
    {
        if (Engine == IntPtr.Zero || _project == null) return;
        string dll = BuildService.ReflectDllPath(_project);
        if (!System.IO.File.Exists(dll)) return;
        int n = EngineInterop.acs_editor_load_game_dll(Engine, dll);
        if (n > 0)      BuildLog($"ユーザー定義型を {n} 件 読み込みました。");
        else if (n < 0) BuildLog($"ユーザー型 DLL の読み込みに失敗しました ({n})。");
        RefreshUserMenu();
        PopulateComponentCombo();   // インスペクタの「+ Add」候補にもユーザー型を反映
    }

    // ゲームを «別ウィンドウの exe» ではなく «エディタ内の Game View タブ» で動かす。
    // (スタンドアロン exe は Build で生成済み。出荷時はそれを配布できる。)
    private void RunGame()
    {
        if (_project == null || Engine == IntPtr.Zero) return;
        // Run explicitly owns a deterministic restart. Scene/Game tab changes
        // never imply Stop, restore, or Start.
        if (EngineInterop.acs_editor_play_state(Engine) != 0)
            StopPlayMode();
        if (StartPlayMode())
            SetGameView(true);
        UpdatePlayButtons();
    }

    private void SetBuildUiEnabled(bool enabled)
    {
        BuildBtn.Content = enabled ? "🔨 Build" : "⏳ Building…";
        UpdateEditorInputEnabled();
    }

    // ビルド/実行の出力は専用の Build ログへ (エンジン/エディタの Console とは分離)。
    private void BuildLog(string msg)
    {
        if (!Dispatcher.CheckAccess()) { Dispatcher.BeginInvoke(() => BuildLog(msg)); return; }
        var line = new BuildLine { Text = $"[{DateTime.Now:HH:mm:ss}] {msg}", Brush = LevelBrush(ClassifyBuildLine(msg)) };
        BuildList.Items.Add(line);   // エラー=赤 / 警告=黄 で色分け
        BuildList.ScrollIntoView(line);
    }

    // ===== ホットリロード: Source 保存を監視 → 自動再ビルド → ゲーム再起動 =====
    private void OnHotReloadToggle(object sender, RoutedEventArgs e)
    {
        if (IsSceneEditingBlocked)
        {
            HotReloadBtn.IsChecked = _hotReload;
            return;
        }
        _hotReload = HotReloadBtn.IsChecked == true;
        if (_hotReload) StartSourceWatch(); else StopSourceWatch();
        Log(_hotReload
            ? "ホットリロード: ON (Source の .cpp/.h 保存で自動再ビルド＋再起動)"
            : "ホットリロード: OFF");
    }

    private void StartSourceWatch()
    {
        if (_project == null) return;
        StopSourceWatch();
        try
        {
            System.IO.Directory.CreateDirectory(_project.SourceDir);
            _srcWatcher = new System.IO.FileSystemWatcher(_project.SourceDir)
            {
                IncludeSubdirectories = true,
                NotifyFilter = System.IO.NotifyFilters.LastWrite | System.IO.NotifyFilters.FileName,
                EnableRaisingEvents = true,
            };
            _srcWatcher.Changed += OnSourceChanged; _srcWatcher.Created += OnSourceChanged;
            _srcWatcher.Deleted += OnSourceChanged; _srcWatcher.Renamed += OnSourceChanged;
        }
        catch (Exception ex) { Log("ソース監視を開始できません: " + ex.Message); }

        _reloadTimer ??= new System.Windows.Threading.DispatcherTimer
            { Interval = TimeSpan.FromMilliseconds(600) };
        _reloadTimer.Tick -= OnReloadTick;   // 二重登録防止
        _reloadTimer.Tick += OnReloadTick;
    }

    private void StopSourceWatch()
    {
        if (_srcWatcher != null)
        {
            _srcWatcher.EnableRaisingEvents = false; _srcWatcher.Dispose(); _srcWatcher = null;
        }
        _reloadTimer?.Stop();
    }

    private static bool IsCodeFile(string path)
    {
        string ext = System.IO.Path.GetExtension(path).ToLowerInvariant();
        return ext is ".cpp" or ".h" or ".hpp" or ".inl" or ".c" or ".cc" or ".cmake" || path.EndsWith("CMakeLists.txt");
    }

    private void OnSourceChanged(object sender, System.IO.FileSystemEventArgs e)
    {
        if (!IsCodeFile(e.FullPath)) return;
        if (System.IO.Path.GetFileName(e.FullPath) == ReflectionCodegen.GenFileName) return;   // 生成物の自己トリガ回避
        // ファイル追加/削除/リネームはファイル集合が変わる → 次ビルドで CMake 再 configure。
        if (e.ChangeType != System.IO.WatcherChangeTypes.Changed) _pendingReconfigure = true;
        Dispatcher.BeginInvoke(() =>
        {
            if (!_hotReload) return;
            _reloadTimer?.Stop(); _reloadTimer?.Start();   // デバウンス
        });
    }

    private async void OnReloadTick(object? sender, EventArgs e)
    {
        _reloadTimer?.Stop();
        if (_building) { _reloadTimer?.Start(); return; }   // ビルド中なら後で
        Log("Source 変更を検出 → ホットリロード (再ビルド＋再起動)…");
        await DoBuild(run: true);
    }

    // ===== 整列 / 分配 (複数選択) =====
    private void DoAlign(int mode, string name)
    {
        if (Engine == IntPtr.Zero) return;
        int n = _view3d ? EngineInterop.acs_editor_align3d_selection(Engine, mode)
                        : EngineInterop.acs_editor_align_selection(Engine, mode);
        if (n > 0)
        {
            Log($"Aligned {n} node(s): {name}.");
            SyncSelectionUi();
            RecordSceneDocumentChange("Align Nodes");
        }
        else Log("Align needs 2+ selected nodes.");
    }
    private void DoDistribute(int axis, string name)
    {
        if (Engine == IntPtr.Zero) return;
        int n = _view3d ? EngineInterop.acs_editor_distribute3d_selection(Engine, axis)
                        : EngineInterop.acs_editor_distribute_selection(Engine, axis);
        if (n > 0)
        {
            Log($"Distributed {n} node(s): {name}.");
            SyncSelectionUi();
            RecordSceneDocumentChange("Distribute Nodes");
        }
        else Log("Distribute needs 3+ selected nodes.");
    }
    private void OnAlignLeft(object s, RoutedEventArgs e)   => DoAlign(0, "left");
    private void OnAlignRight(object s, RoutedEventArgs e)  => DoAlign(1, "right");
    private void OnAlignTop(object s, RoutedEventArgs e)    => DoAlign(2, "top");
    private void OnAlignBottom(object s, RoutedEventArgs e) => DoAlign(3, "bottom");
    private void OnAlignHC(object s, RoutedEventArgs e)     => DoAlign(4, "center-h");
    private void OnAlignVC(object s, RoutedEventArgs e)     => DoAlign(5, "center-v");
    private void OnDistributeH(object s, RoutedEventArgs e) => DoDistribute(0, "horizontal");
    private void OnDistributeV(object s, RoutedEventArgs e) => DoDistribute(1, "vertical");

    // ===== Display プロパティ (色 / base / visible / enabled / sortLayer) =====
    private void OnDispVisible(object s, RoutedEventArgs e)
    {
        if (_populating || _selectedId < 0 || Engine == IntPtr.Zero) return;
        EngineInterop.acs_editor_node_set_visible(Engine, _selectedId, DispVisible.IsChecked == true ? 1 : 0);
        RecordSceneDocumentChange("Visibility");
    }
    private void OnDispEnabled(object s, RoutedEventArgs e)
    {
        if (_populating || _selectedId < 0 || Engine == IntPtr.Zero) return;
        EngineInterop.acs_editor_node_set_enabled(Engine, _selectedId, DispEnabled.IsChecked == true ? 1 : 0);
        RecordSceneDocumentChange("Enabled State");
    }
    // 数値欄の確定: 値が実際に変わったプロパティだけ set する (冗長な undo を避ける)。
    private void ApplyDisplay()
    {
        if (_populating ||
            IsSceneEditingBlocked ||
            _selectedId < 0 ||
            Engine == IntPtr.Zero)
        {
            return;
        }
        int id = _selectedId;

        float curBase = EngineInterop.acs_editor_node_get_base(Engine, id);
        float newBase = ParseF(DispBase.Text, curBase);
        if (Math.Abs(newBase - curBase) > 1e-4f) EngineInterop.acs_editor_node_set_base(Engine, id, newBase);

        int curLayer = EngineInterop.acs_editor_node_get_sortlayer(Engine, id);
        // sortLayer は整数。範囲外/非整数 ("99999999999"・"3.5"・"NaN") は int.TryParse が false を返すので現在値を保持
        // (float 経由だと saturating cast で int.MaxValue 等に化けて無意味な undo を積む)。
        if (!int.TryParse(DispLayer.Text, NumberStyles.Integer, CultureInfo.InvariantCulture, out int newLayer))
            newLayer = curLayer;
        if (newLayer != curLayer) EngineInterop.acs_editor_node_set_sortlayer(Engine, id, newLayer);

        EngineInterop.acs_editor_node_get_color(Engine, id, out float cr, out float cg, out float cb, out float ca);
        float nr = ParseF(ColR.Text, cr), ng = ParseF(ColG.Text, cg), nb = ParseF(ColB.Text, cb), na = ParseF(ColA.Text, ca);
        if (Math.Abs(nr - cr) > 1e-4f || Math.Abs(ng - cg) > 1e-4f ||
            Math.Abs(nb - cb) > 1e-4f || Math.Abs(na - ca) > 1e-4f)
            EngineInterop.acs_editor_node_set_color(Engine, id, nr, ng, nb, na);
        UpdateColorSwatch();
        RecordSceneDocumentChange(
            "Appearance",
            mergeKey: "inspector.appearance",
            mergeWindow: TimeSpan.FromSeconds(1),
            nodeId: id);
    }

    // Color RGBA 欄の現在値を Inspector の色スウォッチに反映する (色相が一目で分かるよう不透明で表示)。
    private void UpdateColorSwatch()
    {
        static byte B(float v) => (byte)Math.Clamp(v * 255f, 0f, 255f);
        float r = ParseF(ColR.Text), g = ParseF(ColG.Text), b = ParseF(ColB.Text);
        ColorSwatch.Background = new System.Windows.Media.SolidColorBrush(
            System.Windows.Media.Color.FromRgb(B(r), B(g), B(b)));
    }

    private void OnDisplayColorSwatchClicked(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (_populating || _selectedId < 0 || Engine == IntPtr.Zero) return;

        static byte B(float v) => (byte)Math.Clamp(v * 255f, 0f, 255f);
        var initial = System.Windows.Media.Color.FromArgb(
            B(ParseF(ColA.Text, 1f)),
            B(ParseF(ColR.Text)),
            B(ParseF(ColG.Text)),
            B(ParseF(ColB.Text)));
        if (!ColorPickerDialog.TryPick(this, initial, allowAlpha: true, out var picked)) return;

        _populating = true;
        try
        {
            ColR.Text = (picked.R / 255f).ToString("0.###", CultureInfo.InvariantCulture);
            ColG.Text = (picked.G / 255f).ToString("0.###", CultureInfo.InvariantCulture);
            ColB.Text = (picked.B / 255f).ToString("0.###", CultureInfo.InvariantCulture);
            ColA.Text = (picked.A / 255f).ToString("0.###", CultureInfo.InvariantCulture);
        }
        finally
        {
            _populating = false;
        }

        ApplyDisplay();
        e.Handled = true;
    }

    // ===== スプライト画像 (矩形の代わりに画像を表示) =====
    private void RefreshSpriteLabel(int id)
    {
        string path = EngineInterop.NodeSprite(Engine, id);
        bool none = string.IsNullOrEmpty(path);
        SpriteLabel.Text = none ? "(なし)" : System.IO.Path.GetFileName(path);
        SpriteLabel.ToolTip = none ? null : path;
    }

    private void OnBrowseSprite(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _selectedId < 0) return;
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "スプライト画像を選択",
            Filter = "画像 (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif)|*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif|すべてのファイル (*.*)|*.*",
        };
        if (dlg.ShowDialog(this) != true) return;
        if (EngineInterop.acs_editor_node_set_sprite(Engine, _selectedId, dlg.FileName) != 0)
        {
            RefreshSpriteLabel(_selectedId);
            RecordSceneDocumentChange("Assign Sprite");
            Log($"Sprite set: {System.IO.Path.GetFileName(dlg.FileName)}");
        }
        else Log("Sprite set failed: " + dlg.FileName);
    }

    private void OnClearSprite(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _selectedId < 0) return;
        EngineInterop.acs_editor_node_clear_sprite(Engine, _selectedId);
        RefreshSpriteLabel(_selectedId);
        RecordSceneDocumentChange("Clear Sprite");
        Log("Sprite cleared (→ 矩形表示).");
    }

    // ===== マテリアル (効果プリセット) — ノードは選ぶだけ。編集はマテリアルエディタで =====

    // MaterialBox を Assets 内の *.acsmat 一覧で埋め、ノードの現在マテリアルを選択状態にする。
    private void RefreshMaterialBox(int id)
    {
        bool wasRefreshing = _refreshingMaterialBox;
        _refreshingMaterialBox = true;
        try
        {
            MaterialBox.Items.Clear();
            MaterialBox.Items.Add(new ComboBoxItem { Content = "(なし)", Tag = null });
            string cur = EngineInterop.NodeMaterial(Engine, id);
            MaterialAssetCatalog catalog = MaterialAssetWorkflow.BuildCatalog(
                _project?.AssetsDir,
                cur);
            foreach (MaterialAssetChoice choice in catalog.Choices)
            {
                MaterialBox.Items.Add(new ComboBoxItem
                {
                    Content = choice.DisplayName,
                    Tag = choice.FullPath,
                    ToolTip = choice.FullPath,
                });
            }
            MaterialBox.SelectedIndex = catalog.SelectedIndex + 1; // index 0 is the explicit None item.
        }
        finally
        {
            _refreshingMaterialBox = wasRefreshing;
        }
    }

    private string AssetRel(string full)
        => MaterialAssetWorkflow.DisplayName(_project?.AssetsDir, full);

    private void OnMaterialSelected(object sender, SelectionChangedEventArgs e)
    {
        if (_populating || _refreshingMaterialBox || Engine == IntPtr.Zero || _selectedId < 0) return;
        if (MaterialBox.SelectedItem is not ComboBoxItem it) return;
        string? path = it.Tag as string;
        int id = _selectedId;
        string current = EngineInterop.NodeMaterial(Engine, id);
        if ((string.IsNullOrEmpty(path) && string.IsNullOrEmpty(current)) ||
            (!string.IsNullOrEmpty(path) && MaterialAssetWorkflow.SamePath(path, current)))
        {
            return;
        }

        int changed;
        if (string.IsNullOrEmpty(path))
        {
            changed = EngineInterop.acs_editor_node_clear_material(Engine, id);
        }
        else
        {
            changed = EngineInterop.acs_editor_node_set_material(Engine, id, path);
        }
        if (changed == 0)
        {
            RefreshMaterialBox(id);
            Log($"Material change failed (node {id}).");
            return;
        }

        Log(string.IsNullOrEmpty(path)
            ? "Material cleared (→ 効果なし)."
            : $"Material ← {AssetRel(path)} (node {id})");
        RecordSceneDocumentChange(string.IsNullOrEmpty(path)
            ? "Clear Material"
            : "Assign Material");
    }

    private void OnEditMaterial(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        string? path = (MaterialBox.SelectedItem as ComboBoxItem)?.Tag as string;
        if (string.IsNullOrEmpty(path))
        {
            // 未割当なら新規作成フローへ。
            OnNewMaterial(sender, e);
            return;
        }
        OpenMaterialEditor(path);
    }

    private void OnNewMaterial(object sender, RoutedEventArgs e)
    {
        if (!TryCreateMaterialAsset(out string path, out string assetId)) return;
        // 選択ノードがあれば即割当。
        if (_selectedId >= 0)
        {
            int id = _selectedId;
            if (EngineInterop.acs_editor_node_set_material(Engine, id, path) != 0)
            {
                RefreshMaterialBox(id);
                RecordSceneDocumentChange("Assign Material");
            }
            else
            {
                RefreshMaterialBox(id);
                Log($"New material was created but could not be assigned to node {id}.");
            }
        }
        OpenMaterialEditor(path, assetId);
    }

    /// <summary>
    /// Creates a project material without overwriting an existing asset. Assignment is kept at
    /// the Inspector call site because 2D and 3D nodes use different native compatibility APIs.
    /// </summary>
    private bool TryCreateMaterialAsset(
        out string path,
        out string assetId)
    {
        path = "";
        assetId = "";
        if (Engine == IntPtr.Zero) return false;
        if (_project == null)
        {
            Log("マテリアル作成にはプロジェクトが必要です。");
            return false;
        }

        try
        {
            string createdAssetId = "";
            void RefreshAuthoritativeDatabase(string materialPath)
            {
                var database = new AssetDatabase(
                    _project.RootDir,
                    _project.AssetsDir);
                _ = database.Refresh(verifyContent: true);
                bool exists = System.IO.File.Exists(materialPath);
                bool indexed = database.TryGetByPath(
                    materialPath,
                    out AssetRecord? record);
                bool coherent = exists
                    ? indexed && record?.Kind == "material"
                    : !indexed;
                if (!coherent)
                {
                    throw new IOException(
                        exists
                            ? "The created material could not be indexed authoritatively."
                            : "The failed material remained in the authoritative asset index.");
                }
                createdAssetId = exists
                    ? MaterialDocumentHostRegistration.NormalizeAssetIdOrNull(
                          record!.AssetId) ??
                      throw new IOException(
                          "The created material metadata has an invalid Asset ID.")
                    : "";
            }

            path = MaterialAssetWorkflow.CreateProjectMaterial(
                _project.AssetsDir,
                static (materialPath, materialName) =>
                    EngineInterop.acs_editor_material_create(
                        materialPath,
                        materialName) != 0,
                RefreshAuthoritativeDatabase);
            assetId = createdAssetId;
            if (assetId.Length == 0)
                throw new IOException(
                    "The created material has no authoritative Asset ID.");
            // The authoritative refresh above is part of the mutation transaction. This second
            // refresh only updates the already-hosted Asset View presentation asynchronously.
            AssetBrowser.Refresh();
        }
        catch (Exception error) when (
            MaterialAssetWorkflow.IsRecoverableCreationFailure(error))
        {
            Log("マテリアルの保存先を作成できません: " + error.Message);
            path = "";
            assetId = "";
            return false;
        }

        Log($"New material: {AssetRel(path)}");
        return true;
    }

    private void OnClearMaterial(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _selectedId < 0) return;
        int id = _selectedId;
        if (string.IsNullOrEmpty(EngineInterop.NodeMaterial(Engine, id)))
        {
            RefreshMaterialBox(id);
            return;
        }
        if (EngineInterop.acs_editor_node_clear_material(Engine, id) == 0)
        {
            RefreshMaterialBox(id);
            Log($"Material clear failed (node {id}).");
            return;
        }

        RefreshMaterialBox(id);
        RecordSceneDocumentChange("Clear Material");
        Log("Material cleared (→ 効果なし).");
    }

    private void OpenMaterialEditor(
        string acsmatPath,
        string? assetId = null)
    {
        foreach (AssetDocumentMutationState mutation in _assetDocumentMutations.Values)
        {
            if (!mutation.Operation.AffectsPath(acsmatPath)) continue;
            Log("Material Editor cannot open this asset while its path is changing. Retry when the Asset View operation finishes.");
            return;
        }

        string? normalizedAssetId;
        try
        {
            normalizedAssetId =
                MaterialDocumentHostRegistration.ResolveAssetIdForOpen(
                    _project,
                    acsmatPath,
                    assetId);
        }
        catch (Exception ex) when (
            ex is ArgumentException or
                IOException or
                UnauthorizedAccessException or
                InvalidDataException or
                NotSupportedException)
        {
            Log(
                "Material Editor could not establish authoritative asset identity: " +
                ex.Message,
                "Document",
                LogLevel.Error);
            return;
        }
        foreach (MaterialEditorWindow open in _materialEditorWindows.ToArray())
        {
            bool sameAsset =
                normalizedAssetId != null &&
                string.Equals(
                    open.CurrentAssetId,
                    normalizedAssetId,
                    StringComparison.Ordinal);
            bool samePath =
                open.CurrentAssetPath is string openPath &&
                MaterialDocumentHostRegistration.PathsEqual(
                    openPath,
                    acsmatPath);
            if (!sameAsset && !samePath)
                continue;
            if (samePath &&
                normalizedAssetId != null &&
                open.CurrentAssetId == null)
            {
                try
                {
                    open.SetAssetIdentity(normalizedAssetId);
                    if (_documentHostInitialized)
                    {
                        if (_hostedMaterialDocuments.ContainsKey(open))
                        {
                            RefreshHostedMaterialIdentity(open);
                        }
                        else if (!TryRegisterHostedMaterialDocument(open))
                        {
                            throw new InvalidOperationException(
                                "The promoted material identity could not join Document Host.");
                        }
                    }
                }
                catch (Exception ex)
                {
                    open.SetAssetIdentity(null);
                    Log(
                        "Material Editor could not promote its path identity to the " +
                        "authoritative Asset ID: " + ex.Message,
                        "Document",
                        LogLevel.Error);
                    return;
                }
            }
            else if (samePath &&
                     normalizedAssetId != null &&
                     open.CurrentAssetId != null &&
                     !sameAsset)
            {
                Log(
                    "Material Editor rejected an Asset ID change for an already-open path.",
                    "Document",
                    LogLevel.Error);
                return;
            }
            if (open.WindowState == WindowState.Minimized)
                open.WindowState = WindowState.Normal;
            open.Activate();
            return;
        }

        // Resolve the engine through the viewport on every use. Holding the raw
        // pointer here would outlive the viewport during owner-window teardown.
        var win = _viewport != null
            ? new MaterialEditorWindow(_viewport, acsmatPath, normalizedAssetId)
            : new MaterialEditorWindow(IntPtr.Zero, acsmatPath);
        if (_viewport == null)
            win.SetAssetIdentity(normalizedAssetId);
        win.Owner = this;
        win.Closed += (_, _) =>
        {
            UnregisterHostedMaterialDocument(win);
            _materialEditorWindows.Remove(win);
        };
        _materialEditorWindows.Add(win);
        if (_documentHostInitialized &&
            !TryRegisterHostedMaterialDocument(win))
        {
            _materialEditorWindows.Remove(win);
            win.AbortBeforeShow();
            Log(
                "Material Editor was not opened because Document Host " +
                "registration failed.",
                "Document",
                LogLevel.Error);
            return;
        }
        try
        {
            win.Show();   // 非モーダル: viewport を見ながら調整できる
        }
        catch
        {
            UnregisterHostedMaterialDocument(win);
            _materialEditorWindows.Remove(win);
            win.AbortBeforeShow();
            throw;
        }
    }


    private void OnHierarchySelect(object sender, RoutedPropertyChangedEventArgs<object> e)
    {
        if (Engine == IntPtr.Zero || _syncingSelection) return;   // 同期中の native 変更は無視
        if (e.NewValue is TreeViewItem item && item.Tag is int id)
        {
            if (_view3d) { EngineInterop.acs_editor_select3d(Engine, id); SyncSelectionUi(); return; }
            EngineInterop.acs_editor_select(Engine, id);   // 単一選択 (集合を {id} に)
            SyncSelectionUi();
        }
    }

    // ===== Inspector: 選択ノードの transform を表示 / 編集 =====
    private void PopulateInspector(int id)
    {
        if (Engine == IntPtr.Zero) return;
        EngineInterop.acs_editor_node_get_transform(Engine, id,
            out float x, out float y, out float rot, out float sx, out float sy);

        int count = EngineInterop.acs_editor_selection_count(Engine);
        bool single = count <= 1;

        _populating = true;
        string nm = EngineInterop.NodeName(Engine, id);
        InspName.Text = nm;
        InspSub.Text  = single ? $"id {id}" : $"id {id} · {count} 個選択中";
        NameBox.Text = nm;
        PosX.Text   = x.ToString("0.###", CultureInfo.InvariantCulture);
        PosY.Text   = y.ToString("0.###", CultureInfo.InvariantCulture);
        RotDeg.Text = (rot * 180.0 / Math.PI).ToString("0.###", CultureInfo.InvariantCulture);
        ScaleX.Text = sx.ToString("0.###", CultureInfo.InvariantCulture);
        ScaleY.Text = sy.ToString("0.###", CultureInfo.InvariantCulture);
        // Display プロパティ。
        DispVisible.IsChecked = EngineInterop.acs_editor_node_get_visible(Engine, id) != 0;
        DispEnabled.IsChecked = EngineInterop.acs_editor_node_get_enabled(Engine, id) != 0;
        DispLayer.Text = EngineInterop.acs_editor_node_get_sortlayer(Engine, id).ToString(CultureInfo.InvariantCulture);
        DispBase.Text  = EngineInterop.acs_editor_node_get_base(Engine, id).ToString("0.###", CultureInfo.InvariantCulture);
        EngineInterop.acs_editor_node_get_color(Engine, id, out float cr, out float cg, out float cb, out float ca);
        ColR.Text = cr.ToString("0.###", CultureInfo.InvariantCulture);
        ColG.Text = cg.ToString("0.###", CultureInfo.InvariantCulture);
        ColB.Text = cb.ToString("0.###", CultureInfo.InvariantCulture);
        ColA.Text = ca.ToString("0.###", CultureInfo.InvariantCulture);
        UpdateColorSwatch();
        RefreshSpriteLabel(id);
        RefreshMaterialBox(id);
        // 複数選択では transform/コンポーネント編集を無効化 (primary を表示するのみ)。
        // Duplicate/Delete は選択全体に効くので常に有効。
        InspFields.IsEnabled    = single;
        MultiHint.Visibility    = single ? Visibility.Collapsed : Visibility.Visible;
        ActionButtons.IsEnabled = true;
        _populating = false;
        PopulateComponents(id);
    }

    private void ApplyInspector()
    {
        if (_populating ||
            IsSceneEditingBlocked ||
            _selectedId < 0 ||
            Engine == IntPtr.Zero)
        {
            return;
        }
        float x   = ParseF(PosX.Text);
        float y   = ParseF(PosY.Text);
        float deg = ParseF(RotDeg.Text);
        float sx  = ParseF(ScaleX.Text, 1.0f);
        float sy  = ParseF(ScaleY.Text, 1.0f);
        EngineInterop.acs_editor_node_set_transform(Engine, _selectedId,
            x, y, (float)(deg * Math.PI / 180.0), sx, sy);
        RecordSceneDocumentChange(
            "Transform",
            mergeKey: "inspector.transform",
            mergeWindow: TimeSpan.FromSeconds(1),
            nodeId: _selectedId);
    }

    private static float ParseF(string s, float fallback = 0.0f) =>
        float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float v)
            && float.IsFinite(v) ? v : fallback;   // "NaN"/"Infinity" は弾いて fallback

    // ===== ノード操作: リネーム / 削除 =====
    private void ApplyRename()
    {
        if (_populating ||
            IsSceneEditingBlocked ||
            _selectedId < 0 ||
            Engine == IntPtr.Zero)
        {
            return;
        }
        string nm = (NameBox.Text ?? "").Trim();
        if (nm.Length == 0) return;
        if (EngineInterop.acs_editor_node_rename(Engine, _selectedId, nm) != 0)
        {
            InspName.Text = nm + "  (id " + _selectedId + ")";
            BuildHierarchy();   // Hierarchy 表示名を更新 (選択はエンジン側で維持)
            RecordSceneDocumentChange("Rename Node");
        }
    }

    // 3D インスペクタの Name 欄 (Populate3DInspector が生成)。F2 でここをフォーカス。
    private System.Windows.Controls.TextBox? _name3dBox;

    /// <summary>3D ノードをリネームする (3D インスペクタの Name 欄から)。</summary>
    private void Apply3DRename(int id, string? raw)
    {
        if (_pop3d || IsSceneEditingBlocked || Engine == IntPtr.Zero) return;
        string nm = (raw ?? "").Trim();
        if (nm.Length == 0) return;
        if (EngineInterop.acs_editor_node3d_set_name(Engine, id, nm) != 0)
        {
            InspName.Text = nm;
            Build3DHierarchy();   // ヒエラルキー表示名を更新
            RecordSceneDocumentChange("Rename Node");
        }
    }

    private void OnDuplicateNode(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        if (_view3d)   // 3D モード: 選択 3D ノードを subtree 複製 (acs_editor_node3d_duplicate)
        {
            int sel = EngineInterop.acs_editor_selected3d(Engine);
            if (sel < 0) return;
            int nid = EngineInterop.acs_editor_node3d_duplicate(Engine, sel);
            if (nid >= 0)
            {
                Log("Duplicated 3D node (subtree).");
                Build3DHierarchy();
                Select3DInHierarchy(nid);
                Populate3DInspector(nid);
                RecordSceneDocumentChange("Duplicate Node");
            }
            return;
        }
        if (EngineInterop.acs_editor_selection_count(Engine) == 0) return;
        int n = EngineInterop.acs_editor_selection_duplicate(Engine);   // 選択全体を一括複製 (1 undo)
        if (n > 0)
        {
            Log($"Duplicated {n} node(s) (subtree).");
            BuildHierarchy();        // engine 選択はクローン群へ移っている
            SyncSelectionUi();
            RecordSceneDocumentChange("Duplicate Nodes");
        }
    }

    private void DeleteSelected()
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        if (_view3d)   // 3D モード: selected3d を削除 (Del/コンテキストは従来 2D 専用で 3D 無反応だった)
        {
            int sel = EngineInterop.acs_editor_selected3d(Engine);
            if (sel < 0) return;
            if (EngineInterop.acs_editor_delete_node3d(Engine, sel) != 0)
            {
                Log("Deleted 3D node (and children).");
                Build3DHierarchy();
                Clear3DInspector();
                UpdateStatusBar();
                RecordSceneDocumentChange("Delete Node");
            }
            return;
        }
        if (EngineInterop.acs_editor_selection_count(Engine) == 0) return;
        int n = EngineInterop.acs_editor_selection_delete(Engine);      // 選択全体を一括削除 (1 undo)
        if (n > 0)
        {
            Log($"Deleted {n} node(s) (and their children).");
            BuildHierarchy();
            SyncSelectionUi();       // 集合は空 → ClearSelectionUi
            RecordSceneDocumentChange("Delete Nodes");
        }
    }
    private void OnDeleteNode(object sender, RoutedEventArgs e) => DeleteSelected();
    private void OnDeleteCmd(object sender, ExecutedRoutedEventArgs e) => DeleteSelected();
    private void OnCut(object sender, RoutedEventArgs e)   // Ctrl+X = コピーしてから削除 (OnCopy/DeleteSelected は 2D/3D 分岐済み)
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        OnCopy(this, null!);
        DeleteSelected();
    }

    // ===== Copy / Paste (subtree、Ctrl+C / Ctrl+V) =====
    private void OnCopy(object sender, ExecutedRoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (_view3d)   // 3D モード: 選択 3D ノードをクリップボードへ
        {
            int sel = EngineInterop.acs_editor_selected3d(Engine);
            if (sel >= 0) { EngineInterop.acs_editor_node3d_copy(Engine, sel); _hasClip3d = true; Log("Copied 3D node."); }
            return;
        }
        if (_selectedId < 0) return;
        _clipboard = EngineInterop.CopySubtree(Engine, _selectedId);
        Log($"Copied node {_selectedId} (subtree).");
    }

    private void OnPaste(object sender, ExecutedRoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
        if (_view3d)   // 3D モード: クリップボードの 3D ノードを貼り付け (+X 小オフセット)
        {
            int nid = EngineInterop.acs_editor_node3d_paste(Engine);
            if (nid >= 0)
            {
                Log("Pasted 3D node.");
                Build3DHierarchy();
                Select3DInHierarchy(nid);
                Populate3DInspector(nid);
                RecordSceneDocumentChange("Paste Node");
            }
            else Log("3D クリップボードが空です。");
            return;
        }
        if (string.IsNullOrEmpty(_clipboard)) return;
        int parent = _selectedId >= 0 ? EngineInterop.acs_editor_node_parent(Engine, _selectedId) : -1;
        int id = EngineInterop.acs_editor_paste_subtree(Engine, _clipboard, parent);
        if (id >= 0)
        {
            Log($"Pasted as node {id}.");
            BuildHierarchy();
            _selectedId = id;
            SelectHierarchyItem(id);   // ツリー選択 → Inspector 更新
            RecordSceneDocumentChange("Paste Node");
        }
    }

    // ===== コマンドの可否 (CanExecute): 不可時はメニュー/ショートカットを自動グレーアウト =====
    private int CurSelCount() => Engine == IntPtr.Zero ? 0
        : (_view3d ? EngineInterop.acs_editor_selected3d_count(Engine) : EngineInterop.acs_editor_selection_count(Engine));
    private void OnCanUndo(object sender, CanExecuteRoutedEventArgs e)
    {
        if (!CanUseSceneDocumentHistory())
        {
            e.CanExecute = false;
            return;
        }
        EditorDocument? document = _documentHost.ActiveDocument;
        e.CanExecute = document != null &&
                       (document.CanUndo || document.HasPendingChanges);
    }

    private void OnCanRedo(object sender, CanExecuteRoutedEventArgs e)
    {
        if (!CanUseSceneDocumentHistory())
        {
            e.CanExecute = false;
            return;
        }
        EditorDocument? document = _documentHost.ActiveDocument;
        e.CanExecute = document != null &&
                       document.CanRedo &&
                       !document.HasPendingChanges;
    }
    private void OnCanCopyDelete(object sender, CanExecuteRoutedEventArgs e)
        => e.CanExecute = !IsSceneEditingBlocked && CurSelCount() > 0;
    private void OnCanPaste(object sender, CanExecuteRoutedEventArgs e)
        => e.CanExecute = !IsSceneEditingBlocked &&
            (_view3d ? _hasClip3d : !string.IsNullOrEmpty(_clipboard));

    // ===== プレハブ: ノードのサブツリーを .acsprefab として保存 / 再インスタンス化 =====
    //   プレハブ = サブツリーの直列化テキスト (ACSCENE 形式)。copy_subtree で保存し、
    //   paste_subtree で複製 (id 再マップ + ObjectRef 内部参照の付け替え) してインスタンス化する。

    private void OnCtxSavePrefab(object sender, RoutedEventArgs e) => SaveAsPrefab(_contextNodeId);

    /// <summary>ノード(とサブツリー)を .acsprefab アセットとして保存する。</summary>
    private void SaveAsPrefab(int id)
    {
        if (Engine == IntPtr.Zero || id < 0 || _project == null) return;
        string text = _view3d ? EngineInterop.CopySubtree3D(Engine, id) : EngineInterop.CopySubtree(Engine, id);
        if (string.IsNullOrEmpty(text)) { Log("プレハブ化に失敗 (サブツリーの直列化が空)。"); return; }
        string nm = EngineInterop.NodeName(Engine, id);
        if (string.IsNullOrWhiteSpace(nm)) nm = "Prefab";
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            Title = "プレハブを保存",
            Filter = "ACS Prefab (*.acsprefab)|*.acsprefab",
            InitialDirectory = _project.AssetsDir,
            FileName = nm + ".acsprefab",
        };
        if (dlg.ShowDialog(this) != true) return;
        try
        {
            string sourceText = StripPrefabLinks(text);
            if (_view3d)
            {
                if (!PrefabNodeIdentity3D.TryEnsureSource(dlg.FileName, sourceText, out sourceText, out _, out string identityError)) throw new InvalidDataException(identityError);
            }
            SceneSourceFile.WriteAtomicText(dlg.FileName, sourceText);
            // 保存元もこのプレハブのインスタンスにする (instance-of リンク)。2D/3D で ABI を切替え。
            if (_view3d)
            {
                string instanceId = EngineInterop.NodePrefabInstanceId3D(Engine, id);
                if (string.IsNullOrEmpty(instanceId)) instanceId = NewPrefabInstanceId3D();
                if (EngineInterop.acs_editor_node3d_set_prefab_link(Engine, id, dlg.FileName, instanceId) == 0) throw new InvalidOperationException("3D Prefab instance linkを設定できませんでした。");
                Populate3DInspector(id);
            }
            else         { EngineInterop.acs_editor_node_set_prefab_src(Engine, id, dlg.FileName);   PopulateInspector(id); }
            RecordSceneDocumentChange("Create Prefab Link");
            AssetBrowser.Refresh();
            Log($"プレハブを保存 → {System.IO.Path.GetFileName(dlg.FileName)}");
        }
        catch (Exception ex) { Log("プレハブ保存エラー: " + ex.Message); }
    }

    private void OnCtxSaveBlueprint(object sender, RoutedEventArgs e) => SaveAsBlueprint(_contextNodeId);

    /// <summary>ノード(とサブツリー)を «Blueprint» (.acsbp) として保存する。コンポーネント木を CMP ブロックに
    /// 入れる (変数/グラフは空)。UE5 風の «1 つのオブジェクト» = 再利用可能な Blueprint Class。</summary>
    private void SaveAsBlueprint(int id)
    {
        if (Engine == IntPtr.Zero || id < 0 || _project == null) return;
        string comp = StripPrefabLinks(_view3d ? EngineInterop.CopySubtree3D(Engine, id) : EngineInterop.CopySubtree(Engine, id));
        if (string.IsNullOrEmpty(comp)) { Log("Blueprint 化に失敗 (サブツリーの直列化が空)。"); return; }
        string nm = EngineInterop.NodeName(Engine, id);
        if (string.IsNullOrWhiteSpace(nm)) nm = "Blueprint";
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            Title = "Blueprint を保存",
            Filter = "ACS Blueprint (*.acsbp)|*.acsbp",
            InitialDirectory = _project.AssetsDir,
            FileName = nm + ".acsbp",
        };
        if (dlg.ShowDialog(this) != true) return;
        try
        {
            if (_view3d && !PrefabNodeIdentity3D.TryEnsureSource(dlg.FileName, comp, out comp, out _, out string identityError)) throw new InvalidDataException(identityError);
            int cmpLines = comp.Replace("\r", "").TrimEnd('\n').Split('\n').Length;   // ログ表示用の行数
            AcsbpFormat.Write(dlg.FileName, AcsbpFormat.WrapComponents(comp));
            AssetBrowser.Refresh();
            Log($"Blueprint を保存 → {System.IO.Path.GetFileName(dlg.FileName)} (コンポーネント木 {cmpLines} 行)");
        }
        catch (Exception ex) { Log("Blueprint 保存エラー: " + ex.Message); }
    }

    /// <summary>アセットの右クリック「シーンに配置」: Blueprint/Prefab をシーンへ実体化する。</summary>
    private void OnAssetPlace(object? sender, AssetActivatedEventArgs e)
    {
        if (IsSceneEditingBlocked) return;
        switch (e.Kind)
        {
            case "blueprint": PlaceBlueprint(e.FullPath, _selectedId); break;
            case "prefab":    InstantiatePrefab(e.FullPath, _selectedId); break;   // 旧 Prefab (レガシー)
            default: Log("このアセットはシーンに配置できません: " + System.IO.Path.GetFileName(e.FullPath)); break;
        }
    }

    /// <summary>右クリック「Blueprint に変換」: 旧 .acsprefab の ACSCENE を .acsbp の CMP に包んで保存する。</summary>
    private void OnAssetConvert(object? sender, AssetActivatedEventArgs e)
    {
        if (e.Kind != "prefab") { Log("変換は旧 Prefab(.acsprefab) のみ対応です: " + System.IO.Path.GetFileName(e.FullPath)); return; }
        ConvertPrefabToBlueprint(e.FullPath);
    }

    private void ConvertPrefabToBlueprint(string prefabPath)
    {
        if (!System.IO.File.Exists(prefabPath)) { Log("変換元が見つかりません。"); return; }
        string acscene;
        try { acscene = StripPrefabLinks(System.IO.File.ReadAllText(prefabPath, System.Text.Encoding.UTF8)); }
        catch (Exception ex) { Log("変換読込エラー: " + ex.Message); return; }
        string bpPath = System.IO.Path.ChangeExtension(prefabPath, ".acsbp");
        if (acscene.TrimStart().StartsWith("ACS3D", StringComparison.Ordinal) && !PrefabNodeIdentity3D.TryEnsureSource(bpPath, acscene, out acscene, out _, out string identityError)) { Log("変換エラー: " + identityError); return; }
        int cmpLines = acscene.Replace("\r", "").TrimEnd('\n').Split('\n').Length;   // ログ表示用の行数
        try { AcsbpFormat.Write(bpPath, AcsbpFormat.WrapComponents(acscene)); }
        catch (Exception ex) { Log("変換書込エラー: " + ex.Message); return; }
        AssetBrowser.Refresh();
        Log($"Blueprint に変換 → {System.IO.Path.GetFileName(bpPath)} (コンポーネント木 {cmpLines} 行)");
    }

    /// <summary>Blueprint(.acsbp) の Components(CMP) をシーンへ実体化する (= BP オブジェクトをスポーン)。</summary>
    private void PlaceBlueprint(string path, int parentId)
    {
        if (Engine == IntPtr.Zero) return;
        string text;
        try { text = System.IO.File.ReadAllText(path); }
        catch (Exception ex) { Log("Blueprint 読込エラー: " + ex.Message); return; }
        string comp = AcsbpFormat.ExtractCmp(text);
        if (string.IsNullOrWhiteSpace(comp)) { Log("この Blueprint はコンポーネントを持たないため配置できません (ロジックのみ)。"); return; }
        bool payloadUses3D = comp.TrimStart().StartsWith(
            "ACS3D",
            StringComparison.Ordinal);
        if (!AllowAssetPlacement(
                payloadUses3D,
                "Blueprint",
                System.IO.Path.GetFileName(path)))
        {
            return;
        }
        if (payloadUses3D)   // 3D Blueprint (ACS3D テキスト) → 3D サブツリーとして実体化
        {
            try { comp = EnsurePrefabNodeIdentities3D(path, comp); }
            catch (Exception ex) { Log("Blueprint node identity移行エラー: " + ex.Message); return; }
            int parent3d = EngineInterop.acs_editor_selected3d(Engine);
            int rid = EngineInterop.acs_editor_prefab_instance3d_instantiate(
                Engine,
                path,
                NewPrefabInstanceId3D(),
                comp,
                parent3d);
            if (rid < 0) { Log("Blueprint の配置に失敗しました。"); return; }
            RefreshAfterSceneChange();   // 3D ヒエラルキー再構築 + 選択 UI 同期 (paste が root を選択済み)
            Log($"Blueprint をシーンに配置 → {System.IO.Path.GetFileName(path)} (3D node {rid})");
            return;
        }
        int id = EngineInterop.acs_editor_paste_subtree(Engine, comp, parentId);
        if (id < 0) { Log("Blueprint の配置に失敗しました。"); return; }
        EngineInterop.acs_editor_node_set_prefab_src(Engine, id, path);   // instance-of リンク (.acsbp。Apply/Revert 対応済)
        BuildHierarchy();
        _selectedId = id;
        SelectHierarchyItem(id);
        PopulateInspector(id);
        RecordSceneDocumentChange("Place Blueprint");
        Log($"Blueprint をシーンに配置 → {System.IO.Path.GetFileName(path)} (node {id})");
    }

    /// <summary>prefab_src が Blueprint(.acsbp) か (= CMP ブロックを持つ統合資産)。</summary>
    private static bool IsBlueprint(string src) => src.EndsWith(".acsbp", StringComparison.OrdinalIgnoreCase);

    /// <summary>srcから実体化する木を読み、3D sourceなら不足PSID3Dをatomic migrationして返す。</summary>
    private static string ReadComponentsFor(string src)
    {
        string text = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8);
        string components = IsBlueprint(src) ? AcsbpFormat.ExtractCmp(text) : text;
        return components.TrimStart().StartsWith("ACS3D", StringComparison.Ordinal)
            ? EnsurePrefabNodeIdentities3D(src, components)
            : components;
    }

    /// <summary>3D source textへ不足PSID3Dを補い、変更時だけ原本へatomic書込する。</summary>
    private static string EnsurePrefabNodeIdentities3D(string src, string components)
    {
        if (!PrefabNodeIdentity3D.TryEnsureSource(src, components, out string identified, out int added, out string error)) throw new InvalidDataException(error);
        if (added > 0)
        {
            string currentSource = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8);
            string currentComponents = IsBlueprint(src) ? AcsbpFormat.ExtractCmp(currentSource) : currentSource;
            if (!string.Equals(currentComponents, components, StringComparison.Ordinal)) throw new IOException("読込後にPrefab原本が変更されました。再度操作してください。");
            string updatedSource = IsBlueprint(src) ? AcsbpFormat.ReplaceCmp(currentSource, identified) : identified;
            SceneSourceFile.WriteAtomicText(src, updatedSource);
        }
        return identified;
    }

    /// <summary>コンポーネント木 comp を src へ書き戻す (.acsbp は CMP ブロックだけ差し替えて VAR/graph を温存)。</summary>
    private static void WriteComponentsTo(string src, string comp)
    {
        if (IsBlueprint(src) && System.IO.File.Exists(src))
        {
            string existing = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8);
            SceneSourceFile.WriteAtomicText(src, AcsbpFormat.ReplaceCmp(existing, comp));
        }
        else SceneSourceFile.WriteAtomicText(src, comp);
    }

    /// <summary>プレハブテンプレートは自己リンクを持たないため、sourceとinstance ID行を除去する。</summary>
    private static string StripPrefabLinks(string text) =>
        System.Text.RegularExpressions.Regex.Replace(text, @"^(?:PFAB(?:3D)?|PINS3D|POVR3D|PCOVR3D) .*\r?\n?", "",
            System.Text.RegularExpressions.RegexOptions.Multiline);

    /// <summary>Nativeへ明示入力する32桁小文字hexの新規3D Prefab instance IDを作る。</summary>
    private static string NewPrefabInstanceId3D() =>
        Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);

    private bool AllowAssetPlacement(
        bool payloadUses3D,
        string assetLabel,
        string fileName)
    {
        AssetScenePlacementDecision decision =
            AssetScenePlacementPolicy.Evaluate(
                activeSourceUses3D: _view3d,
                payloadUses3D);
        if (decision == AssetScenePlacementDecision.Allow)
            return true;

        string message = AssetScenePlacementPolicy.RejectionMessage(
            decision,
            assetLabel,
            fileName);
        StatusText.Text = message;
        Log(message, "Asset", LogLevel.Warn);
        return false;
    }

    /// <summary>.acsprefab を読み、parentId 配下にインスタンス化する(id 再マップは ABI 側)。</summary>
    private void InstantiatePrefab(string path, int parentId)
    {
        if (Engine == IntPtr.Zero) return;
        string text;
        try { text = System.IO.File.ReadAllText(path, System.Text.Encoding.UTF8); }
        catch (Exception ex) { Log("プレハブ読込エラー: " + ex.Message); return; }
        bool payloadUses3D = text.TrimStart().StartsWith(
            "ACS3D",
            StringComparison.Ordinal);
        if (!AllowAssetPlacement(
                payloadUses3D,
                "Prefab",
                System.IO.Path.GetFileName(path)))
        {
            return;
        }
        if (payloadUses3D)   // 3D プレハブ (ACS3D テキスト) → 3D サブツリーとして実体化
        {
            try { text = EnsurePrefabNodeIdentities3D(path, text); }
            catch (Exception ex) { Log("Prefab node identity移行エラー: " + ex.Message); return; }
            int parent3d = EngineInterop.acs_editor_selected3d(Engine);
            int rid = EngineInterop.acs_editor_prefab_instance3d_instantiate(
                Engine,
                path,
                NewPrefabInstanceId3D(),
                text,
                parent3d);
            if (rid >= 0)
            {
                RefreshAfterSceneChange();
                Log($"プレハブをインスタンス化: {System.IO.Path.GetFileName(path)} → 3D node {rid}");
            }
            else Log("プレハブのインスタンス化に失敗: " + System.IO.Path.GetFileName(path));
            return;
        }
        int id = EngineInterop.acs_editor_paste_subtree(Engine, text, parentId);
        if (id >= 0)
        {
            EngineInterop.acs_editor_node_set_prefab_src(Engine, id, path);   // instance-of リンクを張る
            BuildHierarchy();
            _selectedId = id;
            SelectHierarchyItem(id);
            RecordSceneDocumentChange("Instantiate Prefab");
            Log($"プレハブをインスタンス化: {System.IO.Path.GetFileName(path)} → node {id}");
        }
        else Log("プレハブのインスタンス化に失敗: " + System.IO.Path.GetFileName(path));
    }

    /// <summary>インスタンスを prefabText から再生成する(位置/親は維持)。UI 更新はしない。新 id を返す。</summary>
    private int ReinstantiateInstance(int id, string src, string prefabText)
    {
        int parent = EngineInterop.acs_editor_node_parent(Engine, id);
        EngineInterop.acs_editor_node_get_transform(Engine, id, out float x, out float y, out float r, out float sx, out float sy);
        EngineInterop.acs_editor_node_delete(Engine, id);
        int nid = EngineInterop.acs_editor_paste_subtree(Engine, prefabText, parent);
        if (nid >= 0)
        {
            EngineInterop.acs_editor_node_set_transform(Engine, nid, x, y, r, sx, sy);   // 位置は維持
            EngineInterop.acs_editor_node_set_prefab_src(Engine, nid, src);
        }
        return nid;
    }

    /// <summary>src と同じプレハブを指す «他の» インスタンスの id を集める。</summary>
    private System.Collections.Generic.List<int> FindPrefabInstances(string src, int except)
    {
        var list = new System.Collections.Generic.List<int>();
        int cnt = EngineInterop.acs_editor_node_count(Engine);
        for (int i = 0; i < cnt; i++)
        {
            int nid = EngineInterop.acs_editor_node_id_at(Engine, i);
            if (nid == except) continue;
            if (string.Equals(EngineInterop.NodePrefabSrc(Engine, nid), src, StringComparison.OrdinalIgnoreCase))
                list.Add(nid);
        }
        return list;
    }

    /// <summary>3D instanceをNative transactionで再生成し、必要なら明示root overrideも維持する。新id / -1。</summary>
    private int RefreshPrefabInstance3D(
        int id,
        string src,
        string prefabText,
        bool preserveRootOverrides)
    {
        if (!preserveRootOverrides)
            return EngineInterop.acs_editor_prefab_instance3d_refresh(
                Engine,
                id,
                src,
                prefabText);
        PrefabRootProperty3D preserveMask =
            EngineInterop.acs_editor_prefab_instance3d_root_override_mask(
                Engine,
                id);
        return EngineInterop.acs_editor_prefab_instance3d_refresh_with_root_overrides(
            Engine,
            id,
            src,
            prefabText,
            preserveMask);
    }

    /// <summary>FindPrefabInstances の 3D 版 (3D ノードを走査)。</summary>
    private System.Collections.Generic.List<int> FindPrefabInstances3D(string src, int except)
    {
        var list = new System.Collections.Generic.List<int>();
        int cnt = EngineInterop.acs_editor_node3d_count(Engine);
        for (int i = 0; i < cnt; i++)
        {
            int nid = EngineInterop.acs_editor_node3d_id_at(Engine, i);
            if (nid == except) continue;
            if (string.Equals(EngineInterop.NodePrefabSrc3D(Engine, nid), src, StringComparison.OrdinalIgnoreCase))
                list.Add(nid);
        }
        return list;
    }

    /// <summary>この編集をプレハブへ反映し、«シーン内の全インスタンス» を新プレハブで更新する(位置は維持)。</summary>
    private void ApplyToPrefab(int id)
    {
        if (Engine == IntPtr.Zero) return;
        string src = _view3d ? EngineInterop.NodePrefabSrc3D(Engine, id) : EngineInterop.NodePrefabSrc(Engine, id);
        if (string.IsNullOrEmpty(src)) return;
        string comp = StripPrefabLinks(_view3d ? EngineInterop.CopySubtree3D(Engine, id) : EngineInterop.CopySubtree(Engine, id));
        if (string.IsNullOrEmpty(comp)) { Log("Apply 失敗 (直列化が空)。"); return; }
        if (_view3d && !PrefabNodeIdentity3D.TryEnsureSource(src, comp, out comp, out _, out string identityError)) { Log("Apply node identityエラー: " + identityError); return; }
        try { WriteComponentsTo(src, comp); }   // .acsbp は CMP だけ差し替え (VAR/graph 温存)、.acsprefab は全文
        catch (Exception ex) { Log("Apply エラー: " + ex.Message); return; }

        if (_view3d &&
            EngineInterop.acs_editor_prefab_instance3d_clear_root_overrides(
                Engine,
                id,
                PrefabRootProperty3D.All) == 0)
        {
            Log(
                "Apply後の3D Prefab root override状態を解消できませんでした。",
                "Asset",
                LogLevel.Warn);
        }
        if (_view3d &&
            EngineInterop
                .acs_editor_prefab_instance3d_clear_root_component_property_overrides(
                    Engine,
                    id) == 0)
        {
            Log(
                "Apply後の3D Prefab root component override状態を解消できませんでした。",
                "Asset",
                LogLevel.Warn);
        }

        // 他の全インスタンスを再生成 (id を先に集めてから処理 = 走査中の構造変更を回避)。
        var targets = _view3d ? FindPrefabInstances3D(src, id) : FindPrefabInstances(src, id);
        int updated = 0;
        foreach (int t in targets)
            if ((_view3d ? RefreshPrefabInstance3D(t, src, comp, preserveRootOverrides: true) : ReinstantiateInstance(t, src, comp)) >= 0) updated++;
        if (_view3d)
        {
            EngineInterop.acs_editor_select3d(Engine, id);
            RefreshAfterSceneChange();
        }
        else
        {
            BuildHierarchy();
            _selectedId = id;
            SelectHierarchyItem(id);
            if (updated > 0)
                RecordSceneDocumentChange("Apply Prefab");
        }
        Log($"{(IsBlueprint(src) ? "Blueprint" : "プレハブ")}へ反映 (Apply) → {System.IO.Path.GetFileName(src)} ({updated} 個のインスタンスを更新)",
            "Asset", LogLevel.Success);
    }

    /// <summary>このインスタンスをプレハブの状態へ戻す(prefab → instance。編集を破棄して再生成)。</summary>
    private void RevertToPrefab(int id)
    {
        if (Engine == IntPtr.Zero) return;
        string src = _view3d ? EngineInterop.NodePrefabSrc3D(Engine, id) : EngineInterop.NodePrefabSrc(Engine, id);
        if (string.IsNullOrEmpty(src) || !System.IO.File.Exists(src)) { Log("Revert 失敗 (プレハブが見つからない)。"); return; }
        string comp;
        try { comp = ReadComponentsFor(src); }
        catch (Exception ex) { Log("Revert 読込エラー: " + ex.Message); return; }
        if (string.IsNullOrWhiteSpace(comp)) { Log("Revert 失敗 (コンポーネント木が空)。"); return; }
        int nid = _view3d ? RefreshPrefabInstance3D(id, src, comp, preserveRootOverrides: false) : ReinstantiateInstance(id, src, comp);
        if (nid >= 0)
        {
            if (_view3d) { RefreshAfterSceneChange(); }
            else
            {
                BuildHierarchy();
                _selectedId = nid;
                SelectHierarchyItem(nid);
                RecordSceneDocumentChange("Revert Prefab");
            }
            Log($"{(IsBlueprint(src) ? "Blueprint" : "プレハブ")}へ復元 (Revert) ← {System.IO.Path.GetFileName(src)}", "Asset", LogLevel.Info);
        }
    }

    /// <summary>指定した3D root propertyだけを原本値へ戻し、他のoverrideを維持する。</summary>
    private void RevertPrefabRootOverride3D(int id, PrefabRootProperty3D property)
    {
        if (Engine == IntPtr.Zero || property == PrefabRootProperty3D.None || (property & ~PrefabRootProperty3D.All) != PrefabRootProperty3D.None) return;
        string src = EngineInterop.NodePrefabSrc3D(Engine, id);
        if (string.IsNullOrEmpty(src) || !System.IO.File.Exists(src)) { Log("Selective Revert失敗 (プレハブが見つからない)。"); return; }
        string comp;
        try { comp = ReadComponentsFor(src); }
        catch (Exception ex) { Log("Selective Revert読込エラー: " + ex.Message); return; }
        if (string.IsNullOrWhiteSpace(comp)) { Log("Selective Revert失敗 (コンポーネント木が空)。"); return; }
        int nid = EngineInterop.acs_editor_prefab_instance3d_revert_root_overrides(Engine, id, src, comp, property);
        if (nid < 0) { Log($"Selective Revert失敗 ({property})。", "Asset", LogLevel.Warn); return; }
        RefreshAfterSceneChange();
        Log($"3D Prefab rootの{property}だけを原本値へ復元しました。", "Asset", LogLevel.Info);
    }

    /// <summary>指定した3D root component propertyだけを原本値へ戻し、他のoverrideを維持する。</summary>
    private void RevertPrefabRootComponentPropertyOverride3D(int id, int slot, int property, string label)
    {
        if (Engine == IntPtr.Zero || slot < 0 || property < 0 || property >= 32) return;
        uint currentMask = EngineInterop
            .acs_editor_prefab_instance3d_root_component_property_override_mask(
                Engine,
                id,
                slot);
        uint propertyBit = 1u << property;
        if ((currentMask & propertyBit) == 0u)
        {
            Log($"Selective Component Revert失敗 ({label}はoverrideされていません)。", "Asset", LogLevel.Warn);
            return;
        }
        string src = EngineInterop.NodePrefabSrc3D(Engine, id);
        if (string.IsNullOrEmpty(src) || !System.IO.File.Exists(src))
        {
            Log("Selective Component Revert失敗 (プレハブが見つからない)。");
            return;
        }
        string comp;
        try { comp = ReadComponentsFor(src); }
        catch (Exception ex) { Log("Selective Component Revert読込エラー: " + ex.Message); return; }
        if (string.IsNullOrWhiteSpace(comp))
        {
            Log("Selective Component Revert失敗 (コンポーネント木が空)。");
            return;
        }
        int nid = EngineInterop.acs_editor_prefab_instance3d_revert_root_component_property_override(Engine, id, src, comp, slot, property);
        if (nid < 0)
        {
            Log($"Selective Component Revert失敗 ({label})。", "Asset", LogLevel.Warn);
            return;
        }
        RefreshAfterSceneChange();
        Log($"3D Prefab root componentの{label}だけを原本値へ復元しました。", "Asset", LogLevel.Info);
    }

    /// <summary>指定した3D root component propertyだけを原本へ反映し、他のoverrideを維持する。</summary>
    private void ApplyPrefabRootComponentPropertyOverride3D(int id, int slot, int property, string componentTypeName, string label)
    {
        if (Engine == IntPtr.Zero || slot < 0 || property < 0 || property >= 32) return;
        uint currentMask = EngineInterop
            .acs_editor_prefab_instance3d_root_component_property_override_mask(
                Engine,
                id,
                slot);
        uint propertyBit = 1u << property;
        if ((currentMask & propertyBit) == 0u)
        {
            Log($"Selective Component Apply失敗 ({label}はoverrideされていません)。", "Asset", LogLevel.Warn);
            return;
        }
        int propertyCount = EngineInterop.acs_editor_component_prop_count(componentTypeName);
        if (property >= propertyCount ||
            EngineInterop.acs_editor_node3d_component_prop_get(
                Engine,
                id,
                slot,
                property,
                out float x,
                out float y,
                out float z,
                out float w) == 0)
        {
            Log($"Selective Component Apply失敗 ({label}の値を取得できません)。", "Asset", LogLevel.Warn);
            return;
        }

        string src = EngineInterop.NodePrefabSrc3D(Engine, id);
        if (string.IsNullOrEmpty(src) || !System.IO.File.Exists(src))
        {
            Log("Selective Component Apply失敗 (プレハブが見つからない)。");
            return;
        }

        string originalSource;
        try { originalSource = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8); }
        catch (Exception ex) { Log("Selective Component Apply読込エラー: " + ex.Message); return; }
        string components = IsBlueprint(src)
            ? AcsbpFormat.ExtractCmp(originalSource)
            : originalSource;
        if (string.IsNullOrWhiteSpace(components))
        {
            Log("Selective Component Apply失敗 (コンポーネント木が空)。");
            return;
        }

        if (!PrefabRootComponentPropertyApply3D.TryBuildSource(
                components,
                componentTypeName,
                property,
                new PrefabRootComponentPropertyValue3D(x, y, z, w),
                out string updatedComponents,
                out string calculationError))
        {
            Log("Selective Component Apply失敗: " + calculationError, "Asset", LogLevel.Warn);
            return;
        }
        string updatedSource = IsBlueprint(src)
            ? AcsbpFormat.ReplaceCmp(originalSource, updatedComponents)
            : updatedComponents;

        try
        {
            string currentSource = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8);
            if (!string.Equals(currentSource, originalSource, StringComparison.Ordinal))
                throw new IOException("読込後に原本が変更されました。再度Applyしてください。");
            SceneSourceFile.WriteAtomicText(src, updatedSource);
        }
        catch (Exception ex)
        {
            Log("Selective Component Apply書込エラー: " + ex.Message, "Asset", LogLevel.Warn);
            return;
        }

        if (EngineInterop.acs_editor_prefab_instance3d_clear_root_component_property_override(
                Engine,
                id,
                slot,
                property) == 0)
        {
            try { SceneSourceFile.WriteAtomicText(src, originalSource); }
            catch (Exception rollbackError)
            {
                Log(
                    "Selective Component Apply rollbackエラー: " + rollbackError.Message,
                    "Asset",
                    LogLevel.Error);
            }
            Log("Selective Component Apply後のoverrideを解消できませんでした。", "Asset", LogLevel.Warn);
            return;
        }

        System.Collections.Generic.List<int> targets = FindPrefabInstances3D(src, id);
        int updated = 0;
        foreach (int target in targets)
        {
            if (RefreshPrefabInstance3D(
                    target,
                    src,
                    updatedComponents,
                    preserveRootOverrides: true) >= 0)
            {
                updated++;
            }
        }
        EngineInterop.acs_editor_select3d(Engine, id);
        RefreshAfterSceneChange();
        int refreshFailures = targets.Count - updated;
        Log(
            $"3D Prefab root componentの{label}だけを原本へ反映しました ({updated}個更新、{refreshFailures}個失敗)。",
            "Asset",
            refreshFailures == 0 ? LogLevel.Success : LogLevel.Warn);
    }

    /// <summary>指定した3D root propertyだけを原本へ反映し、残りのsourceとoverrideを維持する。</summary>
    private void ApplyPrefabRootOverride3D(int id, PrefabRootProperty3D property)
    {
        if (Engine == IntPtr.Zero || property == PrefabRootProperty3D.None ||
            (property & ~PrefabRootProperty3D.All) != PrefabRootProperty3D.None)
        {
            return;
        }
        PrefabRootProperty3D currentMask =
            EngineInterop.acs_editor_prefab_instance3d_root_override_mask(Engine, id);
        if ((currentMask & property) != property)
        {
            Log($"Selective Apply失敗 ({property}はoverrideされていません)。", "Asset", LogLevel.Warn);
            return;
        }

        string src = EngineInterop.NodePrefabSrc3D(Engine, id);
        if (string.IsNullOrEmpty(src) || !System.IO.File.Exists(src))
        {
            Log("Selective Apply失敗 (プレハブが見つからない)。");
            return;
        }

        string originalSource;
        try { originalSource = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8); }
        catch (Exception ex) { Log("Selective Apply読込エラー: " + ex.Message); return; }
        string components = IsBlueprint(src)
            ? AcsbpFormat.ExtractCmp(originalSource)
            : originalSource;
        if (string.IsNullOrWhiteSpace(components))
        {
            Log("Selective Apply失敗 (コンポーネント木が空)。");
            return;
        }

        var color = new float[4];
        if ((property & PrefabRootProperty3D.Color) != PrefabRootProperty3D.None &&
            EngineInterop.acs_editor_node3d_get_color(Engine, id, color) == 0)
        {
            Log("Selective Apply失敗 (root Colorを取得できません)。");
            return;
        }
        var values = new PrefabRootPropertyValues3D(
            Visible: EngineInterop.acs_editor_node3d_get_visible(Engine, id) != 0,
            Enabled: EngineInterop.acs_editor_node3d_get_enabled(Engine, id) != 0,
            Red: color[0],
            Green: color[1],
            Blue: color[2],
            Alpha: color[3]);
        if (!PrefabRootPropertyApply3D.TryBuildSource(
                components,
                property,
                values,
                out string updatedComponents,
                out string calculationError))
        {
            Log("Selective Apply失敗: " + calculationError, "Asset", LogLevel.Warn);
            return;
        }
        string updatedSource = IsBlueprint(src)
            ? AcsbpFormat.ReplaceCmp(originalSource, updatedComponents)
            : updatedComponents;

        try
        {
            string currentSource = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8);
            if (!string.Equals(currentSource, originalSource, StringComparison.Ordinal))
                throw new IOException("読込後に原本が変更されました。再度Applyしてください。");
            SceneSourceFile.WriteAtomicText(src, updatedSource);
        }
        catch (Exception ex)
        {
            Log("Selective Apply書込エラー: " + ex.Message, "Asset", LogLevel.Warn);
            return;
        }

        if (EngineInterop.acs_editor_prefab_instance3d_clear_root_overrides(
                Engine,
                id,
                property) == 0)
        {
            try { SceneSourceFile.WriteAtomicText(src, originalSource); }
            catch (Exception rollbackError)
            {
                Log(
                    "Selective Apply rollbackエラー: " + rollbackError.Message,
                    "Asset",
                    LogLevel.Error);
            }
            Log("Selective Apply後のroot overrideを解消できませんでした。", "Asset", LogLevel.Warn);
            return;
        }

        System.Collections.Generic.List<int> targets = FindPrefabInstances3D(src, id);
        int updated = 0;
        foreach (int target in targets)
        {
            if (RefreshPrefabInstance3D(
                    target,
                    src,
                    updatedComponents,
                    preserveRootOverrides: true) >= 0)
            {
                updated++;
            }
        }
        EngineInterop.acs_editor_select3d(Engine, id);
        RefreshAfterSceneChange();
        int refreshFailures = targets.Count - updated;
        Log(
            $"3D Prefab rootの{property}だけを原本へ反映しました ({updated}個更新、{refreshFailures}個失敗)。",
            "Asset",
            refreshFailures == 0 ? LogLevel.Success : LogLevel.Warn);
    }

    // ===== Components: 登録 Component 型のアタッチ表示 / 編集 =====
    private void PopulateComponentCombo()
    {
        CompAddBox.Items.Clear();
        int count = EngineInterop.acs_editor_type_count();
        for (int i = 0; i < count; i++)
        {
            if (EngineInterop.CategoryLabel(EngineInterop.acs_editor_type_category_at(i)) == "Component")
                CompAddBox.Items.Add(EngineInterop.TypeName(i));
        }
        if (CompAddBox.Items.Count > 0) CompAddBox.SelectedIndex = 0;
    }

    private void PopulateComponents(int id)
    {
        CompList.Children.Clear();
        if (Engine == IntPtr.Zero) return;
        var panel2 = (System.Windows.Media.Brush)FindResource("Panel2");
        var dim    = (System.Windows.Media.Brush)FindResource("TextDim");

        // プレハブ・インスタンスなら「Prefab: X」+ Apply/Revert バナーを先頭に出す。
        string prefabSrc = EngineInterop.NodePrefabSrc(Engine, id);
        if (!string.IsNullOrEmpty(prefabSrc))
        {
            var banner = new StackPanel { Margin = new Thickness(0, 0, 0, 6) };
            banner.Children.Add(new TextBlock
            {
                Text = (IsBlueprint(prefabSrc) ? "◆ Blueprint: " : "◆ Prefab: ") + System.IO.Path.GetFileName(prefabSrc),
                Foreground = (System.Windows.Media.Brush)FindResource("Accent"), FontSize = 11, FontWeight = FontWeights.SemiBold,
            });
            var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 3, 0, 0) };
            var apply  = new Button { Content = "Apply", FontSize = 11, Padding = new Thickness(10, 2, 10, 2), Margin = new Thickness(0, 0, 4, 0),
                                      ToolTip = "この編集をプレハブ側へ反映 (instance → prefab)" };
            var revert = new Button { Content = "Revert", FontSize = 11, Padding = new Thickness(10, 2, 10, 2),
                                      ToolTip = "編集を破棄しプレハブの状態へ戻す (prefab → instance)" };
            int curId = id;
            apply.Click  += (_, __) => ApplyToPrefab(curId);
            revert.Click += (_, __) => RevertToPrefab(curId);
            row.Children.Add(apply); row.Children.Add(revert);
            banner.Children.Add(row);
            CompList.Children.Add(new Border
            {
                Background = panel2, CornerRadius = new CornerRadius(5),
                Padding = new Thickness(8, 6, 8, 7), Margin = new Thickness(0, 0, 0, 6), Child = banner,
            });
        }

        int count = EngineInterop.acs_editor_node_component_count(Engine, id);
        if (count == 0)
        {
            CompList.Children.Add(new TextBlock
            {
                Text = "（コンポーネントなし）", Foreground = dim, FontSize = 11, Margin = new Thickness(0, 2, 0, 2),
            });
            return;
        }

        int shownComponents = 0;
        for (int i = 0; i < count; i++)
        {
            int idx = i;   // = ABI の component slot
            string cname = EngineInterop.ComponentName(Engine, id, i);
            if (!DetailsComponentMatches(cname)) continue;

            var inner = new StackPanel();

            // 編集プロパティ (reflection スキーマ駆動)。0 個なら明示する。
            int pc = EngineInterop.acs_editor_component_prop_count(cname);
            if (pc == 0)
                inner.Children.Add(new TextBlock
                {
                    Text = "(編集可能なプロパティなし)", Foreground = dim, FontSize = 11, Margin = new Thickness(0, 1, 0, 0),
                });
            else
            {
                // カテゴリ (UPROPERTY(Category=…)) ごとにグループ化して小見出しを挟む。
                string lastCat = "\0";   // 初回必ず不一致
                for (int p = 0; p < pc; p++)
                {
                    string cat = EngineInterop.ComponentPropCategory(cname, p);
                    if (cat != lastCat && cat.Length > 0)        // カテゴリが変わったら見出し
                    {
                        inner.Children.Add(new TextBlock
                        {
                            Text = cat, Foreground = dim, FontSize = 10, FontWeight = FontWeights.SemiBold,
                            Margin = new Thickness(0, p == 0 ? 0 : 5, 0, 1),
                        });
                    }
                    lastCat = cat;
                    var row = BuildPropEditor(id, idx, cname, p);
                    if (row != null) inner.Children.Add(row);   // null = Hidden 指定子 → 出さない
                }
            }

            // CallInEditor (ACS_FUNCTION) メソッドをボタンで出す → クリックで invoke。
            int mc = EngineInterop.acs_editor_component_method_count(cname);
            int curSlot = idx;
            for (int mi = 0; mi < mc; mi++)
            {
                int mflags = EngineInterop.acs_editor_component_method_flags_at(cname, mi);
                if ((mflags & 0x2) == 0) continue;            // CallInEditor 指定のみボタン化
                string mname = EngineInterop.ComponentMethodName(cname, mi);
                var btn = new Button
                {
                    Content = "▶ " + mname, FontSize = 11, Padding = new Thickness(8, 2, 8, 2),
                    Margin = new Thickness(0, 4, 0, 0), HorizontalAlignment = HorizontalAlignment.Left,
                };
                btn.Click += (_, __) =>
                {
                    if (EngineInterop.acs_editor_node_invoke_method(Engine, id, curSlot, mname) != 0)
                    {
                        NotifySceneMutationPending(
                            $"Invoke {cname}.{mname}",
                            $"component.{curSlot}.{cname}.method.{mname}",
                            id);
                        Log($"{cname}.{mname}() を呼び出し", "General", LogLevel.Info);
                    }
                    else Log($"{cname}.{mname}() の呼び出しに失敗");
                };
                inner.Children.Add(btn);
            }

            int capturedSlot = idx;
            CompList.Children.Add(ComponentCard(cname, inner, native: false, remove: () =>
            {
                EngineInterop.acs_editor_node_remove_component_at(Engine, id, capturedSlot);
                PopulateComponents(id);
                RecordSceneDocumentChange("Remove Component");
            }));
            shownComponents++;
        }

        if (shownComponents == 0 && _detailsFilter.Length > 0)
            CompList.Children.Add(EmptyDetailsResult("No components match this filter."));
    }

    /// <summary>
    /// 1 つの編集プロパティ行を組み立てる。種別 (EFieldKind) に応じて
    /// checkbox (Bool) / 整数 box (I32,U32) / 数値 box×N (F32,FVec2-4) を出す。
    /// 編集確定で acs_editor_node_component_prop_set を呼ぶ。
    /// </summary>
    private FrameworkElement? BuildPropEditor(int id, int slot, string typeName, int prop, bool is3d = false)
    {
        string pname = EngineInterop.ComponentPropName(typeName, prop);
        int kind = EngineInterop.acs_editor_component_prop_kind_at(typeName, prop);
        int flags = EngineInterop.acs_editor_component_prop_flags_at(typeName, prop);
        bool hidden   = (flags & 0x2) != 0;   // FIELD_HIDDEN → 出さない
        bool readOnly = (flags & 0x1) != 0;   // FIELD_READONLY (VisibleAnywhere) → 表示のみ
        bool colorField = (flags & 0x8) != 0; // FIELD_COLOR → color swatch/picker
        if (hidden) return null;
        // スキーマは 2D/3D 共通 (型名駆動)。値の get/set だけ 2D/3D の ABI を切替える。
        float vx, vy, vz, vw;
        if (is3d)
            EngineInterop.acs_editor_node3d_component_prop_get(Engine, id, slot, prop,
                out vx, out vy, out vz, out vw);
        else
            EngineInterop.acs_editor_node_component_prop_get(Engine, id, slot, prop,
                out vx, out vy, out vz, out vw);
        float[] vals = { vx, vy, vz, vw };
        float[] committed = { vx, vy, vz, vw };   // 最後に ABI へ送った値

        var panel = new DockPanel { Margin = new Thickness(0, 2, 0, 1) };
        if (readOnly) panel.IsEnabled = false;    // 編集不可 (グレー表示)。値は見える
        var label = new TextBlock
        {
            Text = pname, Width = 78, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (System.Windows.Media.Brush)FindResource("TextDim"), FontSize = 11,
            FontFamily = new System.Windows.Media.FontFamily("Consolas"),
        };
        DockPanel.SetDock(label, Dock.Left);
        panel.Children.Add(label);

        // String (kind 7): 値は 4 float で保持するため文字列は表現できない → 注記のみ。
        if (kind == 7)
        {
            panel.Children.Add(new TextBlock
            {
                Text = "(string — ここでは編集不可)", VerticalAlignment = VerticalAlignment.Center,
                Foreground = (System.Windows.Media.Brush)FindResource("TextDim"), FontSize = 11,
            });
            return panel;
        }

        // 値が実際に変わったときだけ ABI へ反映する。これにより (a) Enter の二重発火
        // (KeyDown→Apply + ClearFocus が誘発する LostKeyboardFocus→Apply) と、(b) 無変更の
        // フォーカス喪失/タブ移動で、undo スナップショットが量産されるのを防ぐ。
        void Commit()
        {
            if (vals[0] == committed[0] && vals[1] == committed[1]
                && vals[2] == committed[2] && vals[3] == committed[3]) return;
            int changed;
            if (is3d)
                changed = EngineInterop.acs_editor_node3d_component_prop_set(
                    Engine, id, slot, prop, vals[0], vals[1], vals[2], vals[3]);
            else
                changed = EngineInterop.acs_editor_node_component_prop_set(
                    Engine, id, slot, prop, vals[0], vals[1], vals[2], vals[3]);
            if (changed == 0) return;
            committed[0] = vals[0]; committed[1] = vals[1];
            committed[2] = vals[2]; committed[3] = vals[3];
            if (is3d)
                MarkPrefabRootComponentPropertyOverride3D(id, slot, prop);
            NotifySceneMutationPending(
                $"Edit {pname}",
                $"component.{slot}.{typeName}.property.{prop}.{pname}",
                id);
        }

        // Bool: チェックボックス。
        if (kind == 0)
        {
            var cb = new CheckBox { IsChecked = vals[0] != 0.0f, VerticalAlignment = VerticalAlignment.Center };
            cb.Checked   += (_, __) => { vals[0] = 1.0f; Commit(); };
            cb.Unchecked += (_, __) => { vals[0] = 0.0f; Commit(); };
            panel.Children.Add(cb);
            return panel;
        }

        // ObjectRef (kind 9): 他ノードへの参照ピッカー。値 = 参照先の安定 ID を float[0] に保持 (-1=なし)。
        if (kind == 9)
        {
            var combo = new ComboBox { MinWidth = 150, FontSize = 11, VerticalAlignment = VerticalAlignment.Center };
            var ids = new System.Collections.Generic.List<int> { -1 };   // 先頭 = (None)
            combo.Items.Add("(None)");
            int count = is3d ? EngineInterop.acs_editor_node3d_count(Engine) : EngineInterop.acs_editor_node_count(Engine);
            for (int i = 0; i < count; i++)
            {
                int nid = is3d ? EngineInterop.acs_editor_node3d_id_at(Engine, i) : EngineInterop.acs_editor_node_id_at(Engine, i);
                if (nid == id) continue;                                  // 自己参照は除外
                string nm = is3d ? Node3DName(nid) : EngineInterop.NodeName(Engine, nid);
                ids.Add(nid);
                combo.Items.Add($"{(string.IsNullOrEmpty(nm) ? "Node" : nm)} (id {nid})");
            }
            int curRef = (int)Math.Round(vals[0]);
            int selIdx = ids.IndexOf(curRef);
            combo.SelectedIndex = selIdx >= 0 ? selIdx : 0;               // 不明な参照は (None) 表示
            combo.SelectionChanged += (_, __) =>
            {
                int si = combo.SelectedIndex;
                if (si >= 0 && si < ids.Count) { vals[0] = ids[si]; Commit(); }
            };
            panel.Children.Add(combo);
            return panel;
        }

        // 既知の列挙系プロパティ (bodyType / shape) はドロップダウンで選ばせる (整数値を保持)。
        string[]? choices = pname switch
        {
            "bodyType" => new[] { "Static", "Dynamic" },
            "shape"    => new[] { "Box", "Circle", "Triangle", "Polygon" },
            _ => null,
        };
        if (choices != null)
        {
            var combo = new ComboBox { MinWidth = 130, FontSize = 11, VerticalAlignment = VerticalAlignment.Center };
            foreach (var ch in choices) combo.Items.Add(ch);
            int sel = (int)Math.Round(vals[0]);
            combo.SelectedIndex = (sel >= 0 && sel < choices.Length) ? sel : 0;
            combo.SelectionChanged += (_, __) => { if (combo.SelectedIndex >= 0) { vals[0] = combo.SelectedIndex; Commit(); } };
            panel.Children.Add(combo);
            return panel;
        }

        // 成分数: FVec2=2, FVec3=3, FVec4=4, それ以外 (F32/I32/U32/Enum)=1。
        int n = kind == 4 ? 2 : kind == 5 ? 3 : kind == 6 ? 4 : 1;
        bool isInt = kind == 1 || kind == 2 || kind == 8;   // Enum も整数値として扱う
        bool colorNamed = pname.IndexOf("color", StringComparison.OrdinalIgnoreCase) >= 0
                       || pname.IndexOf("tint",  StringComparison.OrdinalIgnoreCase) >= 0;
        bool colorVector = colorField && (n == 3 || n == 4);
        string[] axis = (colorField || colorNamed)
            ? new[] { "R", "G", "B", "A" }
            : new[] { "X", "Y", "Z", "W" };

        var boxes = new StackPanel { Orientation = Orientation.Horizontal };
        var editors = new List<TextBox>(n);
        Button? colorSwatch = null;
        static byte ColorByte(float value) =>
            (byte)Math.Clamp(value * 255.0f, 0.0f, 255.0f);
        void RefreshColorSwatch()
        {
            if (colorSwatch == null || n < 3) return;
            colorSwatch.Background = new System.Windows.Media.SolidColorBrush(
                System.Windows.Media.Color.FromRgb(
                    ColorByte(vals[0]),
                    ColorByte(vals[1]),
                    ColorByte(vals[2])));
        }
        for (int c = 0; c < n; c++)
        {
            int ci = c;
            var tb = new TextBox
            {
                Width = 46, Margin = new Thickness(0, 0, 4, 0), FontSize = 11,
                FontFamily = new System.Windows.Media.FontFamily("Consolas"),
                Text = vals[ci].ToString(isInt ? "0" : "0.###", CultureInfo.InvariantCulture),
                ToolTip = n > 1 ? axis[ci] : pname,
            };
            void Apply()
            {
                float v = ParseF(tb.Text, vals[ci]);
                if (isInt) v = (float)Math.Round(v);
                vals[ci] = v;
                tb.Text = v.ToString(isInt ? "0" : "0.###", CultureInfo.InvariantCulture);
                RefreshColorSwatch();
                Commit();
            }
            tb.LostKeyboardFocus += (_, __) => Apply();
            tb.KeyDown += (_, ev) => { if (ev.Key == Key.Enter) { Apply(); Keyboard.ClearFocus(); } };
            EnableScrub(tb, isInt ? 1.0 : 0.1, Apply, isInt);   // 数値欄をドラッグでも増減
            editors.Add(tb);
            boxes.Children.Add(tb);
        }
        if (colorVector)
        {
            colorSwatch = new Button
            {
                Width = 24,
                Height = 20,
                Margin = new Thickness(0, 0, 2, 0),
                Padding = new Thickness(0),
                BorderThickness = new Thickness(1),
                BorderBrush =
                    (System.Windows.Media.Brush)FindResource("CtrlBorder"),
                ToolTip = n == 4
                    ? "RGBA カラーを選択"
                    : "RGB カラーを選択",
            };
            RefreshColorSwatch();
            colorSwatch.Click += (_, __) =>
            {
                for (int component = 0;
                     component < editors.Count;
                     ++component)
                {
                    vals[component] = ParseF(
                        editors[component].Text,
                        vals[component]);
                }
                var initial = System.Windows.Media.Color.FromArgb(
                    n == 4 ? ColorByte(vals[3]) : byte.MaxValue,
                    ColorByte(vals[0]),
                    ColorByte(vals[1]),
                    ColorByte(vals[2]));
                if (!ColorPickerDialog.TryPick(
                        this,
                        initial,
                        allowAlpha: n == 4,
                        out var picked))
                {
                    RefreshColorSwatch();
                    return;
                }

                vals[0] = picked.R / 255.0f;
                vals[1] = picked.G / 255.0f;
                vals[2] = picked.B / 255.0f;
                if (n == 4) vals[3] = picked.A / 255.0f;
                for (int component = 0;
                     component < editors.Count;
                     ++component)
                {
                    editors[component].Text =
                        vals[component].ToString(
                            "0.###",
                            CultureInfo.InvariantCulture);
                }
                RefreshColorSwatch();
                Commit();
            };
            boxes.Children.Add(colorSwatch);
        }
        panel.Children.Add(boxes);
        return panel;
    }

    private void OnAddComponent(object sender, RoutedEventArgs e)
    {
        if (_selectedId < 0 || Engine == IntPtr.Zero) return;
        if (CompAddBox.SelectedItem is not string typeName) return;
        if (EngineInterop.acs_editor_node_add_component(Engine, _selectedId, typeName) != 0)
        {
            PopulateComponents(_selectedId);
            RecordSceneDocumentChange("Add Component");
            Log($"Added component {typeName} → node {_selectedId}.");
        }
    }

    // ===== misc =====
    // 自動分類してログ追加 (実体は MainWindow.Log.cs)。明示タグは Log(msg, tag, level)。
    private void Log(string msg)
    {
        var (tag, level) = ClassifyLog(msg);
        Log(msg, tag, level);
    }

    // ===== File メニュー: シーンの新規 / 開く / 保存 =====
    // ABI がシリアライズ (文字列 ⇄ 実 ANode ツリー) を担い、ファイル I/O はここ (C#) で行う。
    private void ClearSelectionUi()
    {
        _selectedId = -1;
        InspName.Text = "(no selection)";
        InspSub.Text  = "ノードを選択してください";
        InspFields.IsEnabled = false;
        MultiHint.Visibility = Visibility.Collapsed;
        ActionButtons.IsEnabled = false;
    }

    // シーン全体が変わった後 (undo/redo/open/new) に Hierarchy を作り直し、Inspector と
    // ツリーのハイライトを ABI の選択集合に合わせる (無選択なら ClearSelectionUi)。
    private void RefreshAfterSceneChange()
    {
        BuildHierarchy();
        SyncSelectionUi();
        RecordSceneDocumentChange("Scene Structure");
    }

    private async void OnNewScene(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (!await ConfirmSceneReplacementAsync()) return;
        try
        {
            // New replaces the singular managed world. Both native compatibility payloads are
            // cleared so a later save/undo cannot resurrect hidden content from another source.
            EngineInterop.acs_editor_scene_document_new(Engine);
            // The native replacement invalidates retained Camera View requests.
            // Queue the stable-id re-resolution immediately; dispatcher
            // coalescing keeps the later document-change notification from
            // producing a second refresh.
            NotifyCameraViewSceneChanged();
            _scene2DPath = null;
            _scene3DDocumentPath = null;
            _scene2DInitialized =
                _legacySceneSourceMode == SceneDocumentMode.TwoD;
            _scene3DInitialized =
                _legacySceneSourceMode == SceneDocumentMode.ThreeD;
            _scene2DDirty = false;
            _scene3DDirty = false;
            SetCurrentScenePath(null);
            RefreshAfterSceneChange();
            MarkSceneDirty();
            ResetSceneDocumentHistory(_view3d, markSaved: false);
            Log(
                "New empty scene. Legacy source adapter: " +
                (_legacySceneSourceMode == SceneDocumentMode.ThreeD
                    ? ".acs3d"
                    : ".acscene") + ".");
        }
        finally
        {
            CompleteSceneReplacementAutosave();
        }
    }

    private async void OnOpenScene(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "Open ACS Scene",
            Filter =
                "ACS Scene Sources (*.acscene;*.acs3d)|*.acscene;*.acs3d|" +
                "ACS Scene (*.acscene)|*.acscene|" +
                "Legacy ACS 3D Source (*.acs3d)|*.acs3d|" +
                "All files (*.*)|*.*",
            DefaultExt = ".acscene",
        };
        if (dlg.ShowDialog(this) != true) return;

        try
        {
            await OpenScenePathAsync(dlg.FileName);
        }
        catch (Exception error)
        {
            // OpenScenePathAsync owns transactional rollback once a load has
            // begun. This outer event boundary covers failures before that
            // transaction exists (prompt/dispatcher/service failures).
            Log(
                "Open failed: " + error.Message,
                "Scene",
                LogLevel.Error);
        }
    }

    private async System.Threading.Tasks.Task OpenScenePathAsync(
        string requestedPath)
    {
        if (Engine == IntPtr.Zero) return;
        string selectedPath;
        SceneDocumentMode selectedSourceMode;
        try
        {
            selectedPath = ValidateLegacySceneDocumentPath(
                requestedPath,
                out selectedSourceMode);
        }
        catch (Exception ex)
        {
            Log("Open failed: " + ex.Message);
            return;
        }

        bool selectedUses3D =
            selectedSourceMode == SceneDocumentMode.ThreeD;
        SceneRecoveryCandidate? recovery =
            FindRecoveryForOpen(selectedPath, selectedUses3D);
        SceneRecoveryDecision recoveryDecision = SceneRecoveryDecision.Discard;
        if (recovery != null)
        {
            recoveryDecision = await PromptRecoveryAsync(recovery);
            if (recoveryDecision == SceneRecoveryDecision.Cancel) return;
        }
        if (!await ConfirmSceneReplacementAsync()) return;
        ActiveSceneLoad sceneLoad = BeginSceneLoad(
            $"Reading {System.IO.Path.GetFileName(selectedPath)}");
        bool publishScene = false;
        SceneOpenRollbackSnapshot? rollback = null;
        SceneRecoveryCandidate? recoveryToApply = null;
        bool nativeLoadAttempted = false;
        try
        {
            if (recovery != null && recoveryDecision == SceneRecoveryDecision.Discard)
                await DiscardRecoveryAsync(
                    recovery.Identity,
                    resumeAfter: !_replacementAutosaveSuppressed);

            SceneSourceFile.ReadResult source =
                await SceneSourceFile.ReadBoundedTextAsync(
                    selectedPath,
                    sceneLoad.Cancellation.Token);
            if (!source.Exists)
                throw new FileNotFoundException(
                    "The selected scene source no longer exists.",
                    selectedPath);
            string text = source.Text!;
            if (!IsCurrentSceneLoad(sceneLoad))
                return;

            // Manual Open is non-destructive until parsing succeeds. The loading gate hides the
            // current scene, but path/dirty/history and the inactive compatibility payload are
            // committed only after the selected parser accepts the source.
            rollback = CaptureSceneOpenRollbackSnapshot();
            _sceneOpenTransactionInProgress = true;
            nativeLoadAttempted = true;
            int ok = LoadLegacySceneSourceAsDocument(
                Engine, selectedUses3D, text) ? 1 : 0;
            if (ok != 0)
            {
                _legacySceneSourceMode = selectedSourceMode;
                _view3d = selectedUses3D;
                EngineInterop.acs_editor_set_view3d(
                    Engine,
                    selectedUses3D ? 1 : 0);
                _sceneViewMode =
                    EditorSceneViewModePolicy.InitialForLegacySource(selectedPath);
                EditorSceneViewDescriptor openedView =
                    EditorSceneViewModePolicy.Describe(_sceneViewMode);
                EngineInterop.acs_editor_set_ortho3d(
                    Engine,
                    selectedUses3D && openedView.IsOrthographic ? 1 : 0);
                _scene2DPath = selectedUses3D ? null : selectedPath;
                _scene3DDocumentPath = selectedUses3D ? selectedPath : null;
                _scene2DInitialized = !selectedUses3D;
                _scene3DInitialized = selectedUses3D;
                _scene2DDirty = false;
                _scene3DDirty = false;
                SetCurrentScenePath(selectedPath);
                ApplySceneViewModePresentation();
                RefreshAfterSceneChange();
                MarkSceneClean(); // compare against the native canonical form, not source whitespace/order
                Log(
                    $"Loaded scene source ({(selectedUses3D ? ".acs3d" : ".acscene")}) " +
                    $"← {selectedPath}");
                if (recovery != null && recoveryDecision == SceneRecoveryDecision.Recover)
                    recoveryToApply = recovery;
                // This is the final fallible managed commit. If its change notification throws,
                // the catch path restores the document checkpoint together with both native graphs.
                ResetSceneDocumentHistory(_view3d, markSaved: true);
                publishScene = true;
            }
            else
            {
                bool restored = rollback != null &&
                    RestoreSceneOpenRollbackSnapshot(rollback);
                if (restored)
                {
                    Log(
                        "Scene load failed (unrecognized format). The current scene was restored unchanged.",
                        "Scene",
                        LogLevel.Error);
                }
                else
                {
                    // A rejected parser is allowed to have partially mutated native state. If
                    // canonical rollback itself is rejected, fail closed to an explicit blank.
                    _legacySceneSourceMode = selectedSourceMode;
                    _view3d = selectedUses3D;
                    EstablishEmptySceneDocument(
                        selectedUses3D,
                        sourcePath: null,
                        keepSourcePath: false);
                    ApplySceneViewModePresentation();
                    RefreshAfterSceneChange();
                    MarkSceneDirty();
                    ResetSceneDocumentHistory(_view3d, markSaved: false);
                    Log(
                        "Scene load and native rollback both failed. The viewport was reset " +
                        "to a detached empty scene.",
                        "Scene",
                        LogLevel.Error);
                }
                publishScene = true;
            }
        }
        catch (OperationCanceledException)
            when (!IsCurrentSceneLoad(sceneLoad) ||
                  sceneLoad.Cancellation.IsCancellationRequested)
        {
            // A newer generation or editor shutdown owns presentation and input recovery.
        }
        catch (Exception ex)
        {
            if (IsCurrentSceneLoad(sceneLoad))
            {
                bool restored = !nativeLoadAttempted;
                if (!restored && rollback != null)
                {
                    restored = RestoreSceneOpenRollbackSnapshot(rollback);
                }
                if (restored)
                {
                    Log(
                        "Open failed; the current scene remains active: " + ex.Message,
                        "Scene",
                        LogLevel.Error);
                }
                else if (Engine != IntPtr.Zero)
                {
                    _legacySceneSourceMode = selectedSourceMode;
                    _view3d = selectedUses3D;
                    EstablishEmptySceneDocument(
                        selectedUses3D,
                        sourcePath: null,
                        keepSourcePath: false);
                    ApplySceneViewModePresentation();
                    RefreshAfterSceneChange();
                    MarkSceneDirty();
                    ResetSceneDocumentHistory(_view3d, markSaved: false);
                    Log(
                        "Open failed and native rollback was unavailable. The viewport was " +
                        "reset to a detached empty scene: " + ex.Message,
                        "Scene",
                        LogLevel.Error);
                }
                publishScene = true;
            }
        }
        finally
        {
            _sceneOpenTransactionInProgress = false;
            CompleteSceneLoad(
                sceneLoad,
                publishScene,
                publishedCurrentScene =>
                {
                    if (CameraViewScenePublicationPolicy.ShouldRefresh(
                            publishedCurrentScene,
                            nativeLoadAttempted))
                    {
                        NotifyCameraViewSceneChanged();
                    }
                });
            CompleteSceneReplacementAutosave();
        }

        // Recovery is a separate verified transaction over the now-published source baseline.
        // Keeping its worker waits outside File/Open prevents an await or autosave failure from
        // rolling a successfully committed source into a mixed managed/native state.
        if (publishScene && recoveryToApply != null)
            await ApplyRecoveryCandidateAsync(recoveryToApply);
    }

    private async void OnSaveScene(object sender, RoutedEventArgs e) =>
        await SaveHostedSceneDocumentAsync();

    private async System.Threading.Tasks.Task<bool> SaveActiveSceneAsync()
    {
        if (Engine == IntPtr.Zero) return false;
        if (!TryBeginSceneSourceSave(
                out SceneSourceSaveScope? saveScope,
                out string saveBlockedReason))
        {
            Log(
                "Scene save is unavailable: " + saveBlockedReason + ".",
                "Scene",
                LogLevel.Warn);
            return false;
        }
        using SceneSourceSaveScope saveLease = saveScope!;

        if (_view3d)
            return await Save3DSceneAsync();   // Loaded .acs3d source adapter
        // プロジェクトの初期シーンを開いているなら、そこへ直接上書き保存 (ダイアログ無し)。
        string? previousPath = _currentScenePath;
        string? target = _currentScenePath;
        if (string.IsNullOrEmpty(target))
        {
            var dlg = new Microsoft.Win32.SaveFileDialog
            {
                Title = "Save ACS Scene",
                Filter = "ACS Scene (*.acscene)|*.acscene|All files (*.*)|*.*",
                DefaultExt = ".acscene",
                FileName = "scene.acscene",
                InitialDirectory = _project?.AssetsDir,
            };
            if (dlg.ShowDialog(this) != true) return false;
            target = dlg.FileName;
        }
        if (!saveLease.TryAcquireProjectAssetMutationLock(
                out string mutationBlockedReason))
        {
            Log(
                "Scene save is unavailable: " + mutationBlockedReason,
                "Scene",
                LogLevel.Warn);
            return false;
        }
        try
        {
            target = ValidateSceneDocumentPath(target, use3D: false);
            string text = EngineInterop.SceneText(Engine);
            if (_project != null)
            {
                SceneSourceFile.WriteProjectSceneAtomicText(
                    target,
                    text,
                    _project.RootDir,
                    _project.AssetsDir,
                    SceneDocumentMode.TwoD);
            }
            else
            {
                SceneSourceFile.WriteAtomicText(
                    target,
                    text,
                    expectedMode: SceneDocumentMode.TwoD);
            }
            SetCurrentScenePath(target);
            MarkSceneClean(text);
            NotifySceneDocumentSaved(use3D: false, target);
            await OnSceneSourceSavedAsync(use3D: false, previousPath, target);
            Log($"Saved scene → {target}");
            return true;
        }
        catch (Exception ex)
        {
            Log("Save failed: " + ex.Message);
            return false;
        }
    }

    // ===== Undo / Redo (ApplicationCommands.Undo/Redo = Ctrl+Z / Ctrl+Y) =====
    private void OnUndo(object sender, ExecutedRoutedEventArgs e)
    {
        if (!CanUseSceneDocumentHistory()) return;
        if (_documentHost.Undo(out EditorDocumentTransactionInfo? transaction))
        {
            Log($"Undo: {transaction?.Label ?? "Edit"}.");
            e.Handled = true;
        }
    }

    private void OnRedo(object sender, ExecutedRoutedEventArgs e)
    {
        if (!CanUseSceneDocumentHistory()) return;
        if (_documentHost.Redo(out EditorDocumentTransactionInfo? transaction))
        {
            Log($"Redo: {transaction?.Label ?? "Edit"}.");
            e.Handled = true;
        }
    }

    private void OnExit(object sender, RoutedEventArgs e) => Close();

    // ===== カスタムタイトルバー (WindowChrome) =====
    private void OnMinimizeWin(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void OnMaximizeRestoreWin(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void OnCloseWin(object sender, RoutedEventArgs e) => Close();

    protected override void OnStateChanged(EventArgs e)
    {
        base.OnStateChanged(e);
        // 最大化中は「元に戻す」グリフ、通常は「最大化」グリフに切替。
        if (MaxRestoreBtn != null)
            MaxRestoreBtn.Content = WindowState == WindowState.Maximized ? "❐" : "□";
    }

    private void OnAbout(object sender, RoutedEventArgs e)
    {
        EditorAbiSnapshot abi = EngineInterop.AbiSnapshot();
        MessageBox.Show(
            this,
            "ACS Editor\n\n" +
            "WPF (.NET) editor shell hosting the ACS C++ engine\n" +
            "via a versioned C ABI (P/Invoke) + native HWND viewport.\n\n" +
            abi.ToDisplayText(),
            "About ACS Editor",
            MessageBoxButton.OK,
            abi.Compatible
                ? MessageBoxImage.Information
                : MessageBoxImage.Warning);
    }
}
