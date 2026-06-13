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
        for (int i = 0; i < count; ++i)
        {
            int id = EngineInterop.acs_editor_node3d_id_at(Engine, i);
            var tvi = new TreeViewItem
            {
                Header = Node3DName(id),
                Tag = id,
                Foreground = Brushes.Gainsboro,
                IsSelected = (id == sel),
            };
            HierarchyTree.Items.Add(tvi);
        }
        Log($"Hierarchy: {count} 個の 3D ノード");
    }

    private string Node3DName(int id)
    {
        var buf = new byte[64];
        return EngineInterop.acs_editor_node3d_name(Engine, id, buf, buf.Length) != 0
            ? EngineInterop.Utf8Z(buf) : $"Node {id}";
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

    // ===== Inspector: 3D ノードの transform/色/形状を動的生成 =====
    private void Clear3DInspector()
    {
        Insp3DPanel.Children.Clear();
        Insp3DPanel.Visibility = Visibility.Collapsed;
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
        int prim = EngineInterop.acs_editor_node3d_prim(Engine, id);

        _pop3d = true;
        Insp3DPanel.Children.Clear();
        Insp3DPanel.Children.Add(Section("TRANSFORM"));
        Insp3DPanel.Children.Add(Vec3Row("Position", tf[0], tf[1], tf[2], (x, y, z) => Set3DTransform(id, 0, x, y, z)));
        Insp3DPanel.Children.Add(Vec3Row("Rotation°", tf[3], tf[4], tf[5], (x, y, z) => Set3DTransform(id, 1, x, y, z)));
        Insp3DPanel.Children.Add(Vec3Row("Scale", tf[6], tf[7], tf[8], (x, y, z) => Set3DTransform(id, 2, x, y, z)));
        Insp3DPanel.Children.Add(Section("DISPLAY"));
        Insp3DPanel.Children.Add(Vec3Row("Color", col[0], col[1], col[2], (r, g, b) =>
            EngineInterop.acs_editor_node3d_set_color(Engine, id, r, g, b, 1.0f)));

        // 形状ドロップダウン
        var shapeRow = new DockPanel { Margin = new Thickness(0, 4, 0, 2) };
        var lbl = new TextBlock { Text = "Shape", Width = 64, VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim") };
        DockPanel.SetDock(lbl, Dock.Left); shapeRow.Children.Add(lbl);
        var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center };
        foreach (var s in new[] { "Cube", "Sphere", "Plane" }) combo.Items.Add(s);
        combo.SelectedIndex = (prim >= 0 && prim <= 2) ? prim : 0;
        combo.IsEnabled = false;   // 形状変更は Phase 2+ (再生成が要る) — 表示のみ
        shapeRow.Children.Add(combo);
        Insp3DPanel.Children.Add(shapeRow);

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

    private void Add3DNode(int prim, string name)
    {
        if (Engine == IntPtr.Zero) return;
        if (!_view3d) { View3DBtn.IsChecked = true; OnToggle3D(View3DBtn, new RoutedEventArgs()); }
        int id = EngineInterop.acs_editor_add_node3d(Engine, prim, name);
        if (id < 0) return;
        BuildHierarchy();
        Select3DInHierarchy(id);
        Populate3DInspector(id);
        Log($"3D {name} を追加 (id {id})");
    }
}
