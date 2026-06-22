// SPDX-License-Identifier: Apache-2.0
// MainWindow の 3D ビューポート編集 UI (Phase 2): Hierarchy への 3D ノード一覧、
// 動的生成の 3D Inspector (pos/rot/scale/色/形状)、プリミティブ追加。
// 2D の Hierarchy/Inspector とは独立に動き、view3d フラグで切り替わる。
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

public partial class MainWindow
{
    private bool _view3d;                 // 3D ビューポートモードか
    private bool _pop3d;                  // 3D Inspector を populate 中 (編集イベントの再入抑止)

    // ===== Hierarchy: 3D ノード一覧 =====
    private void Build3DHierarchy()
    {
        if (Engine == IntPtr.Zero) return;
        HierarchyTree.Items.Clear();
        int count = EngineInterop.acs_editor_node3d_count(Engine);
        int sel = EngineInterop.acs_editor_selected3d(Engine);
        // DFS pre-order (親が子より先) で来るので、親 TreeViewItem は既に作られている。
        var byId = new System.Collections.Generic.Dictionary<int, TreeViewItem>();
        for (int i = 0; i < count; ++i)
        {
            int id = EngineInterop.acs_editor_node3d_id_at(Engine, i);
            bool inSel = EngineInterop.acs_editor_node3d_is_selected(Engine, id) != 0;
            // Header = 「Visible トグル + 名前」。Unity/Blender 流にヒエラルキーで可視を切替える。
            // IsChecked はハンドラ接続より «前» に設定し、初期化時の発火を避ける (build 中の余計な set を防ぐ)。
            var eye = new CheckBox
            {
                IsChecked = EngineInterop.acs_editor_node3d_get_visible(Engine, id) != 0,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 6, 0),
                Focusable = false,
                ToolTip = "表示 (Visible)",
            };
            int vid = id;   // クロージャ捕捉
            eye.Checked   += (_, __) => { if (Engine != IntPtr.Zero) EngineInterop.acs_editor_node3d_set_visible(Engine, vid, 1); };
            eye.Unchecked += (_, __) => { if (Engine != IntPtr.Zero) EngineInterop.acs_editor_node3d_set_visible(Engine, vid, 0); };
            var hdr = new StackPanel { Orientation = Orientation.Horizontal };
            hdr.Children.Add(eye);
            (string glyph, Brush gcol) = PrimGlyph(id);   // ノード種別アイコン (Cube/Sphere/Plane/Mesh)
            hdr.Children.Add(new TextBlock { Text = glyph, FontSize = 11, Margin = new Thickness(0, 0, 5, 0),
                                             VerticalAlignment = VerticalAlignment.Center, Foreground = gcol });
            hdr.Children.Add(new TextBlock { Text = Node3DName(id), VerticalAlignment = VerticalAlignment.Center });
            var tvi = new TreeViewItem
            {
                Header = hdr,
                Tag = id,
                Foreground = Brushes.Gainsboro,
                IsSelected = (id == sel),
                // multi-select: primary 以外の選択メンバは背景ハイライト (WPF TreeView は IsSelected が1個のみのため)
                Background = (inSel && id != sel) ? new SolidColorBrush(Color.FromArgb(90, 0x4A, 0x90, 0xD9)) : Brushes.Transparent,
                IsExpanded = !_collapsedNodes.Contains(id),   // 畳み状態を維持
            };
            WireCollapseTracking(tvi);
            byId[id] = tvi;
            int parent = EngineInterop.acs_editor_node3d_parent(Engine, id);
            if (parent >= 0 && byId.TryGetValue(parent, out var pitem)) pitem.Items.Add(tvi);
            else HierarchyTree.Items.Add(tvi);
        }
        Log($"Hierarchy: {count} 個の 3D ノード");
        ApplyHierarchyFilter();   // 検索フィルタを再適用 (再構築でも維持)
        UpdateStatusBar();        // ノード数をステータスバーへ
    }

    private string Node3DName(int id)
    {
        var buf = new byte[64];
        return EngineInterop.acs_editor_node3d_name(Engine, id, buf, buf.Length) != 0
            ? EngineInterop.Utf8Z(buf) : $"Node {id}";
    }

    /// <summary>ノードの種別 (prim) を表すグリフ + 色を返す (ヒエラルキーのアイコン用)。</summary>
    private (string, Brush) PrimGlyph(int id)
    {
        int prim = EngineInterop.acs_editor_node3d_prim(Engine, id);
        return prim switch
        {
            0 => ("▣", new SolidColorBrush(Color.FromRgb(0x9A, 0xB8, 0xD8))),   // ▣ Cube
            1 => ("●", new SolidColorBrush(Color.FromRgb(0x86, 0xC0, 0x86))),   // ● Sphere
            2 => ("▬", new SolidColorBrush(Color.FromRgb(0xC8, 0xB0, 0x80))),   // ▬ Plane
            3 => ("◆", new SolidColorBrush(Color.FromRgb(0xD8, 0x96, 0x70))),   // ◆ Mesh
            _ => ("□", new SolidColorBrush(Color.FromRgb(0x80, 0x86, 0x90))),   // □ その他
        };
    }

    /// <summary>ビューポートのピックで選んだ 3D ノードを Hierarchy 上でも選択状態にする。</summary>
    private void Select3DInHierarchy(int id)
    {
        _syncingSelection = true;
        try
        {
            foreach (var obj in HierarchyTree.Items)
                if (obj is TreeViewItem tvi && tvi.Tag is int tid)
                    tvi.IsSelected = (tid == id);
        }
        finally { _syncingSelection = false; }
    }

    /// <summary>3D 複数選択を Hierarchy に反映 (primary=IsSelected、集合メンバ=背景ハイライト)。再帰・再描画なしの in-place 更新。</summary>
    private void Apply3DSelectionHighlight()
    {
        int sel = EngineInterop.acs_editor_selected3d(Engine);
        var hl = new SolidColorBrush(Color.FromArgb(90, 0x4A, 0x90, 0xD9));
        void Walk(ItemCollection items)
        {
            foreach (var o in items)
                if (o is TreeViewItem tvi && tvi.Tag is int tid)
                {
                    tvi.IsSelected = (tid == sel);
                    bool inSel = EngineInterop.acs_editor_node3d_is_selected(Engine, tid) != 0;
                    tvi.Background = (inSel && tid != sel) ? hl : Brushes.Transparent;
                    Walk(tvi.Items);
                }
        }
        _syncingSelection = true;
        try { Walk(HierarchyTree.Items); } finally { _syncingSelection = false; }
    }

    // ===== Inspector: 3D ノードの transform/色/形状を動的生成 =====
    private void Clear3DInspector()
    {
        Insp3DPanel.Children.Clear();
        Insp3DPanel.Visibility = Visibility.Collapsed;
        InspEnabled.Visibility = Visibility.Collapsed;   // ヘッダの Enabled トグルも隠す (選択なし / 2D)
        if (!_view3d)
        {
            // 2D へ戻ったら 2D Inspector の表示を復帰。
            InspFields.Visibility = Visibility.Visible;
            ActionButtons.Visibility = Visibility.Visible;
            return;
        }
        InspName.Text = "(no selection)";
        InspSub.Text  = "3D ノードを選択してください";
    }

    /// <summary>ヘッダの Enabled トグル → 選択中 3D ノードの enabled を切替 (populate 中は抑止)。</summary>
    private void OnInspEnabledChanged(object sender, System.Windows.RoutedEventArgs e)
    {
        if (_pop3d || Engine == IntPtr.Zero) return;
        int id = EngineInterop.acs_editor_selected3d(Engine);
        if (id < 0) return;
        EngineInterop.acs_editor_node3d_set_enabled(Engine, id, InspEnabled.IsChecked == true ? 1 : 0);
    }

    private void Populate3DInspector(int id)
    {
        if (Engine == IntPtr.Zero) return;
        // 2D の Inspector フィールドを隠し、3D 用パネルを出す。
        InspFields.Visibility = Visibility.Collapsed;
        ActionButtons.Visibility = Visibility.Collapsed;
        Insp3DPanel.Visibility = Visibility.Visible;
        InspName.Text = Node3DName(id);
        InspSub.Text  = $"3D node · id {id}";

        var tf = new float[9];
        if (EngineInterop.acs_editor_node3d_get_transform(Engine, id, tf) == 0) return;
        var col = new float[4];
        EngineInterop.acs_editor_node3d_get_color(Engine, id, col);
        int kind = EngineInterop.acs_editor_node3d_kind(Engine, id);   // 0=Cube 1=Sphere 2=Plane 3=Mesh 4=Sprite 5=Polygon

        _pop3d = true;
        // Enabled トグルをヘッダ (オブジェクト名の横) に表示・同期。NODE 節から移動。_pop3d 中なので発火しない。
        InspEnabled.IsChecked  = EngineInterop.acs_editor_node3d_get_enabled(Engine, id) != 0;
        InspEnabled.Visibility = Visibility.Visible;
        Insp3DPanel.Children.Clear();
        Insp3DPanel.Children.Add(Section("TRANSFORM"));
        Insp3DPanel.Children.Add(Vec3Row("Position", tf[0], tf[1], tf[2], (x, y, z) => Set3DTransform(id, 0, x, y, z)));
        Insp3DPanel.Children.Add(Vec3Row("Rotation°", tf[3], tf[4], tf[5], (x, y, z) => Set3DTransform(id, 1, x, y, z)));
        Insp3DPanel.Children.Add(Vec3Row("Scale", tf[6], tf[7], tf[8], (x, y, z) => Set3DTransform(id, 2, x, y, z)));
        // MESH RENDERER (FMeshComponent3D) — «何を・どう» 描くか。シェイプ/色/マテリアルは «ノード固有の特別扱い»
        // ではなく «メッシュコンポーネントのプロパティ» として 1 つに束ねる (2D の描画コンポーネント方式に対応)。
        Insp3DPanel.Children.Add(Section("MESH RENDERER"));
        // 形状 / 種別。プリミティブ (Cube/Sphere/Plane) は編集可能ドロップダウン。
        // Mesh/Sprite/Polygon は種別を読み取り専用ラベルで表示 (誤って "Cube" と出さない)。
        if (kind >= 0 && kind <= 2)
        {
            var shapeRow = new DockPanel { Margin = new Thickness(0, 4, 0, 2) };
            var lbl = new TextBlock { Text = "Shape", Width = 64, VerticalAlignment = VerticalAlignment.Center,
                Foreground = (Brush)FindResource("TextDim") };
            DockPanel.SetDock(lbl, Dock.Left); shapeRow.Children.Add(lbl);
            var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center };
            foreach (var s in new[] { "Cube", "Sphere", "Plane" }) combo.Items.Add(s);
            combo.SelectedIndex = kind;
            combo.SelectionChanged += (_, __) =>
            {
                if (_pop3d) return;
                int sel = combo.SelectedIndex;
                if (sel >= 0 && EngineInterop.acs_editor_node3d_set_prim(Engine, id, sel) != 0)
                    Log($"3D ノード {id} の形状を {combo.SelectedItem} に変更");
            };
            shapeRow.Children.Add(combo);
            Insp3DPanel.Children.Add(shapeRow);
        }
        else
        {
            string typeName = kind switch { 3 => "Mesh", 4 => "Sprite", 5 => "Polygon", _ => "—" };
            Insp3DPanel.Children.Add(LabeledValue3D("Type", typeName));
            if (kind == 4) Insp3DPanel.Children.Add(SpriteRow3D(id));   // スプライト画像の差替え UI
        }
        // 色 (FMeshComponent3D::Color)。material 未設定時のアルベド。
        Insp3DPanel.Children.Add(Vec3Row("Color", col[0], col[1], col[2], (r, g, b) =>
            EngineInterop.acs_editor_node3d_set_color(Engine, id, r, g, b, 1.0f)));
        // マテリアル — .acsmat アセットを参照 (2D と同様、編集はマテリアルエディタで)。インライン数値編集はしない。
        Insp3DPanel.Children.Add(Build3DMaterialSlot(id));

        // Visible はヒエラルキー (各ノードの目トグル) へ、Enabled はヘッダ (名前の横) へ移動済み。

        // COMPONENTS — 3D ノードにも振る舞いコンポーネントを付けられる (2D と同じ反射型・EEd3DRec に保持)。
        Insp3DPanel.Children.Add(Section("COMPONENTS"));
        Insp3DPanel.Children.Add(Build3DComponents(id));

        // 削除ボタン
        var del = new Button { Content = "🗑 Delete", Padding = new Thickness(10, 4, 10, 4), Margin = new Thickness(0, 12, 0, 0),
            HorizontalAlignment = HorizontalAlignment.Left };
        del.Click += (_, __) =>
        {
            EngineInterop.acs_editor_delete_node3d(Engine, id);
            BuildHierarchy();
            Clear3DInspector();
            Log($"3D ノード {id} を削除");
        };
        Insp3DPanel.Children.Add(del);
        _pop3d = false;
    }

    /// <summary>「ラベル: 値」の読み取り専用行 (3D Inspector の種別表示などに使う)。</summary>
    private FrameworkElement LabeledValue3D(string label, string value)
    {
        var row = new DockPanel { Margin = new Thickness(0, 4, 0, 2) };
        var lbl = new TextBlock { Text = label, Width = 64, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        DockPanel.SetDock(lbl, Dock.Left); row.Children.Add(lbl);
        row.Children.Add(new TextBlock { Text = value, VerticalAlignment = VerticalAlignment.Center });
        return row;
    }

    /// <summary>スプライトノードの画像差替え行 (現在のファイル名 + 「…」で再選択)。</summary>
    private FrameworkElement SpriteRow3D(int id)
    {
        var row = new DockPanel { Margin = new Thickness(0, 4, 0, 2) };
        var lbl = new TextBlock { Text = "Sprite", Width = 64, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        DockPanel.SetDock(lbl, Dock.Left); row.Children.Add(lbl);
        var browse = new Button { Content = "…", Width = 28, VerticalAlignment = VerticalAlignment.Center };
        DockPanel.SetDock(browse, Dock.Right); row.Children.Add(browse);
        string cur = EngineInterop.Node3DSprite(Engine, id);
        var name = new TextBlock {
            Text = string.IsNullOrEmpty(cur) ? "(なし)" : System.IO.Path.GetFileName(cur),
            VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis,
            Margin = new Thickness(0, 0, 6, 0) };
        row.Children.Add(name);
        browse.Click += (_, __) =>
        {
            var dlg = new Microsoft.Win32.OpenFileDialog
            {
                Title = "Sprite 画像を選択",
                Filter = "画像 (*.png;*.jpg;*.jpeg;*.bmp;*.tga)|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files (*.*)|*.*",
                InitialDirectory = _project?.AssetsDir,
            };
            if (dlg.ShowDialog(this) != true) return;
            if (EngineInterop.acs_editor_node3d_set_sprite(Engine, id, dlg.FileName) != 0)
            {
                Log($"スプライト画像を差替え: {System.IO.Path.GetFileName(dlg.FileName)} (id {id})");
                Populate3DInspector(id);
            }
            else Log($"スプライト差替え失敗: {System.IO.Path.GetFileName(dlg.FileName)} (画像形式を確認)");
        };
        return row;
    }

    /// <summary>3D ノードの COMPONENTS 節 (一覧 + ✕ 取外し + 「+ Add」コンボ)。型候補は CompAddBox を流用。</summary>
    private FrameworkElement Build3DComponents(int id)
    {
        var panel = new StackPanel();
        var dim  = (Brush)FindResource("TextDim");
        var text = (Brush)FindResource("Text");
        int count = EngineInterop.acs_editor_node3d_component_count(Engine, id);
        if (count == 0)
            panel.Children.Add(new TextBlock { Text = "（コンポーネントなし）", Foreground = dim, FontSize = 11, Margin = new Thickness(0, 2, 0, 4) });
        for (int i = 0; i < count; i++)
        {
            int idx = i;   // ABI の component slot
            string cname = EngineInterop.Component3DName(Engine, id, i);
            var row = new DockPanel { Margin = new Thickness(0, 1, 0, 1) };
            var rm = new Button
            {
                Content = "✕", Width = 22, Height = 20, Padding = new Thickness(0),
                Foreground = new SolidColorBrush(Color.FromRgb(0xE3, 0x9A, 0xA0)), Background = Brushes.Transparent,
                BorderThickness = new Thickness(0), Cursor = Cursors.Hand, ToolTip = "コンポーネントを外す",
            };
            rm.Click += (_, __) => { EngineInterop.acs_editor_node3d_remove_component_at(Engine, id, idx); Populate3DInspector(id); };
            DockPanel.SetDock(rm, Dock.Right); row.Children.Add(rm);
            row.Children.Add(new TextBlock { Text = cname, VerticalAlignment = VerticalAlignment.Center, Foreground = text,
                FontFamily = new FontFamily("Consolas") });
            panel.Children.Add(row);
        }
        // 「+ Add」行 (型候補は 2D 用 CompAddBox から流用)。
        var add = new DockPanel { Margin = new Thickness(0, 4, 0, 0) };
        var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center };
        foreach (var it in CompAddBox.Items) combo.Items.Add(it);
        if (combo.Items.Count > 0) combo.SelectedIndex = 0;
        var btn = new Button { Content = "+ Add", Width = 62, Margin = new Thickness(6, 0, 0, 0) };
        DockPanel.SetDock(btn, Dock.Right);
        btn.Click += (_, __) =>
        {
            if (combo.SelectedItem is string tn && EngineInterop.acs_editor_node3d_add_component(Engine, id, tn) != 0)
            { Log($"3D ノード {id} に {tn} を追加"); Populate3DInspector(id); }
        };
        add.Children.Add(btn); add.Children.Add(combo);
        panel.Children.Add(add);
        return panel;
    }

    /// <summary>「ラベル + チェックボックス」の bool 行 (3D Inspector の Visible/Enabled 用)。populate 中は発火抑止。</summary>
    private FrameworkElement Bool3DRow(string label, bool init, Action<bool> onChange)
    {
        var row = new DockPanel { Margin = new Thickness(0, 3, 0, 1) };
        var lbl = new TextBlock { Text = label, Width = 64, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        DockPanel.SetDock(lbl, Dock.Left); row.Children.Add(lbl);
        var cb = new CheckBox { IsChecked = init, VerticalAlignment = VerticalAlignment.Center };
        cb.Checked   += (_, __) => { if (!_pop3d) onChange(true); };
        cb.Unchecked += (_, __) => { if (!_pop3d) onChange(false); };
        row.Children.Add(cb);
        return row;
    }

    // 3D ノードの «.acsmat マテリアル» 参照スロット。2D の RefreshMaterialBox/OnMaterialSelected を
    // コード生成方式 (名前付き XAML 不使用) で鏡映。Assets/**/*.acsmat を列挙し、選択で set/clear。
    private FrameworkElement Build3DMaterialSlot(int id)
    {
        var row = new DockPanel { Margin = new Thickness(0, 3, 0, 1) };
        var lbl = new TextBlock { Text = "Material", Width = 64, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        DockPanel.SetDock(lbl, Dock.Left); row.Children.Add(lbl);
        var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center };
        combo.Items.Add(new ComboBoxItem { Content = "(なし)", Tag = null });
        string cur = EngineInterop.NodeMaterial3D(Engine, id);
        int sel = 0, idx = 0;
        if (_project != null && System.IO.Directory.Exists(_project.AssetsDir))
        {
            foreach (string f in System.IO.Directory.EnumerateFiles(_project.AssetsDir, "*.acsmat",
                                                                    System.IO.SearchOption.AllDirectories))
            {
                idx++;
                combo.Items.Add(new ComboBoxItem { Content = AssetRel(f), Tag = f });
                if (!string.IsNullOrEmpty(cur) && PathEq(f, cur)) sel = idx;
            }
        }
        if (sel == 0 && !string.IsNullOrEmpty(cur))   // Assets 外/未列挙のパスもそのまま 1 項目に
        {
            combo.Items.Add(new ComboBoxItem { Content = System.IO.Path.GetFileName(cur), Tag = cur });
            sel = combo.Items.Count - 1;
        }
        combo.SelectedIndex = sel;
        combo.SelectionChanged += (_, __) =>
        {
            if (_pop3d) return;
            if (combo.SelectedItem is not ComboBoxItem it) return;
            string? path = it.Tag as string;
            if (string.IsNullOrEmpty(path))
            {
                EngineInterop.acs_editor_node3d_clear_material(Engine, id);
                Log("3D マテリアルを外した (→ ノード色).");
            }
            else
            {
                EngineInterop.acs_editor_node3d_set_material(Engine, id, path);
                Log($"3D ノード {id} にマテリアル {AssetRel(path)} を割当.");
            }
        };
        row.Children.Add(combo);
        return row;
    }

    private void Set3DTransform(int id, int which, float a, float b, float c)
    {
        if (_pop3d) return;
        var tf = new float[9];
        if (EngineInterop.acs_editor_node3d_get_transform(Engine, id, tf) == 0) return;
        tf[which * 3 + 0] = a; tf[which * 3 + 1] = b; tf[which * 3 + 2] = c;
        EngineInterop.acs_editor_node3d_set_transform(Engine, id,
            tf[0], tf[1], tf[2], tf[3], tf[4], tf[5], tf[6], tf[7], tf[8]);
    }

    // ----- 動的 UI ヘルパ -----
    private TextBlock Section(string text) => new()
    {
        Text = text, Margin = new Thickness(0, 12, 0, 4), FontSize = 11, FontWeight = FontWeights.SemiBold,
        Foreground = (Brush)FindResource("TextDim"),
    };

    // 3 成分編集行 (X/Y/Z をテキストボックス、Enter / フォーカス喪失で onChanged)。
    private FrameworkElement Vec3Row(string label, float x, float y, float z, Action<float, float, float> onChanged)
    {
        var grid = new Grid { Margin = new Thickness(0, 3, 0, 3) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(64) });
        for (int i = 0; i < 3; ++i) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var lbl = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        Grid.SetColumn(lbl, 0); grid.Children.Add(lbl);
        var boxes = new TextBox[3];
        float[] v = { x, y, z };
        for (int i = 0; i < 3; ++i)
        {
            var tb = new TextBox
            {
                Text = v[i].ToString("0.###", CultureInfo.InvariantCulture),
                Margin = new Thickness(i == 0 ? 0 : 3, 0, i == 2 ? 0 : 0, 0),
                Style = (Style)FindResource("NumBox"),
            };
            Grid.SetColumn(tb, i + 1); grid.Children.Add(tb); boxes[i] = tb;
        }
        void Apply()
        {
            float fx = ParseF(boxes[0].Text, v[0]);
            float fy = ParseF(boxes[1].Text, v[1]);
            float fz = ParseF(boxes[2].Text, v[2]);
            onChanged(fx, fy, fz);
        }
        foreach (var tb in boxes)
        {
            tb.LostKeyboardFocus += (_, __) => Apply();
            tb.KeyDown += (_, ev) => { if (ev.Key == Key.Enter) { Apply(); Keyboard.ClearFocus(); } };
        }
        return grid;
    }

    // ===== プリミティブ追加 (GameObject メニュー / ツールバー) =====
    private void OnAdd3DCube(object sender, RoutedEventArgs e)   => Add3DNode(0, "Cube");
    private void OnAdd3DSphere(object sender, RoutedEventArgs e) => Add3DNode(1, "Sphere");
    private void OnAdd3DPlane(object sender, RoutedEventArgs e)  => Add3DNode(2, "Plane");

    /// <summary>2D ポリゴン (XY 平面、z=0 のフラットメッシュ) を 3D シーンのノードとして追加する。
    /// «2D も内部的に 3D 空間にある» の体現 (Phase B)。既定は正五角形。</summary>
    private void OnAdd2DPolygon(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        EnsureView3D();
        const int sides = 5;                                   // 正五角形 (半径 1、XY 平面)
        var xy = new float[sides * 2];
        for (int i = 0; i < sides; ++i)
        {
            double ang = Math.PI / 2 + i * 2 * Math.PI / sides;
            xy[i * 2]     = (float)Math.Cos(ang);
            xy[i * 2 + 1] = (float)Math.Sin(ang);
        }
        int id = EngineInterop.acs_editor_add_polygon3d(Engine, xy, sides, 0.45f, 0.78f, 0.95f, 1f, "Polygon2D");
        if (id < 0) return;
        BuildHierarchy();
        Select3DInHierarchy(id);
        Populate3DInspector(id);
        Log($"2D ポリゴンを 3D シーンに追加 (id {id})");
    }

    /// <summary>メッシュファイル (.gltf/.glb/.obj/.fbx) をダイアログで選び 3D ノードとして読み込む。</summary>
    private void OnImport3DMesh(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        EnsureView3D();
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "Import 3D Mesh",
            Filter = "3D Mesh (*.gltf;*.glb;*.obj;*.fbx)|*.gltf;*.glb;*.obj;*.fbx|All files (*.*)|*.*",
            InitialDirectory = _project?.AssetsDir,
        };
        if (dlg.ShowDialog(this) != true) return;
        int id = EngineInterop.acs_editor_add_mesh3d(Engine, dlg.FileName, "");
        if (id < 0) { Log($"メッシュ読込失敗: {System.IO.Path.GetFileName(dlg.FileName)} (形式/内容を確認)"); return; }
        BuildHierarchy();
        Select3DInHierarchy(id);
        Populate3DInspector(id);
        Log($"3D メッシュを読込: {System.IO.Path.GetFileName(dlg.FileName)} (id {id})");
    }

    /// <summary>画像ファイルをダイアログで選び、z=0 のスプライト (テクスチャ付きクアッド) として 3D シーンに追加する。
    /// «2D も内部的に 3D 空間にある» の体現 (Phase B)。アスペクト比は画像から自動。</summary>
    private void OnAddSprite(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        EnsureView3D();
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "Add Sprite (画像)",
            Filter = "画像 (*.png;*.jpg;*.jpeg;*.bmp;*.tga)|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files (*.*)|*.*",
            InitialDirectory = _project?.AssetsDir,
        };
        if (dlg.ShowDialog(this) != true) return;
        int id = EngineInterop.acs_editor_add_sprite3d(Engine, dlg.FileName, "");
        if (id < 0) { Log($"スプライト読込失敗: {System.IO.Path.GetFileName(dlg.FileName)} (画像形式を確認)"); return; }
        BuildHierarchy();
        Select3DInHierarchy(id);
        Populate3DInspector(id);
        Log($"スプライトを 3D シーンに追加: {System.IO.Path.GetFileName(dlg.FileName)} (id {id})");
    }

    private void Add3DNode(int prim, string name)
    {
        if (Engine == IntPtr.Zero) return;
        EnsureView3D();
        int id = EngineInterop.acs_editor_add_node3d(Engine, prim, name);
        if (id < 0) return;
        BuildHierarchy();
        Select3DInHierarchy(id);
        Populate3DInspector(id);
        Log($"3D {name} を追加 (id {id})");
    }

    // ===== 3D シーンの保存 / 読込 (<project>/Assets/scene3d.acs3d) =====
    private string Scene3DPath =>
        _project != null ? System.IO.Path.Combine(_project.RootDir, "Assets", "scene3d.acs3d") : "";

    /// <summary>3D シーンを INI 風テキストへシリアライズしてファイル保存する。</summary>
    public void Save3DScene()
    {
        if (Engine == IntPtr.Zero || _project == null) return;
        try
        {
            var buf = new byte[256 * 1024];
            EngineInterop.acs_editor_scene3d_serialize(Engine, buf, buf.Length);
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Scene3DPath)!);
            System.IO.File.WriteAllText(Scene3DPath, EngineInterop.Utf8Z(buf), System.Text.Encoding.UTF8);
            Log($"3D シーンを保存 ← {Scene3DPath}");
        }
        catch (Exception ex) { Log("3D scene save error: " + ex.Message); }
    }

    /// <summary>ファイルがあれば 3D シーンを読み込む。無ければ ABI の既定シーン (seed) のまま。</summary>
    private void Load3DSceneIfPresent()
    {
        if (Engine == IntPtr.Zero || _project == null) return;
        try
        {
            if (!System.IO.File.Exists(Scene3DPath)) return;
            string text = System.IO.File.ReadAllText(Scene3DPath, System.Text.Encoding.UTF8);
            if (EngineInterop.acs_editor_scene3d_load_text(Engine, text) != 0)
                Log($"3D シーンを読込 ← {Scene3DPath}");
        }
        catch (Exception ex) { Log("3D scene load error: " + ex.Message); }
    }
}
