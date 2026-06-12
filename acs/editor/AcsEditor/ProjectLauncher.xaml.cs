using System;
using System.IO;
using System.Windows;
using System.Windows.Controls.Primitives;

namespace AcsEditor;

/// <summary>起動時のプロジェクトランチャー。新規作成 / 既存を開く / 最近使った一覧。</summary>
public partial class ProjectLauncher : Window
{
    /// <summary>選択 (作成 or オープン) されたプロジェクト。キャンセル時は null。</summary>
    public Project? SelectedProject { get; private set; }

    private string _template = "2d";

    public ProjectLauncher()
    {
        InitializeComponent();

        // 既定の作成先: ~/Documents/AcsProjects
        string docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        LocationBox.Text = Path.Combine(docs, "AcsProjects");

        foreach (string p in ProjectManager.GetRecents())
            RecentsList.Items.Add(p);

        UpdateTemplateToggles();
        StatusText.Text = "テンプレートを選び『作成して開く』、または既存プロジェクトを開いてください。";
    }

    private void OnPickTemplate(object sender, RoutedEventArgs e)
    {
        if (sender is ToggleButton tb && tb.Tag is string tag)
            _template = tag;
        UpdateTemplateToggles();
    }

    private void UpdateTemplateToggles()
    {
        TplBlank.IsChecked = _template == "blank";
        Tpl2D.IsChecked    = _template == "2d";
    }

    private void OnBrowseLocation(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog
        {
            Title = "プロジェクトの作成先フォルダ",
        };
        if (Directory.Exists(LocationBox.Text)) dlg.InitialDirectory = LocationBox.Text;
        if (dlg.ShowDialog(this) == true) LocationBox.Text = dlg.FolderName;
    }

    private void OnCreate(object sender, RoutedEventArgs e)
    {
        try
        {
            var proj = ProjectManager.CreateNew(NameBox.Text, LocationBox.Text, _template);
            SelectedProject = proj;
            DialogResult = true;
            Close();
        }
        catch (Exception ex)
        {
            StatusText.Text = "作成失敗: " + ex.Message;
            MessageBox.Show(this, ex.Message, "プロジェクト作成に失敗", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void OnOpenExisting(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "プロジェクトを開く",
            Filter = "ACS Project (*.acsproject)|*.acsproject|All files (*.*)|*.*",
            DefaultExt = ".acsproject",
        };
        if (dlg.ShowDialog(this) != true) return;
        OpenPath(dlg.FileName);
    }

    private void OnRecentDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (RecentsList.SelectedItem is string path) OpenPath(path);
    }

    private void OpenPath(string acsprojectPath)
    {
        try
        {
            SelectedProject = ProjectManager.Open(acsprojectPath);
            DialogResult = true;
            Close();
        }
        catch (Exception ex)
        {
            StatusText.Text = "オープン失敗: " + ex.Message;
            MessageBox.Show(this, ex.Message, "プロジェクトを開けませんでした", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        SelectedProject = null;
        DialogResult = false;
        Close();
    }
}
