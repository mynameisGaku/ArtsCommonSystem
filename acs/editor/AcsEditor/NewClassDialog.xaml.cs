using System.Windows;

namespace AcsEditor;

/// <summary>新規クラス/ソース生成ダイアログ。クラス名と基底クラスを選ぶ。</summary>
public partial class NewClassDialog : Window
{
    public string ClassName { get; private set; } = "";
    public string BaseClass { get; private set; } = "Empty";

    public NewClassDialog()
    {
        InitializeComponent();
        foreach (string b in ProjectManager.BaseClassOptions) BaseBox.Items.Add(b);
        BaseBox.SelectedIndex = 1;   // 既定 FComponent2D (= 編集可能なユーザーコンポーネント)
        BaseBox.SelectionChanged += (_, _) => UpdateHint();
        UpdateHint();
    }

    private void UpdateHint()
    {
        string b = BaseBox.SelectedItem as string ?? "Empty";
        HintText.Text = b == "Empty"
            ? "指定の名前の空クラスのみを生成します。"
            : b == "FComponent2D"
                ? "<IDENT>_API でエクスポートし、リフレクション登録します。ビルド/ホットリロードでエディタの『ユーザー定義のオブジェクト』に現れ、ノードへ付与できます。"
                : $"{b} を基底に <IDENT>_API でエクスポートするクラスを生成します。";
    }

    private void OnCreate(object sender, RoutedEventArgs e)
    {
        string name = NameBox.Text.Trim();
        if (string.IsNullOrEmpty(name)) { HintText.Text = "クラス名を入力してください。"; return; }
        ClassName = name;
        BaseClass = BaseBox.SelectedItem as string ?? "Empty";
        DialogResult = true;
        Close();
    }

    private void OnCancel(object sender, RoutedEventArgs e) { DialogResult = false; Close(); }
}
