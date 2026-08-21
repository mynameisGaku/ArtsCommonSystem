// SPDX-License-Identifier: Apache-2.0
// MainWindow の 3D ビューポート編集 UI (Phase 2): Hierarchy への 3D ノード一覧、
// 動的生成の 3D Inspector (pos/rot/scale/色/形状)、プリミティブ追加。
// 2D の Hierarchy/Inspector とは独立に動き、view3d フラグで切り替わる。
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

public partial class MainWindow
{
    // Bootstrap state is the canonical 3D world. Legacy .acscene loading explicitly changes
    // this source-adapter flag before any viewport is published.
    private bool _view3d = true;
    private bool _pop3d;                  // 3D Inspector を populate 中 (編集イベントの再入抑止)

    // ===== Hierarchy: 3D ノード一覧 =====
    private void Build3DHierarchy()
    {
        if (Engine == IntPtr.Zero) return;
        HierarchyTree.Items.Clear();
        int count = EngineInterop.acs_editor_node3d_count(Engine);
        int sel = EngineInterop.acs_editor_selected3d(Engine);
        int designatedCameraNode = GetDesignatedCameraNode();
        var cameraNodeIds = new System.Collections.Generic.HashSet<int>();
        UpdateCameraFrustumControl();
        if (CameraAuthoringAvailable)
        {
            int cameraCount = Math.Min(
                Math.Max(
                    0,
                    EngineInterop.acs_editor_camera3d_count(Engine)),
                CameraAuthoringContract.MaximumCameraCount);
            for (int cameraIndex = 0;
                 cameraIndex < cameraCount;
                 ++cameraIndex)
            {
                int cameraNode =
                    EngineInterop.acs_editor_camera3d_node_id_at(
                        Engine,
                        cameraIndex);
                if (cameraNode >= 0)
                    cameraNodeIds.Add(cameraNode);
            }
        }
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
                MarkPrefabRootOverride3D(vid, PrefabRootProperty3D.Visible);
                RecordSceneDocumentChange("Visibility");
            };
            eye.Unchecked += (_, __) =>
            {
                if (Engine == IntPtr.Zero) return;
                EngineInterop.acs_editor_node3d_set_visible(Engine, vid, 0);
                MarkPrefabRootOverride3D(vid, PrefabRootProperty3D.Visible);
                RecordSceneDocumentChange("Visibility");
            };
            var hdr = new StackPanel { Orientation = Orientation.Horizontal };
            hdr.Children.Add(eye);
            bool isCamera = cameraNodeIds.Contains(id);
            (string glyph, Brush gcol) = isCamera
                ? ("▹", new SolidColorBrush(Color.FromRgb(0xA9, 0x8B, 0xE8)))
                : PrimGlyph(id);   // ノード種別アイコン (Cube/Sphere/Plane/Mesh/Camera)
            hdr.Children.Add(new TextBlock { Text = glyph, FontSize = 11, Margin = new Thickness(0, 0, 5, 0),
                                             VerticalAlignment = VerticalAlignment.Center, Foreground = gcol });
            hdr.Children.Add(new TextBlock { Text = Node3DName(id), VerticalAlignment = VerticalAlignment.Center });
            if (isCamera && id == designatedCameraNode)
            {
                hdr.Children.Add(new Border
                {
                    Margin = new Thickness(7, 0, 0, 0),
                    Padding = new Thickness(4, 0, 4, 0),
                    CornerRadius = new CornerRadius(3),
                    Background = new SolidColorBrush(Color.FromArgb(0x32, 0xA9, 0x8B, 0xE8)),
                    Child = new TextBlock
                    {
                        Text = "ACTIVE",
                        FontSize = 8,
                        FontWeight = FontWeights.SemiBold,
                        Foreground = new SolidColorBrush(Color.FromRgb(0xC9, 0xB6, 0xF3)),
                        VerticalAlignment = VerticalAlignment.Center,
                    },
                });
            }
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
        bool wasPopulating = _pop3d;
        _pop3d = true;
        try
        {
            Insp3DPanel.Children.Clear();
            Insp3DPanel.Visibility = Visibility.Collapsed;
            // ヘッダの Enabled トグルも隠す (選択なし / 2D)。
            // IsThreeState の解除で Unchecked が発火しても native state を変更しない。
            InspEnabled.Visibility = Visibility.Collapsed;
            InspEnabled.IsThreeState = false;
            InspEnabled.ToolTip = "有効 (Enabled)";
        }
        finally
        {
            _pop3d = wasPopulating;
        }
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
        int[] selected = Selected3DNodeIds();
        if (selected.Length > 1)
        {
            bool? desired = InspEnabled.IsChecked;
            if (!desired.HasValue) return;
            if (!ApplyMultiEnabled(selected, desired.Value))
                Populate3DInspector(selected[0]);
            return;
        }

        int id = EngineInterop.acs_editor_selected3d(Engine);
        if (id < 0) return;
        EngineInterop.acs_editor_node3d_set_enabled(Engine, id, InspEnabled.IsChecked == true ? 1 : 0);
        MarkPrefabRootOverride3D(id, PrefabRootProperty3D.Enabled);
        RecordSceneDocumentChange("Enabled State");
    }

    private void Populate3DInspector(int id)
    {
        int[] selected = Selected3DNodeIds();
        if (selected.Length > 1 &&
            Array.IndexOf(selected, id) >= 0)
        {
            Populate3DMultiInspector(selected);
            return;
        }

        PopulateSingle3DInspector(id);
    }

    private void PopulateSingle3DInspector(int id)
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
        InspEnabled.IsThreeState = false;
        InspEnabled.IsChecked  = EngineInterop.acs_editor_node3d_get_enabled(Engine, id) != 0;
        InspEnabled.Visibility = Visibility.Visible;
        InspEnabled.ToolTip = "有効 (Enabled)";
        Insp3DPanel.Children.Clear();
        // プレハブ/Blueprint インスタンスなら «◆ Prefab: X» + Apply/Revert バナーを先頭に出す (2D PopulateComponents 鏡映)。
        string prefabSrc = EngineInterop.NodePrefabSrc3D(Engine, id);
        if (!string.IsNullOrEmpty(prefabSrc))
        {
            PrefabRootProperty3D rootOverrides =
                EngineInterop.acs_editor_prefab_instance3d_root_override_mask(
                    Engine,
                    id);
            int rootOverrideCount =
                (rootOverrides.HasFlag(PrefabRootProperty3D.Visible) ? 1 : 0) +
                (rootOverrides.HasFlag(PrefabRootProperty3D.Enabled) ? 1 : 0) +
                (rootOverrides.HasFlag(PrefabRootProperty3D.Color) ? 1 : 0);
            int componentOverrideCount = 0;
            int componentCount =
                EngineInterop.acs_editor_node3d_component_count(Engine, id);
            for (int slot = 0; slot < componentCount; ++slot)
            {
                uint mask = EngineInterop
                    .acs_editor_prefab_instance3d_root_component_property_override_mask(
                        Engine,
                        id,
                        slot);
                while (mask != 0u)
                {
                    mask &= mask - 1u;
                    componentOverrideCount++;
                }
            }
            string rootOverrideSummary =
                $"{rootOverrideCount} root override{(rootOverrideCount == 1 ? "" : "s")}";
            string componentOverrideSummary =
                $"{componentOverrideCount} component override{(componentOverrideCount == 1 ? "" : "s")}";
            string overrideSummary = rootOverrideCount > 0 && componentOverrideCount > 0
                ? $"{rootOverrideSummary}, {componentOverrideSummary}"
                : rootOverrideCount > 0
                    ? rootOverrideSummary
                    : componentOverrideCount > 0
                        ? componentOverrideSummary
                        : "";
            int curId = id;
            var banner = new StackPanel { Margin = new Thickness(0, 0, 0, 6) };
            banner.Children.Add(new TextBlock {
                Text = (IsBlueprint(prefabSrc) ? "◆ Blueprint: " : "◆ Prefab: ") +
                    System.IO.Path.GetFileName(prefabSrc),
                Foreground = (Brush)FindResource("Accent"), FontSize = 11, FontWeight = FontWeights.SemiBold,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, string.IsNullOrEmpty(overrideSummary) ? 4 : 1) });
            if (!string.IsNullOrEmpty(overrideSummary))
            {
                banner.Children.Add(new TextBlock {
                    Text = overrideSummary,
                    Foreground = (Brush)FindResource("TextDim"), FontSize = 10,
                    TextWrapping = TextWrapping.Wrap,
                    Margin = new Thickness(0, 0, 0, 4) });
            }
            var brow = new StackPanel { Orientation = Orientation.Horizontal };
            var apply  = new Button { Content = "Apply",  FontSize = 11, Padding = new Thickness(10, 2, 10, 2), Margin = new Thickness(0, 0, 6, 0),
                ToolTip = "この編集をプレハブ側へ反映 (instance → prefab)" };
            var revert = new Button { Content = "Revert", FontSize = 11, Padding = new Thickness(10, 2, 10, 2),
                ToolTip = "編集を破棄しプレハブの状態へ戻す (prefab → instance)" };
            apply.Click  += (_, __) => ApplyToPrefab(curId);
            revert.Click += (_, __) => RevertToPrefab(curId);
            brow.Children.Add(apply); brow.Children.Add(revert);
            banner.Children.Add(brow);
            if (rootOverrideCount > 0)
            {
                var overrideRow = new WrapPanel { Margin = new Thickness(0, 5, 0, 0) };
                if (rootOverrides.HasFlag(PrefabRootProperty3D.Visible))
                    overrideRow.Children.Add(CreatePrefabRootOverrideApplyButton3D(curId, PrefabRootProperty3D.Visible, "Visible"));
                if (rootOverrides.HasFlag(PrefabRootProperty3D.Visible))
                    overrideRow.Children.Add(CreatePrefabRootOverrideRevertButton3D(curId, PrefabRootProperty3D.Visible, "Visible"));
                if (rootOverrides.HasFlag(PrefabRootProperty3D.Enabled))
                    overrideRow.Children.Add(CreatePrefabRootOverrideApplyButton3D(curId, PrefabRootProperty3D.Enabled, "Enabled"));
                if (rootOverrides.HasFlag(PrefabRootProperty3D.Enabled))
                    overrideRow.Children.Add(CreatePrefabRootOverrideRevertButton3D(curId, PrefabRootProperty3D.Enabled, "Enabled"));
                if (rootOverrides.HasFlag(PrefabRootProperty3D.Color))
                    overrideRow.Children.Add(CreatePrefabRootOverrideApplyButton3D(curId, PrefabRootProperty3D.Color, "Color"));
                if (rootOverrides.HasFlag(PrefabRootProperty3D.Color))
                    overrideRow.Children.Add(CreatePrefabRootOverrideRevertButton3D(curId, PrefabRootProperty3D.Color, "Color"));
                banner.Children.Add(overrideRow);
            }
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

    private int[] Selected3DNodeIds()
    {
        if (Engine == IntPtr.Zero)
            return Array.Empty<int>();

        int count = Math.Clamp(
            EngineInterop.acs_editor_selected3d_count(Engine),
            0,
            Math.Max(0, EngineInterop.acs_editor_node3d_count(Engine)));
        var nativeOrder = new List<int>(count);
        for (int index = 0; index < count; ++index)
            nativeOrder.Add(
                EngineInterop.acs_editor_selected3d_at(Engine, index));
        return InspectorMultiEditContract.NormalizeSelection(
            nativeOrder,
            EngineInterop.acs_editor_selected3d(Engine));
    }

    private void Populate3DMultiInspector(int[] selected)
    {
        if (Engine == IntPtr.Zero || selected.Length < 2)
            return;

        var transforms = new List<float[]>(selected.Length);
        var kinds = new List<int>(selected.Length);
        var enabled = new List<bool>(selected.Length);
        foreach (int nodeId in selected)
        {
            var transform = new float[9];
            if (EngineInterop.acs_editor_node3d_get_transform(
                    Engine,
                    nodeId,
                    transform) == 0)
            {
                Clear3DInspector();
                InspName.Text = "Selection unavailable";
                InspSub.Text = "The scene changed while Details was refreshing.";
                Log(
                    $"Details could not capture selected 3D node {nodeId}.",
                    "Scene",
                    LogLevel.Warn);
                return;
            }

            transforms.Add(transform);
            kinds.Add(EngineInterop.acs_editor_node3d_kind(Engine, nodeId));
            enabled.Add(
                EngineInterop.acs_editor_node3d_get_enabled(
                    Engine,
                    nodeId) != 0);
        }

        InspFields.Visibility = Visibility.Collapsed;
        ActionButtons.Visibility = Visibility.Collapsed;
        Insp3DPanel.Visibility = Visibility.Visible;
        InspName.Text = $"{selected.Length} objects selected";
        InspSub.Text = "3D nodes · shared properties";

        _pop3d = true;
        try
        {
            InspectorMixedBool enabledValue =
                InspectorMultiEditContract.ResolveBool(enabled);
            InspEnabled.IsThreeState = true;
            InspEnabled.IsChecked = enabledValue.IsMixed
                ? null
                : enabledValue.Value;
            InspEnabled.Visibility = Visibility.Visible;
            InspEnabled.ToolTip = enabledValue.IsMixed
                ? "Enabled has multiple values. Click to apply one value to the selection."
                : "Apply Enabled to every selected object.";
            Insp3DPanel.Children.Clear();
            Insp3DPanel.Children.Add(new TextBlock
            {
                Text =
                    $"{InspectorMultiEditContract.MixedPlaceholder} indicates multiple values. " +
                    "Only edited axes are applied.",
                Foreground = (Brush)FindResource("TextDim"),
                FontSize = 10,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 6),
            });

            bool anyDetails = false;
            if (DetailsMatches(
                    "transform",
                    "location",
                    "position",
                    "rotation",
                    "scale",
                    "reset"))
            {
                var transformBody = new StackPanel();
                transformBody.Children.Add(MixedVectorRow(
                    "Location",
                    ResolveMixedComponents(transforms, 0, 3),
                    patch => ApplyMultiTransformPatch(
                        selected,
                        InspectorMultiEditContract.BuildSparsePatch(
                            9,
                            0,
                            patch),
                        "Edit Location",
                        "inspector.multi.transform.location")));
                transformBody.Children.Add(MixedVectorRow(
                    "Rotation",
                    ResolveMixedComponents(transforms, 3, 3),
                    patch => ApplyMultiTransformPatch(
                        selected,
                        InspectorMultiEditContract.BuildSparsePatch(
                            9,
                            3,
                            patch),
                        "Edit Rotation",
                        "inspector.multi.transform.rotation")));
                transformBody.Children.Add(MixedVectorRow(
                    "Scale",
                    ResolveMixedComponents(transforms, 6, 3),
                    patch => ApplyMultiTransformPatch(
                        selected,
                        InspectorMultiEditContract.BuildSparsePatch(
                            9,
                            6,
                            patch),
                        "Edit Scale",
                        "inspector.multi.transform.scale")));
                transformBody.Children.Add(new TextBlock
                {
                    Text =
                        "Legacy zero/near-zero scale must be repaired on that object " +
                        "with a single selection before batch Scale or Reset.",
                    Foreground = (Brush)FindResource("TextDim"),
                    FontSize = 10,
                    TextWrapping = TextWrapping.Wrap,
                    Margin = new Thickness(64, 3, 0, 1),
                });

                var reset = new Button
                {
                    Content = "↺ Reset selected transforms",
                    Padding = new Thickness(8, 3, 8, 3),
                    Margin = new Thickness(64, 6, 0, 1),
                    HorizontalAlignment = HorizontalAlignment.Left,
                    ToolTip =
                        "Reset Location and Rotation to 0 and Scale to 1 for every selected object.",
                };
                reset.Click += (_, __) =>
                {
                    float?[] resetValues =
                    {
                        0.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 0.0f,
                        1.0f, 1.0f, 1.0f,
                    };
                    if (ApplyMultiTransformPatch(
                            selected,
                            resetValues,
                            "Reset Transforms",
                            "inspector.multi.transform.reset"))
                    {
                        Populate3DInspector(selected[0]);
                    }
                };
                transformBody.Children.Add(reset);
                Insp3DPanel.Children.Add(
                    DetailsCategory("Transform", transformBody));
                anyDetails = true;
            }

            FrameworkElement? components =
                BuildMulti3DComponents(selected, kinds);
            if (components != null)
            {
                Insp3DPanel.Children.Add(
                    DetailsCategory("Components", components));
                anyDetails = true;
            }

            if (!anyDetails)
                Insp3DPanel.Children.Add(EmptyDetailsResult());
        }
        finally
        {
            _pop3d = false;
        }
    }

    private FrameworkElement? BuildMulti3DComponents(
        int[] selected,
        IReadOnlyList<int> kinds)
    {
        bool allRenderable = true;
        for (int index = 0; index < kinds.Count; ++index)
            allRenderable &= kinds[index] != 6;

        var panel = new StackPanel();
        bool shownComponent = false;
        if (allRenderable &&
            DetailsMatches(
                "component",
                "native",
                "mesh renderer",
                "mesh",
                "renderer",
                "shape",
                "type",
                "color",
                "material"))
        {
            var body = new StackPanel();
            bool sameKind = true;
            for (int index = 1; index < selected.Length; ++index)
                sameKind &= kinds[index] == kinds[0];
            body.Children.Add(LabeledValue3D(
                "Type",
                sameKind
                    ? MeshRendererTypeName(kinds[0])
                    : InspectorMultiEditContract.MixedPlaceholder));

            var colors = new List<float[]>(selected.Length);
            bool capturedColors = true;
            foreach (int nodeId in selected)
            {
                var color = new float[4];
                capturedColors &=
                    EngineInterop.acs_editor_node3d_get_color(
                        Engine,
                        nodeId,
                        color) != 0;
                colors.Add(color);
            }
            if (capturedColors &&
                DetailsMatches(
                    "component",
                    "native",
                    "mesh renderer",
                    "mesh",
                    "renderer",
                    "color",
                    "tint",
                    "opacity",
                    "alpha"))
            {
                body.Children.Add(MixedVectorRow(
                    "Color",
                    ResolveMixedComponents(colors, 0, 4),
                    patch => ApplyMultiColorPatch(
                        selected,
                        patch,
                        "Edit Mesh Renderer Color",
                        "inspector.multi.mesh-renderer.color")));
            }

            if (DetailsMatches(
                    "component",
                    "native",
                    "mesh renderer",
                    "mesh",
                    "renderer",
                    "material",
                    "shader",
                    "surface"))
            {
                string firstMaterial =
                    EngineInterop.NodeMaterial3D(Engine, selected[0]);
                bool sameMaterial = true;
                for (int index = 1; index < selected.Length; ++index)
                {
                    sameMaterial &= string.Equals(
                        firstMaterial,
                        EngineInterop.NodeMaterial3D(
                            Engine,
                            selected[index]),
                        StringComparison.OrdinalIgnoreCase);
                }

                string materialLabel = sameMaterial
                    ? string.IsNullOrWhiteSpace(firstMaterial)
                        ? "(None)"
                        : System.IO.Path.GetFileName(firstMaterial)
                    : InspectorMultiEditContract.MixedPlaceholder;
                body.Children.Add(
                    LabeledValue3D("Material", materialLabel));
            }

            // Keep the native renderer in the ordinary component stack, after Transform.
            panel.Children.Add(
                ComponentCard("Mesh Renderer", body, native: true));
            shownComponent = true;
        }

        if (TryCaptureCommonReflected3DComponents(
                selected,
                out IReadOnlyList<InspectorReflectedCommonComponent> reflected,
                out string reflectedFailure))
        {
            foreach (InspectorReflectedCommonComponent component in reflected)
            {
                if (!DetailsComponentMatches(component.TypeName))
                    continue;
                FrameworkElement? card =
                    BuildMultiReflected3DComponent(selected, component);
                if (card is null)
                    continue;
                panel.Children.Add(card);
                shownComponent = true;
            }
        }
        else
        {
            Log(
                "Details rejected reflected multi-edit schema: " +
                reflectedFailure,
                "Scene",
                LogLevel.Warn);
        }

        if (!shownComponent)
            return null;

        panel.Children.Add(new TextBlock
        {
            Text =
                "Add/remove component, material assignment, and Call In Editor " +
                "methods require a single selection.",
            Foreground = (Brush)FindResource("TextDim"),
            FontSize = 10,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(2, 5, 0, 2),
        });
        return panel;
    }

    private bool TryCaptureCommonReflected3DComponents(
        int[] selected,
        out IReadOnlyList<InspectorReflectedCommonComponent> common,
        out string failure)
    {
        var nodes =
            new List<InspectorReflectedNodeSnapshot>(selected.Length);
        foreach (int nodeId in selected)
        {
            int componentCount =
                EngineInterop.acs_editor_node3d_component_count(
                    Engine,
                    nodeId);
            if (componentCount < 0 || componentCount > 64)
            {
                common =
                    Array.Empty<InspectorReflectedCommonComponent>();
                failure =
                    $"Node {nodeId} returned an invalid reflected component count.";
                return false;
            }

            var components =
                new List<InspectorReflectedComponentSnapshot>(
                    componentCount);
            for (int slot = 0; slot < componentCount; ++slot)
            {
                string typeName =
                    EngineInterop.Component3DName(
                        Engine,
                        nodeId,
                        slot);
                if (string.IsNullOrWhiteSpace(typeName))
                {
                    common =
                        Array.Empty<InspectorReflectedCommonComponent>();
                    failure =
                        $"Node {nodeId} component slot {slot} has no type identity.";
                    return false;
                }

                int propertyCount =
                    EngineInterop.acs_editor_component_prop_count(
                        typeName);
                if (propertyCount < 0 || propertyCount > 1024)
                {
                    common =
                        Array.Empty<InspectorReflectedCommonComponent>();
                    failure =
                        $"Component {typeName} returned an invalid property count.";
                    return false;
                }

                var properties =
                    new List<InspectorReflectedPropertySchema>(
                        propertyCount);
                for (int property = 0;
                     property < propertyCount;
                     ++property)
                {
                    if (!TryReadReflectedPropertySchema(
                            typeName,
                            property,
                            out InspectorReflectedPropertySchema? schema,
                            out failure))
                    {
                        common =
                            Array.Empty<InspectorReflectedCommonComponent>();
                        return false;
                    }
                    properties.Add(schema!);
                }
                components.Add(new(
                    nodeId,
                    slot,
                    typeName,
                    properties.AsReadOnly()));
            }
            nodes.Add(new(
                nodeId,
                components.AsReadOnly()));
        }

        return InspectorReflectedMultiEditContract.TryIntersect(
            nodes,
            out common,
            out failure);
    }

    private static bool TryReadReflectedPropertySchema(
        string typeName,
        int property,
        out InspectorReflectedPropertySchema? schema,
        out string failure)
    {
        schema = null;
        failure = "";
        string name =
            EngineInterop.ComponentPropName(typeName, property);
        int kind =
            EngineInterop.acs_editor_component_prop_kind_at(
                typeName,
                property);
        int flags =
            EngineInterop.acs_editor_component_prop_flags_at(
                typeName,
                property);
        string category =
            EngineInterop.ComponentPropCategory(typeName, property);
        var defaults = new float[4];
        bool hasDefault;
        try
        {
            hasDefault =
                EngineInterop.acs_editor_component_prop_default_at(
                    typeName,
                    property,
                    out defaults[0],
                    out defaults[1],
                    out defaults[2],
                    out defaults[3]) != 0;
        }
        catch (EntryPointNotFoundException)
        {
            // Additive ABI compatibility: editing remains available against a
            // provider predating reflected defaults, but Reset is omitted.
            hasDefault = false;
            Array.Clear(defaults);
        }

        if (InspectorReflectedPropertySchema.TryCreate(
                name,
                property,
                kind,
                flags,
                category,
                hasDefault,
                defaults,
                out schema,
                out failure))
        {
            return true;
        }
        failure = $"{typeName}.{name}: {failure}";
        return false;
    }

    private FrameworkElement? BuildMultiReflected3DComponent(
        int[] selected,
        InspectorReflectedCommonComponent component)
    {
        bool showAll = DetailsMatches(
            "component",
            "script",
            component.TypeName,
            Friendly(component.TypeName));
        var body = new StackPanel();
        string? lastCategory = null;
        int shownProperties = 0;
        foreach (InspectorReflectedCommonProperty property in
                 component.Properties)
        {
            InspectorReflectedPropertySchema schema = property.Schema;
            if (schema.IsHidden ||
                (!showAll &&
                 !DetailsMatches(schema.Name, schema.Category)))
            {
                continue;
            }

            if (!TryReadMultiReflectedPropertyValues(
                    component.TypeName,
                    property,
                    out IReadOnlyList<IReadOnlyList<float>> values,
                    out string failure))
            {
                Log(
                    "Details could not capture reflected property values: " +
                    failure,
                    "Scene",
                    LogLevel.Warn);
                return null;
            }

            if (!string.IsNullOrEmpty(schema.Category) &&
                !string.Equals(
                    schema.Category,
                    lastCategory,
                    StringComparison.Ordinal))
            {
                body.Children.Add(new TextBlock
                {
                    Text = schema.Category,
                    Foreground = (Brush)FindResource("TextDim"),
                    FontSize = 10,
                    FontWeight = FontWeights.SemiBold,
                    Margin = new Thickness(
                        0,
                        shownProperties == 0 ? 0 : 5,
                        0,
                        1),
                });
            }
            lastCategory = schema.Category;

            FrameworkElement? row =
                BuildMultiReflectedPropertyRow(
                    selected,
                    component,
                    property,
                    values);
            if (row is null)
                continue;
            body.Children.Add(row);
            shownProperties++;
        }

        if (shownProperties == 0)
        {
            body.Children.Add(new TextBlock
            {
                Text = _detailsFilter.Length == 0
                    ? "No common visible properties."
                    : "No common properties match this filter.",
                Foreground = (Brush)FindResource("TextDim"),
                FontSize = 11,
                Margin = new Thickness(0, 1, 0, 1),
            });
        }
        return ComponentCard(
            component.TypeName,
            body,
            native: false);
    }

    private bool TryReadMultiReflectedPropertyValues(
        string typeName,
        InspectorReflectedCommonProperty property,
        out IReadOnlyList<IReadOnlyList<float>> values,
        out string failure)
    {
        var captured =
            new List<IReadOnlyList<float>>(property.Targets.Count);
        foreach (InspectorReflectedPropertyTarget target in
                 property.Targets)
        {
            if (!string.Equals(
                    EngineInterop.Component3DName(
                        Engine,
                        target.NodeId,
                        target.Slot),
                    typeName,
                    StringComparison.Ordinal))
            {
                values = Array.Empty<IReadOnlyList<float>>();
                failure =
                    $"node {target.NodeId} component topology changed.";
                return false;
            }

            var value = new float[4];
            if (EngineInterop.acs_editor_node3d_component_prop_get(
                    Engine,
                    target.NodeId,
                    target.Slot,
                    target.PropertyIndex,
                    out value[0],
                    out value[1],
                    out value[2],
                    out value[3]) == 0 ||
                value.Any(static component =>
                    !float.IsFinite(component)))
            {
                values = Array.Empty<IReadOnlyList<float>>();
                failure =
                    $"node {target.NodeId} property read failed or was non-finite.";
                return false;
            }
            captured.Add(value);
        }

        values = captured.AsReadOnly();
        failure = "";
        return true;
    }

    private FrameworkElement? BuildMultiReflectedPropertyRow(
        int[] selected,
        InspectorReflectedCommonComponent component,
        InspectorReflectedCommonProperty property,
        IReadOnlyList<IReadOnlyList<float>> values)
    {
        InspectorReflectedPropertySchema schema = property.Schema;
        if (schema.IsHidden)
            return null;

        if (schema.Kind == InspectorReflectedPropertyKind.String)
        {
            FrameworkElement stringRow =
                LabeledValue3D(
                    schema.Name,
                    InspectorMultiEditContract.MixedPlaceholder +
                    " (string editing requires one selection)");
            stringRow.IsEnabled = false;
            return stringRow;
        }

        FrameworkElement editor;
        if (schema.Kind == InspectorReflectedPropertyKind.Bool)
        {
            InspectorMixedBool mixed =
                InspectorReflectedMultiEditContract.ResolveMixedBool(
                    values);
            var row = new DockPanel
            {
                Margin = new Thickness(0, 3, 0, 1),
            };
            var label = new TextBlock
            {
                Text = schema.Name,
                Width = 78,
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = (Brush)FindResource("TextDim"),
                FontSize = 11,
                FontFamily =
                    new System.Windows.Media.FontFamily("Consolas"),
            };
            DockPanel.SetDock(label, Dock.Left);
            row.Children.Add(label);
            var check = new CheckBox
            {
                IsThreeState = true,
                IsChecked = mixed.IsMixed ? null : mixed.Value,
                VerticalAlignment = VerticalAlignment.Center,
                ToolTip = mixed.IsMixed
                    ? "Multiple values. Click to apply one value to every target."
                    : "Apply this value to every selected target.",
            };
            check.Checked += (_, __) =>
            {
                if (_pop3d) return;
                _ = ApplyMultiReflectedPropertyPatch(
                    selected,
                    component.TypeName,
                    property,
                    new float?[] { 1.0f, null, null, null },
                    $"Edit {Friendly(component.TypeName)}.{schema.Name}");
            };
            check.Unchecked += (_, __) =>
            {
                if (_pop3d) return;
                _ = ApplyMultiReflectedPropertyPatch(
                    selected,
                    component.TypeName,
                    property,
                    new float?[] { 0.0f, null, null, null },
                    $"Edit {Friendly(component.TypeName)}.{schema.Name}");
            };
            row.Children.Add(check);
            editor = row;
        }
        else if (schema.Kind ==
                 InspectorReflectedPropertyKind.ObjectRef)
        {
            editor = BuildMultiReflectedObjectReferenceRow(
                selected,
                component,
                property,
                values);
        }
        else if ((schema.Kind is
                      InspectorReflectedPropertyKind.Enum or
                      InspectorReflectedPropertyKind.I32 or
                      InspectorReflectedPropertyKind.U32) &&
                 TryGetKnownReflectedChoices(
                     schema.Name,
                     out IReadOnlyList<string> choices))
        {
            editor = BuildMultiReflectedChoiceRow(
                selected,
                component,
                property,
                values,
                choices);
        }
        else
        {
            InspectorMixedFloat[] mixed =
                InspectorReflectedMultiEditContract
                    .ResolveMixedComponents(
                        schema,
                        values);
            editor = MixedVectorRow(
                schema.Name,
                mixed,
                sparsePatch =>
                {
                    var fullPatch = new float?[4];
                    for (int index = 0;
                         index < sparsePatch.Length;
                         ++index)
                    {
                        fullPatch[index] = sparsePatch[index];
                    }
                    return ApplyMultiReflectedPropertyPatch(
                        selected,
                        component.TypeName,
                        property,
                        fullPatch,
                        $"Edit {Friendly(component.TypeName)}.{schema.Name}");
                });
        }

        if (!schema.IsEditable)
            editor.IsEnabled = false;
        return WrapMultiReflectedReset(
            selected,
            component.TypeName,
            property,
            editor);
    }

    private FrameworkElement BuildMultiReflectedObjectReferenceRow(
        int[] selected,
        InspectorReflectedCommonComponent component,
        InspectorReflectedCommonProperty property,
        IReadOnlyList<IReadOnlyList<float>> values)
    {
        InspectorReflectedPropertySchema schema = property.Schema;
        InspectorMixedFloat mixed =
            InspectorReflectedMultiEditContract
                .ResolveMixedComponents(schema, values)[0];
        var row = new DockPanel
        {
            Margin = new Thickness(0, 3, 0, 1),
        };
        var label = new TextBlock
        {
            Text = schema.Name,
            Width = 78,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim"),
            FontSize = 11,
            FontFamily =
                new System.Windows.Media.FontFamily("Consolas"),
        };
        DockPanel.SetDock(label, Dock.Left);
        row.Children.Add(label);

        var combo = new ComboBox
        {
            MinWidth = 150,
            FontSize = 11,
            VerticalAlignment = VerticalAlignment.Center,
        };
        int initialIndex = -1;
        if (mixed.IsMixed)
        {
            combo.Items.Add(new ComboBoxItem
            {
                Content = InspectorMultiEditContract.MixedPlaceholder +
                          " Multiple Values",
                Tag = null,
            });
            initialIndex = 0;
        }
        combo.Items.Add(new ComboBoxItem
        {
            Content = "(None)",
            Tag = -1,
        });
        int noneIndex = combo.Items.Count - 1;
        var unsafeTargets = new HashSet<int>(selected);
        int nodeCount =
            Math.Clamp(
                EngineInterop.acs_editor_node3d_count(Engine),
                0,
                100_000);
        for (int index = 0; index < nodeCount; ++index)
        {
            int nodeId =
                EngineInterop.acs_editor_node3d_id_at(Engine, index);
            if (nodeId < 0 || unsafeTargets.Contains(nodeId))
                continue;
            combo.Items.Add(new ComboBoxItem
            {
                Content =
                    $"{Node3DName(nodeId)} (id {nodeId})",
                Tag = nodeId,
            });
        }

        if (!mixed.IsMixed)
        {
            bool hasCurrent =
                TryReadReflectedInteger(
                    mixed.Value,
                    out int current);
            if (hasCurrent && current == -1)
            {
                initialIndex = noneIndex;
            }
            else if (hasCurrent)
            {
                for (int index = 0;
                     index < combo.Items.Count;
                     ++index)
                {
                    if (combo.Items[index] is ComboBoxItem item &&
                        item.Tag is int candidate &&
                        candidate == current)
                    {
                        initialIndex = index;
                        break;
                    }
                }
                if (initialIndex < 0)
                {
                    combo.Items.Insert(0, new ComboBoxItem
                    {
                        Content =
                            $"Current id {current} (not batch-safe)",
                        Tag = null,
                        IsEnabled = false,
                    });
                    initialIndex = 0;
                }
            }
            else
            {
                combo.Items.Insert(0, new ComboBoxItem
                {
                    Content =
                        "Current reference is invalid (not batch-safe)",
                    Tag = null,
                    IsEnabled = false,
                });
                initialIndex = 0;
            }
        }
        combo.SelectedIndex = initialIndex;
        combo.SelectionChanged += (_, __) =>
        {
            if (_pop3d ||
                combo.SelectedItem is not ComboBoxItem item ||
                item.Tag is not int desired)
            {
                return;
            }
            _ = ApplyMultiReflectedPropertyPatch(
                selected,
                component.TypeName,
                property,
                new float?[] { desired, null, null, null },
                $"Edit {Friendly(component.TypeName)}.{schema.Name}");
        };
        row.Children.Add(combo);
        return row;
    }

    private FrameworkElement BuildMultiReflectedChoiceRow(
        int[] selected,
        InspectorReflectedCommonComponent component,
        InspectorReflectedCommonProperty property,
        IReadOnlyList<IReadOnlyList<float>> values,
        IReadOnlyList<string> choices)
    {
        InspectorReflectedPropertySchema schema = property.Schema;
        InspectorMixedFloat mixed =
            InspectorReflectedMultiEditContract
                .ResolveMixedComponents(schema, values)[0];
        var row = new DockPanel
        {
            Margin = new Thickness(0, 3, 0, 1),
        };
        var label = new TextBlock
        {
            Text = schema.Name,
            Width = 78,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim"),
            FontSize = 11,
            FontFamily =
                new System.Windows.Media.FontFamily("Consolas"),
        };
        DockPanel.SetDock(label, Dock.Left);
        row.Children.Add(label);

        var combo = new ComboBox
        {
            MinWidth = 130,
            FontSize = 11,
            VerticalAlignment = VerticalAlignment.Center,
        };
        int offset = 0;
        if (mixed.IsMixed)
        {
            combo.Items.Add(
                InspectorMultiEditContract.MixedPlaceholder +
                " Multiple Values");
            offset = 1;
        }
        foreach (string choice in choices)
            combo.Items.Add(choice);
        int selectedChoice =
            !mixed.IsMixed &&
            TryReadReflectedInteger(mixed.Value, out int value)
                ? value
                : -1;
        combo.SelectedIndex =
            mixed.IsMixed
                ? 0
                : selectedChoice >= 0 &&
                  selectedChoice < choices.Count
                    ? selectedChoice + offset
                    : -1;
        combo.SelectionChanged += (_, __) =>
        {
            if (_pop3d)
                return;
            int choice = combo.SelectedIndex - offset;
            if (choice < 0 || choice >= choices.Count)
                return;
            _ = ApplyMultiReflectedPropertyPatch(
                selected,
                component.TypeName,
                property,
                new float?[] { choice, null, null, null },
                $"Edit {Friendly(component.TypeName)}.{schema.Name}");
        };
        row.Children.Add(combo);
        return row;
    }

    private static bool TryGetKnownReflectedChoices(
        string propertyName,
        out IReadOnlyList<string> choices)
    {
        choices = propertyName switch
        {
            "bodyType" => new[] { "Static", "Dynamic" },
            "shape" =>
                new[] { "Box", "Circle", "Triangle", "Polygon" },
            _ => Array.Empty<string>(),
        };
        return choices.Count != 0;
    }

    private static bool TryReadReflectedInteger(
        float value,
        out int result)
    {
        result = 0;
        if (!float.IsFinite(value) ||
            value < -16_777_216f ||
            value > 16_777_216f ||
            value != MathF.Truncate(value))
        {
            return false;
        }
        result = (int)value;
        return true;
    }

    private FrameworkElement WrapMultiReflectedReset(
        int[] selected,
        string typeName,
        InspectorReflectedCommonProperty property,
        FrameworkElement editor)
    {
        if (!property.Schema.CanReset)
            return editor;

        var root = new StackPanel();
        root.Children.Add(editor);
        var reset = new Button
        {
            Content = "↺ Reset to Default",
            Padding = new Thickness(7, 2, 7, 2),
            Margin = new Thickness(78, 2, 0, 2),
            HorizontalAlignment = HorizontalAlignment.Left,
            ToolTip =
                "Apply the reflected schema default to every selected target.",
        };
        reset.Click += (_, __) =>
        {
            if (!InspectorReflectedMultiEditContract
                    .TryBuildDefaultPatch(
                        property.Schema,
                        out float?[] patch))
            {
                return;
            }
            _ = ApplyMultiReflectedPropertyPatch(
                selected,
                typeName,
                property,
                patch,
                $"Reset {Friendly(typeName)}.{property.Schema.Name}");
        };
        root.Children.Add(reset);
        return root;
    }

    private static string MeshRendererTypeName(int kind) => kind switch
    {
        0 => "Cube",
        1 => "Sphere",
        2 => "Plane",
        3 => "Static Mesh",
        4 => "Sprite",
        5 => "Polygon",
        _ => "Unknown",
    };

    private static InspectorMixedFloat[] ResolveMixedComponents(
        IReadOnlyList<float[]> values,
        int offset,
        int count)
    {
        var result = new InspectorMixedFloat[count];
        var component = new float[values.Count];
        for (int componentIndex = 0;
             componentIndex < count;
             ++componentIndex)
        {
            for (int valueIndex = 0;
                 valueIndex < values.Count;
                 ++valueIndex)
            {
                component[valueIndex] =
                    values[valueIndex][offset + componentIndex];
            }

            result[componentIndex] =
                InspectorMultiEditContract.ResolveFloat(component);
        }

        return result;
    }

    private FrameworkElement MixedVectorRow(
        string label,
        IReadOnlyList<InspectorMixedFloat> initialValues,
        Func<float?[], bool> onChanged)
    {
        var grid = new Grid { Margin = new Thickness(0, 3, 0, 3) };
        grid.ColumnDefinitions.Add(
            new ColumnDefinition { Width = new GridLength(64) });
        for (int index = 0; index < initialValues.Count; ++index)
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition
                {
                    Width =
                        new GridLength(1, GridUnitType.Star),
                });
        }

        var rowLabel = new TextBlock
        {
            Text = label,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim"),
        };
        Grid.SetColumn(rowLabel, 0);
        grid.Children.Add(rowLabel);

        var values = new InspectorMixedFloat[initialValues.Count];
        var boxes = new TextBox[initialValues.Count];
        var touched = new bool[initialValues.Count];
        var suppressTextChange = new bool[initialValues.Count];
        for (int index = 0; index < initialValues.Count; ++index)
        {
            int componentIndex = index;
            values[index] = initialValues[index];
            var box = new TextBox
            {
                Text =
                    InspectorMultiEditContract.DisplayText(
                        values[index]),
                Margin = new Thickness(index == 0 ? 0 : 3, 0, 0, 0),
                Style = (Style)FindResource("NumBox"),
                ToolTip = values[index].IsMixed
                    ? "Multiple values. Type to apply this axis to the selection."
                    : null,
            };
            box.GotKeyboardFocus += (_, __) =>
            {
                if (!values[componentIndex].IsMixed ||
                    touched[componentIndex] ||
                    box.Text != InspectorMultiEditContract.MixedPlaceholder)
                {
                    return;
                }

                suppressTextChange[componentIndex] = true;
                box.Clear();
                suppressTextChange[componentIndex] = false;
            };
            box.TextChanged += (_, __) =>
            {
                if (!suppressTextChange[componentIndex] &&
                    box.IsKeyboardFocusWithin)
                {
                    touched[componentIndex] = true;
                    box.ClearValue(Control.BorderBrushProperty);
                    box.ToolTip = null;
                }
            };
            Grid.SetColumn(box, index + 1);
            grid.Children.Add(box);
            boxes[index] = box;
        }

        bool Apply()
        {
            var patch = new float?[boxes.Length];
            bool hasPatch = false;
            for (int index = 0; index < boxes.Length; ++index)
            {
                if (!touched[index])
                    continue;
                if (!float.TryParse(
                        boxes[index].Text,
                        NumberStyles.Float,
                        CultureInfo.InvariantCulture,
                        out float parsed) ||
                    !float.IsFinite(parsed))
                {
                    boxes[index].BorderBrush = Brushes.IndianRed;
                    boxes[index].ToolTip =
                        "Enter a finite numeric value.";
                    return false;
                }

                patch[index] = parsed;
                hasPatch = true;
            }

            if (!hasPatch)
            {
                for (int index = 0; index < boxes.Length; ++index)
                {
                    if (values[index].IsMixed &&
                        string.IsNullOrWhiteSpace(boxes[index].Text))
                    {
                        suppressTextChange[index] = true;
                        boxes[index].Text =
                            InspectorMultiEditContract.MixedPlaceholder;
                        suppressTextChange[index] = false;
                    }
                }
                return true;
            }

            if (!onChanged(patch))
            {
                for (int index = 0; index < boxes.Length; ++index)
                {
                    suppressTextChange[index] = true;
                    boxes[index].Text =
                        InspectorMultiEditContract.DisplayText(
                            values[index]);
                    suppressTextChange[index] = false;
                    touched[index] = false;
                    boxes[index].ClearValue(
                        Control.BorderBrushProperty);
                    boxes[index].ToolTip = values[index].IsMixed
                        ? "Multiple values. Type to apply this axis to the selection."
                        : null;
                }
                return false;
            }

            for (int index = 0; index < boxes.Length; ++index)
            {
                if (patch[index].HasValue)
                {
                    values[index] = new InspectorMixedFloat(
                        true,
                        false,
                        patch[index]!.Value);
                }

                suppressTextChange[index] = true;
                boxes[index].Text =
                    InspectorMultiEditContract.DisplayText(
                        values[index]);
                suppressTextChange[index] = false;
                touched[index] = false;
                boxes[index].ClearValue(Control.BorderBrushProperty);
                boxes[index].ToolTip = values[index].IsMixed
                    ? "Multiple values. Type to apply this axis to the selection."
                    : null;
            }

            return true;
        }

        foreach (TextBox box in boxes)
        {
            box.LostKeyboardFocus += (_, __) => Apply();
            box.KeyDown += (_, ev) =>
            {
                if (ev.Key == Key.Enter && Apply())
                    Keyboard.ClearFocus();
            };
        }

        return grid;
    }

    private sealed class MultiArrayMutation
    {
        internal MultiArrayMutation(float[] before, float[] after)
        {
            Before = before;
            After = after;
        }

        internal float[] Before { get; }
        internal float[] After { get; }
    }

    private sealed class MultiTransformMutation
    {
        internal MultiTransformMutation(
            float[] before,
            float[] after,
            uint componentMask)
        {
            Before = before;
            After = after;
            ComponentMask = componentMask;
        }

        internal float[] Before { get; }
        internal float[] After { get; }
        internal uint ComponentMask { get; }
    }

    private readonly record struct MultiEnabledMutation(bool Before);

    private bool ApplyMultiTransformPatch(
        int[] selected,
        IReadOnlyList<float?> patch,
        string label,
        string propertyIdentity)
    {
        if (patch.Count != 9 ||
            !ValidateFinitePatch(patch) ||
            !InspectorMultiEditContract.SameSelection(
                selected,
                Selected3DNodeIds()))
        {
            Log(
                $"{label} was cancelled because the selection or value changed.",
                "Scene",
                LogLevel.Warn);
            return false;
        }

        string selectionIdentity =
            InspectorMultiEditContract.SelectionIdentity(selected);
        using IDisposable transaction = BeginSceneDocumentTransaction(
            label,
            propertyIdentity + "." + selectionIdentity,
            TimeSpan.Zero,
            selected[0]);
        int nonRestorableScaleNode = -1;
        InspectorAtomicBatchResult result =
            InspectorMultiEditContract.ApplyAtomically(
                selected,
                (int nodeId, out MultiTransformMutation mutation) =>
                {
                    var before = new float[9];
                    if (EngineInterop.acs_editor_node3d_get_transform(
                            Engine,
                            nodeId,
                            before) == 0)
                    {
                        mutation = null!;
                        return false;
                    }

                    if (!InspectorMultiEditContract.TryBuildTransformMutation(
                            before,
                            patch,
                            out float[] after,
                            out uint componentMask))
                    {
                        if (InspectorMultiEditContract
                                .ContainsNonRestorablePatchedScale(
                                    before,
                                    patch))
                        {
                            nonRestorableScaleNode = nodeId;
                        }
                        mutation = null!;
                        return false;
                    }

                    mutation = new MultiTransformMutation(
                        before,
                        after,
                        componentMask);
                    return true;
                },
                (nodeId, mutation) =>
                    WriteAndVerifyTransformMasked(
                        nodeId,
                        mutation.After,
                        mutation.ComponentMask),
                (nodeId, mutation) =>
                    WriteAndVerifyTransformMasked(
                        nodeId,
                        mutation.Before,
                        mutation.ComponentMask));
        string? failureDetail = nonRestorableScaleNode >= 0
            ? $"No writes were made. Node {nonRestorableScaleNode} has a " +
              "legacy zero/near-zero value on an edited scale axis; select " +
              "that object alone, enter a non-zero scale, then retry."
            : null;
        return ReportMultiBatchResult(
            label,
            selected.Length,
            result,
            failureDetail);
    }

    private bool ApplyMultiReflectedPropertyPatch(
        int[] selected,
        string typeName,
        InspectorReflectedCommonProperty property,
        IReadOnlyList<float?> patch,
        string label)
    {
        if (patch.Count != 4 ||
            !property.Schema.IsEditable ||
            !patch.Any(static value => value.HasValue) ||
            patch.Any(static value =>
                value.HasValue &&
                !float.IsFinite(value.Value)) ||
            !InspectorMultiEditContract.SameSelection(
                selected,
                Selected3DNodeIds()) ||
            !InspectorMultiEditContract.SameSelection(
                selected,
                property.Targets.Select(
                        static target => target.NodeId)
                    .ToArray()))
        {
            Log(
                $"{label} was cancelled because its selection, schema, or value changed.",
                "Scene",
                LogLevel.Warn);
            return false;
        }

        Dictionary<int, InspectorReflectedPropertyTarget> targets;
        try
        {
            targets = property.Targets.ToDictionary(
                static target => target.NodeId);
        }
        catch (ArgumentException)
        {
            Log(
                $"{label} was cancelled because reflected targets are ambiguous.",
                "Scene",
                LogLevel.Warn);
            return false;
        }

        string selectionIdentity =
            InspectorMultiEditContract.SelectionIdentity(selected);
        using IDisposable transaction = BeginSceneDocumentTransaction(
            label,
            $"inspector.multi.component.{typeName}." +
            $"{property.Schema.Name}.{selectionIdentity}",
            TimeSpan.Zero,
            selected[0]);
        InspectorAtomicBatchResult result =
            InspectorMultiEditContract.ApplyAtomically(
                selected,
                (int nodeId, out MultiArrayMutation mutation) =>
                {
                    if (!targets.TryGetValue(
                            nodeId,
                            out InspectorReflectedPropertyTarget target) ||
                        !string.Equals(
                            EngineInterop.Component3DName(
                                Engine,
                                nodeId,
                                target.Slot),
                            typeName,
                            StringComparison.Ordinal) ||
                        !TryReadReflectedPropertySchema(
                            typeName,
                            target.PropertyIndex,
                            out InspectorReflectedPropertySchema? currentSchema,
                            out _) ||
                        currentSchema is null ||
                        !property.Schema.IsCompatibleWith(currentSchema))
                    {
                        mutation = null!;
                        return false;
                    }

                    var before = new float[4];
                    if (EngineInterop
                            .acs_editor_node3d_component_prop_get(
                                Engine,
                                nodeId,
                                target.Slot,
                                target.PropertyIndex,
                                out before[0],
                                out before[1],
                                out before[2],
                                out before[3]) == 0 ||
                        before.Any(static value =>
                            !float.IsFinite(value)) ||
                        !InspectorReflectedMultiEditContract
                            .TryBuildMutation(
                                property.Schema,
                                before,
                                patch,
                                out float[] after))
                    {
                        mutation = null!;
                        return false;
                    }

                    mutation = new MultiArrayMutation(before, after);
                    return true;
                },
                (nodeId, mutation) =>
                    WriteAndVerifyReflectedProperty(
                        typeName,
                        targets[nodeId],
                        mutation.After),
                (nodeId, mutation) =>
                    WriteAndVerifyReflectedProperty(
                        typeName,
                        targets[nodeId],
                        mutation.Before));

        bool succeeded =
            ReportMultiBatchResult(
                label,
                selected.Length,
                result);
        if (succeeded)
        {
            foreach (InspectorReflectedPropertyTarget target in
                     property.Targets)
            {
                MarkPrefabRootComponentPropertyOverride3D(
                    target.NodeId,
                    target.Slot,
                    target.PropertyIndex);
            }
        }
        if (succeeded &&
            InspectorMultiEditContract.SameSelection(
                selected,
                Selected3DNodeIds()))
        {
            Populate3DInspector(selected[0]);
        }
        return succeeded;
    }

    private bool WriteAndVerifyReflectedProperty(
        string typeName,
        InspectorReflectedPropertyTarget target,
        IReadOnlyList<float> value)
    {
        if (value.Count != 4 ||
            value.Any(static component =>
                !float.IsFinite(component)) ||
            !string.Equals(
                EngineInterop.Component3DName(
                    Engine,
                    target.NodeId,
                    target.Slot),
                typeName,
                StringComparison.Ordinal) ||
            EngineInterop.acs_editor_node3d_component_prop_set(
                Engine,
                target.NodeId,
                target.Slot,
                target.PropertyIndex,
                value[0],
                value[1],
                value[2],
                value[3]) == 0)
        {
            return false;
        }

        var current = new float[4];
        return EngineInterop.acs_editor_node3d_component_prop_get(
                   Engine,
                   target.NodeId,
                   target.Slot,
                   target.PropertyIndex,
                   out current[0],
                   out current[1],
                   out current[2],
                   out current[3]) != 0 &&
               InspectorReflectedMultiEditContract.ValuesEqual(
                   current,
                   value);
    }

    private bool ApplyMultiColorPatch(
        int[] selected,
        IReadOnlyList<float?> patch,
        string label,
        string propertyIdentity)
    {
        if (patch.Count != 4 ||
            !ValidateFinitePatch(patch) ||
            !InspectorMultiEditContract.SameSelection(
                selected,
                Selected3DNodeIds()))
        {
            Log(
                $"{label} was cancelled because the selection or value changed.",
                "Scene",
                LogLevel.Warn);
            return false;
        }

        string selectionIdentity =
            InspectorMultiEditContract.SelectionIdentity(selected);
        using IDisposable transaction = BeginSceneDocumentTransaction(
            label,
            propertyIdentity + "." + selectionIdentity,
            TimeSpan.Zero,
            selected[0]);
        InspectorAtomicBatchResult result =
            InspectorMultiEditContract.ApplyAtomically(
                selected,
                (int nodeId, out MultiArrayMutation mutation) =>
                {
                    var before = new float[4];
                    if (EngineInterop.acs_editor_node3d_get_color(
                            Engine,
                            nodeId,
                            before) == 0 ||
                        !AllFinite(before))
                    {
                        mutation = null!;
                        return false;
                    }

                    var after = (float[])before.Clone();
                    for (int index = 0; index < patch.Count; ++index)
                    {
                        if (patch[index].HasValue)
                            after[index] = patch[index]!.Value;
                    }
                    mutation = new MultiArrayMutation(before, after);
                    return true;
                },
                (nodeId, mutation) =>
                    WriteAndVerifyColor(nodeId, mutation.After),
                (nodeId, mutation) =>
                    WriteAndVerifyColor(nodeId, mutation.Before));
        bool succeeded = ReportMultiBatchResult(label, selected.Length, result);
        if (succeeded)
        {
            foreach (int nodeId in selected)
                MarkPrefabRootOverride3D(nodeId, PrefabRootProperty3D.Color);
        }
        return succeeded;
    }

    private bool ApplyMultiEnabled(int[] selected, bool desired)
    {
        if (!InspectorMultiEditContract.SameSelection(
                selected,
                Selected3DNodeIds()))
        {
            Log(
                "Enabled batch was cancelled because the selection changed.",
                "Scene",
                LogLevel.Warn);
            return false;
        }

        string selectionIdentity =
            InspectorMultiEditContract.SelectionIdentity(selected);
        using IDisposable transaction = BeginSceneDocumentTransaction(
            "Edit Enabled",
            "inspector.multi.enabled." + selectionIdentity,
            TimeSpan.Zero,
            selected[0]);
        InspectorAtomicBatchResult result =
            InspectorMultiEditContract.ApplyAtomically(
                selected,
                (int nodeId, out MultiEnabledMutation mutation) =>
                {
                    var transform = new float[9];
                    if (EngineInterop.acs_editor_node3d_get_transform(
                            Engine,
                            nodeId,
                            transform) == 0)
                    {
                        mutation = default;
                        return false;
                    }

                    mutation = new MultiEnabledMutation(
                        EngineInterop.acs_editor_node3d_get_enabled(
                            Engine,
                            nodeId) != 0);
                    return true;
                },
                (nodeId, mutation) =>
                    mutation.Before == desired ||
                    WriteAndVerifyEnabled(nodeId, desired),
                (nodeId, mutation) =>
                    WriteAndVerifyEnabled(nodeId, mutation.Before));
        bool succeeded = ReportMultiBatchResult(
            "Edit Enabled",
            selected.Length,
            result);
        if (succeeded)
        {
            foreach (int nodeId in selected)
                MarkPrefabRootOverride3D(nodeId, PrefabRootProperty3D.Enabled);
        }
        return succeeded;
    }

    private bool WriteAndVerifyTransformMasked(
        int nodeId,
        float[] transform,
        uint componentMask)
    {
        if (EngineInterop.acs_editor_node3d_set_transform_masked(
                Engine,
                nodeId,
                componentMask,
                transform,
                (uint)transform.Length) == 0)
        {
            return false;
        }

        var current = new float[9];
        return EngineInterop.acs_editor_node3d_get_transform(
                   Engine,
                   nodeId,
                   current) != 0 &&
               TransformArraysEquivalent(
                   current,
                   transform,
                   componentMask);
    }

    private bool WriteAndVerifyColor(int nodeId, float[] color)
    {
        if (EngineInterop.acs_editor_node3d_set_color(
                Engine,
                nodeId,
                color[0],
                color[1],
                color[2],
                color[3]) == 0)
        {
            return false;
        }

        var current = new float[4];
        return EngineInterop.acs_editor_node3d_get_color(
                   Engine,
                   nodeId,
                   current) != 0 &&
               FloatArraysEquivalent(current, color);
    }

    private bool WriteAndVerifyEnabled(int nodeId, bool enabled)
    {
        EngineInterop.acs_editor_node3d_set_enabled(
            Engine,
            nodeId,
            enabled ? 1 : 0);
        var transform = new float[9];
        return EngineInterop.acs_editor_node3d_get_transform(
                   Engine,
                   nodeId,
                   transform) != 0 &&
               (EngineInterop.acs_editor_node3d_get_enabled(
                    Engine,
                    nodeId) != 0) == enabled;
    }

    private bool ReportMultiBatchResult(
        string label,
        int targetCount,
        InspectorAtomicBatchResult result,
        string? failureDetail = null)
    {
        if (result.Succeeded)
        {
            Log(
                $"{label}: applied to {targetCount} selected objects.",
                "Scene",
                LogLevel.Info);
            return true;
        }

        LogLevel level = result.RollbackSucceeded
            ? LogLevel.Warn
            : LogLevel.Error;
        string rollback = failureDetail ??
            (result.FailureStage == InspectorAtomicFailureStage.Capture
                ? "No writes were made because the target state could not be captured safely."
                : result.RollbackSucceeded
                    ? "All prior writes were rolled back."
                    : "Rollback could not restore every target; reload or Undo immediately.");
        Log(
            $"{label} failed at node {result.FailedNodeId} " +
            $"({result.FailureStage}). {rollback}",
            "Scene",
            level);
        return false;
    }

    private static bool ValidateFinitePatch(
        IReadOnlyList<float?> patch)
    {
        bool hasValue = false;
        for (int index = 0; index < patch.Count; ++index)
        {
            if (!patch[index].HasValue)
                continue;
            if (!float.IsFinite(patch[index]!.Value))
                return false;
            hasValue = true;
        }
        return hasValue;
    }

    private static bool FloatArraysEquivalent(
        IReadOnlyList<float> left,
        IReadOnlyList<float> right)
    {
        if (left.Count != right.Count)
            return false;
        for (int index = 0; index < left.Count; ++index)
        {
            if (!float.IsFinite(left[index]) ||
                !float.IsFinite(right[index]))
            {
                return false;
            }

            float scale = MathF.Max(
                1.0f,
                MathF.Max(
                    MathF.Abs(left[index]),
                    MathF.Abs(right[index])));
            if (MathF.Abs(left[index] - right[index]) >
                1.0e-5f * scale)
            {
                return false;
            }
        }
        return true;
    }

    private static bool AllFinite(IReadOnlyList<float> values)
    {
        for (int index = 0; index < values.Count; ++index)
        {
            if (!float.IsFinite(values[index]))
                return false;
        }
        return true;
    }

    private static bool TransformArraysEquivalent(
        IReadOnlyList<float> current,
        IReadOnlyList<float> expected,
        uint componentMask)
    {
        if (current.Count != InspectorMultiEditContract.TransformComponentCount ||
            expected.Count != InspectorMultiEditContract.TransformComponentCount)
        {
            return false;
        }

        for (int index = 0;
             index < InspectorMultiEditContract.TransformComponentCount;
             ++index)
        {
            if (!float.IsFinite(current[index]) ||
                !float.IsFinite(expected[index]))
            {
                return false;
            }

            if ((componentMask & (1u << index)) == 0u)
            {
                if (BitConverter.SingleToInt32Bits(current[index]) !=
                    BitConverter.SingleToInt32Bits(expected[index]))
                {
                    return false;
                }
                continue;
            }

            float scale = MathF.Max(
                1.0f,
                MathF.Max(
                    MathF.Abs(current[index]),
                    MathF.Abs(expected[index])));
            if (MathF.Abs(current[index] - expected[index]) >
                1.0e-5f * scale)
            {
                return false;
            }
        }

        return true;
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

        if (TryGetCameraAuthoringState(id, out CameraAuthoringState camera) &&
            DetailsMatches(
                "component", "native", "camera", "projection", "field of view",
                "fov", "orthographic", "near clip", "far clip", "priority",
                "active", "enabled"))
        {
            panel.Children.Add(BuildCameraComponent(camera));
            shownComponents++;
        }

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
            uint componentOverrideMask = EngineInterop
                .acs_editor_prefab_instance3d_root_component_property_override_mask(
                    Engine,
                    id,
                    idx);
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
                    if (prow == null) continue;   // null = Hidden 指定子 → 出さない
                    if ((componentOverrideMask & (1u << p)) == 0u)
                    {
                        inner.Children.Add(prow);
                        continue;
                    }
                    var overridePropertyRow = new Grid();
                    overridePropertyRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    overridePropertyRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    overridePropertyRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    Grid.SetColumn(prow, 0);
                    overridePropertyRow.Children.Add(prow);
                    Button applyButton = CreatePrefabRootComponentPropertyOverrideApplyButton3D(
                        id,
                        idx,
                        p,
                        cname,
                        EngineInterop.ComponentPropName(cname, p));
                    Grid.SetColumn(applyButton, 1);
                    overridePropertyRow.Children.Add(applyButton);
                    Button revertButton = CreatePrefabRootComponentPropertyOverrideRevertButton3D(
                        id,
                        idx,
                        p,
                        EngineInterop.ComponentPropName(cname, p));
                    Grid.SetColumn(revertButton, 2);
                    overridePropertyRow.Children.Add(revertButton);
                    inner.Children.Add(overridePropertyRow);
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
        bool hasCamera = TryGetCameraAuthoringState(id, out _);
        if (!hasCamera) combo.Items.Add(CameraComponentDisplayName);
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
            if (combo.SelectedItem is not string tn) return;
            if (string.Equals(
                    tn,
                    CameraComponentDisplayName,
                    StringComparison.Ordinal))
            {
                AttachCameraToNode(id);
                return;
            }
            if (EngineInterop.acs_editor_node3d_add_component(Engine, id, tn) != 0)
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
                (r, g, b, a) =>
                {
                    if (EngineInterop.acs_editor_node3d_set_color(
                            Engine, id, r, g, b, a) != 0)
                        MarkPrefabRootOverride3D(
                            id,
                            PrefabRootProperty3D.Color);
                },
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
            if (!TryCreateMaterialAsset(
                    out string path,
                    out string assetId)) return;
            if (EngineInterop.acs_editor_node3d_set_material(Engine, id, path) == 0)
            {
                Log($"3D ノード {id} へ新規マテリアルを割り当てられませんでした。");
                OpenMaterialEditor(path, assetId);
                return;
            }

            RecordSceneDocumentChange("Assign Material");
            Log($"3D ノード {id} に新規マテリアル {AssetRel(path)} を割当.");
            Populate3DInspector(id);
            OpenMaterialEditor(path, assetId);
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

    /// <summary>成功済みの値編集をstable 3D Prefab rootのoverride metadataへ明示的に反映する。</summary>
    private void MarkPrefabRootOverride3D(
        int id,
        PrefabRootProperty3D property)
    {
        if (Engine == IntPtr.Zero || id < 0) return;
        if (EngineInterop.acs_editor_prefab_instance3d_mark_root_override(
                Engine,
                id,
                property) == 0)
        {
            Log(
                $"3D Prefab root overrideを記録できませんでした (node {id}, {property})。",
                "Scene",
                LogLevel.Warn);
        }
    }

    /// <summary>成功済みcomponent編集をstable 3D Prefab rootのoverrideへ記録する。</summary>
    private void MarkPrefabRootComponentPropertyOverride3D(
        int id,
        int slot,
        int property)
    {
        if (Engine == IntPtr.Zero || id < 0 || slot < 0 || property < 0)
            return;
        if (EngineInterop
                .acs_editor_prefab_instance3d_mark_root_component_property_override(
                    Engine,
                    id,
                    slot,
                    property) == 0)
        {
            Log(
                $"3D Prefab root component overrideを記録できませんでした " +
                $"(node {id}, slot {slot}, property {property})。",
                "Scene",
                LogLevel.Warn);
        }
    }

    /// <summary>1つの3D Prefab root overrideだけをRevertする小型buttonを作る。</summary>
    private Button CreatePrefabRootOverrideRevertButton3D(int id, PrefabRootProperty3D property, string label)
    {
        var button = new Button {
            Content = $"Revert {label}",
            FontSize = 10,
            Padding = new Thickness(7, 1, 7, 1),
            Margin = new Thickness(0, 0, 5, 4),
            ToolTip = $"{label} overrideだけを原本値へ戻す",
        };
        button.Click += (_, __) => RevertPrefabRootOverride3D(id, property);
        return button;
    }

    /// <summary>1つの3D Prefab root overrideだけをApplyする小型buttonを作る。</summary>
    private Button CreatePrefabRootOverrideApplyButton3D(int id, PrefabRootProperty3D property, string label)
    {
        var button = new Button {
            Content = $"Apply {label}",
            FontSize = 10,
            Padding = new Thickness(7, 1, 7, 1),
            Margin = new Thickness(0, 0, 5, 4),
            ToolTip = $"{label} overrideだけを原本へ反映する",
        };
        button.Click += (_, __) => ApplyPrefabRootOverride3D(id, property);
        return button;
    }

    /// <summary>1つの3D Prefab root component propertyだけをApplyする小型buttonを作る。</summary>
    private Button CreatePrefabRootComponentPropertyOverrideApplyButton3D(int id, int slot, int property, string componentTypeName, string label)
    {
        var button = new Button {
            Content = "Apply",
            FontSize = 9,
            Padding = new Thickness(5, 1, 5, 1),
            Margin = new Thickness(5, 2, 0, 2),
            VerticalAlignment = VerticalAlignment.Center,
            ToolTip = $"{label} overrideだけを原本へ反映する",
        };
        button.Click += (_, __) => ApplyPrefabRootComponentPropertyOverride3D(id, slot, property, componentTypeName, label);
        return button;
    }

    /// <summary>1つの3D Prefab root component propertyだけをRevertする小型buttonを作る。</summary>
    private Button CreatePrefabRootComponentPropertyOverrideRevertButton3D(int id, int slot, int property, string label)
    {
        var button = new Button {
            Content = "Revert",
            FontSize = 9,
            Padding = new Thickness(5, 1, 5, 1),
            Margin = new Thickness(5, 2, 0, 2),
            VerticalAlignment = VerticalAlignment.Center,
            ToolTip = $"{label} overrideだけを原本値へ戻す",
        };
        button.Click += (_, __) => RevertPrefabRootComponentPropertyOverride3D(id, slot, property, label);
        return button;
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
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
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
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
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
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
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
        if (Engine == IntPtr.Zero || IsSceneEditingBlocked) return;
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
