using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace AcsEditor;

public partial class MainWindow : Window
{
    private EngineViewport? _viewport;
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
    private bool _syncingSelection;  // 選択同期中は OnHierarchySelect の単一選択化を抑止
    private string? _clipboard;      // コピーした subtree のシリアライズ文字列
    private Point _dragStart;        // Hierarchy ドラッグ開始座標 (しきい値判定用)
    private int  _dragNodeId = -1;   // ドラッグ中のノード id (-1 = ドラッグなし)

    public MainWindow()
    {
        InitializeComponent();
        InitLog();              // ConsoleList をタグ付きログビューに束縛
        Loaded += OnLoaded;

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

        // アセットブラウザ: ダブルクリック/ドラッグでの割当とログを受ける。
        AssetBrowser.AssetActivated += OnAssetActivated;
        AssetBrowser.Log += Log;

        // 終了時: ソース監視を止め、起動中のゲームプロセスを終了させる。
        Closed += (_, _) =>
        {
            StopSourceWatch();
            if (_gameProcess != null && !_gameProcess.HasExited) { try { _gameProcess.Kill(); } catch { } }
        };
    }

    /// <summary>プロジェクトを開いた状態で起動する。初期シーンは attach 後にロードする。</summary>
    public MainWindow(Project project) : this()
    {
        _project = project;
        Title = $"ACS Editor — {project.Name}";
        AssetBrowser.SetProject(project);   // Assets フォルダを走査・監視
    }

    // 下部パネルのタブ切替 (Console / Build / Assets)。
    private void OnBottomTab(object sender, RoutedEventArgs e)
    {
        string tab = (sender as System.Windows.Controls.Primitives.ToggleButton)?.Tag as string ?? "console";
        ShowBottomTab(tab);
    }

    private void ShowBottomTab(string tab)
    {
        TabConsole.IsChecked = tab == "console";
        TabBuild.IsChecked   = tab == "build";
        TabAssets.IsChecked  = tab == "assets";
        ConsoleList.Visibility  = tab == "console" ? Visibility.Visible : Visibility.Collapsed;
        BuildList.Visibility    = tab == "build"   ? Visibility.Visible : Visibility.Collapsed;
        AssetBrowser.Visibility = tab == "assets"  ? Visibility.Visible : Visibility.Collapsed;
    }

    // アセットがダブルクリック/ドラッグで起動された: 画像は選択ノードのスプライトに割り当てる。
    private void OnAssetActivated(object? sender, AssetActivatedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        switch (e.Kind)
        {
            case "image":
                if (_selectedId < 0) { Log("画像を割り当てるノードを先に選択してください。"); return; }
                if (EngineInterop.acs_editor_node_set_sprite(Engine, _selectedId, e.FullPath) != 0)
                {
                    RefreshSpriteLabel(_selectedId);
                    Log($"Sprite ← {System.IO.Path.GetFileName(e.FullPath)} (node {_selectedId})");
                }
                else Log("スプライト割当に失敗: " + e.FullPath);
                break;
            case "scene":
                Log($"シーンアセット: {System.IO.Path.GetFileName(e.FullPath)}");
                break;
            case "prefab":
                InstantiatePrefab(e.FullPath, _selectedId);   // 選択ノード配下へ (無ければ root)
                break;
            case "material":
                // .acsmat をダブルクリック → マテリアルエディタを開く。選択ノードがあれば割当も。
                if (_selectedId >= 0)
                {
                    EngineInterop.acs_editor_node_set_material(Engine, _selectedId, e.FullPath);
                    RefreshMaterialBox(_selectedId);
                    Log($"Material ← {System.IO.Path.GetFileName(e.FullPath)} (node {_selectedId})");
                }
                OpenMaterialEditor(e.FullPath);
                break;
            default:
                Log($"{e.Kind}: {System.IO.Path.GetFileName(e.FullPath)}");
                break;
        }
    }

    // 数値 TextBox を「ドラッグでスクラブ」可能にする。未フォーカス時の左ドラッグで値を
    // step×移動px ぶん増減し、ドラッグせず離したら通常のクリック (フォーカス→全選択で編集) 扱い。
    private void EnableScrub(TextBox tb, double step, Action apply, bool integer = false)
    {
        bool armed = false, scrubbing = false;
        Point start = default; double startVal = 0;

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
            armed = false;
            if (tb.IsMouseCaptured) tb.ReleaseMouseCapture();
            Mouse.OverrideCursor = null;
            if (scrubbing)
            {
                scrubbing = false;
                if (Engine != IntPtr.Zero) EngineInterop.acs_editor_end_continuous(Engine);
                e.Handled = true;                                      // ドラッグだった → click 扱いしない
            }
            else { tb.Focus(); tb.SelectAll(); }                       // クリックだった → 編集開始
        };
    }

    // 階層ツリーのダブルクリック → 選択ノードへフォーカス (展開トグル上は無視)。
    private void OnHierarchyDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (Engine == IntPtr.Zero || _selectedId < 0) return;
        EngineInterop.acs_editor_camera_focus(Engine);
        Log($"Focus on node {_selectedId}.");
    }

    private IntPtr Engine => _viewport?.Engine ?? IntPtr.Zero;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        StatusText.Text = "";
        Log("ACS Editor started.");

        _viewport = new EngineViewport();
        ViewportHost.Child = _viewport;
        _viewport.Attached += OnEngineAttached;   // アタッチ後にシーンを取り込む
        _viewport.Picked += OnViewportPicked;     // ビューポート左クリックで選択
        _viewport.TransformChanged += OnViewportTransformChanged;   // ギズモ移動後に Inspector 更新
        _viewport.PolyKeyFinalize += FinalizePoly;                  // 描画中の Enter/Esc でポリゴン確定
        _viewport.SizeChanged += (_, args) =>
            ViewportInfo.Text = $"Viewport {(int)args.NewSize.Width} x {(int)args.NewSize.Height}";
    }

    // ===== Hierarchy: エンジンのシーングラフから構築 =====
    private void OnEngineAttached()
    {
        Log("Viewport attached — engine rendering into hosted HWND.");
        AssetBrowser.Engine = Engine;                  // .acsmat サムネイルを実シェーダ GPU プレビューに
        AssetBrowser.Refresh();                        // device 準備後に再生成 (GPU サムネイル)
        if (_project != null) LoadProjectSettings();   // プロジェクト設定 (Config/ProjectSettings.ini) を読込+適用
        if (_project != null) LoadUserTypes();         // 先にユーザー型を登録 (シーン内の user component を attach 可能に)
        if (_project != null) LoadProjectScene();      // デモを置き換えてプロジェクトの初期シーンへ
        UpdateGizmoToggles(EngineInterop.acs_editor_gizmo_get_mode(Engine));   // 初期 active (Move)
        BuildHierarchy();
        PopulateComponentCombo();
        RefreshCreateMenus();   // 右クリック生成メニュー (ビルトイン / ユーザー定義) を構築
    }

    // ===== プロジェクト設定 (Config/ProjectSettings.ini) =====
    private string SettingsIniPath =>
        System.IO.Path.Combine(_project!.RootDir, "Config", "ProjectSettings.ini");

    /// <summary>INI を読み込み ABI でパース+適用する (無ければ既定値)。AA コンボも同期する。</summary>
    private void LoadProjectSettings()
    {
        if (Engine == IntPtr.Zero || _project == null) return;
        try
        {
            string text = System.IO.File.Exists(SettingsIniPath)
                ? System.IO.File.ReadAllText(SettingsIniPath, System.Text.Encoding.UTF8) : "";
            EngineInterop.acs_editor_settings_load_text(Engine, text);
            SyncAaCombo();
            if (text.Length > 0) Log($"Project settings ← {SettingsIniPath}");
        }
        catch (Exception ex) { Log("Project settings load error: " + ex.Message); }
    }

    /// <summary>ABI の設定を INI へシリアライズして保存し、AA コンボを同期する。</summary>
    private void SaveProjectSettings()
    {
        if (Engine == IntPtr.Zero || _project == null) return;
        try
        {
            var buf = new byte[64 * 1024];
            EngineInterop.acs_editor_settings_serialize(Engine, buf, buf.Length);
            string dir = System.IO.Path.GetDirectoryName(SettingsIniPath)!;
            System.IO.Directory.CreateDirectory(dir);
            System.IO.File.WriteAllText(SettingsIniPath, EngineInterop.Utf8Z(buf), System.Text.Encoding.UTF8);
            SyncAaCombo();
        }
        catch (Exception ex) { Log("Project settings save error: " + ex.Message); }
    }

    /// <summary>Rendering.MsaaSamples の現在値をツールバーの AA コンボへ反映する (再発火させない)。</summary>
    private bool _suppressAa;
    private void SyncAaCombo()
    {
        var buf = new byte[32];
        if (EngineInterop.acs_editor_settings_get_value(Engine, "Rendering", "MsaaSamples", buf, buf.Length) == 0) return;
        int idx = EngineInterop.Utf8Z(buf) switch { "2" => 1, "4" => 2, "8" => 3, _ => 0 };
        if (AaBox != null && AaBox.SelectedIndex != idx) { _suppressAa = true; AaBox.SelectedIndex = idx; _suppressAa = false; }
    }

    private void OnProjectSettings(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        var win = new ProjectSettingsWindow(this, Engine, SaveProjectSettings);
        win.ShowDialog();
    }

    /// <summary>2D/3D ビューポート切替。3D 時はビューポートのドラッグ=軌道、ホイール=ドリー (ABI側)。</summary>
    private void OnToggle3D(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        _view3d = View3DBtn.IsChecked == true;
        EngineInterop.acs_editor_set_view3d(Engine, _view3d ? 1 : 0);   // 初回 ON で既定シーンを seed
        if (_view3d) Load3DSceneIfPresent();    // 保存済み 3D シーンがあれば seed を上書き
        BuildHierarchy();                       // 2D/3D でツリーの中身を切り替える
        if (_view3d) { int s = EngineInterop.acs_editor_selected3d(Engine); if (s >= 0) Populate3DInspector(s); else Clear3DInspector(); }
        else Clear3DInspector();
        OrthoBtn.Visibility = _view3d ? Visibility.Visible : Visibility.Collapsed;   // Ortho は 3D 時のみ
        Log(_view3d ? "3D ビューポート (右/中ドラッグ=軌道 / ホイール=ドリー / 左クリック=選択)" : "2D ビューポート");
    }

    // 3D ビューの投影を 正射(2D ビュー) / 透視 で切り替える。
    private void OnToggleOrtho(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        bool on = OrthoBtn.IsChecked == true;
        EngineInterop.acs_editor_set_ortho3d(Engine, on ? 1 : 0);
        Log(on ? "3D: 正射影 (2D ビュー)" : "3D: 透視投影");
    }

    // プロジェクトの初期シーンをロード (無ければ空シーン)。attach 後に 1 度呼ぶ。
    private void LoadProjectScene()
    {
        if (Engine == IntPtr.Zero || _project == null) return;
        Log($"Project: {_project.Name}  ({_project.RootDir})");
        try
        {
            string scenePath = _project.InitialScenePath;
            // Game.DefaultScene (プロジェクト設定) があればそちらを優先する
            var sbuf = new byte[256];
            if (EngineInterop.acs_editor_settings_get_value(Engine, "Game", "DefaultScene", sbuf, sbuf.Length) != 0)
            {
                string rel = EngineInterop.Utf8Z(sbuf);
                if (rel.Length > 0)
                    scenePath = System.IO.Path.Combine(_project.RootDir, rel.Replace('/', System.IO.Path.DirectorySeparatorChar));
            }
            if (System.IO.File.Exists(scenePath))
            {
                string text = System.IO.File.ReadAllText(scenePath, System.Text.Encoding.UTF8);
                EngineInterop.acs_editor_scene_new(Engine);                  // デモを破棄
                if (EngineInterop.acs_editor_scene_load_text(Engine, text) != 0)
                    Log($"Loaded initial scene ← {scenePath}");
                else
                    Log("Initial scene load failed (format).");
            }
            else
            {
                EngineInterop.acs_editor_scene_new(Engine);
                Log("No initial scene file — started empty.");
            }
            _currentScenePath = scenePath;
            EngineInterop.acs_editor_camera_frame_all(Engine);   // シーン全体がビューに収まるよう調整
        }
        catch (Exception ex) { Log("Initial scene load error: " + ex.Message); }
    }

    // GameObject メニュー: 空ノードを生成 (root 直下 / 選択ノードの子)。
    private void OnCreateEmpty(object sender, RoutedEventArgs e) => CreateEmptyNode(-1);
    private void OnCreateChild(object sender, RoutedEventArgs e) => CreateEmptyNode(_selectedId);

    private void CreateEmptyNode(int parentId)
    {
        if (Engine == IntPtr.Zero) return;
        int id = EngineInterop.acs_editor_add_node(Engine, "Empty", parentId);
        if (id < 0) return;
        BuildHierarchy();
        SyncSelectionUi();   // ABI が新ノードを選択済み → UI を同期
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
            var tvi = new TreeViewItem
            {
                Header = EngineInterop.NodeName(Engine, id),
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
        _selectedId = EngineInterop.acs_editor_selected(Engine);
        SyncHighlightAndNativeSelection();
        if (_selectedId >= 0) PopulateInspector(_selectedId);
        else                  ClearSelectionUi();
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
        if (ctrl)
        {
            EngineInterop.acs_editor_select_toggle(Engine, id);
            SyncSelectionUi();
            e.Handled = true;
        }
        else if (EngineInterop.acs_editor_selection_count(Engine) > 1)
        {
            // 複数選択中のメンバを通常クリック → {id} に畳む。既に primary だと
            // SelectedItemChanged が発火しないので、ここで明示的に単一選択する。
            EngineInterop.acs_editor_select(Engine, id);
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
        if (Engine == IntPtr.Zero) return;
        EngineInterop.acs_editor_select_all(Engine);
        SyncSelectionUi();
        Log("Selected all nodes.");
    }

    private void OnViewportTransformChanged()
    {
        if (Engine == IntPtr.Zero) return;
        if (_view3d) { int s3 = EngineInterop.acs_editor_selected3d(Engine); if (s3 >= 0) Populate3DInspector(s3); return; }
        int sel = EngineInterop.acs_editor_selected(Engine);
        if (sel >= 0) { _selectedId = sel; PopulateInspector(sel); }
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
    private void OnAaChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Engine == IntPtr.Zero || AaBox == null || _suppressAa) return;   // 初期化/同期中は無視
        int[] map = { 1, 2, 4, 8 };
        int idx = AaBox.SelectedIndex;
        if (idx < 0 || idx >= map.Length) return;
        // プロジェクト設定 (Rendering.MsaaSamples) 経由で適用 + 永続化し、Project Settings と共有する。
        // プロジェクト未オープン時は ABI へ直接渡す (設定ファイルの保存先が無いため)。
        if (_project != null && EngineInterop.acs_editor_settings_set(Engine, "Rendering", "MsaaSamples", map[idx].ToString()) != 0)
            SaveProjectSettings();
        else
            EngineInterop.acs_editor_set_msaa(Engine, map[idx]);
        Log(map[idx] == 1 ? "AA: FXAA" : $"AA: MSAA {map[idx]}x");
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
    private void OnPlay(object sender, RoutedEventArgs e)   // Play / Stop トグル
    {
        if (Engine == IntPtr.Zero) return;
        int st = EngineInterop.acs_editor_play_state(Engine);
        if (st == 0)
        {
            EngineInterop.acs_editor_play_start(Engine);
            // プロジェクトの reflect DLL があれば、インプロセス Play で «ユーザーコンポーネント» も実行する。
            string? dll = _project != null ? BuildService.ReflectDllPath(_project) : null;
            if (dll != null && System.IO.File.Exists(dll))
            {
                int r = EngineInterop.acs_editor_logic_play_start(Engine, dll);
                Log(r == 1 ? "▶ Play — 物理 + ユーザーロジック (インプロセス)。"
                           : $"▶ Play — 物理プレビュー (logic 起動失敗 {r})。");
            }
            else Log("▶ Play — 物理プレビュー開始。");
        }
        else
        {
            if (EngineInterop.acs_editor_logic_play_active(Engine) != 0)
                EngineInterop.acs_editor_logic_play_stop(Engine);
            EngineInterop.acs_editor_play_stop(Engine);    // 開始状態へ復元
            BuildHierarchy();                              // 復元後の位置/選択を UI に反映
            Log("⏹ Stop — 開始状態へ復元。");
        }
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
            int n = EngineInterop.acs_editor_preview_start(Engine);
            Log($"Preview 開始 (実コンポーネント {n} 個をライブ実行)", "Play", LogLevel.Success);
        }
        else
        {
            EngineInterop.acs_editor_preview_stop(Engine);
            BuildHierarchy();
            Log("Preview 停止 (位置を復元)", "Play", LogLevel.Info);
        }
    }

    // ===== Scene / Game ビュータブ =====
    // Scene = 編集ビュー (グリッド/ギズモ等)。Game = ゲーム画面のみ (chrome 無し) で Play を再生。
    private void OnSceneTab(object sender, RoutedEventArgs e) => SetGameView(false);
    private void OnGameTab(object sender, RoutedEventArgs e)  => SetGameView(true);

    /// <summary>Blueprint タブ: 中央をノードグラフエディタへ切り替える(HWND ビューポートは隠す)。</summary>
    private void OnBlueprintTab(object sender, RoutedEventArgs e)
    {
        BlueprintTabBtn.IsChecked = true;
        SceneTabBtn.IsChecked     = false;
        GameTabBtn.IsChecked      = false;
        SceneTools.Visibility     = Visibility.Collapsed;     // シーン編集ツールは不要
        ViewportHost.Visibility   = Visibility.Collapsed;     // HWND ビューポートを隠す (airspace 回避)
        BlueprintHost.Visibility  = Visibility.Visible;
        BuildBlueprintPalette();                              // リフレクションからパレットを構築 (ホットリロード後も最新)
        Log("⛓ Blueprint エディタ — ノードグラフ (右クリックでノード追加 / ピンドラッグで接続 / ホイールでズーム)。");
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

        static BlueprintEditor.BpPinSpec Ex(string n) => new(n, true);
        static BlueprintEditor.BpPinSpec Da(string n) => new(n, false);
        var none = Array.Empty<BlueprintEditor.BpPinSpec>();

        var pal = new List<BlueprintEditor.BpNodeTemplate>
        {
            // イベント (実行の起点)。
            new("イベント", "On BeginPlay", ev, none, new[] { Ex("▶") }),
            new("イベント", "On Tick",      ev, none, new[] { Ex("▶"), Da("dt") }),
            new("イベント", "On Destroy",   ev, none, new[] { Ex("▶") }),
            // フロー制御。
            new("フロー", "Branch",       flow, new[] { Ex("▶"), Da("cond") }, new[] { Ex("True"), Ex("False") }),
            new("フロー", "Sequence",     flow, new[] { Ex("▶") },             new[] { Ex("0"), Ex("1"), Ex("2") }),
            new("フロー", "Print String", flow, new[] { Ex("▶"), Da("text") }, new[] { Ex("▶") }),
            // サブシステム。
            new("サブシステム", "Publish Event", bus, new[] { Ex("▶"), Da("channel") },          new[] { Ex("▶") }),
            new("サブシステム", "Spawn Prefab",  bus, new[] { Ex("▶"), Da("path"), Da("pos") },   new[] { Ex("▶"), Da("spawned") }),
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
                pal.Add(new("関数", title, fn, new[] { Ex("▶"), Da("target") }, new[] { Ex("▶") }));
            }
        }
        catch (Exception ex) { Log("Blueprint パレット: 反射メソッド列挙をスキップ (" + ex.GetType().Name + ")"); }

        BlueprintHost.SetPalette(pal);
        BlueprintHost.DefaultDir = _project != null
            ? System.IO.Path.Combine(_project.RootDir, "Assets") : null;   // 保存/開くダイアログの初期位置
    }

    private void SetGameView(bool game)
    {
        // Blueprint から戻る場合の復帰: ビューポートとシーンツールを出す。
        BlueprintTabBtn.IsChecked = false;
        BlueprintHost.Visibility  = Visibility.Collapsed;
        ViewportHost.Visibility   = Visibility.Visible;
        SceneTools.Visibility     = Visibility.Visible;
        if (Engine == IntPtr.Zero) { SceneTabBtn.IsChecked = true; GameTabBtn.IsChecked = false; return; }
        SceneTabBtn.IsChecked = !game;
        GameTabBtn.IsChecked  = game;
        EngineInterop.acs_editor_set_game_view(Engine, game ? 1 : 0);
        if (game)
        {
            // ゲーム全体を中央へフレーミングしてから Play を開始 (= その view を game camera の基準に)。
            EngineInterop.acs_editor_camera_frame_all(Engine);
            if (EngineInterop.acs_editor_play_state(Engine) == 0)
            {
                EngineInterop.acs_editor_play_start(Engine);
                string? dll = _project != null ? BuildService.ReflectDllPath(_project) : null;
                if (dll != null && System.IO.File.Exists(dll))
                    EngineInterop.acs_editor_logic_play_start(Engine, dll);
            }
            Log("▶ Game View — ゲームを再生中。Scene タブで編集へ戻ります。");
        }
        else
        {
            // 編集へ戻る: Play を止めて開始状態へ復元。
            if (EngineInterop.acs_editor_play_state(Engine) != 0)
            {
                if (EngineInterop.acs_editor_logic_play_active(Engine) != 0)
                    EngineInterop.acs_editor_logic_play_stop(Engine);
                EngineInterop.acs_editor_play_stop(Engine);
                BuildHierarchy();
            }
            Log("◳ Scene View — 編集に戻りました。");
        }
        UpdatePlayButtons();
    }

    private void UpdatePlayButtons()
    {
        int st = Engine != IntPtr.Zero ? EngineInterop.acs_editor_play_state(Engine) : 0;
        PlayBtn.Content    = st == 0 ? "▶  Play" : "⏹ Stop";
        PauseBtn.Content   = st == 2 ? "▶ Resume" : "❚❚ Pause";
        PauseBtn.IsEnabled = st != 0;
        StepBtn.IsEnabled  = st == 2;
    }

    private void OnSnapToggle(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        bool on = SnapCheck.IsChecked == true;
        EngineInterop.acs_editor_set_snap(Engine, on ? 1 : 0, 10f, 15f, 0.25f);   // grid 10 / 15° / 0.25
        Log(on ? "Snap ON (grid 10, 15°, 0.25)" : "Snap OFF");
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
            Log($"ポリゴンを作成しました (node {id})。");
        }
        else Log("ポリゴンには頂点が 3 つ以上必要です。");
    }

    // 描画中の Enter/Esc でポリゴンを確定する。
    private void OnGlobalKeyDown(object sender, KeyEventArgs e)
    {
        if (_viewport != null && _viewport.PolyMode && (e.Key == Key.Enter || e.Key == Key.Escape))
        {
            FinalizePoly();
            e.Handled = true;
            return;
        }
        // F7 = Build / F5 = Build & Run (テキスト編集中でも安全な F キー)。
        if (e.Key == Key.F7) { OnBuildProject(this, new RoutedEventArgs()); e.Handled = true; }
        else if (e.Key == Key.F5) { OnBuildAndRun(this, new RoutedEventArgs()); e.Handled = true; }
        // インプロセス Play 中はゲーム入力を DLL の acs::Input へフィードする (オートリピートは無視)。
        else if (!e.IsRepeat) FeedGameKey(e.Key, true);
    }

    // Play 中のキー解放を DLL へフィードする。
    private void OnGlobalKeyUp(object sender, KeyEventArgs e) => FeedGameKey(e.Key, false);

    // WPF Key を acs::EKey 整数へマップし、Play 中なら DLL へフィードする。テキスト編集中
    // (TextBox にフォーカス) は誤爆を避けてスキップする。
    private void FeedGameKey(Key key, bool down)
    {
        if (Engine == IntPtr.Zero) return;
        if (EngineInterop.acs_editor_logic_play_active(Engine) == 0) return;
        if (Keyboard.FocusedElement is System.Windows.Controls.TextBox) return;
        int ek = EKeyFromWpf(key);
        if (ek != 0) EngineInterop.acs_editor_logic_input_key(Engine, ek, down ? 1 : 0);
    }

    // WPF System.Windows.Input.Key → acs::EKey の整数値 (enum 順: Unknown=0, A=1..Z=26,
    // Num0=27.., F1=37.., LeftShift=49.., Up=57/Down=58/Left=59/Right=60, Space=61, Enter=62,
    // Tab=63, Backspace=64, Escape=65)。未対応は 0。
    private static int EKeyFromWpf(Key k)
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
    private void OnNewClass(object sender, RoutedEventArgs e)
    {
        if (_project == null) { Log("プロジェクトがありません。"); return; }
        var dlg = new NewClassDialog { Owner = this };
        if (dlg.ShowDialog() != true) return;
        try
        {
            var made = ProjectManager.GenerateClass(_project, dlg.ClassName, dlg.BaseClass);
            _pendingReconfigure = true;   // 新ファイルを CMake に拾わせる (次ビルドで再 configure)
            ShowBottomTab("build");
            BuildLog($"生成: {string.Join(", ", made.ConvertAll(System.IO.Path.GetFileName))}");
            if (dlg.BaseClass == "FComponent2D")
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
        if (_project == null) { Log("プロジェクトがありません。"); return; }
        if (_building) { BuildLog("ビルド実行中です。"); return; }
        _building = true;
        SetBuildUiEnabled(false);
        ShowBottomTab("build");
        BuildLog($"==== Build Standalone: {_project.Name} ====");
        SaveSceneForBuild();
        bool force = _pendingReconfigure; _pendingReconfigure = false;
        try
        {
            string? exe = await BuildService.BuildAsync(_project, BuildLog, force, standalone: true);
            if (exe != null)
            {
                LoadUserTypes();
                if (_gameProcess != null && !_gameProcess.HasExited)
                { try { _gameProcess.Kill(); _gameProcess.WaitForExit(2000); } catch { } }
                _gameProcess = BuildService.Run(_project, BuildLog);
            }
        }
        catch (Exception ex) { BuildLog("Build エラー: " + ex.Message); }
        finally { _building = false; SetBuildUiEnabled(true); }
    }

    private async System.Threading.Tasks.Task DoBuild(bool run)
    {
        if (_project == null) { BuildLog("プロジェクトがありません。"); return; }
        if (_building) { BuildLog("ビルド実行中です。"); return; }
        _building = true;
        SetBuildUiEnabled(false);
        ShowBottomTab("build");
        BuildLog($"==== Build: {_project.Name} ====");
        SaveSceneForBuild();   // 編集中シーンを main.acscene へ保存 → スタンドアロンがそれを読む
        bool force = _pendingReconfigure; _pendingReconfigure = false;
        try
        {
            string? exe = await BuildService.BuildAsync(_project, BuildLog, force);
            if (exe != null)
            {
                LoadUserTypes();          // リフレクション DLL からユーザー定義型を取り込む
                if (run) RunGame();
            }
        }
        catch (Exception ex) { BuildLog("Build エラー: " + ex.Message); }
        finally
        {
            _building = false;
            SetBuildUiEnabled(true);
        }
    }

    // ビルド前に現在のシーンをプロジェクトの初期シーン (Assets/main.acscene) へ保存する。
    // これでスタンドアロン (Build & Run) が «編集中と同じシーン» を読み込む。
    private void SaveSceneForBuild()
    {
        if (Engine == IntPtr.Zero || _project == null) return;
        try
        {
            string target = string.IsNullOrEmpty(_currentScenePath)
                ? System.IO.Path.Combine(_project.RootDir, _project.InitialScene)
                : _currentScenePath!;
            string text = EngineInterop.SceneText(Engine);
            System.IO.File.WriteAllText(target, text, new System.Text.UTF8Encoding(false));
            _currentScenePath = target;
            BuildLog($"シーンを保存: {target}");
        }
        catch (Exception ex) { BuildLog("シーン保存警告: " + ex.Message); }
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
        if (_project == null) return;
        if (_gameProcess != null && !_gameProcess.HasExited)
        {
            try { _gameProcess.Kill(); _gameProcess.WaitForExit(2000); } catch { }
        }
        // Play を作り直すため、既に Game View ならいったん Scene に戻してから入り直す。
        if (Engine != IntPtr.Zero && EngineInterop.acs_editor_is_game_view(Engine) != 0) SetGameView(false);
        SetGameView(true);
    }

    private void SetBuildUiEnabled(bool enabled)
    {
        BuildBtn.IsEnabled = enabled; RunBtn.IsEnabled = enabled;
        MenuBuild.IsEnabled = enabled; MenuRun.IsEnabled = enabled; MenuBuildRun.IsEnabled = enabled;
        BuildBtn.Content = enabled ? "🔨 Build" : "⏳ Building…";
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
        int n = EngineInterop.acs_editor_align_selection(Engine, mode);
        if (n > 0) { Log($"Aligned {n} node(s): {name}."); SyncSelectionUi(); }
        else Log("Align needs 2+ selected nodes.");
    }
    private void DoDistribute(int axis, string name)
    {
        if (Engine == IntPtr.Zero) return;
        int n = EngineInterop.acs_editor_distribute_selection(Engine, axis);
        if (n > 0) { Log($"Distributed {n} node(s): {name}."); SyncSelectionUi(); }
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
    }
    private void OnDispEnabled(object s, RoutedEventArgs e)
    {
        if (_populating || _selectedId < 0 || Engine == IntPtr.Zero) return;
        EngineInterop.acs_editor_node_set_enabled(Engine, _selectedId, DispEnabled.IsChecked == true ? 1 : 0);
    }
    // 数値欄の確定: 値が実際に変わったプロパティだけ set する (冗長な undo を避ける)。
    private void ApplyDisplay()
    {
        if (_populating || _selectedId < 0 || Engine == IntPtr.Zero) return;
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
    }

    // Color RGBA 欄の現在値を Inspector の色スウォッチに反映する (色相が一目で分かるよう不透明で表示)。
    private void UpdateColorSwatch()
    {
        static byte B(float v) => (byte)Math.Clamp(v * 255f, 0f, 255f);
        float r = ParseF(ColR.Text), g = ParseF(ColG.Text), b = ParseF(ColB.Text);
        ColorSwatch.Background = new System.Windows.Media.SolidColorBrush(
            System.Windows.Media.Color.FromRgb(B(r), B(g), B(b)));
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
        if (dlg.ShowDialog() != true) return;
        if (EngineInterop.acs_editor_node_set_sprite(Engine, _selectedId, dlg.FileName) != 0)
        {
            RefreshSpriteLabel(_selectedId);
            Log($"Sprite set: {System.IO.Path.GetFileName(dlg.FileName)}");
        }
        else Log("Sprite set failed: " + dlg.FileName);
    }

    private void OnClearSprite(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _selectedId < 0) return;
        EngineInterop.acs_editor_node_clear_sprite(Engine, _selectedId);
        RefreshSpriteLabel(_selectedId);
        Log("Sprite cleared (→ 矩形表示).");
    }

    // ===== マテリアル (効果プリセット) — ノードは選ぶだけ。編集はマテリアルエディタで =====

    // MaterialBox を Assets 内の *.acsmat 一覧で埋め、ノードの現在マテリアルを選択状態にする。
    private void RefreshMaterialBox(int id)
    {
        MaterialBox.Items.Clear();
        MaterialBox.Items.Add(new ComboBoxItem { Content = "(なし)", Tag = null });
        string cur = EngineInterop.NodeMaterial(Engine, id);
        int sel = 0, idx = 0;
        if (_project != null && System.IO.Directory.Exists(_project.AssetsDir))
        {
            foreach (string f in System.IO.Directory.EnumerateFiles(_project.AssetsDir, "*.acsmat",
                                                                    System.IO.SearchOption.AllDirectories))
            {
                idx++;
                MaterialBox.Items.Add(new ComboBoxItem { Content = AssetRel(f), Tag = f });
                if (!string.IsNullOrEmpty(cur) && PathEq(f, cur)) sel = idx;
            }
        }
        // ノードに設定済みだが Assets 外/未列挙のパスならそれも 1 項目として足す。
        if (sel == 0 && !string.IsNullOrEmpty(cur))
        {
            MaterialBox.Items.Add(new ComboBoxItem { Content = System.IO.Path.GetFileName(cur), Tag = cur });
            sel = MaterialBox.Items.Count - 1;
        }
        MaterialBox.SelectedIndex = sel;
    }

    private string AssetRel(string full)
    {
        if (_project == null) return System.IO.Path.GetFileName(full);
        string root = _project.AssetsDir;
        if (full.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            return full.Substring(root.Length).TrimStart('\\', '/').Replace('\\', '/');
        return System.IO.Path.GetFileName(full);
    }

    private static bool PathEq(string a, string b)
    {
        try { return string.Equals(System.IO.Path.GetFullPath(a), System.IO.Path.GetFullPath(b),
                                   StringComparison.OrdinalIgnoreCase); }
        catch { return string.Equals(a, b, StringComparison.OrdinalIgnoreCase); }
    }

    private void OnMaterialSelected(object sender, SelectionChangedEventArgs e)
    {
        if (_populating || Engine == IntPtr.Zero || _selectedId < 0) return;
        if (MaterialBox.SelectedItem is not ComboBoxItem it) return;
        string? path = it.Tag as string;
        if (string.IsNullOrEmpty(path))
        {
            EngineInterop.acs_editor_node_clear_material(Engine, _selectedId);
            Log("Material cleared (→ 効果なし).");
        }
        else
        {
            EngineInterop.acs_editor_node_set_material(Engine, _selectedId, path);
            Log($"Material ← {AssetRel(path)} (node {_selectedId})");
        }
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
        if (Engine == IntPtr.Zero) return;
        if (_project == null) { Log("マテリアル作成にはプロジェクトが必要です。"); return; }
        System.IO.Directory.CreateDirectory(_project.AssetsDir);
        string baseName = "Material";
        string path = System.IO.Path.Combine(_project.AssetsDir, baseName + ".acsmat");
        for (int i = 1; System.IO.File.Exists(path); i++)
            path = System.IO.Path.Combine(_project.AssetsDir, $"{baseName}{i}.acsmat");
        if (EngineInterop.acs_editor_material_create(path, System.IO.Path.GetFileNameWithoutExtension(path)) == 0)
        { Log("マテリアル作成に失敗しました。"); return; }
        Log($"New material: {AssetRel(path)}");
        // 選択ノードがあれば即割当。
        if (_selectedId >= 0)
        {
            EngineInterop.acs_editor_node_set_material(Engine, _selectedId, path);
            RefreshMaterialBox(_selectedId);
        }
        OpenMaterialEditor(path);
    }

    private void OnClearMaterial(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || _selectedId < 0) return;
        EngineInterop.acs_editor_node_clear_material(Engine, _selectedId);
        RefreshMaterialBox(_selectedId);
        Log("Material cleared (→ 効果なし).");
    }

    private void OpenMaterialEditor(string acsmatPath)
    {
        var win = new MaterialEditorWindow(Engine, acsmatPath) { Owner = this };
        win.Show();   // 非モーダル: viewport を見ながら調整できる
    }


    private void OnHierarchySelect(object sender, RoutedPropertyChangedEventArgs<object> e)
    {
        if (Engine == IntPtr.Zero || _syncingSelection) return;   // 同期中の native 変更は無視
        if (e.NewValue is TreeViewItem item && item.Tag is int id)
        {
            if (_view3d) { EngineInterop.acs_editor_select3d(Engine, id); Populate3DInspector(id); return; }
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
        if (_populating || _selectedId < 0 || Engine == IntPtr.Zero) return;
        float x   = ParseF(PosX.Text);
        float y   = ParseF(PosY.Text);
        float deg = ParseF(RotDeg.Text);
        float sx  = ParseF(ScaleX.Text, 1.0f);
        float sy  = ParseF(ScaleY.Text, 1.0f);
        EngineInterop.acs_editor_node_set_transform(Engine, _selectedId,
            x, y, (float)(deg * Math.PI / 180.0), sx, sy);
    }

    private static float ParseF(string s, float fallback = 0.0f) =>
        float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float v)
            && float.IsFinite(v) ? v : fallback;   // "NaN"/"Infinity" は弾いて fallback

    // ===== ノード操作: リネーム / 削除 =====
    private void ApplyRename()
    {
        if (_populating || _selectedId < 0 || Engine == IntPtr.Zero) return;
        string nm = (NameBox.Text ?? "").Trim();
        if (nm.Length == 0) return;
        if (EngineInterop.acs_editor_node_rename(Engine, _selectedId, nm) != 0)
        {
            InspName.Text = nm + "  (id " + _selectedId + ")";
            BuildHierarchy();   // Hierarchy 表示名を更新 (選択はエンジン側で維持)
        }
    }

    private void OnDuplicateNode(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || EngineInterop.acs_editor_selection_count(Engine) == 0) return;
        int n = EngineInterop.acs_editor_selection_duplicate(Engine);   // 選択全体を一括複製 (1 undo)
        if (n > 0)
        {
            Log($"Duplicated {n} node(s) (subtree).");
            BuildHierarchy();        // engine 選択はクローン群へ移っている
            SyncSelectionUi();
        }
    }

    private void DeleteSelected()
    {
        if (Engine == IntPtr.Zero || EngineInterop.acs_editor_selection_count(Engine) == 0) return;
        int n = EngineInterop.acs_editor_selection_delete(Engine);      // 選択全体を一括削除 (1 undo)
        if (n > 0)
        {
            Log($"Deleted {n} node(s) (and their children).");
            BuildHierarchy();
            SyncSelectionUi();       // 集合は空 → ClearSelectionUi
        }
    }
    private void OnDeleteNode(object sender, RoutedEventArgs e) => DeleteSelected();
    private void OnDeleteCmd(object sender, ExecutedRoutedEventArgs e) => DeleteSelected();

    // ===== Copy / Paste (subtree、Ctrl+C / Ctrl+V) =====
    private void OnCopy(object sender, ExecutedRoutedEventArgs e)
    {
        if (_selectedId < 0 || Engine == IntPtr.Zero) return;
        _clipboard = EngineInterop.CopySubtree(Engine, _selectedId);
        Log($"Copied node {_selectedId} (subtree).");
    }

    private void OnPaste(object sender, ExecutedRoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero || string.IsNullOrEmpty(_clipboard)) return;
        int parent = _selectedId >= 0 ? EngineInterop.acs_editor_node_parent(Engine, _selectedId) : -1;
        int id = EngineInterop.acs_editor_paste_subtree(Engine, _clipboard, parent);
        if (id >= 0)
        {
            Log($"Pasted as node {id}.");
            BuildHierarchy();
            _selectedId = id;
            SelectHierarchyItem(id);   // ツリー選択 → Inspector 更新
        }
    }

    // ===== プレハブ: ノードのサブツリーを .acsprefab として保存 / 再インスタンス化 =====
    //   プレハブ = サブツリーの直列化テキスト (ACSCENE 形式)。copy_subtree で保存し、
    //   paste_subtree で複製 (id 再マップ + ObjectRef 内部参照の付け替え) してインスタンス化する。

    private void OnCtxSavePrefab(object sender, RoutedEventArgs e) => SaveAsPrefab(_contextNodeId);

    /// <summary>ノード(とサブツリー)を .acsprefab アセットとして保存する。</summary>
    private void SaveAsPrefab(int id)
    {
        if (Engine == IntPtr.Zero || id < 0 || _project == null) return;
        string text = EngineInterop.CopySubtree(Engine, id);
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
            System.IO.File.WriteAllText(dlg.FileName, StripPrefabLinks(text), System.Text.Encoding.UTF8);
            EngineInterop.acs_editor_node_set_prefab_src(Engine, id, dlg.FileName);   // 保存元もこのプレハブのインスタンスにする
            AssetBrowser.Refresh();
            PopulateInspector(id);
            Log($"プレハブを保存 → {System.IO.Path.GetFileName(dlg.FileName)}");
        }
        catch (Exception ex) { Log("プレハブ保存エラー: " + ex.Message); }
    }

    /// <summary>プレハブテンプレートは自己リンクを持たない → PFAB 行を除去する。</summary>
    private static string StripPrefabLinks(string text) =>
        System.Text.RegularExpressions.Regex.Replace(text, @"^PFAB .*\r?\n?", "",
            System.Text.RegularExpressions.RegexOptions.Multiline);

    /// <summary>.acsprefab を読み、parentId 配下にインスタンス化する(id 再マップは ABI 側)。</summary>
    private void InstantiatePrefab(string path, int parentId)
    {
        if (Engine == IntPtr.Zero) return;
        string text;
        try { text = System.IO.File.ReadAllText(path, System.Text.Encoding.UTF8); }
        catch (Exception ex) { Log("プレハブ読込エラー: " + ex.Message); return; }
        int id = EngineInterop.acs_editor_paste_subtree(Engine, text, parentId);
        if (id >= 0)
        {
            EngineInterop.acs_editor_node_set_prefab_src(Engine, id, path);   // instance-of リンクを張る
            BuildHierarchy();
            _selectedId = id;
            SelectHierarchyItem(id);
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

    /// <summary>この編集をプレハブへ反映し、«シーン内の全インスタンス» を新プレハブで更新する(位置は維持)。</summary>
    private void ApplyToPrefab(int id)
    {
        if (Engine == IntPtr.Zero) return;
        string src = EngineInterop.NodePrefabSrc(Engine, id);
        if (string.IsNullOrEmpty(src)) return;
        string text = EngineInterop.CopySubtree(Engine, id);
        if (string.IsNullOrEmpty(text)) { Log("Apply 失敗 (直列化が空)。"); return; }
        string fileText = StripPrefabLinks(text);
        try { System.IO.File.WriteAllText(src, fileText, System.Text.Encoding.UTF8); }
        catch (Exception ex) { Log("Apply エラー: " + ex.Message); return; }

        // 他の全インスタンスを新プレハブで再生成 (id を先に集めてから処理 = 走査中の構造変更を回避)。
        var targets = FindPrefabInstances(src, id);
        int updated = 0;
        foreach (int t in targets) if (ReinstantiateInstance(t, src, fileText) >= 0) updated++;
        BuildHierarchy();
        _selectedId = id;
        SelectHierarchyItem(id);
        Log($"プレハブへ反映 (Apply) → {System.IO.Path.GetFileName(src)} ({updated} 個のインスタンスを更新)",
            "Asset", LogLevel.Success);
    }

    /// <summary>このインスタンスをプレハブの状態へ戻す(prefab → instance。編集を破棄して再生成)。</summary>
    private void RevertToPrefab(int id)
    {
        if (Engine == IntPtr.Zero) return;
        string src = EngineInterop.NodePrefabSrc(Engine, id);
        if (string.IsNullOrEmpty(src) || !System.IO.File.Exists(src)) { Log("Revert 失敗 (プレハブが見つからない)。"); return; }
        string text;
        try { text = System.IO.File.ReadAllText(src, System.Text.Encoding.UTF8); }
        catch (Exception ex) { Log("Revert 読込エラー: " + ex.Message); return; }
        int nid = ReinstantiateInstance(id, src, text);
        if (nid >= 0)
        {
            BuildHierarchy();
            _selectedId = nid;
            SelectHierarchyItem(nid);
            Log($"プレハブへ復元 (Revert) ← {System.IO.Path.GetFileName(src)}", "Asset", LogLevel.Info);
        }
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
        var text   = (System.Windows.Media.Brush)FindResource("Text");

        // プレハブ・インスタンスなら「Prefab: X」+ Apply/Revert バナーを先頭に出す。
        string prefabSrc = EngineInterop.NodePrefabSrc(Engine, id);
        if (!string.IsNullOrEmpty(prefabSrc))
        {
            var banner = new StackPanel { Margin = new Thickness(0, 0, 0, 6) };
            banner.Children.Add(new TextBlock
            {
                Text = "◆ Prefab: " + System.IO.Path.GetFileName(prefabSrc),
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

        for (int i = 0; i < count; i++)
        {
            int idx = i;   // = ABI の component slot
            string cname = EngineInterop.ComponentName(Engine, id, i);

            var inner = new StackPanel();
            // ヘッダ: コンポーネント名 (左) + 取り外し ✕ (右)。
            var header = new DockPanel { Margin = new Thickness(0, 0, 0, 3) };
            var rm = new Button
            {
                Content = "✕", Width = 22, Height = 20, Padding = new Thickness(0),
                Foreground = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0xE3, 0x9A, 0xA0)),
                Background = System.Windows.Media.Brushes.Transparent, BorderThickness = new Thickness(0),
                Cursor = Cursors.Hand, ToolTip = "コンポーネントを外す",
            };
            rm.Click += (_, __) =>
            {
                EngineInterop.acs_editor_node_remove_component_at(Engine, id, idx);
                PopulateComponents(id);
            };
            DockPanel.SetDock(rm, Dock.Right);
            header.Children.Add(rm);
            header.Children.Add(new TextBlock
            {
                Text = cname, VerticalAlignment = VerticalAlignment.Center, Foreground = text,
                FontWeight = FontWeights.SemiBold, FontFamily = new System.Windows.Media.FontFamily("Consolas"),
            });
            inner.Children.Add(header);

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
                        Log($"{cname}.{mname}() を呼び出し", "General", LogLevel.Info);
                    else Log($"{cname}.{mname}() の呼び出しに失敗");
                };
                inner.Children.Add(btn);
            }

            // コンポーネントをカードにまとめる (視覚的グルーピング)。
            CompList.Children.Add(new Border
            {
                Background = panel2, CornerRadius = new CornerRadius(5),
                Padding = new Thickness(8, 6, 8, 7), Margin = new Thickness(0, 0, 0, 6), Child = inner,
            });
        }
    }

    /// <summary>
    /// 1 つの編集プロパティ行を組み立てる。種別 (EFieldKind) に応じて
    /// checkbox (Bool) / 整数 box (I32,U32) / 数値 box×N (F32,FVec2-4) を出す。
    /// 編集確定で acs_editor_node_component_prop_set を呼ぶ。
    /// </summary>
    private FrameworkElement? BuildPropEditor(int id, int slot, string typeName, int prop)
    {
        string pname = EngineInterop.ComponentPropName(typeName, prop);
        int kind = EngineInterop.acs_editor_component_prop_kind_at(typeName, prop);
        int flags = EngineInterop.acs_editor_component_prop_flags_at(typeName, prop);
        bool hidden   = (flags & 0x2) != 0;   // FIELD_HIDDEN → 出さない
        bool readOnly = (flags & 0x1) != 0;   // FIELD_READONLY (VisibleAnywhere) → 表示のみ
        if (hidden) return null;
        EngineInterop.acs_editor_node_component_prop_get(Engine, id, slot, prop,
            out float vx, out float vy, out float vz, out float vw);
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
            committed[0] = vals[0]; committed[1] = vals[1];
            committed[2] = vals[2]; committed[3] = vals[3];
            EngineInterop.acs_editor_node_component_prop_set(
                Engine, id, slot, prop, vals[0], vals[1], vals[2], vals[3]);
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
            int count = EngineInterop.acs_editor_node_count(Engine);
            for (int i = 0; i < count; i++)
            {
                int nid = EngineInterop.acs_editor_node_id_at(Engine, i);
                if (nid == id) continue;                                  // 自己参照は除外
                string nm = EngineInterop.NodeName(Engine, nid);
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
        bool isColor = pname.IndexOf("color", StringComparison.OrdinalIgnoreCase) >= 0
                    || pname.IndexOf("tint",  StringComparison.OrdinalIgnoreCase) >= 0;
        string[] axis = isColor ? new[] { "R", "G", "B", "A" } : new[] { "X", "Y", "Z", "W" };

        var boxes = new StackPanel { Orientation = Orientation.Horizontal };
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
                Commit();
            }
            tb.LostKeyboardFocus += (_, __) => Apply();
            tb.KeyDown += (_, ev) => { if (ev.Key == Key.Enter) { Apply(); Keyboard.ClearFocus(); } };
            EnableScrub(tb, isInt ? 1.0 : 0.1, Apply, isInt);   // 数値欄をドラッグでも増減
            boxes.Children.Add(tb);
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
    // ABI がシリアライズ (文字列 ⇄ 実 FNode2D ツリー) を担い、ファイル I/O はここ (C#) で行う。
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
    }

    private void OnNewScene(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        EngineInterop.acs_editor_scene_new(Engine);
        RefreshAfterSceneChange();
        Log("New (empty) scene.");
    }

    private void OnOpenScene(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "Open ACS Scene",
            Filter = "ACS Scene (*.acscene)|*.acscene|All files (*.*)|*.*",
            DefaultExt = ".acscene",
        };
        if (dlg.ShowDialog(this) != true) return;
        try
        {
            string text = System.IO.File.ReadAllText(dlg.FileName, System.Text.Encoding.UTF8);
            if (EngineInterop.acs_editor_scene_load_text(Engine, text) != 0)
            {
                RefreshAfterSceneChange();
                Log($"Loaded scene ← {dlg.FileName}");
            }
            else
            {
                Log("Scene load failed (unrecognized format).");
            }
        }
        catch (Exception ex)
        {
            Log("Open failed: " + ex.Message);
        }
    }

    private void OnSaveScene(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (_view3d) { Save3DScene(); return; }   // 3D モードは 3D シーンを保存
        // プロジェクトの初期シーンを開いているなら、そこへ直接上書き保存 (ダイアログ無し)。
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
            if (dlg.ShowDialog(this) != true) return;
            target = dlg.FileName;
        }
        try
        {
            string text = EngineInterop.SceneText(Engine);
            System.IO.File.WriteAllText(target, text, new System.Text.UTF8Encoding(false));
            _currentScenePath = target;
            Log($"Saved scene → {target}");
        }
        catch (Exception ex)
        {
            Log("Save failed: " + ex.Message);
        }
    }

    // ===== Undo / Redo (ApplicationCommands.Undo/Redo = Ctrl+Z / Ctrl+Y) =====
    private void OnUndo(object sender, ExecutedRoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (EngineInterop.acs_editor_undo(Engine) != 0)
        {
            RefreshAfterSceneChange(); Log("Undo.");
        }
    }

    private void OnRedo(object sender, ExecutedRoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (EngineInterop.acs_editor_redo(Engine) != 0)
        {
            RefreshAfterSceneChange(); Log("Redo.");
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

    private void OnAbout(object sender, RoutedEventArgs e) =>
        MessageBox.Show(this,
            "ACS Editor\n\nWPF (.NET) editor shell hosting the ACS C++ engine\nvia a C ABI (P/Invoke) + native HWND viewport.\n\n" + EngineInterop.Version(),
            "About ACS Editor", MessageBoxButton.OK, MessageBoxImage.Information);
}
