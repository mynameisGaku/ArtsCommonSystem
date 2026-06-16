using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// Blueprint を独立ウィンドウで表示する。左に «ブラックボード» (宣言済みの名前付き
/// 変数) パネル、中央にノードグラフ。ブラックボードは実行開始時に変数へ種まきされ、
/// 実行後は現在値 (live) を表示する。Get/Set Variable ノードから参照できる。
/// </summary>
public partial class BlueprintWindow : Window
{
    /// <summary>このウィンドウが保持するグラフエディタ (MainWindow が配線/読込先に使う)。</summary>
    public BlueprintEditor Editor => GraphHost;

    private static readonly Brush Fg     = new SolidColorBrush(Color.FromRgb(0xD8, 0xDE, 0xE9));
    private static readonly Brush LiveFg = new SolidColorBrush(Color.FromRgb(0x6B, 0xD0, 0x8A));

    public BlueprintWindow()
    {
        InitializeComponent();
        GraphHost.BlackboardChanged = RefreshBlackboard;   // .acsbp 読込で BB が変わったら再描画
        GraphHost.AfterRun          = RefreshLiveValues;   // 実行後に現在値を更新
        Loaded += (_, __) => RefreshBlackboard();
    }

    // 「＋ 追加」: 入力欄のキーを追加 (既存名は値を更新)。
    private void OnAddKey(object sender, RoutedEventArgs e)
    {
        string name = BbName.Text.Trim();
        if (name.Length == 0) return;
        var ex = Editor.Blackboard.Find(k => k.Name.Trim() == name);
        if (ex != null) ex.Value = BbValue.Text;
        else Editor.Blackboard.Add(new BlueprintEditor.BbEntry(name, BbValue.Text));
        BbName.Clear(); BbValue.Clear();
        RefreshBlackboard();
    }

    // ブラックボードのキー一覧を再描画する (名前/値の編集 + 削除 + live 値ラベル)。
    private void RefreshBlackboard()
    {
        BbList.Children.Clear();
        foreach (var key in Editor.Blackboard)
        {
            var k = key;
            var row = new Grid { Margin = new Thickness(0, 2, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var nameBox = new TextBox { Text = k.Name, FontSize = 11, Margin = new Thickness(0, 0, 4, 0), Foreground = Fg };
            nameBox.TextChanged += (_, __) => k.Name = nameBox.Text;
            Grid.SetColumn(nameBox, 0); row.Children.Add(nameBox);

            var valBox = new TextBox { Text = k.Value, FontSize = 11, Margin = new Thickness(0, 0, 4, 0), Foreground = Fg };
            valBox.TextChanged += (_, __) => k.Value = valBox.Text;
            Grid.SetColumn(valBox, 1); row.Children.Add(valBox);

            var del = new Button { Content = "✕", Width = 22, FontSize = 10, Padding = new Thickness(0) };
            del.Click += (_, __) => { Editor.Blackboard.Remove(k); RefreshBlackboard(); };
            Grid.SetColumn(del, 2); row.Children.Add(del);

            BbList.Children.Add(row);

            // 直近の実行結果 (live) を小さく表示。Tag にキーを持たせ RefreshLiveValues で更新する。
            var live = new TextBlock { FontSize = 10, Foreground = LiveFg, Margin = new Thickness(2, 0, 0, 4), Tag = k };
            live.Text = Editor.LiveVars.TryGetValue(k.Name.Trim(), out var lv) ? "= " + lv : "";
            BbList.Children.Add(live);
        }
    }

    // 実行後: 各キーの現在値 (live) ラベルだけ更新する (編集中の TextBox を壊さない)。
    private void RefreshLiveValues()
    {
        foreach (var child in BbList.Children)
            if (child is TextBlock tb && tb.Tag is BlueprintEditor.BbEntry k)
                tb.Text = Editor.LiveVars.TryGetValue(k.Name.Trim(), out var lv) ? "= " + lv : "";
    }
}
