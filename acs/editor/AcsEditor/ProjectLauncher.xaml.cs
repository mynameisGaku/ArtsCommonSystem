using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Threading.Tasks;

namespace AcsEditor;

public sealed class RecentProjectItem
{
    public required string Name { get; init; }
    public required string Path { get; init; }
    public string Status => File.Exists(Path) ? "" : "MISSING";
}

/// <summary>起動時のプロジェクトランチャー。新規作成 / 既存を開く / 最近使った一覧。</summary>
public partial class ProjectLauncher : Window
{
    /// <summary>選択 (作成 or オープン) されたプロジェクト。キャンセル時は null。</summary>
    public Project? SelectedProject { get; private set; }

    private string _template = "2d";
    private bool _openInProgress;

    public ProjectLauncher()
    {
        InitializeComponent();

        // 既定の作成先: ~/Documents/AcsProjects
        string docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        LocationBox.Text = Path.Combine(docs, "AcsProjects");

        foreach (string p in ProjectManager.GetRecents())
        {
            string name = System.IO.Path.GetFileNameWithoutExtension(p);
            RecentsList.Items.Add(new RecentProjectItem {
                Name = string.IsNullOrWhiteSpace(name) ? "Unnamed Project" : name,
                Path = p
            });
        }
        RecentCountText.Text = RecentsList.Items.Count == 1
            ? "1 PROJECT"
            : $"{RecentsList.Items.Count} PROJECTS";

        UpdateTemplateToggles();
        StatusText.Text = RecentsList.Items.Count == 0
            ? "No recent projects. Open an existing project or create a new one."
            : "Ready";
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

    private async void OnOpenExisting(object sender, RoutedEventArgs e)
    {
        if (_openInProgress) return;
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "プロジェクトを開く",
            Filter = "ACS Project (*.acsproject)|*.acsproject|All files (*.*)|*.*",
            DefaultExt = ".acsproject",
        };
        if (dlg.ShowDialog(this) != true) return;
        await OpenPathAsync(dlg.FileName);
    }

    private async void OnRecentDoubleClick(
        object sender,
        System.Windows.Input.MouseButtonEventArgs e)
    {
        await OpenSelectedRecentAsync();
    }

    private void OnRecentSelectionChanged(object sender, SelectionChangedEventArgs e) =>
        OpenRecentBtn.IsEnabled =
            !_openInProgress &&
            RecentsList.SelectedItem is RecentProjectItem;

    private async void OnOpenRecent(object sender, RoutedEventArgs e) =>
        await OpenSelectedRecentAsync();

    private async void OnRecentKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter) return;
        e.Handled = true;
        await OpenSelectedRecentAsync();
    }

    private Task OpenSelectedRecentAsync()
    {
        return RecentsList.SelectedItem is RecentProjectItem item
            ? OpenPathAsync(item.Path)
            : Task.CompletedTask;
    }

    private async Task OpenPathAsync(string acsprojectPath)
    {
        if (_openInProgress) return;
        _openInProgress = true;
        LauncherContent.IsEnabled = false;
        Cursor = Cursors.Wait;
        StatusText.Text = "Opening project and recovering interrupted asset operations...";
        try
        {
            Project project = await Task.Run(() => ProjectManager.Open(acsprojectPath));
            if (!IsLoaded) return;
            SelectedProject = project;
            DialogResult = true;
            Close();
        }
        catch (Exception ex)
        {
            if (!IsLoaded) return;
            LauncherContent.IsEnabled = true;
            Cursor = null;
            StatusText.Text = "オープン失敗: " + ex.Message;
            MessageBox.Show(this, ex.Message, "プロジェクトを開けませんでした", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally
        {
            _openInProgress = false;
            if (IsLoaded)
            {
                LauncherContent.IsEnabled = true;
                Cursor = null;
                OpenRecentBtn.IsEnabled =
                    RecentsList.SelectedItem is RecentProjectItem;
            }
        }
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        if (_openInProgress) return;
        SelectedProject = null;
        DialogResult = false;
        Close();
    }
}
