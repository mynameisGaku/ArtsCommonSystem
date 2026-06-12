using System.Windows;
using System.Windows.Documents;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>ヒエラルキー D&D 中のドロップ位置を視覚化する。mode: 0=前に挿入, 1=後に挿入, 2=子にする。</summary>
public sealed class HierarchyDropAdorner : Adorner
{
    public Rect TargetRect;   // 被装飾要素 (TreeView) 座標での対象行の矩形
    public int  Mode = -1;    // -1=非表示, 0=before, 1=after, 2=child

    private static readonly Pen   Line = MakePen();
    private static readonly Brush Fill = MakeFill();

    public HierarchyDropAdorner(UIElement adorned) : base(adorned) { IsHitTestVisible = false; }

    private static Pen MakePen()
    {
        var p = new Pen(new SolidColorBrush(Color.FromRgb(0x3C, 0x9A, 0xE8)), 2.0);
        p.Freeze();
        return p;
    }
    private static Brush MakeFill()
    {
        var b = new SolidColorBrush(Color.FromArgb(0x55, 0x3C, 0x9A, 0xE8));
        b.Freeze();
        return b;
    }

    protected override void OnRender(DrawingContext dc)
    {
        if (Mode < 0) return;
        if (Mode == 2)   // 子にする: 行をハイライト
        {
            dc.DrawRoundedRectangle(Fill, Line, TargetRect, 3, 3);
            return;
        }
        // before / after: 行の上端 / 下端に挿入ラインを引く
        double y = Mode == 0 ? TargetRect.Top : TargetRect.Bottom;
        double x0 = TargetRect.Left, x1 = TargetRect.Right;
        dc.DrawLine(Line, new Point(x0, y), new Point(x1, y));
        // 左端に小さな三角マーカー
        var tri = new StreamGeometry();
        using (var g = tri.Open())
        {
            g.BeginFigure(new Point(x0, y - 4), true, true);
            g.LineTo(new Point(x0 + 6, y), true, false);
            g.LineTo(new Point(x0, y + 4), true, false);
        }
        tri.Freeze();
        dc.DrawGeometry(Line.Brush, null, tri);
    }
}
