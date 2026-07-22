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
            eye.Checked += (_, __) =>
            {
                if (Engine == IntPtr.Zero) return;
                EngineInterop.acs_editor_node3d_set_visible(Engine, vid, 1);
                RecordSceneDocumentChange("Visibility");
            };
            eye.Unchecked += (_, __) =>
            {
                if (Engine == IntPtr.Zero) return;
                EngineInterop.acs_editor_node3d_set_visible(Engine, vid, 0);
                RecordSceneDocumentChange("Visibility");
            };
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
        if (EngineInterop.acs_editor_node3d_kind(Engine, id) == 6)               // 空ノード (グループ用)
            return ("⊕", new SolidColorBrush(Color.FromRgb(0x9A, 0x9E, 0xA8)));
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
        ObserveSceneSelectionForMerge(
            use3D: true,
            nodeId: Engine == IntPtr.Zero
                ? id
                : EngineInterop.acs_editor_selected3d(Engine),
            selectionCount: Engine == IntPtr.Zero
                ? (id >= 0 ? 1 : 0)
                : EngineInterop.acs_editor_selected3d_count(Engine));
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
        RecordSceneDocumentChange("Enabled State");
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
        // プレハブ/Blueprint インスタンスなら «◆ Prefab: X» + Apply/Revert バナーを先頭に出す (2D PopulateComponents 鏡映)。
        string prefabSrc = EngineInterop.NodePrefabSrc3D(Engine, id);
        if (!string.IsNullOrEmpty(prefabSrc))
        {
            var banner = new StackPanel { Margin = new Thickness(0, 0, 0, 6) };
            banner.Children.Add(new TextBlock {
                Text = (IsBlueprint(prefabSrc) ? "◆ Blueprint: " : "◆ Prefab: ") + System.IO.Path.GetFileName(prefabSrc),
                Foreground = (Brush)FindResource("Accent"), FontSize = 11, FontWeight = FontWeights.SemiBold,
                Margin = new Thickness(0, 0, 0, 4) });
            var brow = new StackPanel { Orientation = Orientation.Horizontal };
            var apply  = new Button { Content = "Apply",  FontSize = 11, Padding = new Thickness(10, 2, 10, 2), Margin = new Thickness(0, 0, 6, 0),
                ToolTip = "この編集をプレハブ側へ反映 (instance → prefab)" };
            var revert = new Button { Content = "Revert", FontSize = 11, Padding = new Thickness(10, 2, 10, 2),
                ToolTip = "編集を破棄しプレハブの状態へ戻す (prefab → instance)" };
            int curId = id;
            apply.Click  += (_, __) => ApplyToPrefab(curId);
            revert.Click += (_, __) => RevertToPrefab(curId);
            brow.Children.Add(apply); brow.Children.Add(revert);
            banner.Children.Add(brow);
            Insp3DPanel.Children.Add(new Border {
                Background = (Brush)FindResource("Panel2"), CornerRadius = new CornerRadius(5),
                Padding = new Thickness(8, 6, 8, 7), Margin = new Thickness(0, 0, 0, 6), Child = banner });
        }
        bool anyDetails = false;
        // Actor label stays editable, but participates in Details filtering like any other property.
        if (DetailsMatches("actor", "node", "name", "label"))
        {
            var nameRow = new DockPanel { Margin = new Thickness(0, 2, 0, 6) };
            var lbl = new TextBlock { Text = "Name", Width = 64, VerticalAlignment = VerticalAlignment.Center,
                Foreground = (Brush)FindResource("TextDim") };
            DockPanel.SetDock(lbl, Dock.Left); nameRow.Children.Add(lbl);
            _name3dBox = new TextBox { Text = Node3DName(id), Style = (Style)FindResource("NumBox") };
            int nid = id;
            _name3dBox.LostKeyboardFocus += (_, __) => Apply3DRename(nid, _name3dBox?.Text);
            _name3dBox.KeyDown += (_, ev) => { if (ev.Key == Key.Enter) { Apply3DRename(nid, _name3dBox?.Text); Keyboard.ClearFocus(); } };
            nameRow.Children.Add(_name3dBox);
            Insp3DPanel.Children.Add(nameRow);
            anyDetails = true;
        }

        if (DetailsMatches("transform", "location", "position", "rotation", "scale"))
        {
            var transformBody = new StackPanel();
            transformBody.Children.Add(Vec3Row("Location", tf[0], tf[1], tf[2],
                (x, y, z) => Set3DTransform(id, 0, x, y, z),
                id,
                "inspector.transform.location"));
            transformBody.Children.Add(Vec3Row("Rotation", tf[3], tf[4], tf[5],
                (x, y, z) => Set3DTransform(id, 1, x, y, z),
                id,
                "inspector.transform.rotation"));
            transformBody.Children.Add(Vec3Row("Scale", tf[6], tf[7], tf[8],
                (x, y, z) => Set3DTransform(id, 2, x, y, z),
                id,
                "inspector.transform.scale"));
            if (kind == 6)
                transformBody.Children.Add(LabeledValue3D("Mobility", "Empty / Group"));
            Insp3DPanel.Children.Add(DetailsCategory("Transform", transformBody));
            anyDetails = true;
        }

        // Visible はヒエラルキー (各ノードの目トグル) へ、Enabled はヘッダ (名前の横) へ移動済み。

        // Native renderer and reflected behavior components share one component stack.
        // Mesh Renderer is intentionally not special-cased beside Transform anymore.
        FrameworkElement components = Build3DComponents(id, kind, col, out int shownComponents);
        if (shownComponents > 0 || DetailsMatches("components", "add component", "script", "native"))
        {
            Insp3DPanel.Children.Add(DetailsCategory("Components", components));
            anyDetails = true;
        }

        if (!anyDetails)
            Insp3DPanel.Children.Add(EmptyDetailsResult());

        // 削除ボタン
        var del = new Button { Content = "🗑 Delete", Padding = new Thickness(10, 4, 10, 4), Margin = new Thickness(0, 12, 0, 0),
            HorizontalAlignment = HorizontalAlignment.Left };
        del.Click += (_, __) =>
        {
            EngineInterop.acs_editor_delete_node3d(Engine, id);
            BuildHierarchy();
            Clear3DInspector();
            RecordSceneDocumentChange("Delete Node");
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
        // 画像を外して平面へ戻す Clear ボタン (2D の clear_sprite に対応)。
        var clear = new Button { Content = "Clear", Width = 46, Margin = new Thickness(0, 0, 4, 0),
            VerticalAlignment = VerticalAlignment.Center, ToolTip = "画像を外して平面に戻す" };
        clear.Click += (_, __) =>
        {
            if (EngineInterop.acs_editor_node3d_clear_sprite(Engine, id) != 0)
            {
                Log($"スプライト画像を解除 (平面に戻す) (id {id})");
                Populate3DInspector(id);
                RecordSceneDocumentChange("Clear Sprite");
            }
        };
        DockPanel.SetDock(clear, Dock.Right); row.Children.Add(clear);
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
                RecordSceneDocumentChange("Assign Sprite");
            }
            else Log($"スプライト差替え失敗: {System.IO.Path.GetFileName(dlg.FileName)} (画像形式を確認)");
        };
        return row;
    }

    /// <summary>
    /// 3D node component stack. The built-in Mesh Renderer occupies the same visual and
    /// interaction hierarchy as reflected script components, matching the actor/component model.
    /// </summary>
    private FrameworkElement Build3DComponents(int id, int kind, float[] color, out int shownComponents)
    {
        var panel = new StackPanel();
        var dim    = (Brush)FindResource("TextDim");
        shownComponents = 0;

        // The render payload is a native component. It is fixed for renderable nodes, so the
        // card carries a NATIVE badge and no remove action.
        if (kind != 6 && DetailsMatches(
                "component", "native", "mesh renderer", "mesh", "renderer",
                "shape", "type", "sprite", "color", "material"))
        {
            panel.Children.Add(BuildMeshRendererComponent(id, kind, color));
            shownComponents++;
        }

        int count = EngineInterop.acs_editor_node3d_component_count(Engine, id);
        for (int i = 0; i < count; i++)
        {
            int idx = i;   // ABI の component slot
            string cname = EngineInterop.Component3DName(Engine, id, i);
            if (!DetailsComponentMatches(cname)) continue;

            var inner = new StackPanel();

            // 編集プロパティ (reflection スキーマ駆動)。2D と同じ BuildPropEditor を is3d:true で流用。
            int pc = EngineInterop.acs_editor_component_prop_count(cname);
            if (pc == 0)
                inner.Children.Add(new TextBlock { Text = "(編集可能なプロパティなし)", Foreground = dim, FontSize = 11, Margin = new Thickness(0, 1, 0, 0) });
            else
            {
                string lastCat = "\0";   // 初回必ず不一致
                for (int p = 0; p < pc; p++)
                {
                    string cat = EngineInterop.ComponentPropCategory(cname, p);
                    if (cat != lastCat && cat.Length > 0)        // カテゴリ (UPROPERTY(Category=…)) が変わったら見出し
                        inner.Children.Add(new TextBlock { Text = cat, Foreground = dim, FontSize = 10, FontWeight = FontWeights.SemiBold,
                            Margin = new Thickness(0, p == 0 ? 0 : 5, 0, 1) });
                    lastCat = cat;
                    var prow = BuildPropEditor(id, idx, cname, p, is3d: true);
                    if (prow != null) inner.Children.Add(prow);   // null = Hidden 指定子 → 出さない
                }
            }

            // CallInEditor (ACS_FUNCTION) メソッドをボタン化 → クリックで 3D invoke。
            int mc = EngineInterop.acs_editor_component_method_count(cname);
            int curSlot = idx;
            for (int mi = 0; mi < mc; mi++)
            {
                int mflags = EngineInterop.acs_editor_component_method_flags_at(cname, mi);
                if ((mflags & 0x2) == 0) continue;            // CallInEditor 指定のみボタン化
                string mname = EngineInterop.ComponentMethodName(cname, mi);
                var mbtn = new Button { Content = "▶ " + mname, FontSize = 11, Padding = new Thickness(8, 2, 8, 2),
                    Margin = new Thickness(0, 4, 0, 0), HorizontalAlignment = HorizontalAlignment.Left };
                mbtn.Click += (_, __) =>
                {
                    if (EngineInterop.acs_editor_node3d_invoke_method(Engine, id, curSlot, mname) != 0)
                    {
                        NotifySceneMutationPending(
                            $"Invoke {cname}.{mname}",
                            $"component.{curSlot}.{cname}.method.{mname}",
                            id);
                        Log($"{cname}.{mname}() を呼び出し", "General", LogLevel.Info);
                    }
                    else Log($"{cname}.{mname}() の呼び出しに失敗");
                };
                inner.Children.Add(mbtn);
            }

            int capturedSlot = idx;
            panel.Children.Add(ComponentCard(cname, inner, native: false, remove: () =>
            {
                EngineInterop.acs_editor_node3d_remove_component_at(Engine, id, capturedSlot);
                Populate3DInspector(id);
                RecordSceneDocumentChange("Remove Component");
            }));
            shownComponents++;
        }

        if (shownComponents == 0)
            panel.Children.Add(new TextBlock
            {
                Text = _detailsFilter.Length == 0
                    ? "No components are attached."
                    : "No components match this filter.",
                Foreground = dim,
                FontSize = 11,
                Margin = new Thickness(2, 2, 0, 7),
            });

        // Add Component always follows the card stack; it never appears between Transform and
        // native components, and it remains usable while a Details filter is active.
        var add = new Grid { Margin = new Thickness(0, 3, 0, 0) };
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center, MinWidth = 130 };
        foreach (var it in CompAddBox.Items) combo.Items.Add(it);
        if (combo.Items.Count > 0) combo.SelectedIndex = 0;
        var btn = new Button
        {
            Content = "+ Add Component",
            MinWidth = 112,
            Margin = new Thickness(6, 0, 0, 0),
            Foreground = (Brush)FindResource("OkFg"),
        };
        Grid.SetColumn(btn, 1);
        btn.Click += (_, __) =>
        {
            if (combo.SelectedItem is string tn && EngineInterop.acs_editor_node3d_add_component(Engine, id, tn) != 0)
            {
                Log($"3D ノード {id} に {tn} を追加");
                Populate3DInspector(id);
                RecordSceneDocumentChange("Add Component");
            }
        };
        add.Children.Add(combo);
        add.Children.Add(btn);
        panel.Children.Add(add);
        return panel;
    }

    /// <summary>Fixed native mesh renderer component, rendered in the same card stack as scripts.</summary>
    private FrameworkElement BuildMeshRendererComponent(int id, int kind, float[] color)
    {
        var body = new StackPanel();
        bool showAll = _detailsFilter.Length == 0
            || DetailsMatches("component", "native", "mesh renderer", "mesh", "renderer");

        if (showAll || DetailsMatches("shape", "type", "primitive", "sprite"))
        {
            if (kind >= 0 && kind <= 2)
            {
                var shapeRow = new DockPanel { Margin = new Thickness(0, 3, 0, 2) };
                var label = new TextBlock
                {
                    Text = "Shape",
                    Width = 78,
                    VerticalAlignment = VerticalAlignment.Center,
                    Foreground = (Brush)FindResource("TextDim"),
                };
                DockPanel.SetDock(label, Dock.Left);
                shapeRow.Children.Add(label);
                var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center };
                foreach (string shape in new[] { "Cube", "Sphere", "Plane" }) combo.Items.Add(shape);
                combo.SelectedIndex = kind;
                combo.SelectionChanged += (_, __) =>
                {
                    if (_pop3d) return;
                    int selected = combo.SelectedIndex;
                    if (selected >= 0
                        && EngineInterop.acs_editor_node3d_set_prim(Engine, id, selected) != 0)
                    {
                        Log($"3D node {id}: Mesh Renderer shape → {combo.SelectedItem}");
                        RecordSceneDocumentChange("Change Mesh Shape");
                    }
                };
                shapeRow.Children.Add(combo);
                body.Children.Add(shapeRow);
            }
            else
            {
                string typeName = kind switch
                {
                    3 => "Static Mesh",
                    4 => "Sprite",
                    5 => "Polygon",
                    _ => "Unknown",
                };
                body.Children.Add(LabeledValue3D("Type", typeName));
                if (kind == 4) body.Children.Add(SpriteRow3D(id));
            }
        }

        if (showAll || DetailsMatches("color", "tint", "opacity", "alpha"))
            body.Children.Add(Vec4Row("Color", color[0], color[1], color[2], color[3],
                (r, g, b, a) => EngineInterop.acs_editor_node3d_set_color(Engine, id, r, g, b, a),
                id,
                "inspector.appearance.color"));

        if (showAll || DetailsMatches("material", "shader", "surface"))
            body.Children.Add(Build3DMaterialSlot(id));

        return ComponentCard("Mesh Renderer", body, native: true);
    }

    /// <summary>「ラベル + チェックボックス」の bool 行 (3D Inspector の Visible/Enabled 用)。populate 中は発火抑止。</summary>
    private FrameworkElement Bool3DRow(string label, bool init, Action<bool> onChange)
    {
        var row = new DockPanel { Margin = new Thickness(0, 3, 0, 1) };
        var lbl = new TextBlock { Text = label, Width = 64, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        DockPanel.SetDock(lbl, Dock.Left); row.Children.Add(lbl);
        var cb = new CheckBox { IsChecked = init, VerticalAlignment = VerticalAlignment.Center };
        cb.Checked += (_, __) =>
        {
            if (_pop3d) return;
            onChange(true);
            NotifySceneMutationPending();
        };
        cb.Unchecked += (_, __) =>
        {
            if (_pop3d) return;
            onChange(false);
            NotifySceneMutationPending();
        };
        row.Children.Add(cb);
        return row;
    }

    // 3D Mesh Renderer material slot. It mirrors the complete 2D workflow: choose an existing
    // asset, edit it modelessly, create-and-assign a new one, or clear the assignment.
    private FrameworkElement Build3DMaterialSlot(int id)
    {
        var root = new StackPanel { Margin = new Thickness(0, 3, 0, 1) };
        var row = new DockPanel();
        var lbl = new TextBlock { Text = "Material", Width = 64, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        DockPanel.SetDock(lbl, Dock.Left); row.Children.Add(lbl);
        var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center };
        combo.Items.Add(new ComboBoxItem { Content = "(なし)", Tag = null });
        string current = EngineInterop.NodeMaterial3D(Engine, id);
        MaterialAssetCatalog catalog = MaterialAssetWorkflow.BuildCatalog(
            _project?.AssetsDir,
            current);
        foreach (MaterialAssetChoice choice in catalog.Choices)
        {
            combo.Items.Add(new ComboBoxItem
            {
                Content = choice.DisplayName,
                Tag = choice.FullPath,
                ToolTip = choice.FullPath,
            });
        }
        combo.SelectedIndex = catalog.SelectedIndex + 1;
        row.Children.Add(combo);
        root.Children.Add(row);

        var actions = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(64, 4, 0, 0),
        };
        var edit = new Button
        {
            Content = "Edit",
            Padding = new Thickness(7, 2, 7, 2),
            Margin = new Thickness(0, 0, 4, 0),
            Foreground = (Brush)FindResource("InfoFg"),
            ToolTip = "Open the assigned material in the node editor",
        };
        var create = new Button
        {
            Content = "New",
            Padding = new Thickness(7, 2, 7, 2),
            Margin = new Thickness(0, 0, 4, 0),
            Foreground = (Brush)FindResource("OkFg"),
            ToolTip = "Create, assign, and edit a new material",
        };
        var clear = new Button
        {
            Content = "Clear",
            Padding = new Thickness(7, 2, 7, 2),
            Foreground = (Brush)FindResource("WarnFg"),
            ToolTip = "Remove the material assignment",
        };

        string? selectedPath = catalog.SelectedIndex >= 0
            ? catalog.Choices[catalog.SelectedIndex].FullPath
            : null;
        int committedSelection = combo.SelectedIndex;
        bool restoringSelection = false;
        void UpdateActionState()
        {
            edit.IsEnabled = !string.IsNullOrWhiteSpace(selectedPath) &&
                             System.IO.File.Exists(selectedPath);
            clear.IsEnabled = !string.IsNullOrWhiteSpace(selectedPath);
        }

        combo.SelectionChanged += (_, __) =>
        {
            if (_pop3d || restoringSelection) return;
            if (combo.SelectedItem is not ComboBoxItem it) return;
            string? path = it.Tag as string;
            int changed;
            if (string.IsNullOrEmpty(path))
            {
                changed = EngineInterop.acs_editor_node3d_clear_material(Engine, id);
            }
            else
            {
                changed = EngineInterop.acs_editor_node3d_set_material(Engine, id, path);
            }
            if (changed == 0)
            {
                Log($"3D ノード {id} のマテリアル変更に失敗しました。");
                restoringSelection = true;
                combo.SelectedIndex = committedSelection;
                restoringSelection = false;
                return;
            }
            selectedPath = path;
            committedSelection = combo.SelectedIndex;
            UpdateActionState();
            Log(string.IsNullOrEmpty(path)
                ? "3D マテリアルを外した (→ ノード色)."
                : $"3D ノード {id} にマテリアル {AssetRel(path)} を割当.");
            RecordSceneDocumentChange(string.IsNullOrEmpty(path)
                ? "Clear Material"
                : "Assign Material");
        };

        edit.Click += (_, __) =>
        {
            if (!string.IsNullOrWhiteSpace(selectedPath) &&
                System.IO.File.Exists(selectedPath))
            {
                OpenMaterialEditor(selectedPath);
            }
        };
        create.Click += (_, __) =>
        {
            if (!TryCreateMaterialAsset(out string path)) return;
            if (EngineInterop.acs_editor_node3d_set_material(Engine, id, path) == 0)
            {
                Log($"3D ノード {id} へ新規マテリアルを割り当てられませんでした。");
                OpenMaterialEditor(path);
                return;
            }

            RecordSceneDocumentChange("Assign Material");
            Log($"3D ノード {id} に新規マテリアル {AssetRel(path)} を割当.");
            Populate3DInspector(id);
            OpenMaterialEditor(path);
        };
        clear.Click += (_, __) =>
        {
            if (string.IsNullOrWhiteSpace(selectedPath)) return;
            if (EngineInterop.acs_editor_node3d_clear_material(Engine, id) == 0)
            {
                Log($"3D ノード {id} のマテリアル解除に失敗しました。");
                return;
            }

            RecordSceneDocumentChange("Clear Material");
            Log("3D マテリアルを外した (→ ノード色).");
            Populate3DInspector(id);
        };

        actions.Children.Add(edit);
        actions.Children.Add(create);
        actions.Children.Add(clear);
        root.Children.Add(actions);
        UpdateActionState();
        return root;
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
    // RGBA など 4 成分の編集行 (Vec3Row の 4 成分版)。3D 色の Alpha 編集用。
    private FrameworkElement Vec4Row(
        string label,
        float x,
        float y,
        float z,
        float w,
        Action<float, float, float, float> onChanged,
        int nodeId,
        string propertyIdentity)
    {
        var grid = new Grid { Margin = new Thickness(0, 3, 0, 3) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(64) });
        for (int i = 0; i < 4; ++i) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var lbl = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        Grid.SetColumn(lbl, 0); grid.Children.Add(lbl);
        var boxes = new TextBox[4];
        float[] v = { x, y, z, w };
        for (int i = 0; i < 4; ++i)
        {
            var tb = new TextBox
            {
                Text = v[i].ToString("0.###", CultureInfo.InvariantCulture),
                Margin = new Thickness(i == 0 ? 0 : 3, 0, 0, 0),
                Style = (Style)FindResource("NumBox"),
            };
            Grid.SetColumn(tb, i + 1); grid.Children.Add(tb); boxes[i] = tb;
        }

        static byte B(float value) => (byte)Math.Clamp(value * 255f, 0f, 255f);
        var swatch = new Button
        {
            Width = 26,
            Height = 26,
            Margin = new Thickness(6, 0, 0, 0),
            Padding = new Thickness(0),
            BorderBrush = (Brush)FindResource("Edge"),
            BorderThickness = new Thickness(1),
            Cursor = Cursors.Hand,
            ToolTip = "Open RGBA color picker",
            Background = new SolidColorBrush(Color.FromRgb(B(x), B(y), B(z))),
        };
        Grid.SetColumn(swatch, 5);
        grid.Children.Add(swatch);

        void Apply()
        {
            float r = ParseF(boxes[0].Text, v[0]);
            float g = ParseF(boxes[1].Text, v[1]);
            float b = ParseF(boxes[2].Text, v[2]);
            float a = ParseF(boxes[3].Text, v[3]);
            swatch.Background = new SolidColorBrush(Color.FromRgb(B(r), B(g), B(b)));
            if (r == v[0] && g == v[1] && b == v[2] && a == v[3]) return;
            v[0] = r; v[1] = g; v[2] = b; v[3] = a;
            onChanged(r, g, b, a);
            NotifySceneMutationPending(
                $"Edit {label}",
                propertyIdentity,
                nodeId);
        }
        foreach (var tb in boxes)
        {
            tb.LostKeyboardFocus += (_, __) => Apply();
            tb.KeyDown += (_, ev) => { if (ev.Key == Key.Enter) { Apply(); Keyboard.ClearFocus(); } };
        }
        swatch.Click += (_, __) =>
        {
            float r = ParseF(boxes[0].Text, v[0]);
            float g = ParseF(boxes[1].Text, v[1]);
            float b = ParseF(boxes[2].Text, v[2]);
            float a = ParseF(boxes[3].Text, v[3]);
            var initial = Color.FromArgb(B(a), B(r), B(g), B(b));
            if (!ColorPickerDialog.TryPick(this, initial, allowAlpha: true, out Color picked)) return;

            boxes[0].Text = (picked.R / 255f).ToString("0.###", CultureInfo.InvariantCulture);
            boxes[1].Text = (picked.G / 255f).ToString("0.###", CultureInfo.InvariantCulture);
            boxes[2].Text = (picked.B / 255f).ToString("0.###", CultureInfo.InvariantCulture);
            boxes[3].Text = (picked.A / 255f).ToString("0.###", CultureInfo.InvariantCulture);
            Apply();
        };
        return grid;
    }

    private FrameworkElement Vec3Row(
        string label,
        float x,
        float y,
        float z,
        Action<float, float, float> onChanged,
        int nodeId,
        string propertyIdentity)
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
            if (fx == v[0] && fy == v[1] && fz == v[2]) return;
            v[0] = fx; v[1] = fy; v[2] = fz;
            onChanged(fx, fy, fz);
            NotifySceneMutationPending(
                $"Edit {label}",
                propertyIdentity,
                nodeId);
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

    /// <summary>2D ポリゴン (XY 平面、z=0 のフラットメッシュ) を現在のシーンへ追加する。
    /// «2D も内部的に 3D 空間にある» の体現 (Phase B)。既定は正五角形。</summary>
    private void OnAdd2DPolygon(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (!EnsureView3D()) return;
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
        RecordSceneDocumentChange("Create Polygon");
        Log($"2D ポリゴンをシーンへ追加 (.acs3d source, id {id})");
    }

    /// <summary>メッシュファイル (.gltf/.glb/.obj/.fbx) をダイアログで選び 3D ノードとして読み込む。</summary>
    private void OnImport3DMesh(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (!EnsureView3D()) return;
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
        RecordSceneDocumentChange("Import Mesh");
        Log($"3D メッシュを読込: {System.IO.Path.GetFileName(dlg.FileName)} (id {id})");
    }

    /// <summary>画像ファイルをダイアログで選び、z=0 のスプライト (テクスチャ付きクアッド) として現在のシーンへ追加する。
    /// «2D も内部的に 3D 空間にある» の体現 (Phase B)。アスペクト比は画像から自動。</summary>
    private void OnAddSprite(object sender, RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero) return;
        if (!EnsureView3D()) return;
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
        RecordSceneDocumentChange("Create Sprite");
        Log(
            $"スプライトをシーンへ追加 (.acs3d source): " +
            $"{System.IO.Path.GetFileName(dlg.FileName)} (id {id})");
    }

    private void Add3DNode(int prim, string name)
    {
        if (Engine == IntPtr.Zero) return;
        if (!EnsureView3D()) return;
        int id = EngineInterop.acs_editor_add_node3d(Engine, prim, name);
        if (id < 0) return;
        BuildHierarchy();
        Select3DInHierarchy(id);
        Populate3DInspector(id);
        RecordSceneDocumentChange("Create Node");
        Log($"3D {name} を追加 (id {id})");
    }

    // ===== Legacy .acs3d source persistence (<project>/Assets/scene3d.acs3d) =====
    private string Scene3DPath =>
        _project != null ? System.IO.Path.Combine(_project.RootDir, "Assets", "scene3d.acs3d") : "";

    /// <summary>Legacy .acs3d sourceをテキストへシリアライズして保存する。</summary>
    public async void Save3DScene() => await Save3DSceneAsync();

    private async System.Threading.Tasks.Task<bool> Save3DSceneAsync()
    {
        if (Engine == IntPtr.Zero) return false;
        string? previousPath = _currentScenePath;
        string? target = !string.IsNullOrWhiteSpace(_currentScenePath)
                      && string.Equals(System.IO.Path.GetExtension(_currentScenePath), ".acs3d",
                          StringComparison.OrdinalIgnoreCase)
            ? _currentScenePath
            : !string.IsNullOrWhiteSpace(Scene3DPath) ? Scene3DPath : null;
        if (string.IsNullOrWhiteSpace(target))
        {
            var dialog = new Microsoft.Win32.SaveFileDialog {
                Title = "Save Legacy .acs3d Source",
                Filter = "Legacy ACS 3D Source (*.acs3d)|*.acs3d|All files (*.*)|*.*",
                DefaultExt = ".acs3d",
                FileName = "scene3d.acs3d",
                InitialDirectory = _project?.AssetsDir,
            };
            if (dialog.ShowDialog(this) != true) return false;
            target = dialog.FileName;
        }
        try
        {
            target = ValidateSceneDocumentPath(target, use3D: true);
            string text = EngineInterop.Scene3DText(Engine);
            if (_project != null)
            {
                SceneSourceFile.WriteProjectSceneAtomicText(
                    target,
                    text,
                    _project.RootDir,
                    _project.AssetsDir,
                    SceneDocumentMode.ThreeD);
            }
            else
            {
                SceneSourceFile.WriteAtomicText(
                    target,
                    text,
                    expectedMode: SceneDocumentMode.ThreeD);
            }
            SetCurrentScenePath(target);
            MarkSceneClean(text);
            NotifySceneDocumentSaved(use3D: true, target);
            await OnSceneSourceSavedAsync(use3D: true, previousPath, target);
            Log($"Saved scene source (.acs3d) → {target}");
            return true;
        }
        catch (Exception ex)
        {
            Log(".acs3d scene source save error: " + ex.Message);
            return false;
        }
    }

}
