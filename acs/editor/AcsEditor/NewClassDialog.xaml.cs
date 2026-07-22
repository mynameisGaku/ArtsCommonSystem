using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// 新規クラス/ソース生成ダイアログ。クラス名と基底クラスを選ぶ。
/// 基底クラスは Unreal の «Pick Parent Class» 風に階層ツリーで表示し、基底の基底
/// (エンジン基底 → ユーザークラス → その派生) まで入れ子で見えるようにする。
/// </summary>
public partial class NewClassDialog : Window
{
    public string ClassName { get; private set; } = "";
    public string BaseClass { get; private set; } = "Empty";
    public bool   BuildAfter { get; private set; } = true;   // 生成後に即ビルドして型を反映するか

    private static readonly Brush RootFg = new SolidColorBrush(Color.FromRgb(0x9F, 0xC6, 0xF0));   // エンジン基底
    private static readonly Brush UserFg = new SolidColorBrush(Color.FromRgb(0x8B, 0xD0, 0x9A));   // ユーザークラス
    private static readonly Brush EmptyFg = new SolidColorBrush(Color.FromRgb(0xC2, 0xC9, 0xD4));

    public NewClassDialog(Project? project = null)
    {
        InitializeComponent();
        BuildTree(project);
        BaseTree.SelectedItemChanged += (_, __) => UpdateHint();
        UpdateHint();
    }

    // 階層を構築: Empty + エンジン基底 (AComponent/ANode/FScene2D) を根に、
    // プロジェクトのユーザークラスを基底配下へ (連鎖は複数パスで解決)。
    private void BuildTree(Project? project)
    {
        var itemByName = new Dictionary<string, TreeViewItem>();

        var empty = MakeItem("Empty", EmptyFg);
        BaseTree.Items.Add(empty);
        foreach (string rb in ProjectManager.BaseClassOptions)
        {
            if (rb == "Empty") continue;
            var it = MakeItem(rb, RootFg);
            itemByName[rb] = it;
            BaseTree.Items.Add(it);
        }

        if (project != null)
        {
            var pending = new List<(string Name, string Base)>(ProjectManager.ScanUserClasses(project));
            bool progress = true;
            while (progress && pending.Count > 0)
            {
                progress = false;
                for (int i = pending.Count - 1; i >= 0; i--)
                {
                    var (name, bas) = pending[i];
                    if (itemByName.ContainsKey(name)) { pending.RemoveAt(i); continue; }   // 既出
                    if (itemByName.TryGetValue(bas, out var parent))
                    {
                        var it = MakeItem(name, UserFg);
                        parent.Items.Add(it); parent.IsExpanded = true;
                        itemByName[name] = it;
                        pending.RemoveAt(i); progress = true;
                    }
                }
            }
            // 基底が解決できなかったユーザークラスは AComponent 配下へ置く (孤立回避)。
            foreach (var (name, _) in pending)
                if (!itemByName.ContainsKey(name) && itemByName.TryGetValue("AComponent", out var fc))
                {
                    var it = MakeItem(name, UserFg); fc.Items.Add(it); itemByName[name] = it;
                }
        }

        if (itemByName.TryGetValue("AComponent", out var def)) def.IsSelected = true;   // 既定
    }

    private static TreeViewItem MakeItem(string name, Brush fg) =>
        new() { Header = name, Tag = name, Foreground = fg, IsExpanded = true, FontSize = 12 };

    private string SelectedBase() => (BaseTree.SelectedItem as TreeViewItem)?.Tag as string ?? "Empty";

    private void UpdateHint()
    {
        string b = SelectedBase();
        HintText.Text = b == "Empty"
            ? "指定の名前の空クラスのみを生成します。"
            : b == "AComponent"
                ? "<IDENT>_API でエクスポートしリフレクション登録。ビルド/ホットリロードでエディタの『ユーザー定義のオブジェクト』に現れ、ノードへ付与できます。"
                : $"{b} を基底に <IDENT>_API でエクスポートするクラスを生成します。";
    }

    private void OnCreate(object sender, RoutedEventArgs e)
    {
        string name = NameBox.Text.Trim();
        if (string.IsNullOrEmpty(name)) { HintText.Text = "クラス名を入力してください。"; return; }
        ClassName = name;
        BaseClass = SelectedBase();
        BuildAfter = BuildAfterBox.IsChecked == true;
        DialogResult = true;
        Close();
    }

    private void OnCancel(object sender, RoutedEventArgs e) { DialogResult = false; Close(); }
}
