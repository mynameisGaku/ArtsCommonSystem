using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// Blueprint を独立ウィンドウで表示する。左に «My Blueprint» (この BP が持つ変数) パネル、
/// 中央にノードグラフ。UE5 風に、各変数を «型ごとの色ピル + 名前» で並べ、選択すると下の
/// Details で 名前 / 型 / 既定値 を編集できる。変数は実行開始時に値へ種まきされ、Get/Set
/// Variable ノードから参照される。
/// </summary>
public partial class BlueprintWindow : Window
{
    /// <summary>このウィンドウが保持するグラフエディタ (MainWindow が配線/読込先に使う)。</summary>
    public BlueprintEditor Editor => GraphHost;

    private static readonly Brush Fg      = new SolidColorBrush(Color.FromRgb(0xDD, 0xE2, 0xEA));
    private static readonly Brush Dim     = new SolidColorBrush(Color.FromRgb(0x8B, 0x93, 0xA0));
    private static readonly Brush RowSel  = new SolidColorBrush(Color.FromArgb(0x66, 0x4C, 0x9E, 0xE8));
    private static readonly Brush RowHover = new SolidColorBrush(Color.FromArgb(0x14, 0xFF, 0xFF, 0xFF));

    // UE5 の型カラーに寄せた変数型パレット。
    private static readonly string[] Types = { "Bool", "Int", "Float", "String", "Text", "Vector", "Vector3", "Object" };
    private static Brush TypeBrush(string t) => t switch
    {
        "Bool"    => Rgb(0xC0, 0x39, 0x2B),   // 赤
        "Int"     => Rgb(0x2B, 0xD1, 0xB4),   // ティール
        "Float"   => Rgb(0x8F, 0xD1, 0x4F),   // 緑
        "String"  => Rgb(0xD1, 0x56, 0xB0),   // マゼンタ
        "Text"    => Rgb(0xE8, 0xA8, 0xD8),   // 薄ピンク (FText)
        "Vector"  => Rgb(0xE0, 0xB8, 0x4A),   // 金 (Vec2)
        "Vector3" => Rgb(0xE8, 0x8A, 0x3A),   // 橙 (Vec3)
        "Object"  => Rgb(0x4C, 0x9E, 0xE8),   // 青
        _         => Rgb(0x8F, 0xD1, 0x4F),
    };
    private static SolidColorBrush Rgb(byte r, byte g, byte b) => new(Color.FromRgb(r, g, b));

    private BlueprintEditor.BpVar? _sel;
    private readonly System.Collections.Generic.HashSet<string> _collapsedCats = new();   // 折りたたみ中の変数カテゴリ

    // 変数 D&D 用の状態 (リスト行から押下→閾値超でグラフへドラッグ開始)。
    private BlueprintEditor.BpVar? _dragVar;
    private Point _dragOrigin;
    private bool  _dragging;

    public BlueprintWindow()
    {
        InitializeComponent();
        GraphHost.VariablesChanged  = RefreshVars;        // 変数が変わったら再描画
        GraphHost.ComponentsChanged = RefreshComponents;  // コンポーネント木が変わったら再描画
        GraphHost.GraphChanged      = () => { RefreshFunctions(); RefreshLocals(); RefreshDispatchers(); };   // グラフ切替/追加/読込で再描画
        GraphHost.SelectionChanged  = RefreshDetails;     // ノード選択で Details を反映 (UE5 風)
        GraphHost.LocalsChanged     = RefreshLocals;      // ローカル変数の追加/削除で再描画
        GraphHost.DispatchersChanged = RefreshDispatchers; // ディスパッチャの追加/削除で再描画
        GraphHost.PathChanged = () => Title = string.IsNullOrEmpty(GraphHost.CurrentPath)
            ? "Blueprint" : "Blueprint — " + System.IO.Path.GetFileName(GraphHost.CurrentPath);
        Loaded += (_, __) => { RefreshComponents(); RefreshVars(); RefreshFunctions(); RefreshLocals(); RefreshDispatchers(); };
    }

    // この BP の «コンポーネント木» (ACSCENE サブツリー) を解析してノード/コンポーネントを表示する。
    private void RefreshComponents()
    {
        CompTree.Children.Clear();
        var text = Editor.ComponentsText ?? "";
        if (string.IsNullOrWhiteSpace(text))
        {
            CompTree.Children.Add(new TextBlock { Text = "（コンポーネントなし）", Foreground = Dim, FontSize = 11 });
            CompTree.Children.Add(new TextBlock
            {
                Text = "シーンのノードを右クリック →「Blueprint として保存」で作成。",
                Foreground = Dim, FontSize = 10.5, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 3, 0, 0),
            });
            return;
        }
        var order = new List<int>();
        var names = new Dictionary<int, string>();
        var comps = new Dictionary<int, List<string>>();
        foreach (var raw in text.Replace("\r", "").Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0) continue;
            if (line.StartsWith("COMP "))
            {
                var t = line.Split(' ');
                if (t.Length >= 3 && int.TryParse(t[1], out int cid))
                {
                    if (!comps.ContainsKey(cid)) comps[cid] = new List<string>();
                    comps[cid].Add(t[2]);
                }
            }
            else if (char.IsDigit(line[0]) && line.Split(' ') is var t && t.Length >= 3 && int.TryParse(t[0], out int nid))
            {
                names[nid] = t[t.Length - 1];   // ノード行: <id> <parent> … <name> (最終トークン=名前)
                if (!order.Contains(nid)) order.Add(nid);
            }
        }
        foreach (var id in order)
        {
            int nid2 = id;
            var nameRow = new Border { Cursor = Cursors.Hand, Padding = new Thickness(0, 2, 0, 0), Background = Brushes.Transparent };
            nameRow.Child = new TextBlock
            {
                Text = "▸ " + (names.TryGetValue(id, out var nm) ? nm : "#" + id),
                Foreground = Fg, FontSize = 12, FontWeight = FontWeights.SemiBold,
            };
            nameRow.ToolTip = "ビューポートで強調";
            nameRow.MouseLeftButtonUp += (_, __) => Editor.HighlightViewportNode(nid2);   // クリックでビューポートに表示+強調
            CompTree.Children.Add(nameRow);
            if (comps.TryGetValue(id, out var cl))
                for (int s = 0; s < cl.Count; s++)
                {
                    int nodeId = id, slot = s;
                    var row = new Grid { Margin = new Thickness(0, 0, 0, 1) };
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    string ctype = cl[s].Length > 1 && cl[s][0] == 'F' && char.IsUpper(cl[s][1]) ? cl[s].Substring(1) : cl[s];   // 先頭の «F» クラスプレフィックスを表示しない
                    var lbl = new TextBlock { Text = "     ● " + ctype, Foreground = Dim, FontSize = 11, VerticalAlignment = VerticalAlignment.Center };
                    Grid.SetColumn(lbl, 0); row.Children.Add(lbl);
                    var x = new Button
                    {
                        Content = "✕", FontSize = 9, Padding = new Thickness(5, 0, 5, 0), Background = Brushes.Transparent,
                        BorderThickness = new Thickness(0), Foreground = (Brush)FindResource("WarnFg"),
                        VerticalAlignment = VerticalAlignment.Center, ToolTip = "コンポーネントを削除",
                    };
                    x.Click += (_, __) => Editor.RemoveComponentFromNode(nodeId, slot);
                    Grid.SetColumn(x, 1); row.Children.Add(x);
                    CompTree.Children.Add(row);
                }
        }
        if (order.Count == 0)
            CompTree.Children.Add(new TextBlock { Text = "（解析できるノードなし）", Foreground = Dim, FontSize = 11 });
    }

    // 「＋ 追加」: 登録済みコンポーネント型のメニューを出し、選んだ型を root へ追加する。
    private void OnAddComponent(object sender, RoutedEventArgs e)
    {
        var menu = new ContextMenu { PlacementTarget = sender as UIElement };
        if (string.IsNullOrWhiteSpace(Editor.ComponentsText))
        {
            menu.Items.Add(new MenuItem { Header = "(先にノードから「Blueprint として保存」が必要)", IsEnabled = false });
            menu.IsOpen = true;
            return;
        }
        bool any = false;
        int cnt = EngineInterop.acs_editor_type_count();
        for (int i = 0; i < cnt; i++)
        {
            if (EngineInterop.CategoryLabel(EngineInterop.acs_editor_type_category_at(i)) != "Component") continue;
            string tn = EngineInterop.TypeName(i);
            var mi = new MenuItem { Header = tn };
            mi.Click += (_, __) => Editor.AddComponentToRoot(tn);
            menu.Items.Add(mi);
            any = true;
        }
        if (!any) menu.Items.Add(new MenuItem { Header = "(コンポーネント型なし)", IsEnabled = false });
        menu.IsOpen = true;
    }

    // GRAPHS パネル: Event Graph + 関数サブグラフを一覧表示 (クリックで切替)。
    private void RefreshFunctions()
    {
        if (FuncList == null) return;
        FuncList.Children.Clear();
        foreach (var name in Editor.GraphNames)
        {
            var nm = name;
            bool active = nm == Editor.ActiveGraphName;
            bool isFunc = Editor.IsFunctionGraph(nm);
            var row = new Border {
                Padding = new Thickness(7, 3, 7, 3), Margin = new Thickness(0, 1, 0, 1), Cursor = Cursors.Hand,
                Background = active ? RowSel : Brushes.Transparent, CornerRadius = new CornerRadius(3),
            };
            var sp = new StackPanel { Orientation = Orientation.Horizontal };
            sp.Children.Add(new TextBlock { Text = isFunc ? "ƒ " : "▦ ",
                Foreground = isFunc ? (Brush)FindResource("OkFg") : Dim, FontSize = 12.5, VerticalAlignment = VerticalAlignment.Center });
            sp.Children.Add(new TextBlock { Text = nm, Foreground = Fg, FontSize = 12.5, VerticalAlignment = VerticalAlignment.Center,
                TextTrimming = TextTrimming.CharacterEllipsis });
            row.Child = sp;
            row.MouseLeftButtonUp += (_, __) => Editor.SwitchGraph(nm);
            row.MouseEnter += (_, __) => { if (!active) row.Background = RowHover; };
            row.MouseLeave += (_, __) => { if (!active) row.Background = Brushes.Transparent; };
            FuncList.Children.Add(row);
        }
    }
    private void OnAddFunctionPanel(object sender, RoutedEventArgs e) => Editor.NewFunction();
    private void OnAddLocalVar(object sender, RoutedEventArgs e) => Editor.AddLocalVar();
    private void OnAddDispatcher(object sender, RoutedEventArgs e) => Editor.AddDispatcher();

    // EVENT DISPATCHERS セクション。アイコンから D&D で Call / Event(束縛) ノードを作る。
    private void RefreshDispatchers()
    {
        if (DispatcherList == null) return;
        DispatcherList.Children.Clear();
        var disps = Editor.Dispatchers;
        for (int idx = 0; idx < disps.Count; idx++)
        {
            int di = idx; var dn = disps[idx];
            var g = new Grid { Margin = new Thickness(2, 1, 2, 1) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var icon = new Border { Width = 15, Height = 11, CornerRadius = new CornerRadius(3), Background = Rgb(0x35, 0x7A, 0x55),
                VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(2, 0, 6, 0), Cursor = Cursors.SizeAll, ToolTip = "グラフへドラッグで Call / Event" };
            icon.MouseLeftButtonDown += (_, ev) => { DragDrop.DoDragDrop(icon, new DataObject(BlueprintEditor.DispatcherDragFormat, dn), DragDropEffects.Copy); ev.Handled = true; };
            Grid.SetColumn(icon, 0); g.Children.Add(icon);
            var nameBox = MakeBox(dn); nameBox.FontSize = 11; nameBox.Height = 20;
            nameBox.LostFocus += (_, __) => { if (nameBox.Text.Trim() != dn) Editor.RenameDispatcher(di, nameBox.Text.Trim()); };
            Grid.SetColumn(nameBox, 1); g.Children.Add(nameBox);
            var del = new Button { Content = "✕", FontSize = 9, Padding = new Thickness(5, 0, 5, 0), Background = Brushes.Transparent,
                BorderThickness = new Thickness(0), Foreground = (Brush)FindResource("WarnFg"), VerticalAlignment = VerticalAlignment.Center };
            del.Click += (_, __) => Editor.RemoveDispatcher(dn);
            Grid.SetColumn(del, 2); g.Children.Add(del);
            DispatcherList.Children.Add(g);
        }
        if (disps.Count == 0)
            DispatcherList.Children.Add(new TextBlock { Text = "（ディスパッチャなし — ＋ で追加）", Foreground = Dim, FontSize = 10.5, Margin = new Thickness(6, 4, 0, 0) });
    }

    // LOCAL VARIABLES セクション (関数編集中のみ表示)。行はピルから D&D で Get/Set Local ノードを作る。
    private void RefreshLocals()
    {
        if (LocalList == null) return;
        bool show = Editor.ActiveIsFunction;
        LocalHeader.Visibility = LocalListBox.Visibility = LocalDivider.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        LocalList.Children.Clear();
        if (!show) return;
        foreach (var v in Editor.ActiveLocalVars)
        {
            var lv = v;
            var g = new Grid { Margin = new Thickness(2, 1, 2, 1) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(56) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var pill = new Border { Width = 15, Height = 11, CornerRadius = new CornerRadius(3), Background = TypeBrush(lv.Type),
                VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(2, 0, 6, 0), Cursor = Cursors.SizeAll, ToolTip = "グラフへドラッグで Get/Set Local" };
            pill.MouseLeftButtonDown += (_, ev) => { DragDrop.DoDragDrop(pill, new DataObject(BlueprintEditor.LocalVarDragFormat, lv.Name), DragDropEffects.Copy); ev.Handled = true; };
            Grid.SetColumn(pill, 0); g.Children.Add(pill);
            var nameBox = MakeBox(lv.Name); nameBox.FontSize = 11; nameBox.Height = 20;
            nameBox.TextChanged += (_, __) => lv.Name = nameBox.Text;
            nameBox.LostFocus += (_, __) => RefreshLocals();
            Grid.SetColumn(nameBox, 1); g.Children.Add(nameBox);
            var tcombo = new ComboBox { FontSize = 10, Height = 20, Margin = new Thickness(4, 0, 4, 0) };
            foreach (var t in Types) tcombo.Items.Add(t);
            tcombo.SelectedItem = Types.Contains(lv.Type) ? lv.Type : "Float";
            tcombo.SelectionChanged += (_, __) => { if (tcombo.SelectedItem is string t) { lv.Type = t; RefreshLocals(); } };
            Grid.SetColumn(tcombo, 2); g.Children.Add(tcombo);
            var del = new Button { Content = "✕", FontSize = 9, Padding = new Thickness(5, 0, 5, 0), Background = Brushes.Transparent,
                BorderThickness = new Thickness(0), Foreground = (Brush)FindResource("WarnFg"), VerticalAlignment = VerticalAlignment.Center };
            del.Click += (_, __) => Editor.RemoveLocalVar(lv);
            Grid.SetColumn(del, 3); g.Children.Add(del);
            LocalList.Children.Add(g);
        }
        if (Editor.ActiveLocalVars.Count == 0)
            LocalList.Children.Add(new TextBlock { Text = "（ローカル変数なし — ＋ローカル で追加）", Foreground = Dim, FontSize = 10.5, Margin = new Thickness(6, 4, 0, 0) });
    }

    // 「＋ 変数」: 新しい変数を追加して選択する (UE5 風に既定 Float)。
    private void OnAddVar(object sender, RoutedEventArgs e)
    {
        var v = new BlueprintEditor.BpVar(UniqueName("NewVar"), "Float", "0");
        Editor.Variables.Add(v);
        _sel = v;
        RefreshVars();
    }

    private string UniqueName(string baseName)
    {
        string n = baseName; int i = 1;
        while (Editor.Variables.Exists(x => x.Name == n)) n = baseName + (++i);
        return n;
    }

    // 変数リスト + Details を再描画する。
    private void RefreshVars()
    {
        if (_sel != null && !Editor.Variables.Contains(_sel)) _sel = null;

        VarList.Children.Clear();
        bool anyCat = Editor.Variables.Any(x => !string.IsNullOrEmpty(x.Category));
        var cats = new List<string>();
        foreach (var x in Editor.Variables) { var cc = x.Category ?? ""; if (!cats.Contains(cc)) cats.Add(cc); }
        foreach (var cat in cats)
        {
          bool collapsed = _collapsedCats.Contains(cat);
          if (anyCat)   // カテゴリ見出し (UE5 の Category 分類。クリックで折りたたみ)
          {
              var hdr = new Border { Cursor = Cursors.Hand, Padding = new Thickness(4, 6, 0, 2), Background = Brushes.Transparent };
              hdr.Child = new TextBlock { Text = (collapsed ? "▶ " : "▼ ") + (string.IsNullOrEmpty(cat) ? "(未分類)" : cat),
                  Foreground = (Brush)FindResource("SectionFg"), FontSize = 10, FontWeight = FontWeights.SemiBold };
              var cc2 = cat;
              hdr.MouseLeftButtonUp += (_, __) => { if (collapsed) _collapsedCats.Remove(cc2); else _collapsedCats.Add(cc2); RefreshVars(); };
              VarList.Children.Add(hdr);
          }
          if (collapsed) continue;
          foreach (var var in Editor.Variables.Where(x => (x.Category ?? "") == cat))
          {
            var v = var;
            var row = new Border
            {
                Padding = new Thickness(6, 4, 6, 4), Margin = new Thickness(0, 1, 0, 1), Cursor = Cursors.Hand,
                Background = v == _sel ? RowSel : Brushes.Transparent,
                ToolTip = string.IsNullOrEmpty(v.Tooltip) ? null : v.Tooltip,
            };
            var g = new Grid();
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            // 型カラーのピル (UE5 の変数アイコン相当)。
            var pill = new Border
            {
                Width = 17, Height = 12, CornerRadius = new CornerRadius(3), Background = TypeBrush(v.Type),
                VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 9, 0),
            };
            Grid.SetColumn(pill, 0); g.Children.Add(pill);
            var nm = new TextBlock
            {
                Text = v.Name, Foreground = Fg, FontSize = 12.5, VerticalAlignment = VerticalAlignment.Center,
                TextTrimming = TextTrimming.CharacterEllipsis,
            };
            Grid.SetColumn(nm, 1); g.Children.Add(nm);
            var ty = new TextBlock
            {
                Text = (v.InstanceEditable ? "👁 " : "") + v.Type, Foreground = Dim, FontSize = 10.5,
                VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(6, 0, 0, 0),
                ToolTip = v.InstanceEditable ? "インスタンスで編集可" : null,
            };
            Grid.SetColumn(ty, 2); g.Children.Add(ty);
            row.Child = g;
            row.MouseLeftButtonUp += (_, __) => { if (!_dragging) { Editor.ClearNodeSelection(); _sel = v; RefreshVars(); } };
            row.MouseEnter += (_, __) => { if (v != _sel) row.Background = RowHover; };
            row.MouseLeave += (_, __) => { if (v != _sel) row.Background = Brushes.Transparent; };
            // 変数をグラフへドラッグ&ドロップ → Get/Set ノード生成 (Ctrl 併用で Set)。
            row.MouseLeftButtonDown += (_, e) => { _dragOrigin = e.GetPosition(this); _dragVar = v; _dragging = false; };
            row.MouseMove += (_, e) =>
            {
                if (_dragVar == null || e.LeftButton != MouseButtonState.Pressed) return;
                var p = e.GetPosition(this);
                if (Math.Abs(p.X - _dragOrigin.X) < 6 && Math.Abs(p.Y - _dragOrigin.Y) < 6) return;
                _dragging = true;
                var data = new DataObject(BlueprintEditor.VarDragFormat, _dragVar.Name);
                DragDrop.DoDragDrop(row, data, DragDropEffects.Copy);
                _dragVar = null; _dragging = false;
            };
            VarList.Children.Add(row);
          }
        }
        if (Editor.Variables.Count == 0)
            VarList.Children.Add(new TextBlock { Text = "（変数なし — ＋ 変数 で追加）", Foreground = Dim, FontSize = 11, Margin = new Thickness(6, 4, 0, 0) });

        RefreshDetails();
    }

    // Details パネル: 選択対象に反応 (ノード選択中 → ノード情報 / それ以外 → 変数)。UE5 の Details 相当。
    private void RefreshDetails()
    {
        var nv = Editor.SelectedNode();
        if (nv != null) BuildNodeDetails(nv);
        else BuildDetails();
    }

    private static Brush PinTypeBrush(string type) => type switch
    {
        "exec" => Brushes.White,
        "any"  => new SolidColorBrush(Color.FromRgb(0x5B, 0xC8, 0x9A)),
        _      => TypeBrush(type),
    };

    // 選択中ノードの種別/ピンを Details に表示。
    private void BuildNodeDetails(BlueprintEditor.BpNodeView nv)
    {
        DetailsPanel.Children.Clear();
        DetailsPanel.Children.Add(SectionRule("DETAILS"));
        DetailsPanel.Children.Add(new TextBlock { Text = nv.Title, Foreground = Fg, FontSize = 13.5,
            FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        DetailsPanel.Children.Add(new TextBlock { Text = (string.IsNullOrEmpty(nv.Category) ? "ノード" : nv.Category + " ノード"),
            Foreground = Dim, FontSize = 10.5, Margin = new Thickness(0, 0, 0, 6) });
        var ins = nv.Pins.Where(p => !p.output).ToList();
        var outs = nv.Pins.Where(p => p.output).ToList();
        TextBlock Mini(string s) => new() { Text = s, Foreground = (Brush)FindResource("SectionFg"), FontSize = 10, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 6, 0, 2) };
        if (ins.Count > 0)  { DetailsPanel.Children.Add(Mini("入力")); foreach (var p in ins)  DetailsPanel.Children.Add(PinRow(p)); }
        if (outs.Count > 0) { DetailsPanel.Children.Add(Mini("出力")); foreach (var p in outs) DetailsPanel.Children.Add(PinRow(p)); }
    }

    private FrameworkElement PinRow((string name, string type, bool exec, bool output, string literal) p)
    {
        var g = new Grid { Margin = new Thickness(0, 1, 0, 1) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var dot = new Border { Width = 8, Height = 8, CornerRadius = new CornerRadius(4), Background = PinTypeBrush(p.type),
            VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) };
        Grid.SetColumn(dot, 0); g.Children.Add(dot);
        var nm = new TextBlock { Text = p.name, Foreground = Fg, FontSize = 11.5, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        Grid.SetColumn(nm, 1); g.Children.Add(nm);
        var info = new TextBlock { Text = p.exec ? "exec" : (string.IsNullOrEmpty(p.literal) ? p.type : $"{p.type} = {p.literal}"),
            Foreground = Dim, FontSize = 10.5, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(6, 0, 0, 0) };
        Grid.SetColumn(info, 2); g.Children.Add(info);
        return g;
    }

    // 選択中の変数の Name / Type / Default を編集する Details パネル。
    private void BuildDetails()
    {
        DetailsPanel.Children.Clear();
        DetailsPanel.Children.Add(SectionRule("DETAILS"));

        if (_sel == null)
        {
            DetailsPanel.Children.Add(new TextBlock { Text = "ノードか変数を選択してください。", Foreground = Dim, FontSize = 11 });
            return;
        }
        var s = _sel;

        var nameBox = MakeBox(s.Name);
        nameBox.TextChanged += (_, __) => s.Name = nameBox.Text;
        nameBox.LostFocus += (_, __) => RefreshVars();   // リストの表示名を更新
        DetailsPanel.Children.Add(LabeledRow("名前", nameBox));

        var combo = new ComboBox { FontSize = 11, Height = 26 };
        foreach (var t in Types) combo.Items.Add(t);
        combo.SelectedItem = Types.Contains(s.Type) ? s.Type : "Float";
        combo.SelectionChanged += (_, __) => { if (combo.SelectedItem is string t) { s.Type = t; Editor.RetypeVarReferences(s.Name); RefreshVars(); } };   // 型変更で Get/Set ノードも再型付け
        DetailsPanel.Children.Add(LabeledRow("型", combo));

        var valBox = MakeBox(s.Value);
        valBox.TextChanged += (_, __) => s.Value = valBox.Text;
        DetailsPanel.Children.Add(LabeledRow("既定値", valBox));

        var catBox = MakeBox(s.Category);
        catBox.TextChanged += (_, __) => s.Category = catBox.Text;
        catBox.LostFocus += (_, __) => RefreshVars();   // カテゴリ変更でリストを再分類
        DetailsPanel.Children.Add(LabeledRow("カテゴリ", catBox));

        var tipBox = MakeBox(s.Tooltip);
        tipBox.TextChanged += (_, __) => s.Tooltip = tipBox.Text;
        tipBox.LostFocus += (_, __) => RefreshVars();   // ツールチップ更新
        DetailsPanel.Children.Add(LabeledRow("説明", tipBox));

        var inst = new CheckBox { Content = "インスタンスで編集可", IsChecked = s.InstanceEditable, FontSize = 11,
            Foreground = Fg, Margin = new Thickness(0, 6, 0, 0), VerticalContentAlignment = VerticalAlignment.Center };
        inst.Checked   += (_, __) => { s.InstanceEditable = true;  RefreshVars(); };
        inst.Unchecked += (_, __) => { s.InstanceEditable = false; RefreshVars(); };
        DetailsPanel.Children.Add(inst);

        var btns = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 10, 0, 0) };
        var find = new Button { Content = "🔍 参照を検索", Padding = new Thickness(9, 3, 9, 3), Margin = new Thickness(0, 0, 6, 0),
            ToolTip = "この変数を使う Get/Set ノードを選択" };
        find.Click += (_, __) => Editor.SelectVariableReferences(s.Name);
        btns.Children.Add(find);
        var del = new Button { Content = "🗑 削除", Foreground = (Brush)FindResource("WarnFg"), Padding = new Thickness(10, 3, 10, 3) };
        del.Click += (_, __) => {
            int refs = Editor.CountVariableReferences(s.Name);
            var msg = refs > 0 ? $"変数「{s.Name}」を削除しますか？\n{refs} 個の Get/Set ノードが参照しています。" : $"変数「{s.Name}」を削除しますか？";
            if (MessageBox.Show(msg, "変数の削除", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK) return;
            Editor.Variables.Remove(s); _sel = null; RefreshVars();
        };
        btns.Children.Add(del);
        DetailsPanel.Children.Add(btns);
    }

    // 「ラベル + 罫線」のセクション見出し (インスペクタと同じ流儀)。
    private Grid SectionRule(string text)
    {
        var g = new Grid { Margin = new Thickness(0, 0, 0, 8) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var t = new TextBlock
        {
            Text = text, FontWeight = FontWeights.SemiBold, FontSize = 10.5,
            Foreground = (Brush)FindResource("SectionFg"), VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 8, 0),
        };
        Grid.SetColumn(t, 0); g.Children.Add(t);
        var rule = new Border { Height = 1, Background = (Brush)FindResource("Hairline"), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(rule, 1); g.Children.Add(rule);
        return g;
    }

    // ラベル(固定幅) + コントロール の 1 行。
    private FrameworkElement LabeledRow(string label, FrameworkElement control)
    {
        var g = new Grid { Margin = new Thickness(0, 0, 0, 6) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(54) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var lab = new TextBlock { Text = label, Foreground = Dim, FontSize = 11.5, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(lab, 0); g.Children.Add(lab);
        Grid.SetColumn(control, 1); g.Children.Add(control);
        return g;
    }

    private TextBox MakeBox(string text) => new() { Text = text, FontSize = 11, Padding = new Thickness(6, 3, 6, 3), Foreground = Fg };
}
