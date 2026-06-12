using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// TreeViewItem に「複数選択集合の一員か (IsInSet) / primary か (IsPrimary)」を表す添付プロパティ。
/// WPF の TreeView は単一選択しか持たないため、ABI の選択集合を階層ツリーへ反映する手段として
/// 各 TreeViewItem にこのフラグを立てる。ABI が真実点で、C# はここに状態を写すだけ
/// (RefreshHierarchyHighlight)。
///
/// IsInSet が立つと TreeViewItem.Background を淡いアクセント色に直接設定する (スタイルトリガに
/// 依存しない)。primary (native IsSelected) はテーマの TreeViewItem テンプレート側の IsSelected
/// トリガで明るいアクセントに塗られ、こちらより優先される。
/// </summary>
public static class SelectionHighlight
{
    private static readonly Brush InSetBrush;

    static SelectionHighlight()
    {
        InSetBrush = new SolidColorBrush(Color.FromArgb(0x55, 0x3C, 0x9A, 0xE8));
        InSetBrush.Freeze();
    }

    /// <summary>選択集合に含まれるか。</summary>
    public static readonly DependencyProperty IsInSetProperty =
        DependencyProperty.RegisterAttached("IsInSet", typeof(bool), typeof(SelectionHighlight),
            new PropertyMetadata(false, OnIsInSetChanged));

    public static void SetIsInSet(DependencyObject o, bool v) => o.SetValue(IsInSetProperty, v);
    public static bool GetIsInSet(DependencyObject o) => (bool)o.GetValue(IsInSetProperty);

    private static void OnIsInSetChanged(DependencyObject o, DependencyPropertyChangedEventArgs e)
    {
        if (o is Control c)
        {
            if ((bool)e.NewValue) c.Background = InSetBrush;
            else c.ClearValue(Control.BackgroundProperty);   // primary の IsSelected 表示へ戻す
        }
    }

    /// <summary>primary (active) ノードか。IsInSet より強く (明るく) 表示する。</summary>
    public static readonly DependencyProperty IsPrimaryProperty =
        DependencyProperty.RegisterAttached("IsPrimary", typeof(bool), typeof(SelectionHighlight),
            new PropertyMetadata(false));

    public static void SetIsPrimary(DependencyObject o, bool v) => o.SetValue(IsPrimaryProperty, v);
    public static bool GetIsPrimary(DependencyObject o) => (bool)o.GetValue(IsPrimaryProperty);
}
