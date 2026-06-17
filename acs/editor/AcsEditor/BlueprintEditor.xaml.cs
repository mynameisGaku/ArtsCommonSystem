using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;

namespace AcsEditor;

/// <summary>
/// ビジュアル Blueprint グラフエディタ。
/// BP1: ノード/ピン/接続線の描画 + ノードドラッグ + 背景パン。
/// BP2: ピンをドラッグして接続を生成/切断 + ホイールでズーム。
/// 実行/シリアライズ/パレットからのノード追加は後続フェーズ。
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

    private enum PinKind { Exec, Data }
    private sealed record Pin(string Name, PinKind Kind);

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
        public Dictionary<int, string> Literals = new();   // 未接続データ入力ピンの定数値 (入力index→文字列)
    }

    private sealed record BpConn(int FromNode, int FromPin, int ToNode, int ToPin);

    /// <summary>ヒットしたピンの参照 (どのノードの・何番目の・入出力どちらの・種別)。</summary>
    private sealed record PinRef(BpNode Node, int Index, bool Output, PinKind Kind);

    // ----- ノードパレット (右クリック生成のテンプレ。外部=MainWindow から供給) -----
    /// <summary>ピン定義 (名前 + exec/data)。Exec=true で実行ピン。</summary>
    public sealed record BpPinSpec(string Name, bool Exec);
    /// <summary>パレットの 1 エントリ (分類 + タイトル + ヘッダ色 + 入出力ピン)。</summary>
    public sealed record BpNodeTemplate(string Category, string Title, Color Header,
                                        BpPinSpec[] Inputs, BpPinSpec[] Outputs);

    private readonly List<BpNode>         _nodes   = new();
    private readonly List<BpConn>         _conns   = new();
    private readonly List<Path>           _wires   = new();
    private readonly List<BpNodeTemplate> _palette = new();
    private int   _nextId = 100;        // 生成ノードの ID (デモの 1..4 と衝突しない起点)
    private Point _menuGraphPos;        // 右クリック位置 (生成先のグラフ座標)
    private const int InheritOffset = 100000;   // 継承元ノードの ID オフセット (子と衝突しないように)
    private string? _parentPath;        // 親 (継承元) .acsbp の相対パス (null = 継承なし)

    // 表示変換: 先にズーム、続いてパン (RenderTransform = Translate * Scale)。
    private readonly ScaleTransform     _zoom = new(1, 1);
    private readonly TranslateTransform _pan  = new();

    // ドラッグ状態。
    private BpNode?  _dragNode;     // ノード移動中
    private bool     _panning;      // 背景パン中
    private PinRef?  _wireSource;   // ピンからのワイヤ生成中 (アンカー側ピン)
    private Path?    _ghost;        // 生成中ワイヤのプレビュー
    private Point    _lastMouse;

    // 配色。
    private static readonly Brush ExecWire = new SolidColorBrush(Color.FromRgb(0xE6, 0xE9, 0xEF));
    private static readonly Brush DataWire = new SolidColorBrush(Color.FromRgb(0x5B, 0xC8, 0x9A));
    private static readonly Brush NodeBg   = new SolidColorBrush(Color.FromRgb(0x23, 0x29, 0x33));
    private static readonly Brush NodeEdge = new SolidColorBrush(Color.FromRgb(0x3A, 0x44, 0x52));
    private static readonly Brush PinExec  = new SolidColorBrush(Color.FromRgb(0xE6, 0xE9, 0xEF));
    private static readonly Brush PinData  = new SolidColorBrush(Color.FromRgb(0x5B, 0xC8, 0x9A));
    private static readonly Brush LabelFg  = new SolidColorBrush(Color.FromRgb(0xC2, 0xC9, 0xD4));
    private static readonly Brush ExecHi   = new SolidColorBrush(Color.FromRgb(0xE0, 0xB8, 0x4A));   // 実行済みノードの枠
    private static readonly Brush InheritEdge = new SolidColorBrush(Color.FromRgb(0x7A, 0x8A, 0xB0)); // 継承ノードの枠

    public BlueprintEditor()
    {
        InitializeComponent();
        GraphCanvas.RenderTransform = new TransformGroup { Children = { _zoom, _pan } };
        GraphCanvas.PreviewMouseLeftButtonDown += OnPreviewDown;   // ピンドラッグを最優先で奪う
        GraphCanvas.MouseLeftButtonDown        += OnCanvasDown;    // 空き場所 = パン
        GraphCanvas.MouseMove                  += OnCanvasMove;
        GraphCanvas.MouseUp                    += OnCanvasUp;
        GraphCanvas.MouseWheel                 += OnWheel;
        GraphCanvas.MouseRightButtonUp         += OnContextMenu;   // 右クリック = ノードパレット / 削除
        Loaded += (_, __) => { if (_nodes.Count == 0) BuildDemoGraph(); };
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
            ins:  new[] { new Pin("▶", PinKind.Exec), new Pin("path", PinKind.Data), new Pin("pos", PinKind.Data) },
            outs: new[] { new Pin("▶", PinKind.Exec), new Pin("spawned", PinKind.Data) });
        var setp = AddNode(3, "Set Position", 620, 110, fn,
            ins:  new[] { new Pin("▶", PinKind.Exec), new Pin("target", PinKind.Data), new Pin("x", PinKind.Data), new Pin("y", PinKind.Data) },
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
        foreach (var c in _conns) { var p = MakeWire(); _wires.Add(p); GraphCanvas.Children.Add(p); }  // 線は背面
        foreach (var n in _nodes) { n.Visual = MakeNodeVisual(n); GraphCanvas.Children.Add(n.Visual); }
        RedrawWires();
    }

    private BpNode? NodeById(int id) { foreach (var n in _nodes) if (n.Id == id) return n; return null; }

    /// <summary>ピンの «グラフ座標» 中心位置。枠からはみ出さないよう PinR だけ内側に置く。 </summary>
    private static Point PinPos(BpNode n, bool output, int idx)
    {
        double x = n.X + (output ? NodeW - PinR - PinInset : PinR + PinInset);
        double y = n.Y + HeaderH + idx * RowH + RowH / 2.0;
        return new Point(x, y);
    }

    private FrameworkElement MakeNodeVisual(BpNode n)
    {
        int rows = Math.Max(n.Inputs.Count, n.Outputs.Count);
        double h = HeaderH + rows * RowH + 6;

        var inner = new Canvas { ClipToBounds = false, Width = NodeW, Height = h };
        // ヘッダ。
        inner.Children.Add(Place(new Border {
            Width = NodeW, Height = HeaderH, Background = n.Header,
            CornerRadius = new CornerRadius(5, 5, 0, 0) }, 0, 0));
        inner.Children.Add(Place(new TextBlock {
            Text = n.Title, Foreground = Brushes.White, FontSize = 11, FontWeight = FontWeights.SemiBold }, 9, 5));
        // 入力ピン (左辺)。未接続のデータ入力にはインライン定数入力欄を出す。
        for (int i = 0; i < n.Inputs.Count; i++)
        {
            double py = HeaderH + i * RowH + RowH / 2.0;
            var pin = n.Inputs[i];
            inner.Children.Add(Place(MakePinDot(pin.Kind), PinInset, py - PinR));   // 枠の内側に収める
            inner.Children.Add(Place(new TextBlock {
                Text = pin.Name, Foreground = LabelFg, FontSize = 11 }, 14, py - 9));
            if (pin.Kind == PinKind.Data && !IsInputConnected(n, i))
            {
                int idx = i;
                // 定数欄は左寄りの専用帯 (50..104) に置き、出力ラベル帯 (108..) と重ねない。
                var tb = new TextBox {
                    Width = 54, Height = 17, FontSize = 10, Padding = new Thickness(2, 0, 2, 0),
                    Background = new SolidColorBrush(Color.FromRgb(0x18, 0x1C, 0x23)), Foreground = Brushes.White,
                    BorderBrush = NodeEdge, BorderThickness = new Thickness(1), VerticalContentAlignment = VerticalAlignment.Center,
                    Text = n.Literals.TryGetValue(i, out var lv) ? lv : "",
                };
                tb.TextChanged += (_, __) => { if (string.IsNullOrEmpty(tb.Text)) n.Literals.Remove(idx); else n.Literals[idx] = tb.Text; };
                inner.Children.Add(Place(tb, 50, py - 9));
            }
        }
        // 出力ピン (右辺、右寄せ)。ラベルは右側の専用帯に限定し、定数欄と重ねない。
        for (int j = 0; j < n.Outputs.Count; j++)
        {
            double py = HeaderH + j * RowH + RowH / 2.0;
            inner.Children.Add(Place(MakePinDot(n.Outputs[j].Kind), NodeW - 2 * PinR - PinInset, py - PinR));   // 枠の内側に収める
            inner.Children.Add(Place(new TextBlock {
                Text = n.Outputs[j].Name, Foreground = LabelFg, FontSize = 11,
                Width = 54, TextAlignment = TextAlignment.Right }, NodeW - 68, py - 9));
        }

        // 枠線(outline) と内容(inner) を «兄弟» にして重ねる。Border の子に inner を入れると
        // BorderThickness ぶん内容が内側へずれ、右の出力ピンが枠外へはみ出す (実行済み枠 2.4px で顕著)。
        // 背景(body) → 内容(inner) → 枠線(outline) の順に重ね、outline は IsHitTestVisible=false で
        // 下の TextBox/本体ドラッグへクリックを通す。
        var body = new Border {
            Width = NodeW, Height = h, Background = NodeBg, CornerRadius = new CornerRadius(5),
        };
        var outline = new Border {
            Width = NodeW, Height = h, Background = Brushes.Transparent,
            BorderBrush = n.Executed ? ExecHi : (n.Inherited ? InheritEdge : NodeEdge),
            BorderThickness = new Thickness(n.Executed ? 2.4 : 1.2),
            CornerRadius = new CornerRadius(5), IsHitTestVisible = false,
        };
        var outer = new Canvas {
            Width = NodeW, Height = h, ClipToBounds = false,
            Cursor = n.Inherited ? Cursors.Arrow : Cursors.SizeAll,
            Opacity = n.Inherited ? 0.55 : 1.0,   // 継承ノードは淡色で読取専用
        };
        outer.Children.Add(body);
        outer.Children.Add(inner);
        outer.Children.Add(outline);
        Canvas.SetLeft(outer, n.X); Canvas.SetTop(outer, n.Y);
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
            _dragNode = n; _lastMouse = e.GetPosition(GraphCanvas);
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

    /// <summary>クリック元が TextBox (または配下) かを視覚ツリーを遡って判定。</summary>
    private static bool IsTextBoxOrigin(object src)
    {
        var d = src as DependencyObject;
        while (d != null) { if (d is TextBox) return true; d = VisualTreeHelper.GetParent(d); }
        return false;
    }

    private static Ellipse MakePinDot(PinKind kind) => new()
    {
        Width = PinR * 2, Height = PinR * 2,
        Fill = kind == PinKind.Exec ? PinExec : PinData,
        Stroke = new SolidColorBrush(Color.FromRgb(0x10, 0x14, 0x1A)), StrokeThickness = 1.0,
    };

    private static T Place<T>(T el, double left, double top) where T : UIElement
    {
        Canvas.SetLeft(el, left); Canvas.SetTop(el, top); return el;
    }

    private static Path MakeWire() => new() { StrokeThickness = 2.4, Fill = null, SnapsToDevicePixels = true };

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
            _wires[i].Data   = Bez(p0, p1);
            _wires[i].Stroke = exec ? ExecWire : DataWire;
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
        if (tgt != null && _wireSource != null &&
            tgt.Node != _wireSource.Node &&         // 自ノードへは繋がない
            tgt.Output != _wireSource.Output &&     // 出力↔入力
            tgt.Kind == _wireSource.Kind)           // exec↔exec / data↔data
        {
            var outRef = _wireSource.Output ? _wireSource : tgt;
            var inRef  = _wireSource.Output ? tgt : _wireSource;
            AddConnection(outRef.Node, outRef.Index, inRef.Node, inRef.Index, outRef.Kind);
        }
        if (_ghost != null) { GraphCanvas.Children.Remove(_ghost); _ghost = null; }
        _wireSource = null;
        Rebuild();   // 空き場所へ落とした場合 (= 切断) も反映
    }

    // ----- 入力 -----
    /// <summary>ピン上で押した場合のみワイヤ生成を開始 (ノードドラッグ/パンより優先)。</summary>
    private void OnPreviewDown(object sender, MouseButtonEventArgs e)
    {
        Point g = e.GetPosition(GraphCanvas);
        var hit = PinHitTest(g);
        if (hit == null) return;   // ピン以外はノードドラッグ/パンに委ねる

        PinRef source = hit;
        bool picked = false;
        // 既存の «単数側» ピンを掴んだら、その線を «持ち上げて» 反対端から張り直す (空き場所へ落とせば切断)。
        if (!hit.Output && hit.Kind == PinKind.Data)
        {
            int idx = _conns.FindIndex(c => c.ToNode == hit.Node.Id && c.ToPin == hit.Index);
            if (idx >= 0) { var c = _conns[idx]; _conns.RemoveAt(idx);
                var fn = NodeById(c.FromNode); if (fn != null) { source = new PinRef(fn, c.FromPin, true, PinKind.Data); picked = true; } }
        }
        else if (hit.Output && hit.Kind == PinKind.Exec)
        {
            int idx = _conns.FindIndex(c => c.FromNode == hit.Node.Id && c.FromPin == hit.Index);
            if (idx >= 0) { var c = _conns[idx]; _conns.RemoveAt(idx);
                var tn = NodeById(c.ToNode); if (tn != null) { source = new PinRef(tn, c.ToPin, false, PinKind.Exec); picked = true; } }
        }

        _wireSource = source;
        if (picked) Rebuild();   // 持ち上げた線を消してから描き直す
        _ghost = MakeGhost(source.Kind);
        GraphCanvas.Children.Add(_ghost);
        UpdateGhost(g);
        GraphCanvas.CaptureMouse();
        e.Handled = true;
    }

    // ----- 背景パン (空き場所のドラッグ) -----
    private void OnCanvasDown(object sender, MouseButtonEventArgs e)
    {
        if (_wireSource != null || _dragNode != null) return;
        _panning = true; _lastMouse = e.GetPosition(this); GraphCanvas.CaptureMouse();
    }

    private void OnCanvasMove(object sender, MouseEventArgs e)
    {
        if (_wireSource != null) { UpdateGhost(e.GetPosition(GraphCanvas)); return; }
        if (_dragNode != null)
        {
            var p = e.GetPosition(GraphCanvas);
            _dragNode.X += p.X - _lastMouse.X; _dragNode.Y += p.Y - _lastMouse.Y; _lastMouse = p;
            if (_dragNode.Visual is { } b) { Canvas.SetLeft(b, _dragNode.X); Canvas.SetTop(b, _dragNode.Y); }
            RedrawWires();
            return;
        }
        if (_panning)
        {
            var p = e.GetPosition(this);
            _pan.X += p.X - _lastMouse.X; _pan.Y += p.Y - _lastMouse.Y; _lastMouse = p;
        }
    }

    private void OnCanvasUp(object sender, MouseButtonEventArgs e)
    {
        if (_wireSource != null) { FinishWire(e.GetPosition(GraphCanvas)); GraphCanvas.ReleaseMouseCapture(); return; }
        if (_dragNode != null)   { _dragNode = null; GraphCanvas.ReleaseMouseCapture(); return; }
        if (_panning)            { _panning = false; GraphCanvas.ReleaseMouseCapture(); }
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
            double h = HeaderH + Math.Max(n.Inputs.Count, n.Outputs.Count) * RowH + 6;
            if (g.X >= n.X && g.X <= n.X + NodeW && g.Y >= n.Y && g.Y <= n.Y + h) return n;
        }
        return null;
    }

    private void OnContextMenu(object sender, MouseButtonEventArgs e)
    {
        if (_wireSource != null || _dragNode != null || _panning) return;
        Point g = e.GetPosition(GraphCanvas);
        var menu = new ContextMenu();

        var hit = NodeAt(g);
        if (hit != null)
        {
            var dup = new MenuItem { Header = $"ノードを複製   ({hit.Title})" };
            dup.Click += (_, __) => DuplicateNode(hit);
            menu.Items.Add(dup);
            var del = new MenuItem { Header = $"ノードを削除   ({hit.Title})" };
            del.Click += (_, __) => DeleteNode(hit);
            menu.Items.Add(del);
        }
        else
        {
            _menuGraphPos = g;
            if (_palette.Count == 0)
            {
                menu.Items.Add(new MenuItem { Header = "(パレットが空です)", IsEnabled = false });
            }
            else
            {
                // 分類ごとにサブメニューへまとめる (登場順を維持)。
                var byCat = new Dictionary<string, MenuItem>();
                var order = new List<string>();
                foreach (var t in _palette)
                {
                    if (!byCat.TryGetValue(t.Category, out var parent))
                    {
                        parent = new MenuItem { Header = t.Category };
                        byCat[t.Category] = parent; order.Add(t.Category);
                    }
                    var captured = t;
                    var item = new MenuItem { Header = t.Title };
                    item.Click += (_, __) => SpawnFromTemplate(captured, _menuGraphPos);
                    parent.Items.Add(item);
                }
                foreach (var c in order) menu.Items.Add(byCat[c]);
            }
        }
        menu.PlacementTarget = GraphCanvas;
        menu.IsOpen = true;
        e.Handled = true;
    }

    private void SpawnFromTemplate(BpNodeTemplate t, Point g)
    {
        var n = new BpNode { Id = _nextId++, Title = t.Title, X = g.X, Y = g.Y, Header = new SolidColorBrush(t.Header) };
        foreach (var p in t.Inputs)  n.Inputs.Add(new Pin(p.Name, p.Exec ? PinKind.Exec : PinKind.Data));
        foreach (var p in t.Outputs) n.Outputs.Add(new Pin(p.Name, p.Exec ? PinKind.Exec : PinKind.Data));
        _nodes.Add(n);
        Rebuild();
    }

    private void DeleteNode(BpNode n)
    {
        _conns.RemoveAll(c => c.FromNode == n.Id || c.ToNode == n.Id);
        _nodes.Remove(n);
        Rebuild();
    }

    /// <summary>ノードを複製する (少しずらして配置、ピン/定数を複製、接続は引き継がない)。</summary>
    private void DuplicateNode(BpNode n)
    {
        var c = new BpNode { Id = _nextId++, Title = n.Title, X = n.X + 26, Y = n.Y + 26, Header = n.Header };
        c.Inputs.AddRange(n.Inputs);    // Pin は record(不変)なので参照共有で安全
        c.Outputs.AddRange(n.Outputs);
        foreach (var kv in n.Literals) c.Literals[kv.Key] = kv.Value;
        _nodes.Add(c);
        Rebuild();
    }

    // ----- シリアライズ (.acsbp。行ベースのテキスト形式) -----
    //   ACSBP 1
    //   N <id> <x> <y> <RRGGBB> <title...>     ← title は行末まで (空白可)
    //   I <E|D> <name...>                       ← 直前 N の入力ピン
    //   O <E|D> <name...>                       ← 直前 N の出力ピン
    //   C <fromNode> <fromPin> <toNode> <toPin>

    /// <summary>初期ディレクトリ (保存/開くダイアログ)。MainWindow がプロジェクトの Assets を設定。</summary>
    public string? DefaultDir { get; set; }
    /// <summary>現在開いている .acsbp のパス (保存ダイアログを省いて上書き保存するため)。</summary>
    public string? CurrentPath { get; private set; }
    public Action? PathChanged;   // CurrentPath が変わった → ウィンドウタイトル更新用

    /// <summary>現在のグラフを .acsbp テキストへ直列化する。</summary>
    public string Serialize()
    {
        var sb = new StringBuilder();
        sb.Append("ACSBP 1\n");
        if (!string.IsNullOrEmpty(_parentPath)) sb.Append("PARENT ").Append(_parentPath).Append('\n');   // 継承元
        foreach (var v in Variables)   // BP 変数: VAR <type> <name> <value…>
            if (!string.IsNullOrWhiteSpace(v.Name)) sb.Append("VAR ").Append(string.IsNullOrEmpty(v.Type) ? "String" : v.Type)
                .Append(' ').Append(v.Name.Trim()).Append(' ').Append(v.Value ?? "").Append('\n');
        if (!string.IsNullOrEmpty(ComponentsText))   // コンポーネント木: CMP <行数> + 逐語の ACSCENE 行
        {
            var clines = ComponentsText.Replace("\r", "").TrimEnd('\n').Split('\n');
            sb.Append("CMP ").Append(clines.Length).Append('\n');
            foreach (var cl in clines) sb.Append(cl).Append('\n');
        }
        foreach (var n in _nodes)
        {
            if (n.Inherited) continue;   // 継承ノードは親側が持つので保存しない
            var c = (n.Header as SolidColorBrush)?.Color ?? Colors.SteelBlue;
            sb.Append("N ").Append(n.Id).Append(' ')
              .Append(n.X.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append(n.Y.ToString("0.##", CultureInfo.InvariantCulture)).Append(' ')
              .Append($"{c.R:X2}{c.G:X2}{c.B:X2}").Append(' ').Append(n.Title).Append('\n');
            foreach (var p in n.Inputs)  sb.Append("I ").Append(p.Kind == PinKind.Exec ? 'E' : 'D').Append(' ').Append(p.Name).Append('\n');
            foreach (var p in n.Outputs) sb.Append("O ").Append(p.Kind == PinKind.Exec ? 'E' : 'D').Append(' ').Append(p.Name).Append('\n');
            foreach (var kv in n.Literals)
                if (!string.IsNullOrEmpty(kv.Value)) sb.Append("V ").Append(kv.Key).Append(' ').Append(kv.Value).Append('\n');
        }
        foreach (var k in _conns)
            if (k.FromNode < InheritOffset && k.ToNode < InheritOffset)   // 継承ノード間の線は親側が持つ
                sb.Append("C ").Append(k.FromNode).Append(' ').Append(k.FromPin).Append(' ')
                  .Append(k.ToNode).Append(' ').Append(k.ToPin).Append('\n');
        return sb.ToString();
    }

    /// <summary>.acsbp テキストからグラフを復元する (既存は破棄)。PARENT 行があれば親グラフを継承ノードとして読み込む。</summary>
    public void Deserialize(string text)
    {
        _nodes.Clear(); _conns.Clear(); _parentPath = null;
        Variables.Clear();
        ComponentsText = "";

        // CMP <n> (コンポーネント木の逐語 ACSCENE) を抜き出し、残り行をグラフ/変数として解析する
        // (ACSCENE の COMP/数値行が グラフ parser の C/N と衝突するため先に除去する)。
        {
            var all = text.Replace("\r", "").Split('\n');
            var kept = new StringBuilder();
            for (int i = 0; i < all.Length; i++)
            {
                if (all[i].StartsWith("CMP ") && int.TryParse(all[i].Substring(4).Trim(), out int cn))
                {
                    var cb = new StringBuilder();
                    for (int j = 0; j < cn && i + 1 < all.Length; j++) cb.Append(all[++i]).Append('\n');
                    ComponentsText = cb.ToString();
                }
                else kept.Append(all[i]).Append('\n');
            }
            text = kept.ToString();
        }

        foreach (var raw in text.Split('\n'))   // VAR <type> <name> <value…> (旧 BB <name> <value…> も読む)
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
        }

        // PARENT 行を先に処理し、親グラフを «継承ノード» (id +InheritOffset, 読取専用) として読み込む。
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.StartsWith("PARENT "))
            {
                _parentPath = line.Substring(7).Trim();
                string? full = ResolveParentPath(_parentPath);
                if (full != null && System.IO.File.Exists(full))
                    ParseGraph(System.IO.File.ReadAllText(full), InheritOffset, inherited: true);
                break;
            }
        }
        ParseGraph(text, 0, inherited: false);   // 子 (自分) のノード

        int maxId = 0; foreach (var n in _nodes) if (!n.Inherited && n.Id > maxId) maxId = n.Id;
        _nextId = Math.Max(_nextId, maxId + 1);   // 以後の生成 ID が読込ノードと衝突しないように
        Rebuild();
        VariablesChanged?.Invoke();   // パネルを新しい変数で再描画
        ComponentsChanged?.Invoke();  // Components パネルを再描画
    }

    // .acsbp の N/I/O/V/C 行を解析して _nodes/_conns へ追加する。idOffset/inherited で継承ノードに使う。
    private void ParseGraph(string text, int idOffset, bool inherited)
    {
        BpNode? cur = null;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0 || line.StartsWith("ACSBP") || line.StartsWith("PARENT")) continue;
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
                    var pin = new Pin(t[2], t[1] == "E" ? PinKind.Exec : PinKind.Data);
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
                case 'C':
                {
                    var t = line.Split(' ');
                    if (t.Length < 5) break;
                    _conns.Add(new BpConn(ParseInt(t[1]) + idOffset, ParseInt(t[2]), ParseInt(t[3]) + idOffset, ParseInt(t[4])));
                    break;
                }
            }
        }
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
            System.IO.File.WriteAllText(dlg.FileName, Serialize(), new System.Text.UTF8Encoding(false));
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
        CurrentPath = path;
        PathChanged?.Invoke();
    }

    /// <summary>開いているファイルへ上書き保存する (CurrentPath が無ければ false)。</summary>
    public bool SaveToCurrent()
    {
        if (string.IsNullOrEmpty(CurrentPath)) return false;
        try { System.IO.File.WriteAllText(CurrentPath, Serialize(), new System.Text.UTF8Encoding(false)); }
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
        _parentPath = MakeRelative(dlg.FileName);
        Deserialize(Serialize());   // 現在の子 (継承ノードは保存対象外) を保ったまま、新しい親を継承ノードとして読み直す
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

    /// <summary>BP クラスが持つ変数 (名前 + 型 + 既定値)。実行開始時に値へ種まきされる (UE5 の Variables 相当)。</summary>
    public sealed class BpVar { public string Name; public string Type; public string Value; public BpVar(string n, string t, string v) { Name = n; Type = t; Value = v; } }
    /// <summary>この BP の変数一覧 (My Blueprint パネルが編集)。</summary>
    public List<BpVar> Variables { get; } = new();
    public Action? VariablesChanged;   // 変数が変わった (Deserialize 等) → パネル再描画
    public Action? AfterRun;            // RunGraph 終了 (予約)
    public IReadOnlyDictionary<string, string> LiveVars => _vars;

    /// <summary>この BP の «コンポーネント木» (= 生成される実体。ACSCENE サブツリー逐語テキスト)。
    /// 空ならロジックのみの BP。UE5 の Blueprint Class の Components 相当 (旧 Prefab を内包)。</summary>
    public string ComponentsText = "";
    public Action? ComponentsChanged;   // ComponentsText が変わった (Deserialize 等) → Components パネル再描画

    /// <summary>この BP の «root ノード» へコンポーネント型を 1 つ追加する。root の «最後の COMP 行» の直後へ
    /// 挿入し、新コンポーネントを最後の slot にして既存 CPROP の slot 対応を壊さない。成功で true。</summary>
    public bool AddComponentToRoot(string type)
    {
        if (string.IsNullOrWhiteSpace(ComponentsText) || string.IsNullOrWhiteSpace(type)) return false;
        var lines = ComponentsText.Replace("\r", "").Split('\n').ToList();
        int rootId = -1, insertAt = -1;
        for (int i = 0; i < lines.Count; i++)
        {
            var l = lines[i].Trim();
            if (l.Length == 0 || !char.IsDigit(l[0])) continue;   // 最初のノード行 (= root)
            var t = l.Split(' ');
            if (t.Length >= 3 && int.TryParse(t[0], out rootId)) { insertAt = i + 1; break; }
        }
        if (rootId < 0) return false;
        for (int i = insertAt; i < lines.Count; i++)   // root の最後の COMP 行直後へ
        {
            var t = lines[i].Trim().Split(' ');
            if (t.Length >= 2 && t[0] == "COMP" && t[1] == rootId.ToString()) insertAt = i + 1;
        }
        lines.Insert(insertAt, $"COMP {rootId} {type}");
        ComponentsText = string.Join("\n", lines);
        if (!ComponentsText.EndsWith("\n")) ComponentsText += "\n";
        ComponentsChanged?.Invoke();
        return true;
    }

    /// <summary>ノード nodeId の slot 番コンポーネントを削除する (COMP 行 + 該当 CPROP を除去し、後続 slot を 1 詰める)。</summary>
    public bool RemoveComponentFromNode(int nodeId, int slot)
    {
        if (string.IsNullOrWhiteSpace(ComponentsText)) return false;
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
        if (removeIdx < 0) return false;
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
        return true;
    }
    private int _spawnSeq;
    private int _execBudget;

    private void OnRun(object sender, RoutedEventArgs e) => RunGraph();

    /// <summary>
    /// イベントノード (exec 出力を持ち exec 入力を持たないノード) を起点に exec を辿って実行する。
    /// データ入力は «プル評価» で上流をたどる (例: Spawn の spawned → SetPosition の target)。
    /// 個々のノードの作用は今はトレース出力 (将来はエンジンの反射メソッド/サブシステムへ束縛)。
    /// </summary>
    public void RunGraph()
    {
        foreach (var n in _nodes) n.Executed = false;
        _spawnHandles.Clear(); _returns.Clear(); _spawnSeq = 0; _execBudget = 1000;
        _vars.Clear();
        foreach (var v in Variables)   // BP 変数を _vars へ種まき (Get Variable が既定値を読める)
            if (!string.IsNullOrWhiteSpace(v.Name)) _vars[v.Name.Trim()] = v.Value ?? "";

        var entries = _nodes
            .Where(n => n.Outputs.Any(p => p.Kind == PinKind.Exec) && !n.Inputs.Any(p => p.Kind == PinKind.Exec)
                     && !n.Title.StartsWith("On Event"))   // On Event は自動起動せず Publish 受信でのみ発火
            .OrderBy(n => n.Inherited ? 0 : 1).ThenBy(n => n.Id).ToList();   // 親(継承)→子の順 = Super 相当

        Trace($"▶ Blueprint 実行開始 ({entries.Count} イベント)");
        foreach (var ev in entries) ExecFrom(ev);
        Trace("■ Blueprint 実行終了");
        Rebuild();   // 実行済みノードを枠ハイライト
        AfterRun?.Invoke();   // パネルへ live 値の更新を促す
    }

    private void ExecFrom(BpNode n)
    {
        if (_execBudget-- <= 0) { Trace("  … (ステップ上限に達したため停止)"); return; }
        n.Executed = true;
        Trace("  → " + ActionLine(n));

        // Branch は cond を評価し True / False 出力のうち «一方だけ» を発火する。
        if (n.Title.StartsWith("Branch"))
        {
            string want = Truthy(EvalInputByName(n, "cond")) ? "True" : "False";
            for (int po = 0; po < n.Outputs.Count; po++)
                if (n.Outputs[po].Kind == PinKind.Exec && n.Outputs[po].Name == want)
                    FireExec(n, po);
            return;
        }
        // 通常ノード: 全 exec 出力を上から順に発火 (Sequence 等)。
        for (int po = 0; po < n.Outputs.Count; po++)
            if (n.Outputs[po].Kind == PinKind.Exec)
                FireExec(n, po);
    }

    /// <summary>ノード n の po 番出力 exec ピンに繋がる下流をすべて実行する。</summary>
    private void FireExec(BpNode n, int po)
    {
        foreach (var c in _conns)
            if (c.FromNode == n.Id && c.FromPin == po)
            {
                var to = NodeById(c.ToNode);
                if (to != null) ExecFrom(to);
            }
    }

    /// <summary>文字列を真偽として評価 (空/false/0/no/off/未接続=偽、数値は非0=真、他は真)。</summary>
    private static bool Truthy(string s)
    {
        s = (s ?? "").Trim().ToLowerInvariant();
        if (s.Length == 0 || s == "false" || s == "0" || s == "no" || s == "off" || s == "(未接続)") return false;
        if (double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var d)) return d != 0;
        return true;
    }

    /// <summary>a op b を比較して真偽を返す (両方数値なら数値比較、それ以外は ==/!= の文字列比較)。</summary>
    private static bool EvalCompare(string a, string op, string b)
    {
        op = (op ?? "").Trim();
        if (op.Length == 0) op = "==";
        bool na = double.TryParse((a ?? "").Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var da);
        bool nb = double.TryParse((b ?? "").Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var db);
        if (na && nb)
            switch (op)
            {
                case "==": return da == db;
                case "!=": return da != db;
                case "<":  return da <  db;
                case "<=": return da <= db;
                case ">":  return da >  db;
                case ">=": return da >= db;
            }
        string sa = (a ?? "").Trim(), sb = (b ?? "").Trim();
        return op == "!=" ? sa != sb : sa == sb;
    }

    /// <summary>channel に一致する On Event ノードをすべて発火する (Publish からの 1対多 通知)。発火数を返す。</summary>
    private int FireEventSubscribers(string channel)
    {
        channel = (channel ?? "").Trim();
        int count = 0;
        foreach (var ev in _nodes.Where(x => x.Title.StartsWith("On Event")).ToList())
            if (EvalInputByName(ev, "channel").Trim() == channel) { ExecFrom(ev); count++; }
        return count;
    }

    private string ActionLine(BpNode n)
    {
        string t = n.Title;
        if (t.StartsWith("On Event")) return $"イベント On Event(channel={EvalInputByName(n, "channel")})";
        if (t.StartsWith("On ") || t.StartsWith("Event")) return "イベント " + t.Trim();
        if (t.StartsWith("Spawn"))
        {
            string path = EvalInputByName(n, "path");
            string pos  = EvalInputByName(n, "pos");
            string? newId = SpawnPrefab?.Invoke(path, pos);   // 実プレハブを生成し新ノード id を得る
            if (newId != null) _returns[n.Id] = newId;        // spawned 出力 = 実ノード id (下流へ流れる)
            string handle = newId ?? SpawnHandle(n);
            return $"Spawn Prefab(path={path}, pos={pos}) ⇒ {handle}{(newId != null ? " [生成 OK]" : "")}";
        }
        if (t.StartsWith("Set Position"))
        {
            string target = EvalInputByName(n, "target"), x = EvalInputByName(n, "x"), y = EvalInputByName(n, "y");
            string? r = BuiltinOp?.Invoke("SetPosition", new[] { target, x, y });   // 実ノードへ適用 (永続)
            return $"Set Position(target={target}, x={x}, y={y}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Get Position"))
        {
            string target = EvalInputByName(n, "target");
            string? r = BuiltinOp?.Invoke("GetPosition", new[] { target });
            if (r != null) _returns[n.Id] = r;   // "x,y" を pos 出力→下流へ
            return $"Get Position(target={target}) ⇒ {r ?? "対象なし"}";
        }
        if (t.StartsWith("Destroy"))
        {
            string target = EvalInputByName(n, "target");
            string? r = BuiltinOp?.Invoke("Destroy", new[] { target });   // 実ノードを削除
            return $"Destroy(target={target}) [{(r != null ? "削除 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Color"))
        {
            string target = EvalInputByName(n, "target");
            string cr = EvalInputByName(n, "r"), cg = EvalInputByName(n, "g"), cb = EvalInputByName(n, "b");
            string? r = BuiltinOp?.Invoke("SetColor", new[] { target, cr, cg, cb });
            return $"Set Color(target={target}, r={cr}, g={cg}, b={cb}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Visible"))
        {
            string target = EvalInputByName(n, "target"), v = EvalInputByName(n, "visible");
            string? r = BuiltinOp?.Invoke("SetVisible", new[] { target, v });
            return $"Set Visible(target={target}, visible={v}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Scale"))
        {
            string target = EvalInputByName(n, "target"), a = EvalInputByName(n, "sx"), b = EvalInputByName(n, "sy");
            string? r = BuiltinOp?.Invoke("SetScale", new[] { target, a, b });
            return $"Set Scale(target={target}, sx={a}, sy={b}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Rotation"))
        {
            string target = EvalInputByName(n, "target"), d = EvalInputByName(n, "deg");
            string? r = BuiltinOp?.Invoke("SetRotation", new[] { target, d });
            return $"Set Rotation(target={target}, deg={d}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Reparent"))
        {
            string target = EvalInputByName(n, "target"), p = EvalInputByName(n, "parent");
            string? r = BuiltinOp?.Invoke("Reparent", new[] { target, p });
            return $"Reparent(target={target}, parent={p}) [{(r != null ? "OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Variable"))
        {
            string name = EvalInputByName(n, "name").Trim(), val = EvalInputByName(n, "value");
            if (name.Length > 0) _vars[name] = val;
            return $"Set Variable({name} = {val})";
        }
        if (t.StartsWith("Publish"))
        {
            string ch = EvalInputByName(n, "channel");
            if (ch == "(なし)") return t.Trim();   // channel 入力の無い旧 Publish ノードは従来通り
            int fired = FireEventSubscribers(ch);
            return $"Publish Event(channel={ch}) → {fired} 購読";
        }
        if (t.StartsWith("Print"))    return "Print: " + EvalInputByName(n, "text");
        if (t.StartsWith("Compare"))
        {
            string ca = EvalInputByName(n, "a"), cop = EvalInputByName(n, "op"), cb = EvalInputByName(n, "b");
            bool res = EvalCompare(ca, cop, cb);
            _returns[n.Id] = res ? "true" : "false";   // result 出力 → 下流 (Branch.cond 等) へ
            return $"Compare({ca} {cop} {cb}) ⇒ {(res ? "true" : "false")}";
        }
        if (t.StartsWith("Branch"))
        {
            string cv = EvalInputByName(n, "cond");
            return $"Branch(cond={cv}) → {(Truthy(cv) ? "True" : "False")}";
        }
        if (t.StartsWith("Sequence")) return "Sequence";
        if (n.Inputs.Any(p => p.Name == "target") && t.Contains('.'))   // 反射関数ノード "Owner.Method"
        {
            var parts = t.Split('.', 2);
            string owner = parts[0], method = parts[1];
            string target = EvalInputByName(n, "target");
            bool hasArg = n.Inputs.Any(p => p.Name == "arg");
            string arg = hasArg ? EvalInputByName(n, "arg") : "";
            string? ret = InvokeMethod?.Invoke(owner, method, target, arg);   // target ノード (無指定なら選択) へ実呼出
            bool ok = ret != null;
            if (ok && ret!.Length > 0) _returns[n.Id] = ret;                  // 戻り値を data 出力プル用に保持
            string argStr = hasArg ? $", arg={arg}" : "";
            string retStr = ok && ret!.Length > 0 ? $" = {ret}" : "";
            return $"Call {t}(target={target}{argStr}){retStr} [{(ok ? "実呼出 OK" : "未束縛")}]";
        }
        return t;
    }

    private string SpawnHandle(BpNode n)
    {
        if (!_spawnHandles.TryGetValue(n.Id, out var h)) { h = "spawned#" + (++_spawnSeq); _spawnHandles[n.Id] = h; }
        return h;
    }

    private string EvalInputByName(BpNode n, string pinName)
    {
        int idx = n.Inputs.FindIndex(p => p.Name == pinName);
        return idx < 0 ? "(なし)" : EvalInput(n, idx);
    }

    /// <summary>入力ピンの値を評価する: 接続があれば上流出力、無ければ定数、それも無ければ印。</summary>
    private string EvalInput(BpNode n, int inPin)
    {
        foreach (var c in _conns)
            if (c.ToNode == n.Id && c.ToPin == inPin)
            {
                var from = NodeById(c.FromNode);
                if (from != null) return EvalOutput(from, c.FromPin);
            }
        if (n.Literals.TryGetValue(inPin, out var lit) && lit.Length > 0) return lit;   // 定数
        return "(未接続)";
    }

    private string EvalOutput(BpNode from, int outPin)
    {
        // Get Variable は要求時に変数ストアから読む (pure ノード = exec で実行しない)。
        if (from.Title.StartsWith("Get Variable"))
            return _vars.TryGetValue(EvalInputByName(from, "name").Trim(), out var gv) ? gv : "";
        // 関数ノードの data 出力 = 実行時に得た戻り値 (キャッシュ) を返す。
        if (outPin >= 0 && outPin < from.Outputs.Count && from.Outputs[outPin].Kind == PinKind.Data
            && _returns.TryGetValue(from.Id, out var rv)) return rv;
        if (from.Title.StartsWith("Spawn")) return SpawnHandle(from);
        string pin = outPin >= 0 && outPin < from.Outputs.Count ? from.Outputs[outPin].Name : "out";
        return $"{from.Title.Trim()}.{pin}";
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
        e.Handled = true;
    }
}
