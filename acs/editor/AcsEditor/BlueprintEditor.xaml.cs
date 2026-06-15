using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;

namespace AcsEditor;

/// <summary>
/// ビジュアル Blueprint グラフエディタ (BP1: ノード/ピン/接続線の描画 + ノードドラッグ + 背景パン)。
/// 実行/シリアライズ/ノード追加は後続フェーズ。今はデモグラフを表示・編集できる。
/// </summary>
public partial class BlueprintEditor : UserControl
{
    // ----- レイアウト定数 (ピン位置はこれらから解析的に算出する) -----
    private const double NodeW   = 174;
    private const double HeaderH = 26;
    private const double RowH    = 22;
    private const double PinR    = 5;

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
        public Border? Visual;
    }

    private sealed record BpConn(int FromNode, int FromPin, int ToNode, int ToPin);

    private readonly List<BpNode> _nodes = new();
    private readonly List<BpConn> _conns = new();
    private readonly List<Path>   _wires = new();
    private readonly TranslateTransform _pan = new();

    // ドラッグ状態。
    private BpNode? _dragNode;
    private bool    _panning;
    private Point   _lastMouse;

    // 配色。
    private static readonly Brush ExecWire = new SolidColorBrush(Color.FromRgb(0xE6, 0xE9, 0xEF));
    private static readonly Brush DataWire = new SolidColorBrush(Color.FromRgb(0x5B, 0xC8, 0x9A));
    private static readonly Brush NodeBg   = new SolidColorBrush(Color.FromRgb(0x23, 0x29, 0x33));
    private static readonly Brush NodeEdge = new SolidColorBrush(Color.FromRgb(0x3A, 0x44, 0x52));
    private static readonly Brush PinExec  = new SolidColorBrush(Color.FromRgb(0xE6, 0xE9, 0xEF));
    private static readonly Brush PinData  = new SolidColorBrush(Color.FromRgb(0x5B, 0xC8, 0x9A));
    private static readonly Brush LabelFg  = new SolidColorBrush(Color.FromRgb(0xC2, 0xC9, 0xD4));

    public BlueprintEditor()
    {
        InitializeComponent();
        GraphCanvas.RenderTransform = _pan;
        GraphCanvas.MouseLeftButtonDown += OnCanvasDown;
        GraphCanvas.MouseMove           += OnCanvasMove;
        GraphCanvas.MouseUp             += OnCanvasUp;
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
            ins:  new[] { new Pin("▶", PinKind.Exec), new Pin("target", PinKind.Data), new Pin("value", PinKind.Data) },
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

    /// <summary>ピンの «グラフ座標» 中心位置(入力=左辺, 出力=右辺)。 </summary>
    private static Point PinPos(BpNode n, bool output, int idx)
    {
        double x = n.X + (output ? NodeW : 0.0);
        double y = n.Y + HeaderH + idx * RowH + RowH / 2.0;
        return new Point(x, y);
    }

    private Border MakeNodeVisual(BpNode n)
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
        // 入力ピン (左辺)。
        for (int i = 0; i < n.Inputs.Count; i++)
        {
            double py = HeaderH + i * RowH + RowH / 2.0;
            inner.Children.Add(Place(MakePinDot(n.Inputs[i].Kind), -PinR, py - PinR));
            inner.Children.Add(Place(new TextBlock {
                Text = n.Inputs[i].Name, Foreground = LabelFg, FontSize = 11 }, 9, py - 9));
        }
        // 出力ピン (右辺、右寄せ)。
        for (int j = 0; j < n.Outputs.Count; j++)
        {
            double py = HeaderH + j * RowH + RowH / 2.0;
            inner.Children.Add(Place(MakePinDot(n.Outputs[j].Kind), NodeW - PinR, py - PinR));
            inner.Children.Add(Place(new TextBlock {
                Text = n.Outputs[j].Name, Foreground = LabelFg, FontSize = 11,
                Width = NodeW - 22, TextAlignment = TextAlignment.Right }, 9, py - 9));
        }

        var border = new Border {
            Width = NodeW, Height = h, Background = NodeBg, BorderBrush = NodeEdge, BorderThickness = new Thickness(1.2),
            CornerRadius = new CornerRadius(5), Child = inner, Cursor = Cursors.SizeAll,
        };
        Canvas.SetLeft(border, n.X); Canvas.SetTop(border, n.Y);
        border.MouseLeftButtonDown += (s, e) => { _dragNode = n; _lastMouse = e.GetPosition(this);
                                                  border.CaptureMouse(); e.Handled = true; };
        border.MouseMove += (s, e) => {
            if (_dragNode != n) return;
            var p = e.GetPosition(this);
            n.X += p.X - _lastMouse.X; n.Y += p.Y - _lastMouse.Y; _lastMouse = p;
            Canvas.SetLeft(border, n.X); Canvas.SetTop(border, n.Y);
            RedrawWires();
        };
        border.MouseUp += (s, e) => { if (_dragNode == n) { _dragNode = null; border.ReleaseMouseCapture(); } };
        return border;
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
            double dx = Math.Max(50.0, Math.Abs(p1.X - p0.X) * 0.5);
            var fig = new PathFigure { StartPoint = p0, IsClosed = false };
            fig.Segments.Add(new BezierSegment(new Point(p0.X + dx, p0.Y), new Point(p1.X - dx, p1.Y), p1, true));
            _wires[i].Data   = new PathGeometry(new[] { fig });
            _wires[i].Stroke = exec ? ExecWire : DataWire;
        }
    }

    // ----- 背景パン (空き場所のドラッグ) -----
    private void OnCanvasDown(object sender, MouseButtonEventArgs e)
    {
        if (_dragNode != null) return;
        _panning = true; _lastMouse = e.GetPosition(this); GraphCanvas.CaptureMouse();
    }
    private void OnCanvasMove(object sender, MouseEventArgs e)
    {
        if (!_panning) return;
        var p = e.GetPosition(this);
        _pan.X += p.X - _lastMouse.X; _pan.Y += p.Y - _lastMouse.Y; _lastMouse = p;
    }
    private void OnCanvasUp(object sender, MouseButtonEventArgs e)
    {
        if (_panning) { _panning = false; GraphCanvas.ReleaseMouseCapture(); }
    }
}
