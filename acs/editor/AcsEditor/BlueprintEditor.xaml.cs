using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Shapes;

namespace AcsEditor;

/// <summary>
/// ビジュアル Blueprint グラフエディタ。
/// BP1: ノード/ピン/接続線の描画 + ノードドラッグ + 背景パン。
/// BP2: ピンをドラッグして接続を生成/切断 + ホイールでズーム。
/// BP6: データ型システム (型安全な配線) / 複数選択 + Undo/Redo + キーボード /
///      検索式ノードパレット + グリッド背景 / 変数 D&D + コメント枠。
/// </summary>
public partial class BlueprintEditor : UserControl
{
    // ----- レイアウト定数 (ピン位置はこれらから解析的に算出する) -----
    private const double NodeW   = 174;
    private const double HeaderH = 26;
    private const double RowH    = 22;
    private const double PinR    = 5;
    private const double HitR    = 12;   // ピンのクリック判定半径 (グラフ座標)
    private const double PinInset = 2;   // ピンを枠の内側へわずかに寄せる量 (右枠はみ出し防止)
    private const double GridStep = 22;  // グリッド間隔 (スナップ/背景方眼で共有)

    private enum PinKind { Exec, Data }
    // データピンの値型 (空 = ワイルドカード/未指定。型安全な配線の判定と配色に使う)。
    private sealed record Pin(string Name, PinKind Kind, string Type = "");

    private sealed class BpNode
    {
        public int Id;
        public string Title = "";
        public double X, Y;
        public Brush Header = Brushes.SteelBlue;
        public List<Pin> Inputs  = new();
        public List<Pin> Outputs = new();
        public FrameworkElement? Visual;   // ノードの外枠 (Canvas)。枠と内容を兄弟化し BorderThickness によるはみ出しを防ぐ
        public bool Executed;   // 直近の実行で起動されたか (ハイライト用)
        public bool Inherited;  // 親 (継承元) .acsbp から来たノードか (淡色・読取専用)
        public bool Disabled;   // 無効化 (実行時に作用をスキップし exec はそのまま通す)
        public bool HasIssue;   // 検証で問題あり (⚠ バッジ表示)
        public string Comment = "";   // ノード個別コメント (UE5 風: ヘッダ上に黄色い帯で表示)
        public string VarRef = "";    // Get/Set Variable が束縛する変数名 (UE 風: 変数を焼き込む。name ピンは持たない)
        public Dictionary<int, string> Literals = new();   // 未接続データ入力ピンの定数値 (入力index→文字列)
    }

    private sealed record BpConn(int FromNode, int FromPin, int ToNode, int ToPin);

    /// <summary>コメント枠 (ノードをグルーピングする色付き矩形。UE5 の Comment 相当)。</summary>
    private sealed class BpComment
    {
        public int Id;
        public double X, Y, W = 220, H = 140;
        public string Text = "Comment";
        public Color Color = Color.FromRgb(0x2E, 0x6E, 0x8A);
        public bool Inherited;
        public FrameworkElement? Visual;
    }

    /// <summary>ヒットしたピンの参照 (どのノードの・何番目の・入出力どちらの・種別)。</summary>
    private sealed record PinRef(BpNode Node, int Index, bool Output, PinKind Kind)
    {
        public Pin Pin => Output ? Node.Outputs[Index] : Node.Inputs[Index];
    }

    // ----- ノードパレット (右クリック生成のテンプレ。外部=MainWindow から供給) -----
    /// <summary>ピン定義 (名前 + exec/data + データ型)。Exec=true で実行ピン。</summary>
    public sealed record BpPinSpec(string Name, bool Exec, string Type = "");
    /// <summary>パレットの 1 エントリ (分類 + タイトル + ヘッダ色 + 入出力ピン)。</summary>
    public sealed record BpNodeTemplate(string Category, string Title, Color Header,
                                        BpPinSpec[] Inputs, BpPinSpec[] Outputs);

    private readonly List<BpNode>         _nodes    = new();
    private readonly List<BpConn>         _conns    = new();
    private readonly List<BpComment>      _comments = new();
    private readonly List<Path>           _wires    = new();
    private readonly List<BpNodeTemplate> _palette  = new();
    private readonly List<BpNodeTemplate> _recent   = new();   // 最近スポーンしたテンプレ (検索の先頭に出す)
    private int   _nextId = 100;        // 生成ノードの ID (デモの 1..4 と衝突しない起点)
    private int   _nextCommentId = 1;   // 生成コメントの ID

    // ----- 複数グラフ (Event Graph + Function。タブで切替。各グラフは N/I/O/V/X/C/K テキストで保持) -----
    private readonly Dictionary<string, string> _graphTexts = new();   // グラフ名 → 本体テキスト
    private readonly List<string> _graphOrder = new() { EventGraphName };
    private readonly HashSet<string> _graphIsFunc = new();
    private string _activeName = EventGraphName;
    private const string EventGraphName = "Event Graph";
    private Point _menuGraphPos;        // 右クリック位置 (生成先のグラフ座標)
    private const int InheritOffset = 100000;   // 継承元ノードの ID オフセット (子と衝突しないように)
    private string? _parentPath;        // 親 (継承元) .acsbp の相対パス (null = 継承なし)

    // 表示変換: 先にズーム、続いてパン (RenderTransform = Translate * Scale)。
    private readonly ScaleTransform     _zoom = new(1, 1);
    private readonly TranslateTransform _pan  = new();

    // ドラッグ状態。
    private BpNode?  _dragNode;     // ノード移動中 (選択集合まとめて動く)
    private bool     _dragMoved;    // 実際に動いたか (クリックと区別)
    private bool     _panning;      // 背景パン中
    private bool     _spaceDown;    // Space 押下中 (左ドラッグでパン)
    private PinRef?  _wireSource;   // ピンからのワイヤ生成中 (アンカー側ピン)
    private Path?    _ghost;        // 生成中ワイヤのプレビュー
    private readonly List<Line> _guides = new();   // ドラッグ整列ガイド線 (UE 風)
    private Point    _lastMouse;

    // 矩形範囲選択 (マーキー)。
    private bool       _selecting;
    private Point      _marqueeStart;   // グラフ座標
    private Rectangle? _marquee;

    // コメントのドラッグ/リサイズ。
    private BpComment?            _dragComment;
    private List<BpNode>?         _commentCarry;   // コメントと一緒に動くノード
    private BpComment?            _resizeComment;
    private Point                _commentLast;

    private readonly HashSet<BpNode> _selected = new();

    // 配色。
    private static readonly Brush ExecWire = new SolidColorBrush(Color.FromRgb(0xE6, 0xE9, 0xEF));
    private static readonly Brush DataWire = new SolidColorBrush(Color.FromRgb(0x5B, 0xC8, 0x9A));
    private static readonly Brush NodeBg   = new SolidColorBrush(Color.FromRgb(0x23, 0x29, 0x33));
    private static readonly Brush NodeEdge = new SolidColorBrush(Color.FromRgb(0x3A, 0x44, 0x52));
    private static readonly Brush PinExec  = new SolidColorBrush(Color.FromRgb(0xE6, 0xE9, 0xEF));
    private static readonly Brush PinData  = new SolidColorBrush(Color.FromRgb(0x5B, 0xC8, 0x9A));   // ワイルドカード
    private static readonly Brush LabelFg  = new SolidColorBrush(Color.FromRgb(0xC2, 0xC9, 0xD4));
    private static readonly Brush ExecHi   = new SolidColorBrush(Color.FromRgb(0xE0, 0xB8, 0x4A));   // 実行済みノードの枠
    private static readonly Brush InheritEdge = new SolidColorBrush(Color.FromRgb(0x7A, 0x8A, 0xB0)); // 継承ノードの枠
    private static readonly Brush SelEdge  = new SolidColorBrush(Color.FromRgb(0xFF, 0x9E, 0x2D));   // 選択中ノードの枠 (UE5 風オレンジ)
    private static readonly Brush WarnEdge = new SolidColorBrush(Color.FromRgb(0xE0, 0x8A, 0x3A));   // 検証で問題ありの枠/バッジ
    private static readonly Brush FiredWire = new SolidColorBrush(Color.FromRgb(0xF2, 0xC8, 0x5A));  // 発火した exec 配線
    private static readonly Brush DbgEdge  = new SolidColorBrush(Color.FromRgb(0xFF, 0xFF, 0xFF));   // ステップ実行の現在ノード枠
    private static readonly Brush BreakDot = new SolidColorBrush(Color.FromRgb(0xE0, 0x44, 0x44));   // ブレークポイントの赤丸
    private static readonly Brush CollideEdge = new SolidColorBrush(Color.FromRgb(0x4C, 0xE0, 0x6A)); // ビューポートの当たり判定輪郭 (緑)

    // データ型 → 配色 (BlueprintWindow の変数ピル色に揃える)。
    private static readonly Brush TBool   = new SolidColorBrush(Color.FromRgb(0xC0, 0x39, 0x2B));
    private static readonly Brush TInt    = new SolidColorBrush(Color.FromRgb(0x2B, 0xD1, 0xB4));
    private static readonly Brush TFloat  = new SolidColorBrush(Color.FromRgb(0x8F, 0xD1, 0x4F));
    private static readonly Brush TString = new SolidColorBrush(Color.FromRgb(0xD1, 0x56, 0xB0));
    private static readonly Brush TVector = new SolidColorBrush(Color.FromRgb(0xE0, 0xB8, 0x4A));
    private static readonly Brush TVector3 = new SolidColorBrush(Color.FromRgb(0xE8, 0x8A, 0x3A));
    private static readonly Brush TObject = new SolidColorBrush(Color.FromRgb(0x4C, 0x9E, 0xE8));
    private static readonly Brush TWildcard = new SolidColorBrush(Color.FromRgb(0xB2, 0xB8, 0xC2));   // ワイルドカード = 中立グレー (UE 風)

    private static Brush DataTypeBrush(string type) => type switch
    {
        "Bool"   => TBool,
        "Int"    => TInt,
        "Float"  => TFloat,
        "String" => TString,
        "Vector" => TVector,
        "Vector3" => TVector3,
        "Object" => TObject,
        _        => TWildcard,   // 空/未知 = ワイルドカード (中立グレーで型付きと区別)
    };
    private static Brush PinBrush(Pin p) => p.Kind == PinKind.Exec ? PinExec : DataTypeBrush(p.Type);

    public BlueprintEditor()
    {
        InitializeComponent();
        GraphCanvas.RenderTransform = new TransformGroup { Children = { _zoom, _pan } };
        GraphCanvas.PreviewMouseLeftButtonDown += OnPreviewDown;   // ピンドラッグを最優先で奪う
        GraphCanvas.MouseLeftButtonDown        += OnCanvasDown;    // 空き場所 = マーキー / Space でパン
        GraphCanvas.MouseDown                  += OnCanvasMouseDown; // 中ボタン = パン
        GraphCanvas.MouseMove                  += OnCanvasMove;
        GraphCanvas.MouseUp                    += OnCanvasUp;
        GraphCanvas.MouseWheel                 += OnWheel;
        GraphCanvas.MouseRightButtonUp         += OnContextMenu;   // 右クリック = 検索パレット / ノード操作
        // キーボード操作 (Undo/Delete 等)。クリックで GraphCanvas にフォーカスを移す。
        GraphCanvas.KeyDown += OnKeyDown;
        GraphCanvas.KeyUp   += OnKeyUp;
        GraphCanvas.PreviewMouseDown += (_, __) => { if (!GraphCanvas.IsKeyboardFocusWithin) GraphCanvas.Focus(); };
        // 変数 D&D の受け口。
        GraphCanvas.AllowDrop = true;
        GraphCanvas.DragOver += OnDragOver;
        GraphCanvas.Drop     += OnDrop;
        Minimap.MouseLeftButtonDown += OnMinimapClick;   // ミニマップでパン
        ViewportCanvas.SizeChanged += (_, __) => { if (_viewportMode) RenderViewport(); };   // リサイズで再描画
        ViewportCanvas.MouseLeftButtonDown += OnViewportDown;   // 空き場所 → カメラ回転
        ViewportCanvas.MouseMove += OnViewportMove;             // 回転 / コンポーネント移動
        ViewportCanvas.MouseLeftButtonUp += OnViewportUp;
        ViewportCanvas.MouseWheel += OnViewportWheel;           // ズーム
        Loaded += (_, __) => { if (_nodes.Count == 0) BuildDemoGraph(); BuildGraphTabs(); UpdateGrid(); UpdateMinimap(); };
    }

    // ----- デモグラフ: BeginPlay → Spawn → SetPosition(target=spawned) → Publish -----
    private void BuildDemoGraph()
    {
        Brush ev = new SolidColorBrush(Color.FromRgb(0xB0, 0x3A, 0x46));   // イベント = 赤
        Brush fn = new SolidColorBrush(Color.FromRgb(0x2E, 0x5C, 0x8A));   // 関数 = 青
        Brush bus= new SolidColorBrush(Color.FromRgb(0x35, 0x7A, 0x55));   // イベント送出 = 緑

        var begin = AddNode(1, "Event  On BeginPlay", 60, 90, ev,
            ins: new Pin[] { },
            outs: new[] { new Pin("▶", PinKind.Exec) });
        var spawn = AddNode(2, "Spawn Prefab", 330, 70, fn,
            ins:  new[] { new Pin("▶", PinKind.Exec), new Pin("path", PinKind.Data, "String"), new Pin("pos", PinKind.Data, "Vector") },
            outs: new[] { new Pin("▶", PinKind.Exec), new Pin("spawned", PinKind.Data, "Object") });
        var setp = AddNode(3, "Set Position", 620, 110, fn,
            ins:  new[] { new Pin("▶", PinKind.Exec), new Pin("target", PinKind.Data, "Object"), new Pin("x", PinKind.Data, "Float"), new Pin("y", PinKind.Data, "Float") },
            outs: new[] { new Pin("▶", PinKind.Exec) });
        var pub  = AddNode(4, "Publish  \"Spawned\"", 620, 270, bus,
            ins:  new[] { new Pin("▶", PinKind.Exec) },
            outs: new[] { new Pin("▶", PinKind.Exec) });

        _conns.Add(new BpConn(begin.Id, 0, spawn.Id, 0));   // exec: BeginPlay → Spawn
        _conns.Add(new BpConn(spawn.Id, 0, setp.Id, 0));    // exec: Spawn → SetPosition
        _conns.Add(new BpConn(spawn.Id, 1, setp.Id, 1));    // data: spawned → target
        _conns.Add(new BpConn(setp.Id, 0, pub.Id, 0));      // exec: SetPosition → Publish

        Rebuild();
    }

    private BpNode AddNode(int id, string title, double x, double y, Brush header, Pin[] ins, Pin[] outs)
    {
        var n = new BpNode { Id = id, Title = title, X = x, Y = y, Header = header };
        n.Inputs.AddRange(ins); n.Outputs.AddRange(outs);
        _nodes.Add(n);
        return n;
    }

    // ----- 描画 -----
    private void Rebuild()
    {
        GraphCanvas.Children.Clear();
        _wires.Clear();
        foreach (var k in _comments) { k.Visual = MakeCommentVisual(k); GraphCanvas.Children.Add(k.Visual); }   // 最背面
        foreach (var c in _conns) { var p = MakeWire(); _wires.Add(p); GraphCanvas.Children.Add(p); }           // 線は背面
        foreach (var n in _nodes) { n.Visual = MakeNodeVisual(n); GraphCanvas.Children.Add(n.Visual); }
        RedrawWires();
        UpdateMinimap();
        UpdateStatus();
        SelectionChanged?.Invoke();   // Details パネルへ選択ノードを反映
    }

    // ----- 選択ノードの Details (UE5 風: 選択対象に Details パネルが反応) -----
    /// <summary>単一選択中ノードの表示用情報 (BlueprintWindow の Details が描く)。</summary>
    public sealed class BpNodeView
    {
        public string Title = "", Category = "";
        public List<(string name, string type, bool exec, bool output, string literal)> Pins = new();
    }
    public Action? SelectionChanged;   // グラフ選択が変わった → Details 再描画

    /// <summary>ちょうど 1 つノードが選択されていればその情報、そうでなければ null。</summary>
    public BpNodeView? SelectedNode()
    {
        var sel = _selected.Where(n => !n.Inherited).ToList();
        if (sel.Count != 1) return null;
        var n = sel[0];
        string Ty(Pin p) => p.Kind == PinKind.Exec ? "exec" : string.IsNullOrEmpty(p.Type) ? "any" : p.Type;
        var v = new BpNodeView { Title = n.Title, Category = _palette.FirstOrDefault(t => t.Title == n.Title)?.Category ?? "" };
        for (int i = 0; i < n.Inputs.Count; i++)
            v.Pins.Add((n.Inputs[i].Name, Ty(n.Inputs[i]), n.Inputs[i].Kind == PinKind.Exec, false, n.Literals.TryGetValue(i, out var lv) ? lv : ""));
        foreach (var p in n.Outputs) v.Pins.Add((p.Name, Ty(p), p.Kind == PinKind.Exec, true, ""));
        return v;
    }
    /// <summary>グラフのノード選択を解除する (変数クリック時に Details を変数へ切替えるため)。</summary>
    public void ClearNodeSelection() { if (_selected.Count > 0) { _selected.Clear(); Rebuild(); } }

    /// <summary>変数 varName を束縛する Get/Set ノード数。</summary>
    public int CountVariableReferences(string varName) =>
        _nodes.Count(n => (n.Title == "Get Variable" || n.Title == "Set Variable") && n.VarRef == varName);

    /// <summary>変数 varName を束縛する Get/Set ノードの value ピン型を更新する (型変更時。接続の型整合を保つ)。</summary>
    public void RetypeVarReferences(string varName)
    {
        bool any = false;
        foreach (var n in _nodes)
            if ((n.Title == "Get Variable" || n.Title == "Set Variable") && n.VarRef == varName) { RetypeVarNode(n); any = true; }
        if (any) Rebuild();
    }

    /// <summary>変数 varName を参照する Get/Set ノードを全選択し最初へ寄せる (UE5 の Find References)。件数を返す。</summary>
    public int SelectVariableReferences(string varName)
    {
        varName = (varName ?? "").Trim();
        _selected.Clear();
        BpNode? first = null;
        foreach (var n in _nodes)
        {
            if (n.Inherited) continue;
            bool isGet = n.Title == "Get Variable" && n.Literals.TryGetValue(0, out var g) && g.Trim() == varName;
            bool isSet = n.Title == "Set Variable" && n.Literals.TryGetValue(1, out var s) && s.Trim() == varName;
            if (isGet || isSet) { _selected.Add(n); first ??= n; }
        }
        if (first != null) CenterOnNode(first);
        Rebuild();
        LogSink?.Invoke($"「{varName}」の参照: {_selected.Count} 件");
        return _selected.Count;
    }

    private BpNode? NodeById(int id) { foreach (var n in _nodes) if (n.Id == id) return n; return null; }

    private static double NodeHeight(BpNode n) => HeaderH + Math.Max(n.Inputs.Count, n.Outputs.Count) * RowH + 6;

    /// <summary>ピンの «グラフ座標» 中心位置。枠からはみ出さないよう PinR だけ内側に置く。 </summary>
    private static Point PinPos(BpNode n, bool output, int idx)
    {
        if (n.Title == "Reroute")   // UE5 風: 小さなドット (in=左端 / out=右端、同じ高さ)
            return new Point(output ? n.X + RerouteSize - 1 : n.X + 1, n.Y + RerouteSize / 2.0);
        double x = n.X + (output ? NodeW - PinR - PinInset : PinR + PinInset);
        double y = n.Y + HeaderH + idx * RowH + RowH / 2.0;
        return new Point(x, y);
    }
    private const double RerouteSize = 16;

    private FrameworkElement MakeNodeVisual(BpNode n)
    {
        if (n.Title == "Reroute") return MakeRerouteVisual(n);   // UE5 風: 小さなドット
        int rows = Math.Max(n.Inputs.Count, n.Outputs.Count);
        double h = HeaderH + rows * RowH + 6;

        var inner = new Canvas { ClipToBounds = false, Width = NodeW, Height = h };
        // ヘッダ (UE5 風: 上を明るくした縦グラデ)。
        var headerColor = (n.Header as SolidColorBrush)?.Color ?? Colors.SteelBlue;
        inner.Children.Add(Place(new Border {
            Width = NodeW, Height = HeaderH, Background = HeaderBrush(headerColor),
            CornerRadius = new CornerRadius(6, 6, 0, 0) }, 0, 0));
        inner.Children.Add(Place(MakeNodeIcon(n), 9, 8));   // 種別アイコン
        string dispTitle = n.Title;   // UE 風: Get/Set Variable は «Get/Set 変数名» と表示
        if ((n.Title == "Set Variable" || n.Title == "Get Variable") && !string.IsNullOrEmpty(n.VarRef))
            dispTitle = (n.Title.StartsWith("Set") ? "Set " : "Get ") + n.VarRef;
        inner.Children.Add(Place(new TextBlock {
            Text = dispTitle, Foreground = Brushes.White, FontSize = 11.5, FontWeight = FontWeights.SemiBold,
            Width = NodeW - 30, TextTrimming = TextTrimming.CharacterEllipsis }, 24, 5));
        if (!string.IsNullOrEmpty(n.Comment))   // ノード個別コメント (UE5 風: ヘッダ上の黄色い帯)
            inner.Children.Add(Place(new Border {
                Background = new SolidColorBrush(Color.FromRgb(0xF2, 0xD9, 0x6B)), CornerRadius = new CornerRadius(4),
                Padding = new Thickness(7, 1, 7, 2), MaxWidth = NodeW + 50,
                Child = new TextBlock { Text = n.Comment, Foreground = new SolidColorBrush(Color.FromRgb(0x30, 0x29, 0x10)),
                    FontSize = 10.5, TextTrimming = TextTrimming.CharacterEllipsis } }, 0, -22));
        // 入力ピン (左辺)。未接続のデータ入力にはインライン定数入力欄を出す。
        for (int i = 0; i < n.Inputs.Count; i++)
        {
            double py = HeaderH + i * RowH + RowH / 2.0;
            var pin = n.Inputs[i];
            if (PinIsDropTarget(n, false, pin)) inner.Children.Add(Place(MakePinGlow(pin), PinInset - 4, py - PinR - 4));
            var ishape = MakePinShape(pin, IsInputConnected(n, i)); ishape.ToolTip = PinTip(pin);
            inner.Children.Add(Place(ishape, PinInset, py - PinR));   // 枠の内側に収める
            if (!(pin.Kind == PinKind.Exec && pin.Name == "▶"))   // 三角ピン自体が示すので「▶」ラベルは出さない
                inner.Children.Add(Place(new TextBlock {
                    Text = pin.Name, Foreground = LabelFg, FontSize = 11 }, 15, py - 8));
            if (pin.Kind == PinKind.Data && !IsInputConnected(n, i))
            {
                // 型に応じた定数エディタ (Bool=チェック / op=ドロップダウン / その他=テキスト)。
                var editor = MakeLiteralEditor(n, i, pin);
                if (editor != null) inner.Children.Add(Place(editor, 50, py - 10));
                else if (pin.Type == "Object" && pin.Name == "target")   // 未接続の target は self に既定
                    inner.Children.Add(Place(new TextBlock { Text = "self", Foreground = new SolidColorBrush(Color.FromRgb(0x6B, 0x74, 0x82)), FontSize = 10, FontStyle = FontStyles.Italic }, 52, py - 8));
            }
        }
        // 出力ピン (右辺、右寄せ)。ラベルは右側の専用帯に限定し、定数欄と重ねない。
        for (int j = 0; j < n.Outputs.Count; j++)
        {
            double py = HeaderH + j * RowH + RowH / 2.0;
            if (PinIsDropTarget(n, true, n.Outputs[j])) inner.Children.Add(Place(MakePinGlow(n.Outputs[j]), NodeW - 2 * PinR - PinInset - 4, py - PinR - 4));
            var oshape = MakePinShape(n.Outputs[j], IsOutputConnected(n, j)); oshape.ToolTip = PinTip(n.Outputs[j]);
            inner.Children.Add(Place(oshape, NodeW - 2 * PinR - PinInset, py - PinR));   // 枠の内側に収める
            if (!(n.Outputs[j].Kind == PinKind.Exec && n.Outputs[j].Name == "▶"))   // 「▶」ラベルは省く (三角ピンが示す)
                inner.Children.Add(Place(new TextBlock {
                    Text = n.Outputs[j].Name, Foreground = LabelFg, FontSize = 11,
                    Width = 54, TextAlignment = TextAlignment.Right }, NodeW - 68, py - 8));
            // 実行後の値ウォッチ (data 出力の右外に小さなピルで表示)。
            if (_showWatch && n.Outputs[j].Kind == PinKind.Data && _watch.TryGetValue((n.Id, j), out var wv))
            {
                var pill = new Border {
                    Background = new SolidColorBrush(Color.FromArgb(0xF0, 0x10, 0x14, 0x1A)),
                    BorderBrush = DataTypeBrush(n.Outputs[j].Type), BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(3), Padding = new Thickness(4, 0, 4, 1),
                    Child = new TextBlock { Text = wv.Length > 16 ? wv.Substring(0, 16) + "…" : (wv.Length == 0 ? "\"\"" : wv),
                        Foreground = Brushes.White, FontSize = 10 },
                };
                inner.Children.Add(Place(pill, NodeW + 7, py - 9));
            }
        }

        // 枠線(outline) と内容(inner) を «兄弟» にして重ねる。Border の子に inner を入れると
        // BorderThickness ぶん内容が内側へずれ、右の出力ピンが枠外へはみ出す (実行済み枠 2.4px で顕著)。
        // 背景(body) → 内容(inner) → 枠線(outline) の順に重ね、outline は IsHitTestVisible=false で
        // 下の TextBox/本体ドラッグへクリックを通す。
        bool sel = _selected.Contains(n);
        bool dbg = n.Id == _debugCurrent;
        var body = new Border {
            Width = NodeW, Height = h, Background = NodeBg, CornerRadius = new CornerRadius(6),
            Effect = new DropShadowEffect { Color = Colors.Black, BlurRadius = 10, ShadowDepth = 2.5, Opacity = 0.55, Direction = 270 },
        };
        var outline = new Border {
            Width = NodeW, Height = h, Background = Brushes.Transparent,
            BorderBrush = dbg ? DbgEdge : (sel ? SelEdge : (n.HasIssue ? WarnEdge : (n.Executed ? ExecHi : (n.Inherited ? InheritEdge : NodeEdge)))),
            BorderThickness = new Thickness(dbg ? 3.0 : (sel ? 2.4 : (n.HasIssue ? 2.0 : (n.Executed ? 2.2 : 1.2)))),
            CornerRadius = new CornerRadius(6), IsHitTestVisible = false,
        };
        var outer = new Canvas {
            Width = NodeW, Height = h, ClipToBounds = false,
            Cursor = n.Inherited ? Cursors.Arrow : Cursors.SizeAll,
            Opacity = n.Inherited ? 0.55 : (n.Disabled ? 0.45 : 1.0),   // 継承=読取専用 / 無効=淡色
        };
        outer.Children.Add(body);
        outer.Children.Add(inner);
        outer.Children.Add(outline);
        Canvas.SetLeft(outer, n.X); Canvas.SetTop(outer, n.Y);
        if (n.HasIssue)   // 検証で問題ありの ⚠ バッジ
            inner.Children.Add(Place(new TextBlock { Text = "⚠", Foreground = WarnEdge, FontSize = 12, FontWeight = FontWeights.Bold }, NodeW - 19, 0));
        if (n.Disabled)   // 無効バッジ
            inner.Children.Add(Place(new TextBlock { Text = "⊘", Foreground = Brushes.White, FontSize = 11 }, NodeW - 19, 1));
        if (_breakpoints.Contains(n.Id))   // ブレークポイントの赤丸
            inner.Children.Add(Place(new Ellipse { Width = 9, Height = 9, Fill = BreakDot,
                Stroke = Brushes.White, StrokeThickness = 1 }, -3, -3));
        if (n.Inherited)
        {
            // 継承元の «継承» バッジ。
            inner.Children.Add(Place(new TextBlock {
                Text = "継承", Foreground = InheritEdge, FontSize = 9 }, NodeW - 34, 1));
            return outer;   // 継承ノードはドラッグ/編集不可
        }
        // ノード本体のドラッグ (ピン上は OnPreviewDown が先取りして e.Handled にするのでここへ来ない)。
        // 定数入力欄の上で押した場合はドラッグせず編集に委ねる (パンも抑止)。
        outer.MouseLeftButtonDown += (s, e) => {
            if (IsTextBoxOrigin(e.OriginalSource)) { e.Handled = true; return; }
            bool ctrl = (Keyboard.Modifiers & ModifierKeys.Control) != 0;
            if (!_selected.Contains(n))
            {
                if (!ctrl) _selected.Clear();
                _selected.Add(n);
                Rebuild();
            }
            else if (ctrl)
            {
                _selected.Remove(n); Rebuild(); e.Handled = true; return;   // Ctrl クリックで選択解除 (ドラッグしない)
            }
            BeginEdit();
            _dragNode = n; _dragMoved = false; _lastMouse = e.GetPosition(GraphCanvas);
            GraphCanvas.CaptureMouse(); e.Handled = true;
        };
        return outer;
    }

    /// <summary>Reroute ノードを UE5 風の小さなドットで描く (型色 + 選択時オレンジ枠)。</summary>
    private FrameworkElement MakeRerouteVisual(BpNode n)
    {
        bool isExec = n.Inputs.Count > 0 && n.Inputs[0].Kind == PinKind.Exec;
        var col = isExec ? PinExec : DataTypeBrush(n.Inputs.Count > 0 ? n.Inputs[0].Type : "");
        bool sel = _selected.Contains(n);
        const double D = 13;
        var outer = new Canvas { Width = RerouteSize, Height = RerouteSize, ClipToBounds = false, Cursor = Cursors.SizeAll,
            Background = Brushes.Transparent };
        Canvas.SetLeft(outer, n.X); Canvas.SetTop(outer, n.Y);
        outer.Children.Add(Place(new Ellipse {
            Width = D, Height = D, Fill = col,
            Stroke = sel ? SelEdge : new SolidColorBrush(Color.FromArgb(0xAA, 0, 0, 0)), StrokeThickness = sel ? 2 : 1,
        }, (RerouteSize - D) / 2, (RerouteSize - D) / 2));
        if (!string.IsNullOrEmpty(n.Comment))
            outer.Children.Add(Place(new TextBlock { Text = n.Comment, Foreground = LabelFg, FontSize = 10 }, RerouteSize + 2, -3));
        outer.MouseLeftButtonDown += (s, e) => {
            bool ctrl = (Keyboard.Modifiers & ModifierKeys.Control) != 0;
            if (!_selected.Contains(n)) { if (!ctrl) _selected.Clear(); _selected.Add(n); Rebuild(); }
            else if (ctrl) { _selected.Remove(n); Rebuild(); e.Handled = true; return; }
            BeginEdit();
            _dragNode = n; _dragMoved = false; _lastMouse = e.GetPosition(GraphCanvas);
            GraphCanvas.CaptureMouse(); e.Handled = true;
        };
        return outer;
    }

    /// <summary>入力ピン inPin に接続があるか。</summary>
    private bool IsInputConnected(BpNode n, int inPin)
    {
        foreach (var c in _conns) if (c.ToNode == n.Id && c.ToPin == inPin) return true;
        return false;
    }

    private static readonly string[] CompareOps = { "==", "!=", "<", "<=", ">", ">=" };

    /// <summary>未接続データ入力ピンの «型に応じた» 定数エディタを作る (Bool=チェック / op=ドロップダウン / 他=テキスト)。
    /// ハンドラは初期値設定の «後» に張り、Rebuild 毎の再生成で誤って Literals を書き換えないようにする。</summary>
    private FrameworkElement? MakeLiteralEditor(BpNode n, int i, Pin pin)
    {
        if (pin.Type == "Object") return null;   // UE 風: Object 参照は «接続» で指定 (Self / Get 変数 / spawned)。手入力欄は出さない
        int idx = i;
        string cur = n.Literals.TryGetValue(i, out var lv) ? lv : "";
        if (n.Title == "Call Function" && pin.Name == "name")
        {
            // 関数名は «関数一覧から選ぶ» + «文字列入力» の両対応 (編集可コンボ)。値=関数名(その関数の ToString)。
            var combo = new ComboBox { IsEditable = true, Width = 96, Height = 18, FontSize = 10, Padding = new Thickness(3, 0, 0, 0) };
            foreach (var g in _graphOrder) if (_graphIsFunc.Contains(g)) combo.Items.Add(g);
            if (!string.IsNullOrEmpty(cur) && !combo.Items.Contains(cur)) combo.Items.Add(cur);
            if (combo.Items.Contains(cur)) combo.SelectedItem = cur; else combo.Loaded += (_, __) => combo.Text = cur;
            void Apply() { var t = combo.Text?.Trim() ?? ""; bool changed = t != cur; if (string.IsNullOrEmpty(t)) n.Literals.Remove(idx); else n.Literals[idx] = t; if (changed) SyncCallPins(n); }   // 選択/入力でシグネチャ同期
            combo.DropDownClosed += (_, __) => Apply();
            combo.LostKeyboardFocus += (_, __) => Apply();
            return combo;
        }
        if (pin.Type == "Bool")
        {
            var cb = new CheckBox { IsChecked = Truthy(cur), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(3, 1, 0, 0) };
            cb.Checked   += (_, __) => { BeginEdit(); n.Literals[idx] = "true";  CommitEdit(); };
            cb.Unchecked += (_, __) => { BeginEdit(); n.Literals[idx] = "false"; CommitEdit(); };
            return cb;
        }
        if (pin.Name == "op")
        {
            var combo = new ComboBox { Width = 54, Height = 18, FontSize = 10, Padding = new Thickness(3, 0, 0, 0) };
            foreach (var o in CompareOps) combo.Items.Add(o);
            combo.SelectedItem = CompareOps.Contains(cur) ? cur : "==";
            combo.SelectionChanged += (_, __) => { if (combo.SelectedItem is string s) { BeginEdit(); n.Literals[idx] = s; CommitEdit(); } };
            return combo;
        }
        var tb = new TextBox {
            Width = 54, Height = 17, FontSize = 10, Padding = new Thickness(2, 0, 2, 0),
            Background = new SolidColorBrush(Color.FromRgb(0x18, 0x1C, 0x23)), Foreground = Brushes.White,
            BorderBrush = NodeEdge, BorderThickness = new Thickness(1), VerticalContentAlignment = VerticalAlignment.Center,
            Text = cur,
        };
        tb.GotKeyboardFocus  += (_, __) => BeginEdit();
        tb.TextChanged += (_, __) => { if (string.IsNullOrEmpty(tb.Text)) n.Literals.Remove(idx); else n.Literals[idx] = tb.Text; };
        tb.LostKeyboardFocus += (_, __) => CommitEdit();
        return tb;
    }

    // ----- グラフ検証 (到達不能ノードを ⚠ バッジで示す) -----
    private void OnValidate(object sender, RoutedEventArgs e) => ValidateGraph(verbose: true);

    /// <summary>各ノードを検証し、問題ありを HasIssue に立てる。verbose でログにも出す。問題件数を返す。</summary>
    private int ValidateGraph(bool verbose)
    {
        int issues = 0;
        foreach (var n in _nodes) n.HasIssue = false;
        foreach (var n in _nodes)
        {
            if (n.Inherited) continue;
            bool hasExecIn = n.Inputs.Any(p => p.Kind == PinKind.Exec);
            if (!hasExecIn) continue;   // イベント/pure ノードは exec 入力を持たない (対象外)
            bool connected = false;
            for (int i = 0; i < n.Inputs.Count; i++)
                if (n.Inputs[i].Kind == PinKind.Exec && IsInputConnected(n, i)) { connected = true; break; }
            if (!connected)
            {
                n.HasIssue = true; issues++;
                if (verbose) LogSink?.Invoke($"⚠ 「{n.Title.Trim()}」(#{n.Id}) は実行入力が未接続のため到達しません。");
            }
        }
        // データ接続の型不一致 (変数の型変更後などに発生しうる)。直結で型が違い、数値同士でもワイルドカードでもない場合を問題とする。
        foreach (var c in _conns)
        {
            var fn = NodeById(c.FromNode); var tn = NodeById(c.ToNode);
            if (fn == null || tn == null || tn.Inherited) continue;
            if (c.FromPin >= fn.Outputs.Count || c.ToPin >= tn.Inputs.Count) continue;
            var fp = fn.Outputs[c.FromPin]; var tp = tn.Inputs[c.ToPin];
            if (fp.Kind != PinKind.Data || tp.Kind != PinKind.Data) continue;
            string a = fp.Type ?? "", b = tp.Type ?? "";
            bool numeric = (a == "Int" || a == "Float") && (b == "Int" || b == "Float");
            if (a.Length > 0 && b.Length > 0 && a != b && !numeric)
            {
                tn.HasIssue = true; issues++;
                if (verbose) LogSink?.Invoke($"⚠ 「{tn.Title.Trim()}」(#{tn.Id}) の «{tp.Name}» に型不一致の接続 ({a} → {b})。");
            }
        }
        if (verbose) LogSink?.Invoke(issues == 0 ? "✓ 検証 OK。" : $"検証: 問題 {issues} 件 (到達不能 / 型不一致)。");
        if (verbose) SetValidateStatus(issues);
        Rebuild();
        return issues;
    }

    /// <summary>検証ボタンに結果を反映する (UE5 の Compile 状態相当。OK=緑 / 問題=橙)。編集で中立へ戻る。</summary>
    private void SetValidateStatus(int issues)
    {
        if (ValidateBtn == null) return;
        ValidateBtn.Content = issues == 0 ? "✓ OK" : $"⚠ {issues}";
        ValidateBtn.Foreground = issues == 0
            ? (Brush)(TryFindResource("OkFg") ?? Brushes.LightGreen)
            : WarnEdge;
    }
    private void ResetValidateStatus()
    {
        if (ValidateBtn == null) return;
        ValidateBtn.Content = "✓ 検証";
        ValidateBtn.Foreground = (Brush)(TryFindResource("Text") ?? Brushes.White);
    }

    /// <summary>クリック元が TextBox (または配下) かを視覚ツリーを遡って判定。</summary>
    private static bool IsTextBoxOrigin(object src)
    {
        var d = src as DependencyObject;
        while (d != null) { if (d is TextBox) return true; d = VisualTreeHelper.GetParent(d); }
        return false;
    }

    private static Ellipse MakePinDot(Brush fill) => new()
    {
        Width = PinR * 2, Height = PinR * 2, Fill = fill,
        Stroke = new SolidColorBrush(Color.FromRgb(0x10, 0x14, 0x1A)), StrokeThickness = 1.0,
    };

    // UE5 風ピン: exec=右向き三角 / data=円。未接続は «中空» (輪郭のみ)、接続済みは «塗り»。
    private static Shape MakePinShape(Pin pin, bool connected)
    {
        if (pin.Kind == PinKind.Exec)
        {
            return new Path
            {
                Data = Geometry.Parse($"M 0.6,0.4 L 0.6,{PinR * 2 - 0.4} L {PinR * 2 + 0.4},{PinR} Z"),
                Stroke = PinExec, StrokeThickness = 1.5, StrokeLineJoin = PenLineJoin.Round,
                Fill = connected ? PinExec : Brushes.Transparent,
            };
        }
        var col = DataTypeBrush(pin.Type);
        return new Ellipse
        {
            Width = PinR * 2, Height = PinR * 2,
            Stroke = col, StrokeThickness = connected ? 1.0 : 1.8,
            Fill = connected ? col : Brushes.Transparent,   // 中空 = 輪郭のみ (背景が透ける)
        };
    }

    /// <summary>出力ピン (n の outPin) に接続があるか。</summary>
    private bool IsOutputConnected(BpNode n, int outPin)
    {
        foreach (var c in _conns) if (c.FromNode == n.Id && c.FromPin == outPin) return true;
        return false;
    }

    /// <summary>ノード種別アイコン (UE5 風): イベント=三角 / pure=円 / 関数・アクション=角丸四角。</summary>
    private static Shape MakeNodeIcon(BpNode n)
    {
        bool execIn = n.Inputs.Any(p => p.Kind == PinKind.Exec);
        bool execOut = n.Outputs.Any(p => p.Kind == PinKind.Exec);
        var fill = new SolidColorBrush(Color.FromArgb(0xE0, 0xFF, 0xFF, 0xFF));
        if (!execIn && !execOut) return new Ellipse { Width = 10, Height = 10, Fill = fill };                       // pure
        if (execOut && !execIn)  return new Path { Data = Geometry.Parse("M 0,0 L 0,11 L 10,5.5 Z"), Fill = fill }; // イベント
        return new Rectangle { Width = 10, Height = 10, RadiusX = 2.5, RadiusY = 2.5, Fill = fill };                // 関数/アクション
    }

    /// <summary>UE5 風ヘッダ: 上を明るく、下を地色にした縦グラデーション。</summary>
    private static Brush HeaderBrush(Color c) => new LinearGradientBrush(Lighten(c, 0.24), c, 90);
    private static Color Lighten(Color c, double f) => Color.FromRgb(
        (byte)(c.R + (255 - c.R) * f), (byte)(c.G + (255 - c.G) * f), (byte)(c.B + (255 - c.B) * f));

    // ピンのツールチップ (名前 : 型)。
    private static string PinTip(Pin p) => p.Kind == PinKind.Exec ? $"{p.Name} (実行)"
        : $"{p.Name} : {(string.IsNullOrEmpty(p.Type) ? "ワイルドカード" : p.Type)}";

    // 配線ドラッグ中に «繋げられるピン» を光らせる輪。
    private static Ellipse MakePinGlow(Pin p) => new()
    {
        Width = PinR * 2 + 8, Height = PinR * 2 + 8, IsHitTestVisible = false,
        Fill = new SolidColorBrush(Color.FromArgb(0x66, ((SolidColorBrush)PinBrush(p)).Color.R,
            ((SolidColorBrush)PinBrush(p)).Color.G, ((SolidColorBrush)PinBrush(p)).Color.B)),
    };

    /// <summary>配線ドラッグ中、ピン (このノードの output 側/idx) が «接続可能な相手» か。</summary>
    private bool PinIsDropTarget(BpNode n, bool output, Pin pin)
    {
        if (_wireSource == null || n == _wireSource.Node) return false;
        if (output == _wireSource.Output) return false;      // 出力↔入力 (反対側のみ)
        if (pin.Kind != _wireSource.Kind) return false;      // exec↔exec / data↔data
        if (pin.Kind == PinKind.Exec) return true;
        return _wireSource.Output ? CanConnectData(_wireSource.Pin.Type, pin.Type)   // 上流出力 → このノードの入力
                                  : CanConnectData(pin.Type, _wireSource.Pin.Type);  // このノードの出力 → 下流入力
    }

    private static T Place<T>(T el, double left, double top) where T : UIElement
    {
        Canvas.SetLeft(el, left); Canvas.SetTop(el, top); return el;
    }

    private static Path MakeWire() => new() {
        StrokeThickness = 2.4, Fill = null, SnapsToDevicePixels = true,
        StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,   // UE5 風: 丸いキャップ
        Effect = new DropShadowEffect { Color = Colors.Black, BlurRadius = 3, ShadowDepth = 1, Opacity = 0.5, Direction = 270 },
    };

    /// <summary>2 ピン間のベジエ (出力→入力。水平に張り出してから繋ぐ)。</summary>
    private static PathGeometry Bez(Point p0, Point p1)
    {
        double dx = Math.Max(50.0, Math.Abs(p1.X - p0.X) * 0.5);
        var fig = new PathFigure { StartPoint = p0, IsClosed = false };
        fig.Segments.Add(new BezierSegment(new Point(p0.X + dx, p0.Y), new Point(p1.X - dx, p1.Y), p1, true));
        return new PathGeometry(new[] { fig });
    }

    private void RedrawWires()
    {
        for (int i = 0; i < _conns.Count; i++)
        {
            var c = _conns[i];
            var from = NodeById(c.FromNode); var to = NodeById(c.ToNode);
            if (from == null || to == null) continue;
            Point p0 = PinPos(from, output: true, c.FromPin);
            Point p1 = PinPos(to, output: false, c.ToPin);
            bool exec = from.Outputs.Count > c.FromPin && from.Outputs[c.FromPin].Kind == PinKind.Exec;
            bool fired = _showWatch && exec && _firedOut.Contains((c.FromNode, c.FromPin));
            _wires[i].Data   = Bez(p0, p1);
            _wires[i].Stroke = fired ? FiredWire : (exec ? ExecWire : DataTypeBrush(from.Outputs.Count > c.FromPin ? from.Outputs[c.FromPin].Type : ""));
            _wires[i].StrokeThickness = fired ? 3.4 : 2.4;
        }
    }

    // ----- ピン判定 / 接続編集 -----
    private static double Dist2(Point a, Point b) { double dx = a.X - b.X, dy = a.Y - b.Y; return dx * dx + dy * dy; }

    /// <summary>グラフ座標 g に最も近いピン (HitR 以内)。無ければ null。</summary>
    private PinRef? PinHitTest(Point g)
    {
        PinRef? best = null; double bd = HitR * HitR;
        foreach (var n in _nodes)
        {
            if (n.Inherited) continue;   // 継承ノードへは接続できない (読取専用)
            for (int i = 0; i < n.Inputs.Count; i++)
            {
                double d = Dist2(PinPos(n, false, i), g);
                if (d < bd) { bd = d; best = new PinRef(n, i, false, n.Inputs[i].Kind); }
            }
            for (int j = 0; j < n.Outputs.Count; j++)
            {
                double d = Dist2(PinPos(n, true, j), g);
                if (d < bd) { bd = d; best = new PinRef(n, j, true, n.Outputs[j].Kind); }
            }
        }
        return best;
    }

    private static Point Cubic(Point a, Point b, Point c, Point d, double t)
    {
        double u = 1 - t, w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        return new Point(w0 * a.X + w1 * b.X + w2 * c.X + w3 * d.X, w0 * a.Y + w1 * b.Y + w2 * c.Y + w3 * d.Y);
    }

    /// <summary>グラフ座標 g に最も近い配線の index (tol 以内)。無ければ -1。ベジエを 16 分割してサンプル。</summary>
    private int WireHitTest(Point g, double tol = 7)
    {
        double best = tol * tol; int hit = -1;
        for (int i = 0; i < _conns.Count; i++)
        {
            var c = _conns[i]; var from = NodeById(c.FromNode); var to = NodeById(c.ToNode);
            if (from == null || to == null) continue;
            Point p0 = PinPos(from, true, c.FromPin), p1 = PinPos(to, false, c.ToPin);
            double dx = Math.Max(50.0, Math.Abs(p1.X - p0.X) * 0.5);
            Point h0 = new(p0.X + dx, p0.Y), h1 = new(p1.X - dx, p1.Y);
            for (int s = 0; s <= 16; s++)
            {
                double d = Dist2(Cubic(p0, h0, h1, p1, s / 16.0), g);
                if (d < best) { best = d; hit = i; }
            }
        }
        return hit;
    }

    /// <summary>配線 connIdx の途中に Reroute ノードを挿入する (中継点。ダブルクリックから)。</summary>
    private void InsertReroute(int connIdx, Point g)
    {
        BeginEdit();
        var c = _conns[connIdx];
        var from = NodeById(c.FromNode);
        if (from == null) { CancelEdit(); return; }
        bool isExec = from.Outputs.Count > c.FromPin && from.Outputs[c.FromPin].Kind == PinKind.Exec;
        string type = from.Outputs.Count > c.FromPin ? from.Outputs[c.FromPin].Type : "";
        var rr = new BpNode { Id = _nextId++, Title = "Reroute", X = g.X - NodeW / 2, Y = g.Y - HeaderH,
            Header = new SolidColorBrush(Color.FromRgb(0x5A, 0x64, 0x72)) };
        rr.Inputs.Add(new Pin("in", isExec ? PinKind.Exec : PinKind.Data, type));
        rr.Outputs.Add(new Pin("out", isExec ? PinKind.Exec : PinKind.Data, type));
        _nodes.Add(rr);
        _conns[connIdx] = new BpConn(c.FromNode, c.FromPin, rr.Id, 0);
        _conns.Add(new BpConn(rr.Id, 0, c.ToNode, c.ToPin));
        Rebuild();
        CommitEdit();
    }

    /// <summary>出力(outType) → 入力(inType) の接続可否と «必要な変換ノード名»。
    /// 戻り値: ok=false=拒否 / (true,null)=直結 / (true,"To X")=その変換ノードを自動挿入。
    /// 空=ワイルドカードと同型は直結。それ以外は型が完全一致しない限り変換ノードを挟む (厳密型)。</summary>
    private static (bool ok, string? conv) ConnectInfo(string outType, string inType)
    {
        outType = (outType ?? "").Trim(); inType = (inType ?? "").Trim();
        if (outType.Length == 0 || inType.Length == 0) return (true, null);   // ワイルドカードは直結
        if (outType == inType) return (true, null);
        var conv = ConversionNodeFor(outType, inType);
        return conv != null ? (true, conv) : (false, null);
    }

    /// <summary>from → to の変換に使うノード名 (無ければ null)。</summary>
    private static string? ConversionNodeFor(string from, string to)
    {
        if (to == "String" && (from is "Int" or "Float" or "Bool" or "Vector" or "Vector3")) return "To String";
        if (from == "Int"     && to == "Float")   return "To Float";
        if (from == "Float"   && to == "Int")     return "To Int";
        if (from == "Vector"  && to == "Vector3") return "To Vector3";
        if (from == "Vector3" && to == "Vector")  return "To Vector2";
        return null;
    }

    /// <summary>接続可能か (直結 or 変換可)。ピン強調・検索フィルタ用。</summary>
    private static bool CanConnectData(string outType, string inType) => ConnectInfo(outType, inType).ok;

    /// <summary>out と in の間に型変換ノードを生成して挟む (out→conv→in)。</summary>
    private void InsertConversion(PinRef outRef, PinRef inRef, string convTitle)
    {
        var op = PinPos(outRef.Node, true, outRef.Index);
        var ip = PinPos(inRef.Node, false, inRef.Index);
        var pos = new Point((op.X + ip.X) / 2 - NodeW / 2, (op.Y + ip.Y) / 2 - 12);
        var col = new SolidColorBrush(Color.FromRgb(0x4A, 0x55, 0x6E));   // 変換ノード色
        var conv = new BpNode { Id = _nextId++, Title = convTitle, X = pos.X, Y = pos.Y, Header = col };
        conv.Inputs.Add(new Pin("in", PinKind.Data, outRef.Pin.Type));    // 入力=ソース型 (直結)
        conv.Outputs.Add(new Pin("out", PinKind.Data, inRef.Pin.Type));   // 出力=宛先型 (直結)
        _nodes.Add(conv);
        AddConnection(outRef.Node, outRef.Index, conv, 0, PinKind.Data);
        AddConnection(conv, 0, inRef.Node, inRef.Index, PinKind.Data);
        LogSink?.Invoke($"型変換ノード «{convTitle}» を自動挿入 ({outRef.Pin.Type} → {inRef.Pin.Type})");
    }

    /// <summary>接続を追加 (単数制約を満たすよう既存を除去: exec は出力側・data は入力側が単数)。</summary>
    private void AddConnection(BpNode fn, int fp, BpNode tn, int tp, PinKind kind)
    {
        if (kind == PinKind.Exec) _conns.RemoveAll(c => c.FromNode == fn.Id && c.FromPin == fp);
        else                      _conns.RemoveAll(c => c.ToNode   == tn.Id && c.ToPin   == tp);
        _conns.RemoveAll(c => c.FromNode == fn.Id && c.FromPin == fp && c.ToNode == tn.Id && c.ToPin == tp);
        _conns.Add(new BpConn(fn.Id, fp, tn.Id, tp));
    }

    private Path MakeGhost(PinKind kind) => new()
    {
        StrokeThickness = 2.4, Fill = null, IsHitTestVisible = false, Opacity = 0.85,
        Stroke = kind == PinKind.Exec ? ExecWire : DataWire,
        StrokeDashArray = new DoubleCollection { 4, 3 },
    };

    private void UpdateGhost(Point g)
    {
        if (_ghost == null || _wireSource == null) return;
        Point sp = PinPos(_wireSource.Node, _wireSource.Output, _wireSource.Index);
        Point p0 = _wireSource.Output ? sp : g;   // 出力→入力 の向きで描く
        Point p1 = _wireSource.Output ? g  : sp;
        _ghost.Data = Bez(p0, p1);
    }

    private void FinishWire(Point g)
    {
        var tgt = PinHitTest(g);
        bool connected = false;
        if (tgt != null && _wireSource != null &&
            tgt.Node != _wireSource.Node &&         // 自ノードへは繋がない
            tgt.Output != _wireSource.Output &&     // 出力↔入力
            tgt.Kind == _wireSource.Kind)           // exec↔exec / data↔data
        {
            var outRef = _wireSource.Output ? _wireSource : tgt;
            var inRef  = _wireSource.Output ? tgt : _wireSource;
            if (outRef.Kind == PinKind.Exec)
            {
                AddConnection(outRef.Node, outRef.Index, inRef.Node, inRef.Index, PinKind.Exec);
                connected = true;
            }
            else
            {
                var (ok, conv) = ConnectInfo(outRef.Pin.Type, inRef.Pin.Type);
                if (ok && conv == null) { AddConnection(outRef.Node, outRef.Index, inRef.Node, inRef.Index, PinKind.Data); connected = true; }
                else if (ok)            { InsertConversion(outRef, inRef, conv!); connected = true; }   // 変換ノードを自動挿入
                else LogSink?.Invoke($"型が一致しない接続を拒否: {outRef.Pin.Type} → {inRef.Pin.Type}");
            }
        }
        if (_ghost != null) { GraphCanvas.Children.Remove(_ghost); _ghost = null; }
        var src = _wireSource;
        _wireSource = null;
        if (!connected && tgt == null && src != null)
        {
            // 空き場所へ離した → «型に合うノード» を検索パレットで提示し、選べば自動接続。
            Rebuild();
            OpenNodeSearch(GraphToControl(g), g, src);
            return;
        }
        Rebuild();   // 空き場所へ落とした場合 (= 切断) も反映
        CommitEdit();
    }

    // ----- 入力 -----
    /// <summary>ピン上で押した場合のみワイヤ生成を開始 (ノードドラッグ/パンより優先)。</summary>
    private void OnPreviewDown(object sender, MouseButtonEventArgs e)
    {
        Point g = e.GetPosition(GraphCanvas);
        var hit = PinHitTest(g);
        if (hit == null) return;   // ピン以外はノードドラッグ/パン/マーキーに委ねる
        if (hit.Node.Title == "Reroute") return;   // Reroute (ドット) はドラッグ移動に委ねる (ドロップ先にはなる)

        BeginEdit();
        PinRef source = hit;
        // 既存の «単数側» ピンを掴んだら、その線を «持ち上げて» 反対端から張り直す (空き場所へ落とせば切断)。
        if (!hit.Output && hit.Kind == PinKind.Data)
        {
            int idx = _conns.FindIndex(c => c.ToNode == hit.Node.Id && c.ToPin == hit.Index);
            if (idx >= 0) { var c = _conns[idx]; _conns.RemoveAt(idx);
                var fn = NodeById(c.FromNode); if (fn != null) source = new PinRef(fn, c.FromPin, true, PinKind.Data); }
        }
        else if (hit.Output && hit.Kind == PinKind.Exec)
        {
            int idx = _conns.FindIndex(c => c.FromNode == hit.Node.Id && c.FromPin == hit.Index);
            if (idx >= 0) { var c = _conns[idx]; _conns.RemoveAt(idx);
                var tn = NodeById(c.ToNode); if (tn != null) source = new PinRef(tn, c.ToPin, false, PinKind.Exec); }
        }

        _wireSource = source;
        Rebuild();   // 持ち上げた線を消す + «繋げられるピン» を光らせる (互換ピン強調)
        _ghost = MakeGhost(source.Kind);
        GraphCanvas.Children.Add(_ghost);
        UpdateGhost(g);
        GraphCanvas.CaptureMouse();
        e.Handled = true;
    }

    // ----- 背景の左クリック (空き場所): Space 中はパン、通常はマーキー選択 -----
    private void OnCanvasDown(object sender, MouseButtonEventArgs e)
    {
        if (_wireSource != null || _dragNode != null) return;
        Point gd = e.GetPosition(GraphCanvas);
        if (e.ClickCount == 2)
        {
            int wi = WireHitTest(gd);
            if (wi >= 0) InsertReroute(wi, gd);            // 配線上ダブルクリック → Reroute 挿入
            else OpenNodeSearch(GraphToControl(gd), gd, null);   // 空ダブルクリック → ノード検索
            return;
        }
        if (_spaceDown)
        {
            _panning = true; _lastMouse = e.GetPosition(this); GraphCanvas.CaptureMouse();
            return;
        }
        // マーキー選択を開始。
        _selecting = true;
        _marqueeStart = e.GetPosition(GraphCanvas);
        if ((Keyboard.Modifiers & ModifierKeys.Control) == 0) { _selected.Clear(); Rebuild(); }
        _marquee = new Rectangle {
            Stroke = SelEdge, StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 3, 2 },
            Fill = new SolidColorBrush(Color.FromArgb(0x22, 0x55, 0xC8, 0xF0)), IsHitTestVisible = false,
        };
        Place(_marquee, _marqueeStart.X, _marqueeStart.Y);
        _marquee.Width = 0; _marquee.Height = 0;
        GraphCanvas.Children.Add(_marquee);
        GraphCanvas.CaptureMouse();
    }

    // 中ボタンドラッグ = パン (UE 風。左はマーキー / 右はメニューのため別ボタンに分ける)。
    private void OnCanvasMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Middle && _wireSource == null && _dragNode == null && !_selecting)
        {
            _panning = true; _lastMouse = e.GetPosition(this); GraphCanvas.CaptureMouse(); e.Handled = true;
        }
    }

    private void OnCanvasMove(object sender, MouseEventArgs e)
    {
        if (_wireSource != null) { UpdateGhost(e.GetPosition(GraphCanvas)); return; }
        if (_resizeComment != null)
        {
            var p = e.GetPosition(GraphCanvas);
            _resizeComment.W = Math.Max(90, _resizeComment.W + (p.X - _commentLast.X));
            _resizeComment.H = Math.Max(64, _resizeComment.H + (p.Y - _commentLast.Y));
            _commentLast = p;
            if (_resizeComment.Visual != null) { _resizeComment.Visual.Width = _resizeComment.W; _resizeComment.Visual.Height = _resizeComment.H;
                if (_resizeComment.Visual is Canvas cc) SizeCommentVisual(cc, _resizeComment); }
            return;
        }
        if (_dragComment != null)
        {
            var p = e.GetPosition(GraphCanvas);
            double ddx = p.X - _commentLast.X, ddy = p.Y - _commentLast.Y; _commentLast = p;
            _dragComment.X += ddx; _dragComment.Y += ddy;
            if (_dragComment.Visual is { } cv) { Canvas.SetLeft(cv, _dragComment.X); Canvas.SetTop(cv, _dragComment.Y); }
            if (_commentCarry != null)
                foreach (var n in _commentCarry) { n.X += ddx; n.Y += ddy; if (n.Visual is { } b) { Canvas.SetLeft(b, n.X); Canvas.SetTop(b, n.Y); } }
            RedrawWires();
            return;
        }
        if (_dragNode != null)
        {
            var p = e.GetPosition(GraphCanvas);
            double dx = p.X - _lastMouse.X, dy = p.Y - _lastMouse.Y; _lastMouse = p;
            if (dx != 0 || dy != 0) _dragMoved = true;
            // 選択集合をまとめて動かす (_dragNode 単独でも選択に含まれている)。
            foreach (var n in _selected)
            {
                n.X += dx; n.Y += dy;
                if (n.Visual is { } b) { Canvas.SetLeft(b, n.X); Canvas.SetTop(b, n.Y); }
            }
            // Ctrl 押下中はグリッドにスナップ (_dragNode を基準に集合ごと補正)。
            if ((Keyboard.Modifiers & ModifierKeys.Control) != 0)
            {
                double sx = Math.Round(_dragNode.X / GridStep) * GridStep - _dragNode.X;
                double sy = Math.Round(_dragNode.Y / GridStep) * GridStep - _dragNode.Y;
                if (sx != 0 || sy != 0)
                    foreach (var n in _selected) { n.X += sx; n.Y += sy; if (n.Visual is { } b) { Canvas.SetLeft(b, n.X); Canvas.SetTop(b, n.Y); } }
                ClearGuides();
            }
            else if (_selected.Count == 1) ShowAlignGuides();   // 単一ドラッグ時は整列ガイド+磁気スナップ
            RedrawWires();
            return;
        }
        if (_selecting && _marquee != null)
        {
            var p = e.GetPosition(GraphCanvas);
            double x = Math.Min(p.X, _marqueeStart.X), y = Math.Min(p.Y, _marqueeStart.Y);
            Place(_marquee, x, y);
            _marquee.Width = Math.Abs(p.X - _marqueeStart.X); _marquee.Height = Math.Abs(p.Y - _marqueeStart.Y);
            return;
        }
        if (_panning)
        {
            var p = e.GetPosition(this);
            _pan.X += p.X - _lastMouse.X; _pan.Y += p.Y - _lastMouse.Y; _lastMouse = p;
            UpdateGrid(); UpdateMinimap();
        }
    }

    // ----- ドラッグ整列ガイド (UE/プロ系エディタ風: 端/中心が他ノードと揃うと線+磁気スナップ) -----
    private void ClearGuides() { foreach (var g in _guides) GraphCanvas.Children.Remove(g); _guides.Clear(); }

    private void AddGuide(double v, bool vertical)
    {
        double lo = -300, hi = 2500;   // ノード範囲に合わせて線の長さを決める (巨大座標は WPF が描かない)
        if (_nodes.Count > 0)
        {
            if (vertical) { lo = _nodes.Min(n => n.Y) - 300; hi = _nodes.Max(n => n.Y + NodeHeight(n)) + 300; }
            else { lo = _nodes.Min(n => n.X) - 300; hi = _nodes.Max(n => n.X + NodeW) + 300; }
        }
        var ln = vertical
            ? new Line { X1 = v, Y1 = lo, X2 = v, Y2 = hi }
            : new Line { X1 = lo, Y1 = v, X2 = hi, Y2 = v };
        ln.Stroke = new SolidColorBrush(Color.FromRgb(0xFF, 0x9E, 0x2D));   // UE オレンジ
        ln.StrokeThickness = 1.2; ln.IsHitTestVisible = false;
        ln.StrokeDashArray = new DoubleCollection { 4, 3 };
        GraphCanvas.Children.Add(ln); _guides.Add(ln);
    }

    private void ShowAlignGuides()
    {
        ClearGuides();
        if (_dragNode == null) return;
        var d = _dragNode;
        double dh = NodeHeight(d);
        double[] dxs = { d.X, d.X + NodeW / 2, d.X + NodeW };   // 左 / 中央X / 右
        double[] dys = { d.Y, d.Y + dh / 2, d.Y + dh };         // 上 / 中央Y / 下
        const double Snap = 3.5;   // 磁気スナップは弱めに (効きすぎ防止)
        double? snapDX = null, snapDY = null;
        double gx = 0, gy = 0;
        foreach (var o in _nodes)
        {
            if (o == d || o.Inherited) continue;
            double oh = NodeHeight(o);
            double[] oxs = { o.X, o.X + NodeW / 2, o.X + NodeW };
            double[] oys = { o.Y, o.Y + oh / 2, o.Y + oh };
            if (snapDX == null)
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
                    if (Math.Abs(dxs[i] - oxs[j]) < Snap) { snapDX = oxs[j] - dxs[i]; gx = oxs[j]; }
            if (snapDY == null)
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
                    if (Math.Abs(dys[i] - oys[j]) < Snap) { snapDY = oys[j] - dys[i]; gy = oys[j]; }
        }
        if (snapDX is double sx && sx != 0) { d.X += sx; if (d.Visual is { } b) Canvas.SetLeft(b, d.X); }
        if (snapDY is double sy && sy != 0) { d.Y += sy; if (d.Visual is { } b) Canvas.SetTop(b, d.Y); }
        if (snapDX != null) AddGuide(gx, true);
        if (snapDY != null) AddGuide(gy, false);
    }

    /// <summary>検証用: ノード dragId を refId の左端近くへ置いてガイドを描く (--bpshot align)。</summary>
    public void AlignGuideForTest(int dragId, int refId)
    {
        var d = NodeById(dragId); var r = NodeById(refId);
        if (d == null || r == null) return;
        d.X = r.X + 3; d.Y = r.Y + 150;   // 左端を 3px ずらして整列圏内へ
        if (d.Visual is { } b) { Canvas.SetLeft(b, d.X); Canvas.SetTop(b, d.Y); }
        _selected.Clear(); _selected.Add(d); _dragNode = d;
        ShowAlignGuides();
        _dragNode = null;
        RedrawWires();
    }

    private void OnCanvasUp(object sender, MouseButtonEventArgs e)
    {
        if (_wireSource != null) { FinishWire(e.GetPosition(GraphCanvas)); GraphCanvas.ReleaseMouseCapture(); return; }
        if (_resizeComment != null) { _resizeComment = null; GraphCanvas.ReleaseMouseCapture(); CommitEdit(); return; }
        if (_dragComment != null) { _dragComment = null; _commentCarry = null; GraphCanvas.ReleaseMouseCapture(); CommitEdit(); return; }
        if (_dragNode != null)
        {
            _dragNode = null; GraphCanvas.ReleaseMouseCapture();
            ClearGuides();
            if (_dragMoved) CommitEdit(); else CancelEdit();
            return;
        }
        if (_selecting)
        {
            FinishMarquee();
            _selecting = false; GraphCanvas.ReleaseMouseCapture();
            return;
        }
        if (_panning) { _panning = false; GraphCanvas.ReleaseMouseCapture(); }
    }

    private void FinishMarquee()
    {
        if (_marquee != null)
        {
            double mx = Canvas.GetLeft(_marquee), my = Canvas.GetTop(_marquee), mw = _marquee.Width, mh = _marquee.Height;
            GraphCanvas.Children.Remove(_marquee); _marquee = null;
            var rect = new Rect(mx, my, mw, mh);
            if (mw >= 3 || mh >= 3)
            {
                foreach (var n in _nodes)
                {
                    if (n.Inherited) continue;
                    var nr = new Rect(n.X, n.Y, NodeW, NodeHeight(n));
                    if (rect.IntersectsWith(nr)) _selected.Add(n);
                }
            }
            // mw,mh が小さい (= ただのクリック) なら選択クリアのみ (OnCanvasDown で実施済み)。
        }
        Rebuild();
    }

    // ----- キーボード -----
    private void OnKeyDown(object sender, KeyEventArgs e)
    {
        if (IsTextBoxOrigin(e.OriginalSource)) return;   // 変数名/定数の編集中は無効
        bool ctrl = (Keyboard.Modifiers & ModifierKeys.Control) != 0;
        bool shift = (Keyboard.Modifiers & ModifierKeys.Shift) != 0;
        switch (e.Key)
        {
            case Key.Space:  _spaceDown = true; GraphCanvas.Cursor = Cursors.Hand; break;
            case Key.Delete: case Key.Back: DeleteSelected(); e.Handled = true; break;
            case Key.Z when ctrl: if (shift) Redo(); else Undo(); e.Handled = true; break;
            case Key.Y when ctrl: Redo(); e.Handled = true; break;
            case Key.S when ctrl: if (!SaveToCurrent()) OnSave(this, new RoutedEventArgs()); e.Handled = true; break;
            case Key.C when ctrl: CopySelection(); e.Handled = true; break;
            case Key.X when ctrl: CopySelection(); DeleteSelected(); e.Handled = true; break;
            case Key.V when ctrl: PasteClipboard(); e.Handled = true; break;
            case Key.D when ctrl: DuplicateSelection(); e.Handled = true; break;
            case Key.A when ctrl: SelectAll(); e.Handled = true; break;
            case Key.F when ctrl: OpenFindInGraph(); e.Handled = true; break;   // グラフ内検索
            case Key.Tab: OpenNodeSearch(new Point(ActualWidth / 2 - 150, 90),
                              new Point((ActualWidth / 2 - _pan.X) / _zoom.ScaleX, (140 - _pan.Y) / _zoom.ScaleY), null); e.Handled = true; break;
            case Key.C: WrapSelectionInComment(); e.Handled = true; break;   // コメント枠で囲む (UE の C)
            case Key.F: FrameAll(); e.Handled = true; break;                  // 全体を画面に収める
            case Key.Left:  NudgeSelected(shift ? -GridStep : -1, 0); e.Handled = true; break;
            case Key.Right: NudgeSelected(shift ?  GridStep :  1, 0); e.Handled = true; break;
            case Key.Up:    NudgeSelected(0, shift ? -GridStep : -1); e.Handled = true; break;
            case Key.Down:  NudgeSelected(0, shift ?  GridStep :  1); e.Handled = true; break;
        }
    }

    /// <summary>選択ノードを (dx,dy) だけ動かす (矢印キー。Shift でグリッド単位)。</summary>
    private void NudgeSelected(double dx, double dy)
    {
        if (_selected.Count == 0) return;
        BeginEdit();
        foreach (var n in _selected) if (!n.Inherited) { n.X += dx; n.Y += dy; }
        Rebuild();
        CommitEdit();
    }

    /// <summary>選択ノードを揃える (整列。2 個以上で有効)。</summary>
    private void AlignSelected(string mode)
    {
        var sel = _selected.Where(n => !n.Inherited).ToList();
        if (sel.Count < 2) return;
        BeginEdit();
        switch (mode)
        {
            case "left":    { double v = sel.Min(n => n.X);                  foreach (var n in sel) n.X = v; break; }
            case "right":   { double v = sel.Max(n => n.X + NodeW);          foreach (var n in sel) n.X = v - NodeW; break; }
            case "top":     { double v = sel.Min(n => n.Y);                  foreach (var n in sel) n.Y = v; break; }
            case "bottom":  { double v = sel.Max(n => n.Y + NodeHeight(n));  foreach (var n in sel) n.Y = v - NodeHeight(n); break; }
            case "hcenter": { double v = sel.Average(n => n.X + NodeW / 2);  foreach (var n in sel) n.X = v - NodeW / 2; break; }
            case "vcenter": { double v = sel.Average(n => n.Y + NodeHeight(n) / 2); foreach (var n in sel) n.Y = v - NodeHeight(n) / 2; break; }
        }
        Rebuild();
        CommitEdit();
    }

    private void OnKeyUp(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Space) { _spaceDown = false; GraphCanvas.Cursor = Cursors.Arrow; }
    }

    /// <summary>すべての (継承でない) ノードを選択する (Ctrl+A)。</summary>
    public void SelectAll()
    {
        _selected.Clear();
        foreach (var n in _nodes) if (!n.Inherited) _selected.Add(n);
        Rebuild();
    }

    private void DeleteSelected()
    {
        if (_selected.Count == 0) return;
        BeginEdit();
        foreach (var n in _selected.ToList())
        {
            if (n.Inherited) continue;
            _conns.RemoveAll(c => c.FromNode == n.Id || c.ToNode == n.Id);
            _nodes.Remove(n);
        }
        _selected.Clear();
        Rebuild();
        CommitEdit();
    }

    // ----- ホイールズーム (カーソル位置を中心に) -----
    private void OnWheel(object sender, MouseWheelEventArgs e)
    {
        double s0 = _zoom.ScaleX;
        double s1 = Math.Clamp(s0 * (e.Delta > 0 ? 1.1 : 1.0 / 1.1), 0.3, 2.5);
        if (Math.Abs(s1 - s0) < 1e-6) return;
        Point c = e.GetPosition(this);                    // 制御座標
        double gx = (c.X - _pan.X) / s0, gy = (c.Y - _pan.Y) / s0;   // カーソル下のグラフ点
        _zoom.ScaleX = _zoom.ScaleY = s1;
        _pan.X = c.X - s1 * gx; _pan.Y = c.Y - s1 * gy;   // その点が動かないようパンを補正
        UpdateGrid(); UpdateMinimap(); UpdateStatus();
        e.Handled = true;
    }

    /// <summary>全ノード/コメントが画面に収まるようパン/ズームを合わせる (F キー)。</summary>
    private void FrameAll()
    {
        if (_nodes.Count == 0 && _comments.Count == 0) return;
        double minX = double.MaxValue, minY = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (var n in _nodes) { minX = Math.Min(minX, n.X); minY = Math.Min(minY, n.Y); maxX = Math.Max(maxX, n.X + NodeW); maxY = Math.Max(maxY, n.Y + NodeHeight(n)); }
        foreach (var k in _comments) { minX = Math.Min(minX, k.X); minY = Math.Min(minY, k.Y); maxX = Math.Max(maxX, k.X + k.W); maxY = Math.Max(maxY, k.Y + k.H); }
        double gw = maxX - minX, gh = maxY - minY;
        double vw = ActualWidth, vh = ActualHeight - 36;   // ツールバー分を除く
        if (gw <= 0 || gh <= 0 || vw <= 0 || vh <= 0) return;
        double s = Math.Clamp(Math.Min(vw / (gw + 80), vh / (gh + 80)), 0.3, 1.5);
        _zoom.ScaleX = _zoom.ScaleY = s;
        _pan.X = (vw - s * gw) / 2 - s * minX;
        _pan.Y = (vh - s * gh) / 2 - s * minY + 36;
        UpdateGrid(); UpdateMinimap();
    }

    // ----- グリッド背景 -----
    private void UpdateGrid()
    {
        double z = _zoom.ScaleX;
        double minor = 22 * z;
        double major = minor * 5;
        if (minor < 4) { GridBg.Fill = new SolidColorBrush(Color.FromRgb(0x14, 0x18, 0x1E)); return; }

        var dg = new DrawingGroup();
        using (var dc = dg.Open())
        {
            dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(0x14, 0x18, 0x1E)), null, new Rect(0, 0, major, major));
            var pMinor = new Pen(new SolidColorBrush(Color.FromArgb(0x22, 0xFF, 0xFF, 0xFF)), 1);
            var pMajor = new Pen(new SolidColorBrush(Color.FromArgb(0x48, 0xFF, 0xFF, 0xFF)), 1.2);
            for (int i = 0; i <= 5; i++)
            {
                double o = i * minor;
                var pen = (i % 5 == 0) ? pMajor : pMinor;
                dc.DrawLine(pen, new Point(o, 0), new Point(o, major));
                dc.DrawLine(pen, new Point(0, o), new Point(major, o));
            }
        }
        GridBg.Fill = new DrawingBrush(dg)
        {
            TileMode = TileMode.Tile, Viewport = new Rect(0, 0, major, major),
            ViewportUnits = BrushMappingMode.Absolute, Stretch = Stretch.None,
            Transform = new TranslateTransform(Mod(_pan.X, major), Mod(_pan.Y, major)),
        };
    }
    private static double Mod(double a, double m) { double r = a % m; return r < 0 ? r + m : r; }

    /// <summary>グラフ座標 → この UserControl のローカル座標 (ポップアップ配置用)。</summary>
    private Point GraphToControl(Point g) => new(g.X * _zoom.ScaleX + _pan.X, g.Y * _zoom.ScaleY + _pan.Y);

    // ----- 全体ミニマップ -----
    private const double MmPad = 8;
    private double _mmScale = 1, _mmMinX, _mmMinY;
    private bool _minimapOn = true;

    private void OnToggleMinimap(object sender, RoutedEventArgs e)
    {
        _minimapOn = MinimapToggle.IsChecked == true;
        UpdateMinimap();
    }

    /// <summary>下部ステータスバーの情報 (ノード数 / ズーム%) を更新する。</summary>
    private void UpdateStatus()
    {
        if (StatusInfo == null) return;
        int nc = _nodes.Count(n => !n.Inherited);
        StatusInfo.Text = $"{nc} ノード ・ {(_zoom.ScaleX * 100):0}%";
    }

    private void UpdateMinimap()
    {
        if (Minimap == null || MinimapPanel == null) return;
        if (!_minimapOn) { MinimapPanel.Visibility = Visibility.Collapsed; return; }
        Minimap.Children.Clear();
        if (_nodes.Count == 0 && _comments.Count == 0) { MinimapPanel.Visibility = Visibility.Collapsed; return; }
        MinimapPanel.Visibility = Visibility.Visible;

        double minX = double.MaxValue, minY = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (var n in _nodes) { minX = Math.Min(minX, n.X); minY = Math.Min(minY, n.Y); maxX = Math.Max(maxX, n.X + NodeW); maxY = Math.Max(maxY, n.Y + NodeHeight(n)); }
        foreach (var k in _comments) { minX = Math.Min(minX, k.X); minY = Math.Min(minY, k.Y); maxX = Math.Max(maxX, k.X + k.W); maxY = Math.Max(maxY, k.Y + k.H); }
        // 現在のビューポート (グラフ座標) も含め、矩形が常に見えるようにする。
        double vgx0 = -_pan.X / _zoom.ScaleX, vgy0 = -_pan.Y / _zoom.ScaleY;
        double vw = ActualWidth > 0 ? ActualWidth : 800, vh = ActualHeight > 0 ? ActualHeight : 600;
        double vgx1 = (vw - _pan.X) / _zoom.ScaleX, vgy1 = (vh - _pan.Y) / _zoom.ScaleY;
        minX = Math.Min(minX, vgx0); minY = Math.Min(minY, vgy0); maxX = Math.Max(maxX, vgx1); maxY = Math.Max(maxY, vgy1);

        double gw = Math.Max(1, maxX - minX), gh = Math.Max(1, maxY - minY);
        double mw = MinimapPanel.Width - 2, mh = MinimapPanel.Height - 2;
        double s = Math.Min((mw - 2 * MmPad) / gw, (mh - 2 * MmPad) / gh);
        _mmScale = s; _mmMinX = minX; _mmMinY = minY;
        Point M(double gx, double gy) => new(MmPad + (gx - minX) * s, MmPad + (gy - minY) * s);

        foreach (var k in _comments)
        {
            var p = M(k.X, k.Y);
            var r = new Rectangle { Width = Math.Max(2, k.W * s), Height = Math.Max(2, k.H * s),
                Fill = new SolidColorBrush(Color.FromArgb(0x44, k.Color.R, k.Color.G, k.Color.B)) };
            Minimap.Children.Add(Place(r, p.X, p.Y));
        }
        foreach (var n in _nodes)
        {
            var p = M(n.X, n.Y);
            var col = (n.Header as SolidColorBrush)?.Color ?? Colors.Gray;
            var r = new Rectangle { Width = Math.Max(2, NodeW * s), Height = Math.Max(2, NodeHeight(n) * s),
                Fill = new SolidColorBrush(col), RadiusX = 1, RadiusY = 1, Opacity = _selected.Contains(n) ? 1 : 0.8 };
            if (_selected.Contains(n)) { r.Stroke = SelEdge; r.StrokeThickness = 1; }
            Minimap.Children.Add(Place(r, p.X, p.Y));
        }
        Point a = M(vgx0, vgy0), b = M(vgx1, vgy1);
        var vr = new Rectangle { Width = Math.Max(2, b.X - a.X), Height = Math.Max(2, b.Y - a.Y),
            Stroke = Brushes.White, StrokeThickness = 1, Fill = new SolidColorBrush(Color.FromArgb(0x18, 0xFF, 0xFF, 0xFF)) };
        Minimap.Children.Add(Place(vr, a.X, a.Y));
    }

    private void OnMinimapClick(object sender, MouseButtonEventArgs e)
    {
        var p = e.GetPosition(Minimap);
        double gx = _mmMinX + (p.X - MmPad) / _mmScale, gy = _mmMinY + (p.Y - MmPad) / _mmScale;
        _pan.X = (ActualWidth > 0 ? ActualWidth : 800) / 2 - _zoom.ScaleX * gx;
        _pan.Y = (ActualHeight > 0 ? ActualHeight : 600) / 2 - _zoom.ScaleY * gy;
        UpdateGrid(); UpdateMinimap();
        e.Handled = true;
    }

    // ----- ノードパレット / 右クリック -----
    /// <summary>パレット (生成可能なノードテンプレ) を差し替える。MainWindow がリフレクションから構築。</summary>
    public void SetPalette(IReadOnlyList<BpNodeTemplate> templates)
    {
        _palette.Clear();
        _palette.AddRange(templates);
    }

    /// <summary>グラフ座標 g を含むノード (最前面優先)。無ければ null。</summary>
    private BpNode? NodeAt(Point g)
    {
        for (int i = _nodes.Count - 1; i >= 0; i--)
        {
            var n = _nodes[i];
            if (n.Inherited) continue;   // 継承ノードは削除/複製不可
            double h = NodeHeight(n);
            if (g.X >= n.X && g.X <= n.X + NodeW && g.Y >= n.Y && g.Y <= n.Y + h) return n;
        }
        return null;
    }

    private void OnContextMenu(object sender, MouseButtonEventArgs e)
    {
        if (_wireSource != null || _dragNode != null || _panning || _selecting) return;
        Point g = e.GetPosition(GraphCanvas);
        // ピン上の右クリック → 接続を切断 (UE5 風)。
        var pinHit = PinHitTest(g);
        if (pinHit != null && !pinHit.Node.Inherited)
        {
            var pinObj = pinHit.Output ? pinHit.Node.Outputs[pinHit.Index] : pinHit.Node.Inputs[pinHit.Index];
            int n = pinHit.Output
                ? _conns.Count(c => c.FromNode == pinHit.Node.Id && c.FromPin == pinHit.Index)
                : _conns.Count(c => c.ToNode == pinHit.Node.Id && c.ToPin == pinHit.Index);
            if (n > 0 || pinObj.Kind == PinKind.Data)
            {
                var menu = new ContextMenu();
                if (pinObj.Kind == PinKind.Data)   // データピン → 変数に昇格 (UE5 風)
                {
                    var prom = new MenuItem { Header = "変数に昇格" };
                    prom.Click += (_, __) => PromotePinToVariable(pinHit);
                    menu.Items.Add(prom);
                }
                if (n > 0)
                {
                    if (menu.Items.Count > 0) menu.Items.Add(new Separator());
                    var brk = new MenuItem { Header = $"このピンの接続を切断 ({n})" };
                    brk.Click += (_, __) => {
                        BeginEdit();
                        if (pinHit.Output) _conns.RemoveAll(c => c.FromNode == pinHit.Node.Id && c.FromPin == pinHit.Index);
                        else _conns.RemoveAll(c => c.ToNode == pinHit.Node.Id && c.ToPin == pinHit.Index);
                        Rebuild(); CommitEdit();
                    };
                    menu.Items.Add(brk);
                }
                menu.PlacementTarget = GraphCanvas; menu.IsOpen = true; e.Handled = true;
                return;
            }
        }
        var hit = NodeAt(g);
        if (hit != null)
        {
            var menu = new ContextMenu();
            var dup = new MenuItem { Header = $"ノードを複製   ({hit.Title})" };
            dup.Click += (_, __) => { if (!_selected.Contains(hit)) { _selected.Clear(); _selected.Add(hit); } DuplicateSelection(); };
            menu.Items.Add(dup);
            var del = new MenuItem { Header = $"ノードを削除   ({hit.Title})" };
            del.Click += (_, __) => { if (!_selected.Contains(hit)) { _selected.Clear(); _selected.Add(hit); } DeleteSelected(); };
            menu.Items.Add(del);
            var dis = new MenuItem { Header = hit.Disabled ? "ノードを有効化" : "ノードを無効化" };
            dis.Click += (_, __) => ToggleDisable(hit);
            menu.Items.Add(dis);
            var bp = new MenuItem { Header = _breakpoints.Contains(hit.Id) ? "ブレークポイント解除" : "ブレークポイントを置く" };
            bp.Click += (_, __) => ToggleBreakpoint(hit);
            menu.Items.Add(bp);
            var cmt = new MenuItem { Header = string.IsNullOrEmpty(hit.Comment) ? "コメントを追加…" : "コメントを編集…" };
            cmt.Click += (_, __) => OpenNodeCommentEditor(hit);
            menu.Items.Add(cmt);
            if (hit.Title == "Set Variable" || hit.Title == "Get Variable")   // 束縛する変数を選び直す
            {
                var pick = new MenuItem { Header = "変数を選択" };
                foreach (var v in Variables)
                {
                    var vn = v.Name; if (string.IsNullOrWhiteSpace(vn)) continue;
                    var mi = new MenuItem { Header = vn, IsChecked = hit.VarRef == vn };
                    mi.Click += (_, __) => { BeginEdit(); hit.VarRef = vn; RetypeVarNode(hit); Rebuild(); CommitEdit(); };
                    pick.Items.Add(mi);
                }
                if (pick.Items.Count == 0) pick.Items.Add(new MenuItem { Header = "(変数なし — ＋変数 で追加)", IsEnabled = false });
                menu.Items.Add(new Separator()); menu.Items.Add(pick);
            }
            if (hit.Title == "Function Entry")   // 関数の入力パラメータ (Entry の data 出力) を足す
            {
                var add = new MenuItem { Header = "入力パラメータを追加" };
                add.Click += (_, __) => AddParamPin(hit, output: true);
                menu.Items.Add(new Separator()); menu.Items.Add(add);
            }
            else if (hit.Title == "Return")   // 関数の戻り値 (Return の data 入力) を足す
            {
                var add = new MenuItem { Header = "戻り値を追加" };
                add.Click += (_, __) => AddParamPin(hit, output: false);
                menu.Items.Add(new Separator()); menu.Items.Add(add);
            }
            else if (hit.Title == "Call Function")   // 呼ぶ関数のシグネチャにピンを同期
            {
                var sync = new MenuItem { Header = "関数ピンを同期" };
                sync.Click += (_, __) => SyncCallPins(hit);
                menu.Items.Add(new Separator()); menu.Items.Add(sync);
            }
            // 関数に折りたたむ (2 個以上選択時。UE の Collapse to Function)。
            if (_selected.Count >= 2 && _selected.Contains(hit))
            {
                var collapse = new MenuItem { Header = "関数に折りたたむ" };
                collapse.Click += (_, __) => CollapseToFunction();
                menu.Items.Add(new Separator()); menu.Items.Add(collapse);
            }
            // 整列 (2 個以上選択時)。
            if (_selected.Count >= 2 && _selected.Contains(hit))
            {
                var align = new MenuItem { Header = "整列" };
                void Add(string h, string m) { var mi = new MenuItem { Header = h }; mi.Click += (_, __) => AlignSelected(m); align.Items.Add(mi); }
                Add("左そろえ", "left"); Add("右そろえ", "right"); Add("上そろえ", "top");
                Add("下そろえ", "bottom"); Add("横中央", "hcenter"); Add("縦中央", "vcenter");
                menu.Items.Add(new Separator());
                menu.Items.Add(align);
            }
            menu.PlacementTarget = GraphCanvas; menu.IsOpen = true;
            e.Handled = true;
            return;
        }
        // 配線上の右クリック → 配線を削除。
        int wi = WireHitTest(g);
        if (wi >= 0)
        {
            var menu = new ContextMenu();
            var rm = new MenuItem { Header = "配線を削除" };
            rm.Click += (_, __) => { BeginEdit(); if (wi < _conns.Count) _conns.RemoveAt(wi); Rebuild(); CommitEdit(); };
            menu.Items.Add(rm);
            menu.PlacementTarget = GraphCanvas; menu.IsOpen = true;
            e.Handled = true;
            return;
        }
        // 空き場所: 検索パレットを開く (右クリック位置に生成)。
        OpenNodeSearch(GraphToControl(g), g, null);
        e.Handled = true;
    }

    private static string PinTokType(string ty) => string.IsNullOrEmpty(ty) ? "D" : "D:" + ty;

    /// <summary>選択ノードを新しい関数に折りたたみ、その場所を Call Function ノードに置き換える (UE の Collapse to Function)。
    /// 境界の «外→S» データ=引数 / «S→外» データ=戻り値 / exec 境界=関数の入口/出口にする。exec を含む選択のみ対応。</summary>
    private void CollapseToFunction()
    {
        var S = _selected.Where(n => !n.Inherited).ToList();
        if (S.Count < 1) { LogSink?.Invoke("折りたたむノードを選択してください。"); return; }
        var ids = S.Select(n => n.Id).ToHashSet();

        var dataIn = new List<BpConn>(); var dataOut = new List<BpConn>();
        var execIn = new List<BpConn>(); var execOut = new List<BpConn>();
        var internalC = new List<BpConn>();
        foreach (var c in _conns)
        {
            bool fi = ids.Contains(c.FromNode), ti = ids.Contains(c.ToNode);
            if (fi && ti) { internalC.Add(c); continue; }
            if (!fi && !ti) continue;
            var fn = NodeById(c.FromNode); if (fn == null) continue;
            bool isExec = c.FromPin < fn.Outputs.Count && fn.Outputs[c.FromPin].Kind == PinKind.Exec;
            if (ti) { if (isExec) execIn.Add(c); else dataIn.Add(c); }     // 外 → S
            else    { if (isExec) execOut.Add(c); else dataOut.Add(c); }   // S → 外
        }
        if (execIn.Count == 0 && execOut.Count == 0) { LogSink?.Invoke("実行(exec)を含む選択のみ折りたたみできます (pure のみは未対応)。"); return; }

        // データ引数: 外部ソースピンごとに 1 つ。戻り値: 内部ソースピンごとに 1 つ。
        var paramOf = new Dictionary<(int, int), int>(); var paramType = new List<string>();
        foreach (var c in dataIn) { var k = (c.FromNode, c.FromPin); if (!paramOf.ContainsKey(k)) { var fn = NodeById(c.FromNode)!; paramOf[k] = paramType.Count; paramType.Add(c.FromPin < fn.Outputs.Count ? fn.Outputs[c.FromPin].Type : ""); } }
        var retOf = new Dictionary<(int, int), int>(); var retType = new List<string>();
        foreach (var c in dataOut) { var k = (c.FromNode, c.FromPin); if (!retOf.ContainsKey(k)) { var fn = NodeById(c.FromNode)!; retOf[k] = retType.Count; retType.Add(c.FromPin < fn.Outputs.Count ? fn.Outputs[c.FromPin].Type : ""); } }

        string fname = UniqueGraphName("NewFunction");
        int entryId = _nextId++, returnId = _nextId++;

        // 関数本体テキスト: Entry / Return / S ノード+内部接続 / 境界の配線。
        var fb = new StringBuilder();
        fb.Append($"N {entryId} 60 140 357A55 Function Entry\nO E ▶\n");
        for (int k = 0; k < paramType.Count; k++) fb.Append($"O {PinTokType(paramType[k])} arg{k}\n");
        fb.Append($"N {returnId} 820 140 357A55 Return\nI E ▶\n");
        for (int k = 0; k < retType.Count; k++) fb.Append($"I {PinTokType(retType[k])} ret{k}\n");
        fb.Append(SerializeNodes(S));   // S の N/I/O/V/X/NC/VR + 内部接続 C
        if (execIn.Count > 0) fb.Append($"C {entryId} 0 {execIn[0].ToNode} {execIn[0].ToPin}\n");   // Entry.▶ は単出力 → 最初の入口へ
        foreach (var c in dataIn) fb.Append($"C {entryId} {1 + paramOf[(c.FromNode, c.FromPin)]} {c.ToNode} {c.ToPin}\n");
        foreach (var kv in retOf) fb.Append($"C {kv.Key.Item1} {kv.Key.Item2} {returnId} {1 + kv.Value}\n");   // 戻り値は «内部ソースごと» に 1 本 (重複防止)
        foreach (var c in execOut) fb.Append($"C {c.FromNode} {c.FromPin} {returnId} 0\n");   // 全ての実行出口を Return.▶ へ (入力は多入力可)

        BeginEdit();
        _graphTexts[fname] = fb.ToString();
        _graphOrder.Add(fname); _graphIsFunc.Add(fname);

        // 現在グラフ: Call Function ノードを作り、S と境界/内部接続を除去して置換。
        double cx = S.Average(n => n.X), cy = S.Average(n => n.Y);
        var call = new BpNode { Id = _nextId++, Title = "Call Function", X = cx, Y = cy, Header = new SolidColorBrush(Color.FromRgb(0x2E, 0x5C, 0x8A)) };
        call.Inputs.Add(new Pin("▶", PinKind.Exec)); call.Inputs.Add(new Pin("name", PinKind.Data, "String"));
        for (int k = 0; k < paramType.Count; k++) call.Inputs.Add(new Pin($"arg{k}", PinKind.Data, paramType[k]));
        call.Outputs.Add(new Pin("▶", PinKind.Exec));
        for (int k = 0; k < retType.Count; k++) call.Outputs.Add(new Pin($"ret{k}", PinKind.Data, retType[k]));
        call.Literals[1] = fname;

        _nodes.RemoveAll(n => ids.Contains(n.Id));
        _conns.RemoveAll(c => ids.Contains(c.FromNode) || ids.Contains(c.ToNode));
        _nodes.Add(call);
        foreach (var c in execIn) _conns.Add(new BpConn(c.FromNode, c.FromPin, call.Id, 0));   // 全ての外部実行ソース → Call.▶ (入力は多入力可)
        foreach (var kv in paramOf) _conns.Add(new BpConn(kv.Key.Item1, kv.Key.Item2, call.Id, 2 + kv.Value));
        if (execOut.Count > 0) _conns.Add(new BpConn(call.Id, 0, execOut[0].ToNode, execOut[0].ToPin));   // Call.▶ は単出力 → 最初の出口へ
        foreach (var c in dataOut) _conns.Add(new BpConn(call.Id, 1 + retOf[(c.FromNode, c.FromPin)], c.ToNode, c.ToPin));

        _selected.Clear(); _selected.Add(call);
        Rebuild(); BuildGraphTabs(); GraphChanged?.Invoke();
        CommitEdit();
        int distinctEntry = execIn.Select(c => (c.ToNode, c.ToPin)).Distinct().Count();
        int distinctExit = execOut.Select(c => (c.ToNode, c.ToPin)).Distinct().Count();
        if (distinctEntry > 1) LogSink?.Invoke("⚠ 実行入口が複数あるため、関数内では最初の入口のみ接続しました (Entry.▶ は単出力)。");
        if (distinctExit > 1) LogSink?.Invoke("⚠ 実行出口が複数あるため、Call の続きは最初の出口のみ接続しました (Call.▶ は単出力)。");
        LogSink?.Invoke($"ƒ {S.Count} ノードを関数 «{fname}» に折りたたみました (引数{paramType.Count}/戻り{retType.Count})。");
    }

    /// <summary>Function Entry / Return に型付きデータピンを追加する (関数の入出力)。</summary>
    private void AddParamPin(BpNode n, bool output)
    {
        BeginEdit();
        var list = output ? n.Outputs : n.Inputs;
        int k = list.Count(p => p.Kind == PinKind.Data);
        var pin = new Pin($"arg{k}", PinKind.Data, "Float");
        if (output) n.Outputs.Add(pin); else n.Inputs.Add(pin);
        Rebuild();
        CommitEdit();
    }

    /// <summary>関数 fname のシグネチャ (Function Entry の data 出力 = 引数 / Return の data 入力 = 戻り値)。</summary>
    private (List<Pin> args, List<Pin> rets) FunctionSignature(string fname)
    {
        var args = new List<Pin>(); var rets = new List<Pin>();
        if (string.IsNullOrEmpty(fname) || !_graphTexts.TryGetValue(fname, out var body)) return (args, rets);
        string cur = "";
        foreach (var raw in body.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0) continue;
            if (line[0] == 'N') { var t = line.Split(new[] { ' ' }, 6); cur = t.Length >= 6 ? t[5] : ""; }
            else if ((line[0] == 'O' && cur == "Function Entry") || (line[0] == 'I' && cur == "Return"))
            {
                var t = line.Split(new[] { ' ' }, 3);
                if (t.Length < 3 || t[1] == "E") continue;   // exec は対象外
                var pin = ParsePin(t[1], t[2]);
                if (line[0] == 'O') args.Add(pin); else rets.Add(pin);
            }
        }
        return (args, rets);
    }

    /// <summary>Call Function ノードのピンを、呼ぶ関数のシグネチャ (引数/戻り値) に合わせて再構成する。</summary>
    private void SyncCallPins(BpNode call)
    {
        if (call.Title != "Call Function") return;
        string fname = call.Literals.TryGetValue(1, out var nm) ? nm.Trim() : "";
        var (args, rets) = FunctionSignature(fname);
        BeginEdit();
        // 既存の «引数/戻り値» ピンへの接続を一旦切る (▶ exec と name は維持: 入力 0,1 / 出力 0)。
        _conns.RemoveAll(c => (c.ToNode == call.Id && c.ToPin >= 2) || (c.FromNode == call.Id && c.FromPin >= 1));
        var name = call.Literals.TryGetValue(1, out var nmv) ? nmv : "";
        call.Inputs.Clear();  call.Inputs.Add(new Pin("▶", PinKind.Exec)); call.Inputs.Add(new Pin("name", PinKind.Data, "String"));
        foreach (var a in args) call.Inputs.Add(a);
        call.Outputs.Clear(); call.Outputs.Add(new Pin("▶", PinKind.Exec));
        foreach (var r in rets) call.Outputs.Add(r);
        call.Literals.Clear(); if (!string.IsNullOrEmpty(name)) call.Literals[1] = name;   // name 定数のみ残す
        Rebuild();
        CommitEdit();
        LogSink?.Invoke($"Call Function のピンを «{fname}» のシグネチャに同期 (引数{args.Count}/戻り{rets.Count})。");
    }

    /// <summary>ノード (選択中なら選択集合) の無効化を切り替える。</summary>
    private void ToggleDisable(BpNode hit)
    {
        BeginEdit();
        var targets = _selected.Contains(hit) ? _selected.Where(n => !n.Inherited).ToList() : new List<BpNode> { hit };
        bool to = !hit.Disabled;
        foreach (var n in targets) n.Disabled = to;
        Rebuild();
        CommitEdit();
    }

    // ----- 検索式ノードパレット (type-to-search) -----
    private Popup? _searchPopup;

    /// <summary>検索ポップアップを開く。wireFrom があればその型に «合う» ノードだけ絞り込み、選んだら自動接続。</summary>
    private void OpenNodeSearch(Point controlPos, Point graphPos, PinRef? wireFrom)
    {
        _searchPopup?.SetCurrentValue(Popup.IsOpenProperty, false);
        _menuGraphPos = graphPos;

        var box = new TextBox { FontSize = 12, Padding = new Thickness(6, 4, 6, 4), Margin = new Thickness(0, 0, 0, 4) };
        var list = new ListBox { Height = 280, BorderThickness = new Thickness(0), Background = Brushes.Transparent };
        var hint = new TextBlock {
            Text = wireFrom == null ? "ノードを検索…" : $"{(wireFrom.Output ? "出力" : "入力")} {(wireFrom.Kind == PinKind.Exec ? "exec" : wireFrom.Pin.Type)} に繋がるノード",
            Foreground = new SolidColorBrush(Color.FromRgb(0x8B, 0x93, 0xA0)), FontSize = 10.5, Margin = new Thickness(2, 0, 0, 4),
        };
        var panel = new StackPanel { Margin = new Thickness(7) };
        panel.Children.Add(hint); panel.Children.Add(box); panel.Children.Add(list);
        var border = new Border {
            Background = new SolidColorBrush(Color.FromRgb(0x20, 0x25, 0x2E)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x3A, 0x44, 0x52)), BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(5), Width = 300, Child = panel,
        };
        var pop = new Popup {
            Child = border, Placement = PlacementMode.Relative, PlacementTarget = this,
            HorizontalOffset = controlPos.X, VerticalOffset = controlPos.Y,
            StaysOpen = false, AllowsTransparency = true, PopupAnimation = PopupAnimation.Fade,
        };
        _searchPopup = pop;

        // 候補 (wireFrom があれば互換ピンを持つテンプレのみ)。
        var candidates = _palette.Where(t => wireFrom == null || MatchPinIndex(t, wireFrom) >= 0).ToList();

        void AddHeader(string cat) => list.Items.Add(new ListBoxItem {
            Content = new TextBlock { Text = cat, Foreground = new SolidColorBrush(Color.FromRgb(0x78, 0x86, 0x98)),
                FontSize = 10, FontWeight = FontWeights.SemiBold }, IsEnabled = false, Focusable = false,
            Padding = new Thickness(6, 5, 6, 1) });
        void AddItem(BpNodeTemplate t)
        {
            var sp = new StackPanel { Orientation = Orientation.Horizontal };
            sp.Children.Add(new Rectangle { Width = 9, Height = 9, RadiusX = 2, RadiusY = 2,
                Fill = new SolidColorBrush(t.Header), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 7, 0) });
            sp.Children.Add(new TextBlock { Text = t.Title, Foreground = Brushes.White, FontSize = 12, VerticalAlignment = VerticalAlignment.Center });
            list.Items.Add(new ListBoxItem { Content = sp, ToolTip = t.Category, Tag = t, Padding = new Thickness(8, 3, 6, 3) });
        }
        void Refresh()
        {
            string q = box.Text.Trim();
            list.Items.Clear();
            if (q.Length == 0)
            {
                // 検索語なし: 最近使ったノード → カテゴリ別 (見出し付き)。
                var recent = _recent.Where(r => candidates.Contains(r)).Take(5).ToList();
                if (recent.Count > 0) { AddHeader("最近使った"); foreach (var t in recent) AddItem(t); }
                foreach (var grp in candidates.Where(c => !recent.Contains(c)).GroupBy(c => c.Category))
                {
                    AddHeader(grp.Key);
                    foreach (var t in grp) AddItem(t);
                }
            }
            else   // 検索語あり: フラットに絞り込み。
                foreach (var t in candidates.Where(t => t.Title.Contains(q, StringComparison.OrdinalIgnoreCase)
                                         || t.Category.Contains(q, StringComparison.OrdinalIgnoreCase)).Take(120))
                    AddItem(t);
            for (int idx = 0; idx < list.Items.Count; idx++)   // 最初の «選べる» 項目を選択
                if (list.Items[idx] is ListBoxItem li0 && li0.Tag is BpNodeTemplate) { list.SelectedIndex = idx; break; }
        }

        void Commit(BpNodeTemplate t)
        {
            var n = SpawnFromTemplate(t, _menuGraphPos);   // 先に生成 (ワイヤ持ち上げ分も束ねて 1 手に)
            if (wireFrom != null)
            {
                int pi = MatchPinIndex(t, wireFrom);
                if (pi >= 0)
                {
                    BeginEdit();
                    bool srcIsOut = wireFrom.Output;
                    var outNode = srcIsOut ? wireFrom.Node : n;
                    int outIdx   = srcIsOut ? wireFrom.Index : pi;
                    var inNode  = srcIsOut ? n : wireFrom.Node;
                    int inIdx    = srcIsOut ? pi : wireFrom.Index;
                    AddConnection(outNode, outIdx, inNode, inIdx, wireFrom.Kind);
                    Rebuild();
                    CommitEdit();
                }
            }
            pop.IsOpen = false;
            GraphCanvas.Focus();
        }

        // カテゴリ見出し (Tag 無し) を飛ばして «選べる» 項目へ移動する。
        void Move(int dir)
        {
            int i = list.SelectedIndex;
            for (int step = 0; step < list.Items.Count; step++)
            {
                i += dir;
                if (i < 0 || i >= list.Items.Count) return;
                if (list.Items[i] is ListBoxItem li && li.Tag is BpNodeTemplate) { list.SelectedIndex = i; list.ScrollIntoView(list.SelectedItem); return; }
            }
        }
        box.TextChanged += (_, __) => Refresh();
        box.PreviewKeyDown += (_, ke) =>
        {
            if (ke.Key == Key.Down) { Move(1); ke.Handled = true; }
            else if (ke.Key == Key.Up) { Move(-1); ke.Handled = true; }
            else if (ke.Key == Key.Enter && list.SelectedItem is ListBoxItem li && li.Tag is BpNodeTemplate t) { Commit(t); ke.Handled = true; }
            else if (ke.Key == Key.Escape) { pop.IsOpen = false; ke.Handled = true; }
        };
        list.MouseDoubleClick += (_, __) => { if (list.SelectedItem is ListBoxItem li && li.Tag is BpNodeTemplate t) Commit(t); };
        pop.Opened += (_, __) => { Refresh(); box.Focus(); };
        // 閉じる時 (Esc / クリック外し) に保留中の編集 (ワイヤ持ち上げ→切断 等) を確定する。
        pop.Closed += (_, __) => { CommitEdit(); GraphCanvas.Focus(); };
        pop.IsOpen = true;
    }

    private Popup? _findPopup;
    /// <summary>グラフ内検索 (Ctrl+F): タイトルに一致するノードを選択し、最初へ寄せる。Enter で次へ巡回。</summary>
    private void OpenFindInGraph()
    {
        _findPopup?.SetCurrentValue(Popup.IsOpenProperty, false);
        var box = new TextBox { Width = 200, FontSize = 12, Padding = new Thickness(6, 3, 6, 3) };
        var info = new TextBlock { Foreground = new SolidColorBrush(Color.FromRgb(0x8B, 0x93, 0xA0)), FontSize = 10.5,
            VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 2, 0), MinWidth = 40 };
        var sp = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(8, 5, 8, 5) };
        sp.Children.Add(new TextBlock { Text = "🔍", VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0), Foreground = LabelFg });
        sp.Children.Add(box); sp.Children.Add(info);
        var border = new Border {
            Background = new SolidColorBrush(Color.FromRgb(0x20, 0x25, 0x2E)), BorderBrush = new SolidColorBrush(Color.FromRgb(0x3A, 0x44, 0x52)),
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(5), Child = sp };
        var pop = new Popup { Child = border, Placement = PlacementMode.Relative, PlacementTarget = this,
            HorizontalOffset = ActualWidth / 2 - 140, VerticalOffset = 8, StaysOpen = false, AllowsTransparency = true };
        _findPopup = pop;
        var matches = new List<BpNode>(); int mi = 0;
        void Find()
        {
            string q = box.Text.Trim();
            matches = q.Length == 0 ? new() : _nodes.Where(n => !n.Inherited && n.Title.Contains(q, StringComparison.OrdinalIgnoreCase)).ToList();
            mi = 0;
            _selected.Clear(); foreach (var m in matches) _selected.Add(m);
            info.Text = q.Length == 0 ? "" : (matches.Count == 0 ? "0 件" : $"{matches.Count} 件");
            if (matches.Count > 0) CenterOnNode(matches[0]);
            Rebuild();
        }
        box.TextChanged += (_, __) => Find();
        box.PreviewKeyDown += (_, ke) =>
        {
            if (ke.Key == Key.Enter && matches.Count > 0) { mi = (mi + 1) % matches.Count; CenterOnNode(matches[mi]); info.Text = $"{mi + 1}/{matches.Count}"; ke.Handled = true; }
            else if (ke.Key == Key.Escape) { pop.IsOpen = false; ke.Handled = true; }
        };
        pop.Opened += (_, __) => box.Focus();
        pop.Closed += (_, __) => GraphCanvas.Focus();
        pop.IsOpen = true;
    }

    /// <summary>ノード個別コメントを編集する小ポップアップ (UE5 風)。</summary>
    private void OpenNodeCommentEditor(BpNode n)
    {
        var box = new TextBox { Width = 230, FontSize = 12, Padding = new Thickness(6, 3, 6, 3), Text = n.Comment };
        var panel = new StackPanel();
        panel.Children.Add(new TextBlock { Text = "ノードコメント (Enter 確定 / Esc 取消)", Foreground = LabelFg, FontSize = 10, Margin = new Thickness(2, 0, 0, 3) });
        panel.Children.Add(box);
        var border = new Border {
            Background = new SolidColorBrush(Color.FromRgb(0x20, 0x25, 0x2E)), BorderBrush = new SolidColorBrush(Color.FromRgb(0x3A, 0x44, 0x52)),
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(5), Padding = new Thickness(7), Child = panel };
        var sc = GraphToControl(new Point(n.X, n.Y));
        var pop = new Popup { Child = border, Placement = PlacementMode.Relative, PlacementTarget = this,
            HorizontalOffset = sc.X, VerticalOffset = sc.Y - 50, StaysOpen = false, AllowsTransparency = true };
        void Commit() { BeginEdit(); n.Comment = box.Text.Trim(); Rebuild(); CommitEdit(); pop.IsOpen = false; }
        box.PreviewKeyDown += (_, ke) => {
            if (ke.Key == Key.Enter) { Commit(); ke.Handled = true; }
            else if (ke.Key == Key.Escape) { pop.IsOpen = false; ke.Handled = true; }
        };
        pop.Opened += (_, __) => { box.Focus(); box.SelectAll(); };
        pop.Closed += (_, __) => GraphCanvas.Focus();
        pop.IsOpen = true;
    }

    /// <summary>検証用: 変数名コンボのドロップダウンを開く (--bpshot varcombo)。</summary>
    public void OpenVarComboForTest()
    {
        var cb = FindEditableCombo(GraphCanvas);
        if (cb != null) { cb.Focus(); cb.IsDropDownOpen = true; }
    }
    private static System.Windows.Controls.ComboBox? FindEditableCombo(DependencyObject root)
    {
        int cnt = System.Windows.Media.VisualTreeHelper.GetChildrenCount(root);
        for (int i = 0; i < cnt; i++)
        {
            var c = System.Windows.Media.VisualTreeHelper.GetChild(root, i);
            if (c is System.Windows.Controls.ComboBox cb && cb.IsEditable) return cb;
            var r = FindEditableCombo(c);
            if (r != null) return r;
        }
        return null;
    }

    /// <summary>検証用: 既定位置で検索ポップアップを開く (--bpsearch のスクショ確認)。</summary>
    public void DebugOpenSearch() => OpenNodeSearch(new Point(200, 70), new Point(220, 150), null);

    /// <summary>検証用: グラフ検証して問題バッジ + ボタン状態を付ける (--bpshot validate のスクショ確認)。</summary>
    public void ValidateForTest() => ValidateGraph(true);
    /// <summary>検証用: ステップ実行を steps 回進める (--bpshot step)。</summary>
    public void DebugStepForTest(int steps) { for (int i = 0; i < steps; i++) OnDebugStep(this, new RoutedEventArgs()); }
    /// <summary>検証用: グラフから C++ を生成して書き出す (--bpshot gencpp)。ビルドは起こさない。</summary>
    public void GenerateCppForTest() => GenerateCppFile(build: false);
    /// <summary>検証用: Float→String の接続で変換ノードが自動挿入されることを示す (--bpshot autoconv)。</summary>
    public void AutoConvertForTest()
    {
        _nodes.Clear(); _conns.Clear(); _selected.Clear();
        BeginEdit();
        var add = AddNode(60, "Add", 130, 150, new SolidColorBrush(Color.FromRgb(0x3E, 0x6E, 0x5E)),
            new[] { new Pin("a", PinKind.Data, "Float"), new Pin("b", PinKind.Data, "Float") }, new[] { new Pin("result", PinKind.Data, "Float") });
        add.Literals[0] = "2"; add.Literals[1] = "3";
        var ev = AddNode(61, "Event  On BeginPlay", 130, 320, new SolidColorBrush(Color.FromRgb(0xB0, 0x3A, 0x46)),
            Array.Empty<Pin>(), new[] { new Pin("▶", PinKind.Exec) });
        var pr = AddNode(62, "Print String", 560, 300, new SolidColorBrush(Color.FromRgb(0x4A, 0x8C, 0x5A)),
            new[] { new Pin("▶", PinKind.Exec), new Pin("text", PinKind.Data, "String") }, new[] { new Pin("▶", PinKind.Exec) });
        _conns.Add(new BpConn(ev.Id, 0, pr.Id, 0));
        var (_, conv) = ConnectInfo("Float", "String");   // Float → String は «To String» を挟む
        if (conv != null) InsertConversion(new PinRef(add, 0, true, PinKind.Data), new PinRef(pr, 1, false, PinKind.Data), conv);
        Rebuild(); CommitEdit();
        FrameAll();
    }

    /// <summary>検証用: 最初の Function サブグラフへ切り替える (--bpshot func)。</summary>
    public void SwitchToFirstFunctionForTest()
    {
        var f = _graphOrder.FirstOrDefault(g => _graphIsFunc.Contains(g));
        if (f != null) SwitchGraph(f);
    }
    /// <summary>検証用: ビューポートタブを表示する (--bpshot viewport)。</summary>
    public void ShowViewportForTest() => ShowViewport();

    /// <summary>検証用: カメラ方位/仰角を設定してビューポートを表示する (--bpshot vpangle)。</summary>
    public void ViewportAngleForTest(double yaw, double pitch, double zoom = 1.0) { _vp3dYaw = yaw; _vp3dPitch = pitch; _vp3dZoom = zoom; ShowViewport(); }

    /// <summary>検証用: ノードのデータピンを変数に昇格する (--bpshot promote)。</summary>
    public void PromotePinForTest(int nodeId, bool output, int index)
    {
        var node = NodeById(nodeId); if (node == null) return;
        var list = output ? node.Outputs : node.Inputs;
        if (index < 0 || index >= list.Count) return;
        PromotePinToVariable(new PinRef(node, index, output, list[index].Kind));
    }

    /// <summary>検証用: ビューポートでノードを (dx,dy) 移動して再描画 (ドラッグ配置の確認。--bpshot vpmove)。</summary>
    public void MoveComponentForTest(int id, double dx, double dy)
    {
        ShowViewport(); _vpSelected = id;
        MoveComponentRow(id, dx, dy);
        RenderViewport();
    }

    /// <summary>検証用: ノード 2,3 を選択して関数に折りたたむ (--bpshot collapse)。</summary>
    public void CollapseForTest()
    {
        _selected.Clear();
        foreach (var id in new[] { 2, 3 }) { var n = NodeById(id); if (n != null) _selected.Add(n); }
        CollapseToFunction();
    }

    /// <summary>検証用: 指定ノードだけ選択する (Details の表示確認。--bpshot node&lt;id&gt;)。</summary>
    public void SelectOneForTest(int id)
    {
        var n = NodeById(id) ?? _nodes.FirstOrDefault(x => !x.Inherited);
        _selected.Clear(); if (n != null) _selected.Add(n);
        Rebuild();
    }

    /// <summary>検証用: 拡大して先頭ノード付近を中央に寄せる (細部のズレ監査用)。</summary>
    public void DebugZoomForTest(double z)
    {
        var n = _nodes.FirstOrDefault(x => !x.Inherited);
        _zoom.ScaleX = _zoom.ScaleY = z;
        if (n != null) { _pan.X = 120 - z * n.X; _pan.Y = 90 - z * n.Y; }
        UpdateGrid(); Rebuild();
    }

    /// <summary>検証用: ノード nodeId の outPin から配線ドラッグを開始した状態にする (互換ピン強調のスクショ)。</summary>
    public void DebugStartWireForTest(int nodeId, int outPin)
    {
        var n = NodeById(nodeId);
        if (n == null || outPin >= n.Outputs.Count) return;
        _wireSource = new PinRef(n, outPin, true, n.Outputs[outPin].Kind);
        Rebuild();
    }

    // ===== 自己テスト (インタプリタ + 直列化の単体テスト。--bptest から実行) =====
    /// <summary>インタプリタと直列化を一連のケースで検証し、(成功数, 失敗数, ログ) を返す。</summary>
    public (int pass, int fail, string log) SelfTest()
    {
        int pass = 0, fail = 0;
        var log = new StringBuilder();
        var savedLog = LogSink; LogSink = null;   // トレースを抑止

        // r = «id 50 の式ノード» の outPin。BeginPlay → Set Variable(r = expr) で評価する。
        string Calc(string exprLines, int outPin) =>
            "ACSBP 1\nVAR Float r 0\n" +
            "N 1 0 0 B03A46 Event BeginPlay\nO E ▶\n" +
            "N 2 0 0 6A4C8C Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\n" +
            exprLines + "C 1 0 2 0\n" + $"C 50 {outPin} 2 2\n";

        void Check(string name, string acsbp, string varName, string expected)
        {
            Deserialize(acsbp);
            RunGraph();
            string got = _vars.TryGetValue(varName, out var v) ? v : "(none)";
            bool ok = got == expected;
            if (ok) pass++; else fail++;
            log.Append(ok ? "  OK   " : "  FAIL ").Append(name).Append("  (期待=").Append(expected).Append(" 実際=").Append(got).Append(")\n");
        }

        Check("Add",        Calc("N 50 0 0 1 Add\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 2\nV 1 3\n", 0), "r", "5");
        Check("Subtract",   Calc("N 50 0 0 1 Subtract\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 7\nV 1 4\n", 0), "r", "3");
        Check("Multiply",   Calc("N 50 0 0 1 Multiply\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 6\nV 1 7\n", 0), "r", "42");
        Check("Divide",     Calc("N 50 0 0 1 Divide\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 20\nV 1 5\n", 0), "r", "4");
        Check("Modulo",     Calc("N 50 0 0 1 Modulo\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 17\nV 1 5\n", 0), "r", "2");
        Check("And(t,f)",   Calc("N 50 0 0 1 And\nI D:Bool a\nI D:Bool b\nO D:Bool result\nV 0 true\nV 1 false\n", 0), "r", "false");
        Check("Or(t,f)",    Calc("N 50 0 0 1 Or\nI D:Bool a\nI D:Bool b\nO D:Bool result\nV 0 true\nV 1 false\n", 0), "r", "true");
        Check("Not(t)",     Calc("N 50 0 0 1 Not\nI D:Bool in\nO D:Bool result\nV 0 true\n", 0), "r", "false");
        Check("MakeVector", Calc("N 50 0 0 1 Make Vector\nI D:Float x\nI D:Float y\nO D:Vector vector\nV 0 3\nV 1 4\n", 0), "r", "3,4");
        Check("BreakVec.y", Calc("N 50 0 0 1 Break Vector\nI D:Vector in\nO D:Float x\nO D:Float y\nV 0 8,9\n", 1), "r", "9");
        Check("Compare>",   Calc("N 50 0 0 1 Compare\nI E ▶\nI D a\nI D:String op\nI D b\nO E ▶\nO D:Bool result\nV 1 5\nV 2 >\nV 3 3\n", 1), "r", "true");
        Check("ArrayLen",   Calc("N 50 0 0 1 Array Length\nI D array\nO D:Int length\nV 0 a,b,c,d\n", 0), "r", "4");
        Check("ArrayGet",   Calc("N 50 0 0 1 Array Get\nI D array\nI D:Int index\nO D element\nV 0 10,20,30\nV 1 1\n", 0), "r", "20");
        Check("Select",     Calc("N 50 0 0 1 Select\nI D a\nI D b\nI D:Bool pick\nO D result\nV 0 yes\nV 1 no\nV 2 true\n", 0), "r", "yes");
        Check("ToInt",      Calc("N 50 0 0 1 To Int\nI D in\nO D:Int out\nV 0 7.9\n", 0), "r", "7");
        Check("Max",        Calc("N 50 0 0 1 Max\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 3\nV 1 8\n", 0), "r", "8");
        Check("Clamp",      Calc("N 50 0 0 1 Clamp\nI D:Float value\nI D:Float min\nI D:Float max\nO D:Float result\nV 0 12\nV 1 0\nV 2 10\n", 0), "r", "10");
        Check("Lerp",       Calc("N 50 0 0 1 Lerp\nI D:Float a\nI D:Float b\nI D:Float t\nO D:Float result\nV 0 0\nV 1 10\nV 2 0.5\n", 0), "r", "5");
        Check("Abs",        Calc("N 50 0 0 1 Abs\nI D:Float value\nO D:Float result\nV 0 -7\n", 0), "r", "7");
        Check("Append",     Calc("N 50 0 0 1 Append\nI D:String a\nI D:String b\nO D:String result\nV 0 foo\nV 1 bar\n", 0), "r", "foobar");
        // 比較 (個別演算子)。
        Check("Greater",    Calc("N 50 0 0 1 Greater\nI D:Float a\nI D:Float b\nO D:Bool result\nV 0 5\nV 1 3\n", 0), "r", "true");
        Check("LessEqual",  Calc("N 50 0 0 1 Less Equal\nI D:Float a\nI D:Float b\nO D:Bool result\nV 0 3\nV 1 3\n", 0), "r", "true");
        Check("NotEqual",   Calc("N 50 0 0 1 Not Equal\nI D:Float a\nI D:Float b\nO D:Bool result\nV 0 2\nV 1 2\n", 0), "r", "false");
        // 数学 追加。
        Check("Deg2Rad",    Calc("N 50 0 0 1 Deg To Rad\nI D:Float deg\nO D:Float rad\nV 0 180\n", 0), "r", "3.141593");
        Check("MapRange",   Calc("N 50 0 0 1 Map Range\nI D:Float value\nI D:Float inMin\nI D:Float inMax\nI D:Float outMin\nI D:Float outMax\nO D:Float result\nV 0 5\nV 1 0\nV 2 10\nV 3 0\nV 4 100\n", 0), "r", "50");
        Check("MoveToward", Calc("N 50 0 0 1 Move Towards\nI D:Float current\nI D:Float target\nI D:Float step\nO D:Float result\nV 0 0\nV 1 10\nV 2 3\n", 0), "r", "3");
        // ベクトル。
        Check("VecAdd",     Calc("N 50 0 0 1 Vector Add\nI D:Vector a\nI D:Vector b\nO D:Vector result\nV 0 1,2\nV 1 3,4\n", 0), "r", "4,6");
        Check("VecLength",  Calc("N 50 0 0 1 Vector Length\nI D:Vector v\nO D:Float length\nV 0 3,4\n", 0), "r", "5");
        Check("VecDot",     Calc("N 50 0 0 1 Vector Dot\nI D:Vector a\nI D:Vector b\nO D:Float result\nV 0 1,2\nV 1 3,4\n", 0), "r", "11");
        Check("MakeVec3",   Calc("N 50 0 0 1 Make Vector3\nI D:Float x\nI D:Float y\nI D:Float z\nO D:Vector3 vector\nV 0 1\nV 1 2\nV 2 3\n", 0), "r", "1,2,3");
        Check("ToVector3",  Calc("N 50 0 0 1 To Vector3\nI D:Vector in\nO D:Vector3 out\nV 0 7,8\n", 0), "r", "7,8,0");
        // 文字列。
        Check("Contains",   Calc("N 50 0 0 1 Contains\nI D:String in\nI D:String sub\nO D:Bool result\nV 0 hello\nV 1 ell\n", 0), "r", "true");
        Check("Substring",  Calc("N 50 0 0 1 Substring\nI D:String in\nI D:Int start\nI D:Int count\nO D:String result\nV 0 hello\nV 1 1\nV 2 3\n", 0), "r", "ell");
        Check("FormatText", Calc("N 50 0 0 1 Format Text\nI D:String format\nI D arg0\nI D arg1\nI D arg2\nO D:String result\nV 0 Hi {0} {1}\nV 1 A\nV 2 B\n", 0), "r", "Hi A B");
        Check("InRange",    Calc("N 50 0 0 1 In Range\nI D:Float value\nI D:Float min\nI D:Float max\nO D:Bool result\nV 0 5\nV 1 0\nV 2 10\n", 0), "r", "true");
        Check("InRangeEx",  Calc("N 50 0 0 1 In Range Exclusive\nI D:Float value\nI D:Float min\nI D:Float max\nO D:Bool result\nV 0 0\nV 1 0\nV 2 10\n", 0), "r", "false");
        Check("IsValid",    Calc("N 50 0 0 1 Is Valid\nI D:Object object\nO D:Bool result\nV 0 self\n", 0), "r", "true");

        // For Loop: 最終 index = 3
        Check("ForLoop", "ACSBP 1\nVAR Float r 0\nN 1 0 0 1 Event BeginPlay\nO E ▶\n" +
            "N 2 0 0 1 For Loop\nI E ▶\nI D:Int first\nI D:Int last\nO E Loop Body\nO D:Int index\nO E Completed\nV 1 1\nV 2 3\n" +
            "N 3 0 0 1 Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\n" +
            "C 1 0 2 0\nC 2 0 3 0\nC 2 1 3 2\n", "r", "3");
        // ForEach: 最終 element
        Check("ForEach", "ACSBP 1\nVAR Float r 0\nN 1 0 0 1 Event BeginPlay\nO E ▶\n" +
            "N 2 0 0 1 ForEach\nI E ▶\nI D array\nO E Loop Body\nO D element\nO D:Int index\nO E Completed\nV 1 5,10,15\n" +
            "N 3 0 0 1 Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\n" +
            "C 1 0 2 0\nC 2 0 3 0\nC 2 1 3 2\n", "r", "15");
        // Branch: cond=true → True 枝
        Check("Branch", "ACSBP 1\nVAR Float r 0\nN 1 0 0 1 Event BeginPlay\nO E ▶\n" +
            "N 2 0 0 1 Branch\nI E ▶\nI D:Bool cond\nO E True\nO E False\nV 1 true\n" +
            "N 3 0 0 1 Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\nV 2 1\n" +
            "N 4 0 0 1 Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\nV 2 2\n" +
            "C 1 0 2 0\nC 2 0 3 0\nC 2 1 4 0\n", "r", "1");
        // クロスグラフ: Call Function が共有変数を設定
        Check("CallFunc", "ACSBP 1\nVAR Int r 0\nN 1 0 0 1 Event BeginPlay\nO E ▶\n" +
            "N 2 0 0 1 Call Function\nI E ▶\nI D:String name\nO E ▶\nV 1 Compute\n" +
            "C 1 0 2 0\nGRAPH Compute F\nN 100 0 0 1 Function Entry\nO E ▶\n" +
            "N 101 0 0 1 Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\nV 2 99\nC 100 0 101 0\n", "r", "99");
        // 型付き引数/戻り値: Square(6) = 36 を Call の戻り値から受け取る
        Check("FuncArgRet", "ACSBP 1\nVAR Float r 0\nN 1 0 0 1 Event BeginPlay\nO E ▶\n" +
            "N 2 0 0 1 Call Function\nI E ▶\nI D:String name\nI D:Float arg0\nO E ▶\nO D:Float ret0\nV 1 Square\nV 2 6\n" +
            "N 3 0 0 1 Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\n" +
            "C 1 0 2 0\nC 2 0 3 0\nC 2 1 3 2\n" +
            "GRAPH Square F\nN 100 0 0 1 Function Entry\nO E ▶\nO D:Float arg0\n" +
            "N 101 0 0 1 Multiply\nI D:Float a\nI D:Float b\nO D:Float result\n" +
            "N 102 0 0 1 Return\nI E ▶\nI D:Float ret0\n" +
            "C 100 0 102 0\nC 100 1 101 0\nC 100 1 101 1\nC 101 0 102 1\n", "r", "36");

        // Collapse to Function: exec ノード(Set Variable)を関数化しても結果が一致する (Add は外部の引数になる)。
        {
            Deserialize("ACSBP 1\nVAR Float r 0\n" +
                "N 1 0 0 1 Event BeginPlay\nO E ▶\n" +
                "N 2 0 0 1 Set Variable\nI E ▶\nI D:String name\nI D value\nO E ▶\nV 1 r\n" +
                "N 3 0 0 1 Add\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 2\nV 1 3\n" +
                "C 1 0 2 0\nC 3 0 2 2\n");
            var setNode = NodeById(2);
            _selected.Clear(); if (setNode != null) _selected.Add(setNode);
            CollapseToFunction();
            RunGraph();
            string got = _vars.TryGetValue("r", out var cv) ? cv : "(none)";
            bool ok = got == "5";
            if (ok) pass++; else fail++;
            log.Append(ok ? "  OK   " : "  FAIL ").Append("CollapseToFunc  (期待=5 実際=").Append(got).Append(")\n");
        }

        // 直列化の往復: Serialize→Deserialize→Serialize が一致する
        {
            Deserialize(Calc("N 50 0 0 1 Add\nI D:Float a\nI D:Float b\nO D:Float result\nV 0 2\nV 1 3\n", 0));
            string s1 = Serialize();
            Deserialize(s1);
            string s2 = Serialize();
            bool ok = s1 == s2;
            if (ok) pass++; else fail++;
            log.Append(ok ? "  OK   " : "  FAIL ").Append("Serialize 往復").Append('\n');
        }

        LogSink = savedLog;
        log.Insert(0, $"[BP SelfTest] passed={pass} failed={fail}\n");
        return (pass, fail, log.ToString());
    }

    /// <summary>テンプレ t が wireFrom と繋げられる «反対側» ピンの index。無ければ -1。</summary>
    private static int MatchPinIndex(BpNodeTemplate t, PinRef wireFrom)
    {
        var pins = wireFrom.Output ? t.Inputs : t.Outputs;   // 出力からは入力ピンへ、入力からは出力ピンへ
        for (int i = 0; i < pins.Length; i++)
        {
            var p = pins[i];
            if (wireFrom.Kind == PinKind.Exec) { if (p.Exec) return i; }
            else if (!p.Exec)
            {
                bool ok = wireFrom.Output ? CanConnectData(wireFrom.Pin.Type, p.Type) : CanConnectData(p.Type, wireFrom.Pin.Type);
                if (ok) return i;
            }
        }
        return -1;
    }

    private BpNode SpawnFromTemplate(BpNodeTemplate t, Point g)
    {
        BeginEdit();
        var n = new BpNode { Id = _nextId++, Title = t.Title, X = g.X, Y = g.Y, Header = new SolidColorBrush(t.Header) };
        foreach (var p in t.Inputs)  n.Inputs.Add(new Pin(p.Name, p.Exec ? PinKind.Exec : PinKind.Data, p.Type));
        foreach (var p in t.Outputs) n.Outputs.Add(new Pin(p.Name, p.Exec ? PinKind.Exec : PinKind.Data, p.Type));
        _nodes.Add(n);
        _selected.Clear(); _selected.Add(n);
        _recent.Remove(t); _recent.Insert(0, t);   // 最近使ったノード
        Rebuild();
        CommitEdit();
        return n;
    }

    /// <summary>選択ノードを複製する (少しずらして配置、内部の接続も引き継ぐ)。</summary>
    private void DuplicateSelection()
    {
        if (_selected.Count == 0) return;
        BeginEdit();
        var map = new Dictionary<int, BpNode>();
        var newSel = new List<BpNode>();
        foreach (var n in _selected.Where(x => !x.Inherited).ToList())
        {
            var c = new BpNode { Id = _nextId++, Title = n.Title, X = n.X + 28, Y = n.Y + 28, Header = n.Header, VarRef = n.VarRef, Comment = n.Comment };
            c.Inputs.AddRange(n.Inputs);    // Pin は record(不変)なので参照共有で安全
            c.Outputs.AddRange(n.Outputs);
            foreach (var kv in n.Literals) c.Literals[kv.Key] = kv.Value;
            _nodes.Add(c); map[n.Id] = c; newSel.Add(c);
        }
        // 選択内部の接続を複製。
        foreach (var k in _conns.ToList())
            if (map.TryGetValue(k.FromNode, out var fn) && map.TryGetValue(k.ToNode, out var tn))
                _conns.Add(new BpConn(fn.Id, k.FromPin, tn.Id, k.ToPin));
        _selected.Clear(); foreach (var n in newSel) _selected.Add(n);
        Rebuild();
        CommitEdit();
    }

    // ----- クリップボード (Copy/Paste) -----
    private static string _clipboard = "";

    private void CopySelection()
    {
        var set = _selected.Where(n => !n.Inherited).ToList();
        if (set.Count == 0) return;
        _clipboard = SerializeNodes(set);
    }

    private void PasteClipboard()
    {
        if (string.IsNullOrEmpty(_clipboard)) return;
        BeginEdit();
        var pasted = DeserializeNodes(_clipboard, offsetX: 30, offsetY: 30);
        if (pasted.Count > 0) { _selected.Clear(); foreach (var n in pasted) _selected.Add(n); }
        Rebuild();
        CommitEdit();
    }

    // ----- コメント枠 -----
    private FrameworkElement MakeCommentVisual(BpComment k)
    {
        var cv = new Canvas { Width = k.W, Height = k.H, ClipToBounds = false, Opacity = k.Inherited ? 0.5 : 1.0 };
        Canvas.SetLeft(cv, k.X); Canvas.SetTop(cv, k.Y);
        SizeCommentVisual(cv, k);
        if (!k.Inherited)
        {
            // ヘッダのドラッグ = コメント + 内包ノードを移動。
            var header = (Border)cv.Children[1];
            header.MouseLeftButtonDown += (_, e) =>
            {
                BeginEdit();
                _dragComment = k; _commentLast = e.GetPosition(GraphCanvas);
                _commentCarry = NodesInComment(k);
                GraphCanvas.CaptureMouse(); e.Handled = true;
            };
            header.MouseRightButtonUp += (_, e) =>
            {
                var menu = new ContextMenu();
                var ren = new MenuItem { Header = "名前を変更" };
                ren.Click += (_, __) => EditCommentTitle(k);
                var col = new MenuItem { Header = "色を変える" };
                col.Click += (_, __) => { BeginEdit(); CycleCommentColor(k); Rebuild(); CommitEdit(); };
                var del = new MenuItem { Header = "コメントを削除" };
                del.Click += (_, __) => { BeginEdit(); _comments.Remove(k); Rebuild(); CommitEdit(); };
                menu.Items.Add(ren); menu.Items.Add(col); menu.Items.Add(del);
                menu.IsOpen = true; e.Handled = true;
            };
            header.MouseLeftButtonDown += (_, e) => { if (e.ClickCount == 2) { EditCommentTitle(k); e.Handled = true; } };
            // 右下のリサイズグリップ。
            var grip = (Path)cv.Children[3];
            grip.MouseLeftButtonDown += (_, e) => { BeginEdit(); _resizeComment = k; _commentLast = e.GetPosition(GraphCanvas); GraphCanvas.CaptureMouse(); e.Handled = true; };
        }
        return cv;
    }

    // コメント枠の見た目 (body / header / title / grip) を (再)構成する。
    private void SizeCommentVisual(Canvas cv, BpComment k)
    {
        cv.Children.Clear();
        cv.Width = k.W; cv.Height = k.H;
        var body = new Border {
            Width = k.W, Height = k.H, CornerRadius = new CornerRadius(5),
            Background = new SolidColorBrush(Color.FromArgb(0x22, k.Color.R, k.Color.G, k.Color.B)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(0xAA, k.Color.R, k.Color.G, k.Color.B)),
            BorderThickness = new Thickness(1.4), IsHitTestVisible = false,   // body はクリックを下へ通す
        };
        var header = new Border {
            Width = k.W, Height = 22, Cursor = Cursors.SizeAll,
            Background = new SolidColorBrush(Color.FromArgb(0xCC, k.Color.R, k.Color.G, k.Color.B)),
            CornerRadius = new CornerRadius(5, 5, 0, 0),
        };
        var title = new TextBlock {
            Text = k.Text, Foreground = Brushes.White, FontSize = 11.5, FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(8, 3, 8, 0), IsHitTestVisible = false,
            TextTrimming = TextTrimming.CharacterEllipsis, Width = k.W - 16,
        };
        var grip = new Path {
            Data = Geometry.Parse($"M {k.W - 14},{k.H} L {k.W},{k.H} L {k.W},{k.H - 14} Z"),
            Fill = new SolidColorBrush(Color.FromArgb(0x99, k.Color.R, k.Color.G, k.Color.B)), Cursor = Cursors.SizeNWSE,
        };
        cv.Children.Add(body);      // index 0
        cv.Children.Add(header);    // index 1
        cv.Children.Add(title);     // index 2
        cv.Children.Add(grip);      // index 3
        Canvas.SetLeft(title, 0); Canvas.SetTop(title, 0);
    }

    private List<BpNode> NodesInComment(BpComment k)
    {
        var rect = new Rect(k.X, k.Y, k.W, k.H);
        var res = new List<BpNode>();
        foreach (var n in _nodes)
        {
            if (n.Inherited) continue;
            var cx = n.X + NodeW / 2.0; var cy = n.Y + NodeHeight(n) / 2.0;
            if (rect.Contains(cx, cy)) res.Add(n);
        }
        return res;
    }

    private void EditCommentTitle(BpComment k)
    {
        if (k.Visual is not Canvas cv) return;
        var edit = new TextBox { Text = k.Text, Width = k.W - 10, FontSize = 11.5, Background = Brushes.White, Foreground = Brushes.Black };
        Canvas.SetLeft(edit, 4); Canvas.SetTop(edit, 1);
        cv.Children.Add(edit);
        edit.Focus(); edit.SelectAll();
        void Done() { BeginEdit(); k.Text = edit.Text; Rebuild(); CommitEdit(); }
        edit.LostKeyboardFocus += (_, __) => Done();
        edit.KeyDown += (_, ke) => { if (ke.Key == Key.Enter) { Done(); ke.Handled = true; } else if (ke.Key == Key.Escape) Rebuild(); };
    }

    private static readonly Color[] CommentColors =
    {
        Color.FromRgb(0x2E, 0x6E, 0x8A), Color.FromRgb(0x6A, 0x4C, 0x8C), Color.FromRgb(0x35, 0x7A, 0x55),
        Color.FromRgb(0x8A, 0x5C, 0x2E), Color.FromRgb(0xB0, 0x3A, 0x46), Color.FromRgb(0x5A, 0x64, 0x72),
    };
    private void CycleCommentColor(BpComment k)
    {
        int i = Array.FindIndex(CommentColors, c => c == k.Color);
        k.Color = CommentColors[(i + 1) % CommentColors.Length];
    }

    /// <summary>選択ノードを囲むコメント枠を作る (選択が空なら画面中央に既定枠)。C キー。</summary>
    private void WrapSelectionInComment()
    {
        BeginEdit();
        var k = new BpComment { Id = _nextCommentId++, Color = CommentColors[(_nextCommentId) % CommentColors.Length] };
        var sel = _selected.Where(n => !n.Inherited).ToList();
        if (sel.Count > 0)
        {
            double minX = sel.Min(n => n.X), minY = sel.Min(n => n.Y);
            double maxX = sel.Max(n => n.X + NodeW), maxY = sel.Max(n => n.Y + NodeHeight(n));
            k.X = minX - 16; k.Y = minY - 34; k.W = (maxX - minX) + 32; k.H = (maxY - minY) + 50;
        }
        else
        {
            var center = new Point((ActualWidth / 2 - _pan.X) / _zoom.ScaleX, (ActualHeight / 2 - _pan.Y) / _zoom.ScaleY);
            k.X = center.X - 110; k.Y = center.Y - 70;
        }
        _comments.Add(k);
        Rebuild();
        CommitEdit();
        EditCommentTitle(k);   // UE 風: 作成直後にタイトルを即編集
    }

    // ----- 変数 D&D (My Blueprint パネルから) -----
    public const string VarDragFormat = "ACSBP_VAR";

    private void OnDragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(VarDragFormat) ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private void OnDrop(object sender, DragEventArgs e)
    {
        if (!e.Data.GetDataPresent(VarDragFormat)) return;
        string name = ((e.Data.GetData(VarDragFormat) as string) ?? "").Trim();
        if (string.IsNullOrWhiteSpace(name)) return;
        Point g = e.GetPosition(GraphCanvas);
        // UE 風: ドロップ時に Get / Set を選ぶメニュー。
        var menu = new ContextMenu();
        var get = new MenuItem { Header = $"Get {name}" };
        get.Click += (_, __) => DropVariable(name, false, g);
        var setm = new MenuItem { Header = $"Set {name}" };
        setm.Click += (_, __) => DropVariable(name, true, g);
        menu.Items.Add(get); menu.Items.Add(setm);
        menu.PlacementTarget = GraphCanvas; menu.IsOpen = true;
    }

    /// <summary>変数 name の Get/Set ノードを生成して追加し、返す (接続は呼び元が行う)。</summary>
    private BpNode MakeVarNode(string name, bool set, string type, Point g)
    {
        var vbl = new SolidColorBrush(Color.FromRgb(0x6A, 0x4C, 0x8C));
        BpNode n;
        if (set)   // UE 風: 変数を VarRef に焼き込み、value は変数の型。name ピンは持たない。
        {
            n = new BpNode { Id = _nextId++, Title = "Set Variable", X = g.X, Y = g.Y, Header = vbl, VarRef = name };
            n.Inputs.Add(new Pin("▶", PinKind.Exec));
            n.Inputs.Add(new Pin("value", PinKind.Data, type));
            n.Outputs.Add(new Pin("▶", PinKind.Exec));
        }
        else
        {
            n = new BpNode { Id = _nextId++, Title = "Get Variable", X = g.X, Y = g.Y, Header = vbl, VarRef = name };
            n.Outputs.Add(new Pin("value", PinKind.Data, type));
        }
        _nodes.Add(n);
        return n;
    }

    /// <summary>変数 name の Get/Set ノードを生成する (D&D 落下点に)。</summary>
    private void DropVariable(string name, bool set, Point g)
    {
        BeginEdit();
        string type = Variables.FirstOrDefault(v => v.Name == name)?.Type ?? "";
        var n = MakeVarNode(name, set, type, g);
        _selected.Clear(); _selected.Add(n);
        Rebuild();
        CommitEdit();
    }

    /// <summary>一意な変数名を作る (base, base2, base3 …)。</summary>
    private string UniqueVarName(string baseName)
    {
        if (string.IsNullOrWhiteSpace(baseName) || baseName == "value") baseName = "NewVar";
        string nm = baseName; int k = 2;
        while (Variables.Any(v => v.Name == nm)) nm = baseName + k++;
        return nm;
    }

    /// <summary>データピンを «変数に昇格»: 同型の変数を作り Get/Set ノードを生成して繋ぐ (UE5 風)。</summary>
    private void PromotePinToVariable(PinRef p)
    {
        var pin = p.Output ? p.Node.Outputs[p.Index] : p.Node.Inputs[p.Index];
        if (pin.Kind != PinKind.Data) return;
        string type = string.IsNullOrEmpty(pin.Type) ? "Float" : pin.Type;
        string vname = UniqueVarName(pin.Name);
        BeginEdit();
        Variables.Add(new BpVar(vname, type, ""));
        VariablesChanged?.Invoke();
        var pp = PinPos(p.Node, p.Output, p.Index);
        if (p.Output)   // 出力 → Set Variable を右に置き、value へ
        {
            var n = MakeVarNode(vname, true, type, new Point(pp.X + 80, pp.Y - 11));
            _conns.Add(new BpConn(p.Node.Id, p.Index, n.Id, 2));
        }
        else            // 入力 → Get Variable を左に置き、value から繋ぐ (既存接続は置換)
        {
            var n = MakeVarNode(vname, false, type, new Point(pp.X - 210, pp.Y - 33));
            _conns.RemoveAll(c => c.ToNode == p.Node.Id && c.ToPin == p.Index);
            _conns.Add(new BpConn(n.Id, 0, p.Node.Id, p.Index));
            _selected.Clear(); _selected.Add(n);
        }
        Rebuild();
        CommitEdit();
    }

    // ----- Undo / Redo (スナップショット式: グラフ全体を .acsbp テキストへ直列化して保持) -----
    private readonly Stack<string> _undo = new();
    private readonly Stack<string> _redo = new();
    private bool    _restoring;        // 復元中は新たなスナップショットを取らない
    private string? _pendingUndo;      // 操作開始時の状態 (実際に変化したらコミット)

    private void BeginEdit()
    {
        if (_showWatch) { _showWatch = false; _watch.Clear(); _firedOut.Clear(); }   // 編集を始めたらデバッグ表示を消す
        ResetValidateStatus();   // 編集で検証結果は古くなる
        if (!_restoring && _pendingUndo == null) _pendingUndo = Serialize();
    }
    private void CancelEdit() { _pendingUndo = null; }
    private void CommitEdit()
    {
        if (_pendingUndo == null) return;
        if (_pendingUndo != Serialize()) { _undo.Push(_pendingUndo); _redo.Clear(); if (_undo.Count > 100) TrimUndo(); }
        _pendingUndo = null;
    }
    private void TrimUndo() { var keep = _undo.ToArray().Take(100).Reverse().ToArray(); _undo.Clear(); foreach (var s in keep) _undo.Push(s); }

    private void Undo()
    {
        if (_undo.Count == 0) return;
        _redo.Push(Serialize());
        Restore(_undo.Pop());
    }
    private void Redo()
    {
        if (_redo.Count == 0) return;
        _undo.Push(Serialize());
        Restore(_redo.Pop());
    }
    private void Restore(string snapshot)
    {
        _restoring = true;
        try { LoadGraph(snapshot); }
        finally { _restoring = false; }
    }

    // ----- シリアライズ (.acsbp。行ベースのテキスト形式) -----
    //   ACSBP 1
    //   N <id> <x> <y> <RRGGBB> <title...>     ← title は行末まで (空白可)
    //   I <E|D|D:Type> <name...>                ← 直前 N の入力ピン (型は任意)
    //   O <E|D|D:Type> <name...>                ← 直前 N の出力ピン
    //   V <pin> <value...>                       ← 直前 N の入力定数
    //   C <fromNode> <fromPin> <toNode> <toPin>
    //   K <id> <x> <y> <w> <h> <RRGGBB> <text...>  ← コメント枠

    /// <summary>初期ディレクトリ (保存/開くダイアログ)。MainWindow がプロジェクトの Assets を設定。</summary>
    public string? DefaultDir { get; set; }
    /// <summary>現在開いている .acsbp のパス (保存ダイアログを省いて上書き保存するため)。</summary>
    public string? CurrentPath { get; private set; }
    public Action? PathChanged;   // CurrentPath が変わった → ウィンドウタイトル更新用

    /// <summary>現在のグラフを .acsbp テキストへ直列化する (Event Graph + Function サブグラフ)。</summary>
    public string Serialize()
    {
        SyncActiveGraph();   // 現在編集中のグラフ本体を _graphTexts へ書き戻す
        var sb = new StringBuilder();
        sb.Append("ACSBP 1\n");
        if (!string.IsNullOrEmpty(_parentPath)) sb.Append("PARENT ").Append(_parentPath).Append('\n');   // 継承元
        foreach (var v in Variables)   // BP 変数: VAR <type> <name> <value…> (+ メタは VMETA <name> <e> <category>)
            if (!string.IsNullOrWhiteSpace(v.Name))
            {
                sb.Append("VAR ").Append(string.IsNullOrEmpty(v.Type) ? "String" : v.Type)
                  .Append(' ').Append(v.Name.Trim()).Append(' ').Append(v.Value ?? "").Append('\n');
                if (v.InstanceEditable || !string.IsNullOrEmpty(v.Category))
                    sb.Append("VMETA ").Append(v.Name.Trim()).Append(' ').Append(v.InstanceEditable ? '1' : '0')
                      .Append(' ').Append(v.Category ?? "").Append('\n');
                if (!string.IsNullOrEmpty(v.Tooltip))
                    sb.Append("VTIP ").Append(v.Name.Trim()).Append(' ').Append(v.Tooltip.Replace("\n", " ")).Append('\n');
            }
        if (!string.IsNullOrEmpty(ComponentsText))   // コンポーネント木: CMP <行数> + 逐語の ACSCENE 行
            AcsbpFormat.AppendCmpBlock(sb, ComponentsText);
        sb.Append(_graphTexts.TryGetValue(EventGraphName, out var eg) ? eg : "");   // Event Graph 本体
        foreach (var name in _graphOrder)   // Function サブグラフ
        {
            if (name == EventGraphName) continue;
            sb.Append("GRAPH ").Append(name).Append(_graphIsFunc.Contains(name) ? " F" : " E").Append('\n');
            sb.Append(_graphTexts.TryGetValue(name, out var gb) ? gb : "");
        }
        return sb.ToString();
    }

    /// <summary>編集中グラフ (_nodes/_conns/_comments) の本体テキスト (N/I/O/V/X/C/K) を作る。</summary>
    private string SerializeGraphBody()
    {
        var sb = new StringBuilder();
        foreach (var n in _nodes)
        {
            if (n.Inherited) continue;   // 継承ノードは親側が持つので保存しない
            var c = (n.Header as SolidColorBrush)?.Color ?? Colors.SteelBlue;
            sb.Append("N ").Append(n.Id).Append(' ')
              .Append(n.X.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append(n.Y.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append($"{c.R:X2}{c.G:X2}{c.B:X2}").Append(' ').Append(n.Title).Append('\n');
            foreach (var p in n.Inputs)  sb.Append("I ").Append(PinToken(p)).Append(' ').Append(p.Name).Append('\n');
            foreach (var p in n.Outputs) sb.Append("O ").Append(PinToken(p)).Append(' ').Append(p.Name).Append('\n');
            foreach (var kv in n.Literals)
                if (!string.IsNullOrEmpty(kv.Value)) sb.Append("V ").Append(kv.Key).Append(' ').Append(kv.Value).Append('\n');
            if (n.Disabled) sb.Append("X\n");   // 無効化フラグ (直前 N に係る)
            if (!string.IsNullOrEmpty(n.Comment)) sb.Append("NC ").Append(n.Comment.Replace("\n", " ")).Append('\n');   // ノードコメント
            if (!string.IsNullOrEmpty(n.VarRef)) sb.Append("VR ").Append(n.VarRef).Append('\n');   // 束縛変数 (Get/Set Variable)
        }
        foreach (var k in _conns)
            if (k.FromNode < InheritOffset && k.ToNode < InheritOffset)   // 継承ノード間の線は親側が持つ
                sb.Append("C ").Append(k.FromNode).Append(' ').Append(k.FromPin).Append(' ')
                  .Append(k.ToNode).Append(' ').Append(k.ToPin).Append('\n');
        foreach (var k in _comments)
        {
            if (k.Inherited) continue;
            sb.Append("K ").Append(k.Id).Append(' ')
              .Append(k.X.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append(k.Y.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append(k.W.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append(k.H.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append($"{k.Color.R:X2}{k.Color.G:X2}{k.Color.B:X2}").Append(' ').Append(k.Text.Replace('\n', ' ')).Append('\n');
        }
        return sb.ToString();
    }

    private void SyncActiveGraph() => _graphTexts[_activeName] = SerializeGraphBody();

    // ピン種別トークン: exec=E / data=D / 型付き data=D:Type。
    private static string PinToken(Pin p) => p.Kind == PinKind.Exec ? "E"
        : string.IsNullOrEmpty(p.Type) ? "D" : "D:" + p.Type;

    /// <summary>.acsbp テキストからグラフを復元する (既存は破棄)。Undo/読込で共通。</summary>
    public void Deserialize(string text) => LoadGraph(text);

    private void LoadGraph(string text)
    {
        _nodes.Clear(); _conns.Clear(); _comments.Clear(); _selected.Clear(); _parentPath = null;
        _dragNode = null; _wireSource = null; _ghost = null;
        Variables.Clear();
        ComponentsText = "";
        _graphTexts.Clear(); _graphOrder.Clear(); _graphOrder.Add(EventGraphName);
        _graphIsFunc.Clear(); _activeName = EventGraphName;
        _breakpoints.Clear(); _stepIdx = -1; _debugCurrent = -1;

        // CMP (コンポーネント木の逐語 ACSCENE) を抜き出し、残り行をグラフ/変数として解析する
        // (ACSCENE の COMP/数値行が グラフ parser の C/N と衝突するため先に除去する)。
        (ComponentsText, text) = AcsbpFormat.SplitCmp(text);

        // GRAPH <name> <F|E> セクションを分離する (先頭 = Event Graph、以降 = Function サブグラフ)。
        var (mainText, funcs) = SplitGraphs(text);
        foreach (var (name, isFunc, body) in funcs)
        {
            if (!_graphTexts.ContainsKey(name)) { _graphOrder.Add(name); if (isFunc) _graphIsFunc.Add(name); }
            _graphTexts[name] = body;
        }

        foreach (var raw in mainText.Split('\n'))   // VAR <type> <name> <value…> (旧 BB <name> <value…> も読む)
        {
            var l = raw.TrimEnd('\r');
            if (l.StartsWith("VAR "))
            {
                var t = l.Split(new[] { ' ' }, 4);
                if (t.Length >= 3) Variables.Add(new BpVar(t[2], t[1], t.Length >= 4 ? t[3] : ""));
            }
            else if (l.StartsWith("BB "))
            {
                var t = l.Split(new[] { ' ' }, 3);
                if (t.Length >= 2) Variables.Add(new BpVar(t[1], "String", t.Length >= 3 ? t[2] : ""));
            }
            else if (l.StartsWith("VMETA "))
            {
                var t = l.Split(new[] { ' ' }, 4);
                var vv = t.Length >= 3 ? Variables.FirstOrDefault(x => x.Name == t[1]) : null;
                if (vv != null) { vv.InstanceEditable = t[2] == "1"; vv.Category = t.Length >= 4 ? t[3] : ""; }
            }
            else if (l.StartsWith("VTIP "))
            {
                var t = l.Split(new[] { ' ' }, 3);
                var vv = t.Length >= 2 ? Variables.FirstOrDefault(x => x.Name == t[1]) : null;
                if (vv != null && t.Length >= 3) vv.Tooltip = t[2];
            }
        }

        // PARENT 行を先に処理し、親グラフを «継承ノード» (id +InheritOffset, 読取専用) として読み込む。
        foreach (var raw in mainText.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.StartsWith("PARENT "))
            {
                _parentPath = line.Substring(7).Trim();
                LoadInherited();
                break;
            }
        }
        ParseGraph(mainText, 0, inherited: false);   // 子 (自分=Event Graph) のノード
        _graphTexts[EventGraphName] = SerializeGraphBody();   // 切替で戻れるよう本体を保存

        RecomputeNextIds();
        BuildGraphTabs();
        Rebuild();
        VariablesChanged?.Invoke();   // パネルを新しい変数で再描画
        ComponentsChanged?.Invoke();  // Components パネルを再描画
        GraphChanged?.Invoke();       // GRAPHS パネルを再描画
    }

    private void LoadInherited()
    {
        if (string.IsNullOrEmpty(_parentPath)) return;
        string? full = ResolveParentPath(_parentPath);
        if (full != null && System.IO.File.Exists(full))
        {
            var (pm, _) = SplitGraphs(AcsbpFormat.SplitCmp(System.IO.File.ReadAllText(full)).Item2);
            ParseGraph(pm, InheritOffset, inherited: true);
        }
    }

    private void RecomputeNextIds()
    {
        int maxId = 0; foreach (var n in _nodes) if (!n.Inherited && n.Id > maxId) maxId = n.Id;
        _nextId = Math.Max(_nextId, maxId + 1);   // 以後の生成 ID が読込ノードと衝突しないように
        int maxK = 0; foreach (var k in _comments) if (!k.Inherited && k.Id > maxK) maxK = k.Id;
        _nextCommentId = Math.Max(_nextCommentId, maxK + 1);
    }

    /// <summary>GRAPH セクションを分離する。先頭 (最初の GRAPH 行より前) = main、以降 = (名前, 関数か, 本体)。</summary>
    private static (string main, List<(string name, bool func, string body)> graphs) SplitGraphs(string text)
    {
        var graphs = new List<(string, bool, string)>();
        var main = new StringBuilder();
        StringBuilder? cur = null; string curName = ""; bool curFunc = false;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.StartsWith("GRAPH "))
            {
                if (cur != null) graphs.Add((curName, curFunc, cur.ToString()));
                var t = line.Split(new[] { ' ' }, 3);
                curName = t.Length >= 2 ? t[1] : "Function";
                curFunc = t.Length < 3 || t[2].Trim() != "E";   // 既定は関数
                cur = new StringBuilder();
            }
            else if (cur != null) cur.Append(line).Append('\n');
            else main.Append(line).Append('\n');
        }
        if (cur != null) graphs.Add((curName, curFunc, cur.ToString()));
        return (main.ToString(), graphs);
    }

    // ----- グラフタブ (ビューポート + Event Graph + Function の切替) -----
    private bool _viewportMode;
    private int _vpSelected = -1;   // ビューポートで強調中のコンポーネントノード id
    private int _vpDragId = -1;     // ビューポートでドラッグ中のノード id
    private Point _vpDragLast;
    // ----- 3D ビューポート (透視投影 + オービット) -----
    private double _vp3dYaw = 0.85, _vp3dPitch = 0.5, _vp3dZoom = 1.0;   // カメラ方位/仰角/ズーム
    private bool _vp3dOrbiting;     // 空き場所ドラッグでカメラ回転中
    private double _vp3dGrabOX, _vp3dGrabOY;   // コンポーネントドラッグ時の «つかみオフセット» (2D)
    // カメラ基底 (毎描画で更新。Unproject が再利用)。
    private double _cEyeX, _cEyeY, _cEyeZ, _cFwdX, _cFwdY, _cFwdZ, _cRgtX, _cRgtY, _cRgtZ, _cUpX, _cUpY, _cUpZ, _cFocal, _cVW, _cVH, _cDist;

    /// <summary>COMPONENTS パネルからの選択: ビューポートを開き、そのノードを強調する。</summary>
    public void HighlightViewportNode(int nodeId)
    {
        _vpSelected = nodeId;
        if (!_viewportMode) ShowViewport(); else RenderViewport();
    }

    private void BuildGraphTabs()
    {
        if (GraphTabs == null) return;
        GraphTabs.Children.Clear();
        var activeBg = new SolidColorBrush(Color.FromRgb(0x2E, 0x5C, 0x8A));
        // ビューポートタブ (UE5 風: コンポーネントの見た目/当たり判定プレビュー)。
        var vp = new Button {
            Content = "⊞ ビューポート", Padding = new Thickness(10, 2, 10, 2), Margin = new Thickness(0, 0, 3, 0), FontSize = 11.5,
            Background = _viewportMode ? activeBg : Brushes.Transparent, Foreground = Brushes.White,
            BorderThickness = new Thickness(_viewportMode ? 0 : 1), ToolTip = "BP が生成するコンポーネントの見た目と当たり判定",
        };
        vp.Click += (_, __) => ShowViewport();
        GraphTabs.Children.Add(vp);
        foreach (var name in _graphOrder)
        {
            string n = name;
            bool active = !_viewportMode && n == _activeName;
            var btn = new Button {
                Content = (_graphIsFunc.Contains(n) ? "ƒ " : "▦ ") + n,
                Padding = new Thickness(10, 2, 10, 2), Margin = new Thickness(0, 0, 3, 0), FontSize = 11.5,
                Background = active ? activeBg : Brushes.Transparent,
                Foreground = Brushes.White, BorderThickness = new Thickness(active ? 0 : 1),
            };
            btn.Click += (_, __) => { ExitViewport(); SwitchGraph(n); };
            if (_graphIsFunc.Contains(n))   // 関数タブは右クリックで削除
                btn.MouseRightButtonUp += (_, e) => { DeleteFunction(n); e.Handled = true; };
            GraphTabs.Children.Add(btn);
        }
        var add = new Button { Content = "＋ 関数", Padding = new Thickness(9, 2, 9, 2), FontSize = 11.5,
            Foreground = (Brush)(TryFindResource("OkFg") ?? Brushes.LightGreen), Background = Brushes.Transparent };
        add.Click += (_, __) => { ExitViewport(); AddFunction(); };
        GraphTabs.Children.Add(add);
    }

    private void ShowViewport()
    {
        _viewportMode = true;
        GridBg.Visibility = Visibility.Collapsed; GraphCanvas.Visibility = Visibility.Collapsed;
        if (MinimapPanel != null) MinimapPanel.Visibility = Visibility.Collapsed;
        ViewportCanvas.Visibility = Visibility.Visible;
        RenderViewport();
        BuildGraphTabs();
    }
    private void ExitViewport()
    {
        if (!_viewportMode) return;
        _viewportMode = false;
        ViewportCanvas.Visibility = Visibility.Collapsed;
        GridBg.Visibility = Visibility.Visible; GraphCanvas.Visibility = Visibility.Visible;
        UpdateMinimap();
        BuildGraphTabs();
    }

    private static byte ColorByte(string s) => (byte)Math.Clamp((int)Math.Round(ParseDouble(s) * 255), 0, 255);

    /// <summary>ビューポート: BP のコンポーネント木 (ComponentsText) を «見た目(塗り)» + «当たり判定(緑輪郭)» として描く。</summary>
    private void RenderViewport()
    {
        if (ViewportCanvas == null) return;
        ViewportCanvas.Children.Clear();
        double vw = ViewportCanvas.ActualWidth, vh = ViewportCanvas.ActualHeight;
        if (vw < 10) vw = 900; if (vh < 10) vh = 560;

        var text = (ComponentsText ?? "").Replace("\r", "");
        var comps = new Dictionary<int, List<string>>();
        foreach (var raw in text.Split('\n'))
        {
            var l = raw.Trim();
            if (!l.StartsWith("COMP ")) continue;
            var t = l.Split(' ');
            if (t.Length >= 3 && int.TryParse(t[1], out int cid)) { if (!comps.ContainsKey(cid)) comps[cid] = new(); comps[cid].Add(t[2]); }
        }
        // ノード行を «ローカル変換» として読み、親子を合成して «ワールド変換» にする。
        var local = new Dictionary<int, (int parent, double lx, double ly, double lrot, double lsx, double lsy, double bs, Color col, string name, bool collide, bool circle)>();
        var order = new List<int>();
        foreach (var raw in text.Split('\n'))
        {
            var l = raw.Trim();
            if (l.Length == 0 || !char.IsDigit(l[0])) continue;
            var t = l.Split(' ');
            if (t.Length < 12) continue;
            int id = ParseInt(t[0]), parent = ParseInt(t[1]);
            var col = Color.FromArgb(ColorByte(t[11]), ColorByte(t[8]), ColorByte(t[9]), ColorByte(t[10]));
            string name = t.Length >= 13 ? t[12] : "#" + id;
            bool collide = false, circle = false;
            if (comps.TryGetValue(id, out var cl))
                foreach (var c in cl)
                    if (c.Contains("RigidBody") || c.Contains("Collid") || c.Contains("Collision") || c.Contains("Physics")) { collide = true; if (c.Contains("Circle")) circle = true; }
            if (!local.ContainsKey(id)) order.Add(id);
            local[id] = (parent, ParseDouble(t[2]), ParseDouble(t[3]), ParseDouble(t[4]), ParseDouble(t[5]), ParseDouble(t[6]), ParseDouble(t[7]), col, name, collide, circle);
        }
        // 親の位置/回転/スケールを子へ適用 (2D ワールド変換。循環は seen で打ち切り)。
        (double wx, double wy, double wrot, double wsx, double wsy) World(int id, HashSet<int> seen)
        {
            var n = local[id];
            if (n.parent < 0 || !local.ContainsKey(n.parent) || !seen.Add(id)) return (n.lx, n.ly, n.lrot, n.lsx, n.lsy);
            var p = World(n.parent, seen);
            double r = p.wrot * Math.PI / 180.0, cr = Math.Cos(r), sr = Math.Sin(r);
            double lx = n.lx * p.wsx, ly = n.ly * p.wsy;
            return (p.wx + (lx * cr - ly * sr), p.wy + (lx * sr + ly * cr), p.wrot + n.lrot, p.wsx * n.lsx, p.wsy * n.lsy);
        }
        var nodes = new List<(int id, double x, double y, double rot, double w, double h, Color col, string name, bool collide, bool circle)>();
        foreach (var id in order)
        {
            var n = local[id];
            var w = World(id, new HashSet<int>());
            nodes.Add((id, w.wx, w.wy, w.wrot, Math.Max(2, n.bs * Math.Abs(w.wsx)), Math.Max(2, n.bs * Math.Abs(w.wsy)), n.col, n.name, n.collide, n.circle));
        }
        if (nodes.Count == 0)
        {
            ViewportCanvas.Children.Add(Place(new TextBlock {
                Text = "（コンポーネントなし）", Foreground = LabelFg, FontSize = 14 }, 24, 22));
            ViewportCanvas.Children.Add(Place(new TextBlock {
                Text = "シーンのノードを右クリック →「Blueprint として保存」で、見た目と当たり判定を持つ BP を作成。",
                Foreground = new SolidColorBrush(Color.FromRgb(0x8B, 0x93, 0xA0)), FontSize = 11.5 }, 24, 46));
            return;
        }
        // ----- 3D 透視ビュー (UE 風: 地面グリッド + オービットカメラ + 立体スラブ) -----
        double minX = double.MaxValue, minY = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (var n in nodes) { minX = Math.Min(minX, n.x - n.w / 2); maxX = Math.Max(maxX, n.x + n.w / 2); minY = Math.Min(minY, n.y - n.h / 2); maxY = Math.Max(maxY, n.y + n.h / 2); }
        double cx = (minX + maxX) / 2, cy2 = (minY + maxY) / 2;
        double gw = Math.Max(1, maxX - minX), gh = Math.Max(1, maxY - minY);
        double radius = 0.5 * Math.Sqrt(gw * gw + gh * gh) + 40;
        SetupCamera3D(vw, vh, cx, -cy2, 0, radius * 2.4);   // ワールド: X=x, Y=-y(上が+), Z=奥行き

        var Pr = (Func<double, double, double, (double sx, double sy, double z, bool ok)>)((wx, wy, wz) => Project3D(wx, wy, wz));
        void Line3D(double ax, double ay, double az, double bx, double by, double bz, Brush st, double th, DoubleCollection? dash = null)
        {
            var a = Pr(ax, ay, az); var b = Pr(bx, by, bz);
            if (!a.ok || !b.ok) return;
            var ln = new Line { X1 = a.sx, Y1 = a.sy, X2 = b.sx, Y2 = b.sy, Stroke = st, StrokeThickness = th, IsHitTestVisible = false };
            if (dash != null) ln.StrokeDashArray = dash;
            ViewportCanvas.Children.Add(ln);
        }
        void Poly((double sx, double sy, double z, bool ok)[] pts, Color fill, byte alpha)
        {
            var poly = new System.Windows.Shapes.Polygon { Fill = new SolidColorBrush(Color.FromArgb(alpha, fill.R, fill.G, fill.B)), IsHitTestVisible = false };
            foreach (var p in pts) poly.Points.Add(new Point(p.sx, p.sy));
            ViewportCanvas.Children.Add(poly);
        }
        // 地面グリッド: «無限平面風» にカメラ距離に応じて広域へ、遠方ほどフェードして地平線へ溶かす。
        double groundY = -maxY;
        double ext = Math.Max(radius * 3, _cDist * 2.4);
        double NiceStep(double v) { if (v <= 0) return 20; double ee = Math.Pow(10, Math.Floor(Math.Log10(v))); double m = v / ee; return (m < 1.5 ? 1 : m < 3 ? 2 : m < 7 ? 5 : 10) * ee; }
        double gstep = NiceStep(ext / 30);
        double gx0 = Math.Floor((cx - ext) / gstep) * gstep, gx1 = Math.Ceiling((cx + ext) / gstep) * gstep;
        double gz0 = Math.Floor(-ext / gstep) * gstep, gz1 = Math.Ceiling(ext / gstep) * gstep;
        Brush GridBrush(double dist, bool axis)   // 中心からの距離でフェード (端＝透明 → 無限に見える)
        {
            double f = Math.Max(0, 1 - dist / ext);
            byte a = (byte)Math.Min(255, (axis ? 0xC0 : 0x52) * f * f);
            return axis ? new SolidColorBrush(Color.FromArgb(a, 0x86, 0x92, 0xA2)) : new SolidColorBrush(Color.FromArgb(a, 0x6A, 0x76, 0x86));
        }
        for (double X = gx0; X <= gx1; X += gstep) { bool ax = Math.Abs(X) < gstep / 2; var br = GridBrush(Math.Abs(X - cx), ax); if (((SolidColorBrush)br).Color.A > 4) Line3D(X, groundY, gz0, X, groundY, gz1, br, ax ? 1.4 : 0.8); }
        for (double Z = gz0; Z <= gz1; Z += gstep) { bool ax = Math.Abs(Z) < gstep / 2; var br = GridBrush(Math.Abs(Z), ax); if (((SolidColorBrush)br).Color.A > 4) Line3D(gx0, groundY, Z, gx1, groundY, Z, br, ax ? 1.4 : 0.8); }
        // 原点軸 (X=赤 / Y=緑(上) / Z=青(奥行き))。
        double al = Math.Max(80, radius * 0.5);
        Line3D(0, 0, 0, al, 0, 0, new SolidColorBrush(Color.FromRgb(0xE0, 0x60, 0x60)), 1.8);
        Line3D(0, 0, 0, 0, al, 0, new SolidColorBrush(Color.FromRgb(0x60, 0xE0, 0x60)), 1.8);
        Line3D(0, 0, 0, 0, 0, al, new SolidColorBrush(Color.FromRgb(0x60, 0x98, 0xF0)), 1.8);

        // コンポーネントを «奥→手前» で描く (画家のアルゴリズム)。
        double Depth(double x, double y) => Pr(x, -y, 0).z;
        double thick = Math.Max(10, radius * 0.07);
        foreach (var n in nodes.OrderByDescending(n => Depth(n.x, n.y)))
        {
            double hwx = n.w / 2, hhy = n.h / 2;
            double rr = n.rot * Math.PI / 180.0, cR = Math.Cos(rr), sR = Math.Sin(rr);
            (double x, double y) C2(double ox, double oy) => (n.x + ox * cR - oy * sR, n.y + ox * sR + oy * cR);
            var p2 = new[] { C2(-hwx, -hhy), C2(hwx, -hhy), C2(hwx, hhy), C2(-hwx, hhy) };
            var fp = new (double sx, double sy, double z, bool ok)[4];   // 前面 z=0
            var bp = new (double sx, double sy, double z, bool ok)[4];   // 背面 z=-thick
            bool ok = true;
            for (int i = 0; i < 4; i++) { fp[i] = Pr(p2[i].x, -p2[i].y, 0); bp[i] = Pr(p2[i].x, -p2[i].y, -thick); if (!fp[i].ok || !bp[i].ok) ok = false; }
            if (!ok) continue;
            var side = Color.FromRgb((byte)(n.col.R * 0.62), (byte)(n.col.G * 0.62), (byte)(n.col.B * 0.62));
            Poly(bp, side, 0xFF);                                                    // 背面
            for (int i = 0; i < 4; i++) { int j = (i + 1) % 4; Poly(new[] { fp[i], fp[j], bp[j], bp[i] }, side, 0xFF); }   // 側面
            Poly(fp, n.col, 0xFF);                                                   // 前面 (見た目)
            var edge = new SolidColorBrush(Color.FromArgb(0x90, 0x10, 0x12, 0x16));
            for (int i = 0; i < 4; i++) { int j = (i + 1) % 4; Line3D(p2[i].x, -p2[i].y, 0, p2[j].x, -p2[j].y, 0, edge, 0.8); }   // 前面の縁
            if (n.collide) DrawWireBox3D(p2, n.x, n.y, 6, thick + 6, CollideEdge, 1.6, new DoubleCollection { 3, 2 });          // 当たり判定
            if (n.id == _vpSelected) DrawWireBox3D(p2, n.x, n.y, 11, thick + 11, SelEdge, 2.0, null);                          // 選択枠
            var top = Pr(n.x, -(n.y - n.h / 2), 0);
            if (top.ok) ViewportCanvas.Children.Add(Place(new TextBlock { Text = n.name, Foreground = n.id == _vpSelected ? SelEdge : LabelFg, FontSize = 10.5, FontWeight = n.id == _vpSelected ? FontWeights.SemiBold : FontWeights.Normal }, top.sx - n.name.Length * 3, top.sy - 18));
            // ドラッグ用の透明ヒット矩形 (前面のバウンディング)。
            double mnx = fp.Min(p => p.sx), mny = fp.Min(p => p.sy), mxx = fp.Max(p => p.sx), mxy = fp.Max(p => p.sy);
            var hit = new Rectangle { Width = Math.Max(4, mxx - mnx), Height = Math.Max(4, mxy - mny), Fill = Brushes.Transparent, Cursor = Cursors.SizeAll };
            var nn = n;
            hit.MouseLeftButtonDown += (_, ev) => {
                BeginEdit(); _vpDragId = nn.id; _vpSelected = nn.id;
                var w = Unproject3D(ev.GetPosition(ViewportCanvas));
                if (w != null) { _vp3dGrabOX = nn.x - w.Value.x; _vp3dGrabOY = nn.y - (-w.Value.y); }
                ViewportCanvas.CaptureMouse(); RenderViewport(); ev.Handled = true;
            };
            ViewportCanvas.Children.Add(Place(hit, mnx, mny));
        }

        // 凡例 + 操作ヒント。
        var legend = new StackPanel { Orientation = Orientation.Horizontal };
        legend.Children.Add(new TextBlock { Text = "3D ビュー  ・  見た目 = 塗り    ", Foreground = LabelFg, FontSize = 11, VerticalAlignment = VerticalAlignment.Center });
        legend.Children.Add(new Rectangle { Width = 13, Height = 11, Stroke = CollideEdge, StrokeThickness = 1.5, StrokeDashArray = new DoubleCollection { 3, 2 }, Fill = Brushes.Transparent, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 4, 0) });
        legend.Children.Add(new TextBlock { Text = "= 当たり判定   ・  ドラッグ=回転 / ホイール=ズーム / コンポーネントをドラッグ=移動", Foreground = new SolidColorBrush(Color.FromRgb(0x6B, 0x74, 0x82)), FontSize = 11, VerticalAlignment = VerticalAlignment.Center });
        ViewportCanvas.Children.Add(Place(legend, 14, 12));
    }

    private void SetupCamera3D(double vw, double vh, double cx, double cy, double cz, double baseR)
    {
        double R = baseR * _vp3dZoom;
        _cDist = R;
        double cosP = Math.Cos(_vp3dPitch), sinP = Math.Sin(_vp3dPitch);
        _cEyeX = cx + R * cosP * Math.Sin(_vp3dYaw);
        _cEyeY = cy + R * sinP;
        _cEyeZ = cz + R * cosP * Math.Cos(_vp3dYaw);
        double fx = cx - _cEyeX, fy = cy - _cEyeY, fz = cz - _cEyeZ; double fl = Math.Sqrt(fx * fx + fy * fy + fz * fz); if (fl < 1e-6) fl = 1;
        _cFwdX = fx / fl; _cFwdY = fy / fl; _cFwdZ = fz / fl;
        double rx = -_cFwdZ, rz = _cFwdX; double rl = Math.Sqrt(rx * rx + rz * rz); if (rl < 1e-6) rl = 1;   // right = norm(cross(fwd,(0,1,0)))
        _cRgtX = rx / rl; _cRgtY = 0; _cRgtZ = rz / rl;
        _cUpX = _cRgtY * _cFwdZ - _cRgtZ * _cFwdY;   // up = cross(right, fwd)
        _cUpY = _cRgtZ * _cFwdX - _cRgtX * _cFwdZ;
        _cUpZ = _cRgtX * _cFwdY - _cRgtY * _cFwdX;
        _cFocal = (vh / 2) / Math.Tan(0.45);   // fovY ≈ 51.6°
        _cVW = vw; _cVH = vh;
    }
    private (double sx, double sy, double z, bool ok) Project3D(double wx, double wy, double wz)
    {
        double dx = wx - _cEyeX, dy = wy - _cEyeY, dz = wz - _cEyeZ;
        double cz = dx * _cFwdX + dy * _cFwdY + dz * _cFwdZ;
        if (cz < 1) return (0, 0, cz, false);
        double cxv = dx * _cRgtX + dy * _cRgtY + dz * _cRgtZ;
        double cyv = dx * _cUpX + dy * _cUpY + dz * _cUpZ;
        return (_cVW / 2 + (cxv / cz) * _cFocal, _cVH / 2 - (cyv / cz) * _cFocal, cz, true);
    }
    private (double x, double y)? Unproject3D(Point screen)   // z=0 平面上のワールド点 (2D y = -worldY)
    {
        double px = (screen.X - _cVW / 2) / _cFocal, py = -(screen.Y - _cVH / 2) / _cFocal;
        double dx = _cFwdX + px * _cRgtX + py * _cUpX, dy = _cFwdY + px * _cRgtY + py * _cUpY, dz = _cFwdZ + px * _cRgtZ + py * _cUpZ;
        if (Math.Abs(dz) < 1e-6) return null;
        double t = -_cEyeZ / dz;
        return (_cEyeX + t * dx, _cEyeY + t * dy);
    }
    /// <summary>2D 矩形 (4隅) を中心からスケール拡張し、前後 (z=zf..-zb) のワイヤボックスを描く。</summary>
    private void DrawWireBox3D((double x, double y)[] p2, double ccx, double ccy, double zf, double zb, Brush col, double th, DoubleCollection? dash)
    {
        var e = new (double x, double y)[4];
        for (int i = 0; i < 4; i++) e[i] = (ccx + (p2[i].x - ccx) * 1.06 + Math.Sign(p2[i].x - ccx) * 2, ccy + (p2[i].y - ccy) * 1.06 + Math.Sign(p2[i].y - ccy) * 2);
        var f = new (double sx, double sy, double z, bool ok)[4]; var b = new (double sx, double sy, double z, bool ok)[4];
        for (int i = 0; i < 4; i++) { f[i] = Project3D(e[i].x, -e[i].y, zf); b[i] = Project3D(e[i].x, -e[i].y, -zb); }
        void L((double sx, double sy, double z, bool ok) a, (double sx, double sy, double z, bool ok) c) { if (a.ok && c.ok) { var ln = new Line { X1 = a.sx, Y1 = a.sy, X2 = c.sx, Y2 = c.sy, Stroke = col, StrokeThickness = th, IsHitTestVisible = false }; if (dash != null) ln.StrokeDashArray = dash; ViewportCanvas.Children.Add(ln); } }
        for (int i = 0; i < 4; i++) { int j = (i + 1) % 4; L(b[i], b[j]); L(f[i], b[i]); L(f[i], f[j]); }   // 奥→手前 (画家のアルゴリズム)
    }

    private void OnViewportDown(object sender, MouseButtonEventArgs e)
    {
        if (_vpDragId >= 0) return;                 // コンポーネントヒット矩形が処理済みなら来ない
        _vp3dOrbiting = true; _vpDragLast = e.GetPosition(ViewportCanvas); ViewportCanvas.CaptureMouse();
    }
    private void OnViewportMove(object sender, MouseEventArgs e)
    {
        if (_vp3dOrbiting)                          // 空き場所ドラッグ → カメラ回転
        {
            var p = e.GetPosition(ViewportCanvas);
            _vp3dYaw += (p.X - _vpDragLast.X) * 0.01;   // 右ドラッグ=反時計回り (直感的な向き)
            _vp3dPitch = Math.Min(Math.Max(_vp3dPitch + (p.Y - _vpDragLast.Y) * 0.01, -1.45), 1.45);
            _vpDragLast = p; RenderViewport(); return;
        }
        if (_vpDragId < 0) return;                  // コンポーネント移動 (z=0 平面へレイ投影)
        var w = Unproject3D(e.GetPosition(ViewportCanvas));
        if (w == null) return;
        SetComponentRow(_vpDragId, w.Value.x + _vp3dGrabOX, -w.Value.y + _vp3dGrabOY);
        RenderViewport();
    }
    private void OnViewportUp(object sender, MouseButtonEventArgs e)
    {
        if (_vp3dOrbiting) { _vp3dOrbiting = false; ViewportCanvas.ReleaseMouseCapture(); return; }
        if (_vpDragId < 0) return;
        _vpDragId = -1;
        ViewportCanvas.ReleaseMouseCapture();
        ComponentsChanged?.Invoke();
        CommitEdit();
        RenderViewport();
    }
    private void OnViewportWheel(object sender, MouseWheelEventArgs e)
    {
        _vp3dZoom = Math.Min(Math.Max(_vp3dZoom * (e.Delta > 0 ? 0.9 : 1.111), 0.2), 6.0);
        if (_viewportMode) RenderViewport();
    }

    /// <summary>ComponentsText のノード行 id の位置を (dx,dy) ずらす (検証ハーネス用)。</summary>
    private void MoveComponentRow(int id, double dx, double dy)
    {
        var lines = (ComponentsText ?? "").Replace("\r", "").Split('\n');
        for (int i = 0; i < lines.Length; i++)
        {
            var l = lines[i];
            if (l.Length == 0 || !char.IsDigit(l[0])) continue;
            var t = l.Split(' ');
            if (t.Length < 12 || ParseInt(t[0]) != id) continue;
            t[2] = (ParseDouble(t[2]) + dx).ToString("0.####", System.Globalization.CultureInfo.InvariantCulture);
            t[3] = (ParseDouble(t[3]) + dy).ToString("0.####", System.Globalization.CultureInfo.InvariantCulture);
            lines[i] = string.Join(' ', t);
            break;
        }
        ComponentsText = string.Join("\n", lines);
    }

    /// <summary>ComponentsText のノード行 id の位置を (x,y) に設定する (3D ドラッグの絶対移動)。</summary>
    private void SetComponentRow(int id, double x, double y)
    {
        var lines = (ComponentsText ?? "").Replace("\r", "").Split('\n');
        for (int i = 0; i < lines.Length; i++)
        {
            var l = lines[i];
            if (l.Length == 0 || !char.IsDigit(l[0])) continue;
            var t = l.Split(' ');
            if (t.Length < 12 || ParseInt(t[0]) != id) continue;
            t[2] = x.ToString("0.####", System.Globalization.CultureInfo.InvariantCulture);
            t[3] = y.ToString("0.####", System.Globalization.CultureInfo.InvariantCulture);
            lines[i] = string.Join(' ', t);
            break;
        }
        ComponentsText = string.Join("\n", lines);
    }

    /// <summary>編集グラフを切り替える (現在を保存し、対象を読み込む)。</summary>
    public void SwitchGraph(string name)
    {
        if (name == _activeName || !_graphOrder.Contains(name)) return;
        SyncActiveGraph();
        _activeName = name;
        _selected.Clear(); _stepIdx = -1; _debugCurrent = -1;
        LoadActiveGraph(_graphTexts.TryGetValue(name, out var b) ? b : "");
        BuildGraphTabs();
        GraphChanged?.Invoke();
    }
    public Action? GraphChanged;   // 編集グラフが変わった → ウィンドウ側の表示更新用

    // ----- My Blueprint パネル (BlueprintWindow) 向けの公開アクセサ -----
    /// <summary>全グラフ名 (Event Graph + Function、登場順)。</summary>
    public IReadOnlyList<string> GraphNames => _graphOrder;
    /// <summary>name が Function サブグラフか。</summary>
    public bool IsFunctionGraph(string name) => _graphIsFunc.Contains(name);
    /// <summary>編集中グラフ名。</summary>
    public string ActiveGraphName => _activeName;
    /// <summary>新しい関数を追加して切り替える (パネルの「＋関数」)。</summary>
    public void NewFunction() => AddFunction();

    private void LoadActiveGraph(string body)
    {
        _nodes.Clear(); _conns.Clear(); _comments.Clear();
        if (_activeName == EventGraphName) LoadInherited();   // Event Graph のみ継承ノードを再付与
        ParseGraph(body, 0, inherited: false);
        RecomputeNextIds();
        _zoom.ScaleX = _zoom.ScaleY = 1; _pan.X = _pan.Y = 0;   // 切替時はビューをリセット
        Rebuild(); UpdateGrid(); UpdateMinimap();
    }

    private void AddFunction()
    {
        BeginEdit();
        string name = UniqueGraphName("NewFunction");
        _graphOrder.Add(name); _graphIsFunc.Add(name);
        // 関数の入口 (Function Entry: exec 出力) を種として置く。
        _graphTexts[name] = "N 100 80 100 357A55 Function Entry\nO E ▶\n";
        SwitchGraph(name);
        BuildGraphTabs();
        CommitEdit();
        LogSink?.Invoke($"ƒ 関数を追加: {name}");
    }

    private void DeleteFunction(string name)
    {
        if (!_graphIsFunc.Contains(name)) return;
        BeginEdit();
        if (_activeName == name) { _activeName = EventGraphName; }
        _graphOrder.Remove(name); _graphIsFunc.Remove(name); _graphTexts.Remove(name);
        LoadActiveGraph(_graphTexts.TryGetValue(_activeName, out var b) ? b : "");
        BuildGraphTabs();
        GraphChanged?.Invoke();
        CommitEdit();
        LogSink?.Invoke($"関数を削除: {name}");
    }

    private string UniqueGraphName(string baseName)
    {
        string n = baseName; int i = 1;
        while (_graphOrder.Contains(n)) n = baseName + (++i);
        return n;
    }

    // .acsbp の N/I/O/V/C/K 行を解析して _nodes/_conns/_comments へ追加する。idOffset/inherited で継承ノードに使う。
    private void ParseGraph(string text, int idOffset, bool inherited)
    {
        BpNode? cur = null;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0 || line.StartsWith("ACSBP") || line.StartsWith("PARENT")) continue;
            if (line.StartsWith("NC ")) { if (cur != null) cur.Comment = line.Substring(3); continue; }   // ノードコメント
            if (line.StartsWith("VR ")) { if (cur != null) cur.VarRef = line.Substring(3).Trim(); continue; }   // 束縛変数
            switch (line[0])
            {
                case 'N':
                {
                    var t = line.Split(new[] { ' ' }, 6);
                    if (t.Length < 5) break;
                    cur = new BpNode {
                        Id = ParseInt(t[1]) + idOffset, X = ParseDouble(t[2]), Y = ParseDouble(t[3]),
                        Header = new SolidColorBrush(ParseHex(t[4])),
                        Title = t.Length >= 6 ? t[5] : "", Inherited = inherited,
                    };
                    _nodes.Add(cur);
                    break;
                }
                case 'I':
                case 'O':
                {
                    if (cur == null) break;
                    var t = line.Split(new[] { ' ' }, 3);
                    if (t.Length < 3) break;
                    var pin = ParsePin(t[1], t[2]);
                    if (line[0] == 'I') cur.Inputs.Add(pin); else cur.Outputs.Add(pin);
                    break;
                }
                case 'V':
                {
                    if (cur == null) break;
                    var t = line.Split(new[] { ' ' }, 3);
                    if (t.Length < 3) break;
                    cur.Literals[ParseInt(t[1])] = t[2];   // 入力ピンの定数値
                    break;
                }
                case 'X':
                {
                    if (cur != null) cur.Disabled = true;   // 無効化フラグ
                    break;
                }
                case 'C':
                {
                    var t = line.Split(' ');
                    if (t.Length < 5) break;
                    _conns.Add(new BpConn(ParseInt(t[1]) + idOffset, ParseInt(t[2]), ParseInt(t[3]) + idOffset, ParseInt(t[4])));
                    break;
                }
                case 'K':
                {
                    var t = line.Split(new[] { ' ' }, 8);
                    if (t.Length < 7) break;
                    _comments.Add(new BpComment {
                        Id = ParseInt(t[1]) + idOffset, X = ParseDouble(t[2]), Y = ParseDouble(t[3]),
                        W = ParseDouble(t[4]), H = ParseDouble(t[5]), Color = ParseHex(t[6]),
                        Text = t.Length >= 8 ? t[7] : "Comment", Inherited = inherited,
                    });
                    break;
                }
            }
        }
        MigrateVarNodes();   // 旧形式 (name ピン) の Get/Set Variable を «変数焼き込み» 形式へ
    }

    /// <summary>旧形式 (name ピン + リテラル) の Get/Set Variable を «変数を VarRef に焼き込み・name ピン無し» へ移行。
    /// 既に新形式なら値ピンの型を変数に同期するだけ。冪等。</summary>
    private void MigrateVarNodes()
    {
        foreach (var n in _nodes)
        {
            if (n.Title != "Get Variable" && n.Title != "Set Variable") continue;
            int ni = n.Inputs.FindIndex(p => p.Name == "name");
            if (ni >= 0)
            {
                if (string.IsNullOrEmpty(n.VarRef) && n.Literals.TryGetValue(ni, out var nm)) n.VarRef = nm.Trim();
                n.Inputs.RemoveAt(ni);
                var nl = new Dictionary<int, string>();   // リテラルのキーを詰める
                foreach (var kv in n.Literals) { if (kv.Key == ni) continue; nl[kv.Key > ni ? kv.Key - 1 : kv.Key] = kv.Value; }
                n.Literals = nl;
                for (int ci = _conns.Count - 1; ci >= 0; ci--)   // 接続の入力ピン番号を詰める (name への接続は破棄)
                {
                    var c = _conns[ci];
                    if (c.ToNode != n.Id) continue;
                    if (c.ToPin == ni) _conns.RemoveAt(ci);
                    else if (c.ToPin > ni) _conns[ci] = c with { ToPin = c.ToPin - 1 };
                }
            }
            RetypeVarNode(n);
        }
    }

    /// <summary>Get/Set Variable の value ピン型を、束縛変数 (VarRef) の型へ同期する。</summary>
    private void RetypeVarNode(BpNode n)
    {
        string ty = Variables.FirstOrDefault(x => x.Name == n.VarRef)?.Type ?? "";
        if (n.Title == "Set Variable") { int vi = n.Inputs.FindIndex(p => p.Name == "value"); if (vi >= 0) n.Inputs[vi] = n.Inputs[vi] with { Type = ty }; }
        else { int vi = n.Outputs.FindIndex(p => p.Name == "value"); if (vi >= 0) n.Outputs[vi] = n.Outputs[vi] with { Type = ty }; }
    }

    // ピン種別トークン (E / D / D:Type) と名前から Pin を作る。
    private static Pin ParsePin(string kindTok, string name)
    {
        if (kindTok == "E") return new Pin(name, PinKind.Exec);
        string type = kindTok.StartsWith("D:") ? kindTok.Substring(2) : "";
        return new Pin(name, PinKind.Data, type);
    }

    /// <summary>選択ノード群を «移植可能» なテキストへ (id を 0..k に正規化)。Copy/Duplicate 用。</summary>
    private string SerializeNodes(List<BpNode> set)
    {
        var sb = new StringBuilder();
        var ids = set.Select(n => n.Id).ToHashSet();
        foreach (var n in set)
        {
            var c = (n.Header as SolidColorBrush)?.Color ?? Colors.SteelBlue;
            sb.Append("N ").Append(n.Id).Append(' ')
              .Append(n.X.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append(n.Y.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append($"{c.R:X2}{c.G:X2}{c.B:X2}").Append(' ').Append(n.Title).Append('\n');
            foreach (var p in n.Inputs)  sb.Append("I ").Append(PinToken(p)).Append(' ').Append(p.Name).Append('\n');
            foreach (var p in n.Outputs) sb.Append("O ").Append(PinToken(p)).Append(' ').Append(p.Name).Append('\n');
            foreach (var kv in n.Literals) if (!string.IsNullOrEmpty(kv.Value)) sb.Append("V ").Append(kv.Key).Append(' ').Append(kv.Value).Append('\n');
            if (n.Disabled) sb.Append("X\n");
            if (!string.IsNullOrEmpty(n.Comment)) sb.Append("NC ").Append(n.Comment.Replace("\n", " ")).Append('\n');
            if (!string.IsNullOrEmpty(n.VarRef)) sb.Append("VR ").Append(n.VarRef).Append('\n');
        }
        foreach (var k in _conns)
            if (ids.Contains(k.FromNode) && ids.Contains(k.ToNode))
                sb.Append("C ").Append(k.FromNode).Append(' ').Append(k.FromPin).Append(' ').Append(k.ToNode).Append(' ').Append(k.ToPin).Append('\n');
        return sb.ToString();
    }

    /// <summary>SerializeNodes のテキストを «新しい id» で取り込み、貼り付けたノードを返す。</summary>
    private List<BpNode> DeserializeNodes(string text, double offsetX, double offsetY)
    {
        var map = new Dictionary<int, int>();    // 旧 id → 新 id
        var added = new List<BpNode>();
        BpNode? cur = null;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0) continue;
            if (line.StartsWith("NC ")) { if (cur != null) cur.Comment = line.Substring(3); continue; }   // ノードコメント
            if (line.StartsWith("VR ")) { if (cur != null) cur.VarRef = line.Substring(3).Trim(); continue; }   // 束縛変数
            switch (line[0])
            {
                case 'N':
                {
                    var t = line.Split(new[] { ' ' }, 6);
                    if (t.Length < 5) break;
                    int oldId = ParseInt(t[1]); int newId = _nextId++;
                    map[oldId] = newId;
                    cur = new BpNode { Id = newId, X = ParseDouble(t[2]) + offsetX, Y = ParseDouble(t[3]) + offsetY,
                        Header = new SolidColorBrush(ParseHex(t[4])), Title = t.Length >= 6 ? t[5] : "" };
                    _nodes.Add(cur); added.Add(cur);
                    break;
                }
                case 'I': case 'O':
                {
                    if (cur == null) break;
                    var t = line.Split(new[] { ' ' }, 3);
                    if (t.Length < 3) break;
                    var pin = ParsePin(t[1], t[2]);
                    if (line[0] == 'I') cur.Inputs.Add(pin); else cur.Outputs.Add(pin);
                    break;
                }
                case 'V':
                {
                    if (cur == null) break;
                    var t = line.Split(new[] { ' ' }, 3);
                    if (t.Length < 3) break;
                    cur.Literals[ParseInt(t[1])] = t[2];
                    break;
                }
                case 'X':
                {
                    if (cur != null) cur.Disabled = true;
                    break;
                }
                case 'C':
                {
                    var t = line.Split(' ');
                    if (t.Length < 5) break;
                    if (map.TryGetValue(ParseInt(t[1]), out int fn) && map.TryGetValue(ParseInt(t[3]), out int tn))
                        _conns.Add(new BpConn(fn, ParseInt(t[2]), tn, ParseInt(t[4])));
                    break;
                }
            }
        }
        MigrateVarNodes();
        return added;
    }

    /// <summary>親 (継承元) の相対/絶対パスを実ファイルへ解決 (相対は DefaultDir 基準)。</summary>
    private string? ResolveParentPath(string path)
    {
        if (System.IO.File.Exists(path)) return path;
        if (DefaultDir != null) { string p = System.IO.Path.Combine(DefaultDir, path); if (System.IO.File.Exists(p)) return p; }
        return null;
    }

    private static int    ParseInt(string s)    => int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : 0;
    private static double ParseDouble(string s) => double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : 0.0;
    private static Color  ParseHex(string h)
    {
        if (h.Length < 6) return Colors.SteelBlue;
        return Color.FromRgb(Convert.ToByte(h.Substring(0, 2), 16),
                             Convert.ToByte(h.Substring(2, 2), 16),
                             Convert.ToByte(h.Substring(4, 2), 16));
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        if (SaveToCurrent()) return;   // 開いているファイルへ上書き
        var dlg = new Microsoft.Win32.SaveFileDialog {
            Filter = "ACS Blueprint (*.acsbp)|*.acsbp", DefaultExt = ".acsbp", FileName = "graph.acsbp" };
        if (DefaultDir != null && System.IO.Directory.Exists(DefaultDir)) dlg.InitialDirectory = DefaultDir;
        if (dlg.ShowDialog() == true)
        {
            AcsbpFormat.Write(dlg.FileName, Serialize());
            CurrentPath = dlg.FileName;
            PathChanged?.Invoke();
            LogSink?.Invoke($"Blueprint を保存: {System.IO.Path.GetFileName(dlg.FileName)}");
        }
    }

    private void OnOpen(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog {
            Filter = "ACS Blueprint (*.acsbp)|*.acsbp", DefaultExt = ".acsbp" };
        if (DefaultDir != null && System.IO.Directory.Exists(DefaultDir)) dlg.InitialDirectory = DefaultDir;
        if (dlg.ShowDialog() == true) LoadFromFile(dlg.FileName);
    }

    /// <summary>.acsbp ファイルを読み込んでグラフを復元する (アセットブラウザのダブルクリック等から)。</summary>
    public void LoadFromFile(string path)
    {
        if (!System.IO.File.Exists(path)) return;
        Deserialize(System.IO.File.ReadAllText(path));
        _undo.Clear(); _redo.Clear();   // 新規ファイルの読込で履歴をリセット
        CurrentPath = path;
        PathChanged?.Invoke();
    }

    /// <summary>開いているファイルへ上書き保存する (CurrentPath が無ければ false)。</summary>
    public bool SaveToCurrent()
    {
        if (string.IsNullOrEmpty(CurrentPath)) return false;
        try { AcsbpFormat.Write(CurrentPath, Serialize()); }
        catch { return false; }
        LogSink?.Invoke($"Blueprint を保存: {System.IO.Path.GetFileName(CurrentPath)}");
        return true;
    }

    /// <summary>継承元 (親) の .acsbp を選んで «継承» する。子ノードは保ったまま親を継承ノードとして読み込む。</summary>
    private void OnSetParent(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog {
            Filter = "ACS Blueprint (*.acsbp)|*.acsbp", DefaultExt = ".acsbp", Title = "継承元 (親) の .acsbp を選択" };
        if (DefaultDir != null && System.IO.Directory.Exists(DefaultDir)) dlg.InitialDirectory = DefaultDir;
        if (dlg.ShowDialog() != true) return;
        BeginEdit();
        _parentPath = MakeRelative(dlg.FileName);
        Deserialize(Serialize());   // 現在の子 (継承ノードは保存対象外) を保ったまま、新しい親を継承ノードとして読み直す
        CommitEdit();
    }

    /// <summary>DefaultDir 直下なら名前のみ、そうでなければ絶対パスを返す (PARENT 行に書く相対パス)。</summary>
    private string MakeRelative(string full)
    {
        if (DefaultDir != null)
        {
            string d = System.IO.Path.GetFullPath(DefaultDir);
            string f = System.IO.Path.GetFullPath(full);
            if (f.StartsWith(d, StringComparison.OrdinalIgnoreCase)) return System.IO.Path.GetFileName(f);
        }
        return full;
    }

    // ----- 実行 (BP5: イベントから exec チェーンを辿る簡易インタプリタ) -----
    /// <summary>実行トレースの出力先 (MainWindow のコンソール)。</summary>
    public Action<string>? LogSink;
    /// <summary>反射関数ノードの実呼出 (ownerType, method, target, arg) → 戻り値文字列 (void は "")。
    /// 束縛できなければ null。target はノード ID 文字列 (空/非数値なら選択ノード=self)。MainWindow が束縛。</summary>
    public Func<string, string, string, string, string?>? InvokeMethod;
    /// <summary>Spawn Prefab ノードの実生成 (path, pos) → 生成ノード ID 文字列 (失敗 null)。MainWindow が束縛。</summary>
    public Func<string, string, string?>? SpawnPrefab;
    /// <summary>組込シーン操作ノード (op, args) → 結果文字列 (void は "", 失敗 null)。MainWindow が束縛。</summary>
    public Func<string, string[], string?>? BuiltinOp;
    private void Trace(string s) => LogSink?.Invoke(s);

    private readonly Dictionary<int, string> _spawnHandles = new();   // ノード ID → spawn ハンドル (実行内で一意)
    private readonly Dictionary<int, string> _returns = new();        // ノード ID → 関数の戻り値 (data 出力のプル用)
    private readonly Dictionary<string, string> _vars = new();        // 名前付き変数 (Set/Get Variable。実行開始時に Variables で種まき)
    private readonly Random _rng = new(12345);                         // Random ノードのデバッグ実行用 (シード固定で再現性)
    private readonly Dictionary<(int, int), string> _watch = new();   // (ノードID, 出力ピン) → 直近に流れた値 (実行後の表示)
    private readonly HashSet<(int, int)> _firedOut = new();           // 発火した (ノードID, 出力exec ピン) → 配線ハイライト
    private bool _showWatch;                                          // 実行後の値ウォッチ表示中
    private readonly Dictionary<int, int> _flowState = new();         // 状態付きフローノード (Gate 開閉 / DoOnce 発火済 / FlipFlop 次A) の状態
    private readonly List<int> _trace = new();                        // 実行順 (ノードID。ライブデバッグのステップ再生に使う)
    private readonly HashSet<int> _breakpoints = new();               // ブレークポイントを置いたノードID
    private int _stepIdx = -1;     // ステップ再生の現在位置 (_trace のindex。-1=デバッグ外)
    private int _debugCurrent = -1; // 現在強調表示しているノードID
    private readonly Dictionary<string, int> _funcEntry = new();      // 関数名 → Function Entry ノードID (実行時に展開)
    private readonly HashSet<int> _runtimeFuncIds = new();            // 実行のため一時展開した関数ノードのID (実行後に除去)
    private readonly Stack<int> _callStack = new();                  // 呼出中の Call Function ノードID (Return の戻り先)
    private readonly Stack<string[]> _argStack = new();              // 呼出中の引数値 (Function Entry の出力が読む)
    private readonly Dictionary<(int, int), string> _callResult = new();  // (Call ノードID, 戻り値index) → Return が書いた値

    /// <summary>BP クラスが持つ変数 (名前 + 型 + 既定値)。実行開始時に値へ種まきされる (UE5 の Variables 相当)。</summary>
    public sealed class BpVar
    {
        public string Name; public string Type; public string Value;
        public string Category = "";        // My Blueprint パネルでの分類 (UE5 の Category)
        public string Tooltip = "";         // 変数の説明 (UE5 の Tooltip。ホバーで表示)
        public bool InstanceEditable;       // インスタンスで編集可 (UE5 の Instance Editable)
        public BpVar(string n, string t, string v) { Name = n; Type = t; Value = v; }
    }
    /// <summary>この BP の変数一覧 (My Blueprint パネルが編集)。</summary>
    public List<BpVar> Variables { get; } = new();
    public Action? VariablesChanged;   // 変数が変わった (Deserialize 等) → パネル再描画

    /// <summary>この BP の «コンポーネント木» (= 生成される実体。ACSCENE サブツリー逐語テキスト)。
    /// 空ならロジックのみの BP。UE5 の Blueprint Class の Components 相当 (旧 Prefab を内包)。</summary>
    public string ComponentsText = "";
    public Action? ComponentsChanged;   // ComponentsText が変わった (Deserialize 等) → Components パネル再描画

    /// <summary>この BP の «root ノード» へコンポーネント型を 1 つ追加する。root の «最後の COMP 行» の直後へ
    /// 挿入し、新コンポーネントを最後の slot にして既存 CPROP の slot 対応を壊さない。成功で true。</summary>
    public bool AddComponentToRoot(string type)
    {
        if (string.IsNullOrWhiteSpace(ComponentsText) || string.IsNullOrWhiteSpace(type)) return false;
        BeginEdit();
        var lines = ComponentsText.Replace("\r", "").Split('\n').ToList();
        int rootId = -1, insertAt = -1;
        for (int i = 0; i < lines.Count; i++)
        {
            var l = lines[i].Trim();
            if (l.Length == 0 || !char.IsDigit(l[0])) continue;   // 最初のノード行 (= root)
            var t = l.Split(' ');
            if (t.Length >= 3 && int.TryParse(t[0], out rootId)) { insertAt = i + 1; break; }
        }
        if (rootId < 0) { CancelEdit(); return false; }
        for (int i = insertAt; i < lines.Count; i++)   // root の最後の COMP 行直後へ
        {
            var t = lines[i].Trim().Split(' ');
            if (t.Length >= 2 && t[0] == "COMP" && t[1] == rootId.ToString()) insertAt = i + 1;
        }
        lines.Insert(insertAt, $"COMP {rootId} {type}");
        ComponentsText = string.Join("\n", lines);
        if (!ComponentsText.EndsWith("\n")) ComponentsText += "\n";
        ComponentsChanged?.Invoke();
        if (_viewportMode) RenderViewport();
        CommitEdit();
        return true;
    }

    /// <summary>ノード nodeId の slot 番コンポーネントを削除する (COMP 行 + 該当 CPROP を除去し、後続 slot を 1 詰める)。</summary>
    public bool RemoveComponentFromNode(int nodeId, int slot)
    {
        if (string.IsNullOrWhiteSpace(ComponentsText)) return false;
        BeginEdit();
        var lines = ComponentsText.Replace("\r", "").Split('\n').ToList();
        int seen = 0, removeIdx = -1;
        for (int i = 0; i < lines.Count; i++)
        {
            var t = lines[i].Trim().Split(' ');
            if (t.Length >= 3 && t[0] == "COMP" && int.TryParse(t[1], out int nid) && nid == nodeId)
            {
                if (seen == slot) { removeIdx = i; break; }
                seen++;
            }
        }
        if (removeIdx < 0) { CancelEdit(); return false; }
        lines.RemoveAt(removeIdx);
        for (int i = lines.Count - 1; i >= 0; i--)   // CPROP の slot 補正 (=削除 / >削除なら -1)
        {
            var t = lines[i].Trim().Split(' ');
            if (t.Length >= 3 && t[0] == "CPROP" && int.TryParse(t[1], out int nid) && nid == nodeId && int.TryParse(t[2], out int s))
            {
                if (s == slot) lines.RemoveAt(i);
                else if (s > slot) { t[2] = (s - 1).ToString(); lines[i] = string.Join(" ", t); }
            }
        }
        ComponentsText = string.Join("\n", lines);
        if (!ComponentsText.EndsWith("\n")) ComponentsText += "\n";
        ComponentsChanged?.Invoke();
        if (_viewportMode) RenderViewport();
        CommitEdit();
        return true;
    }
    private int _spawnSeq;
    private int _execBudget;

    /// <summary>実行可能なグラフ (ノード) を持つか。コンポーネントのみ BP の判定に使う。</summary>
    public bool HasGraph => _nodes.Count > 0;

}
