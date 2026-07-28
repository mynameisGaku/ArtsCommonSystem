using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Threading;
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
    // WPF evaluates this property on its Dispatcher. Availability must be
    // snapshotted by the launcher worker instead of probing an offline path
    // from this binding getter.
    public required string Status { get; init; }
}

/// <summary>起動時のプロジェクトランチャー。新規作成 / 既存を開く / 最近使った一覧。</summary>
public partial class ProjectLauncher : Window
{
    /// <summary>選択 (作成 or オープン) されたプロジェクト。キャンセル時は null。</summary>
    public Project? SelectedProject { get; private set; }

    private string _template = "3d";
    private readonly ProjectLauncherAsyncGate _recentLoadGate = new();
    private readonly ProjectLauncherAsyncGate _operationGate = new();
    private readonly CancellationTokenSource _lifetime = new();
    private bool _closeRequested;
    private bool _allowClose;

    public ProjectLauncher()
    {
        InitializeComponent();

        // 既定の作成先: ~/Documents/AcsProjects
        string docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        LocationBox.Text = Path.Combine(docs, "AcsProjects");

        UpdateTemplateToggles();
        RecentCountText.Text = "LOADING...";
        StatusText.Text = "Loading recent projects...";
        Loaded += OnLauncherLoaded;
        Closing += OnLauncherClosing;
        Closed += OnLauncherClosed;
    }

    private async void OnLauncherLoaded(object sender, RoutedEventArgs e)
    {
        Loaded -= OnLauncherLoaded;
        ProjectLauncherAsyncTicket ticket = _recentLoadGate.BeginLatest();
        IReadOnlyList<ProjectLauncherRecentEntry> entries;
        try
        {
            entries =
                await ProjectLauncherBackgroundOperations.LoadRecentEntriesAsync(
                    () => ProjectManager.GetRecentPathsSnapshot(),
                    cancellationToken: _lifetime.Token);
        }
        catch (OperationCanceledException)
            when (_lifetime.IsCancellationRequested)
        {
            return;
        }
        catch (Exception error)
        {
            if (!CanPublishRecentEntries(ticket))
                return;
            RecentCountText.Text = "UNAVAILABLE";
            if (!_operationGate.IsExclusiveActive)
            {
                StatusText.Text =
                    "Recent projects could not be loaded: " + error.Message;
            }
            return;
        }

        if (!CanPublishRecentEntries(ticket))
            return;
        RecentsList.Items.Clear();
        foreach (ProjectLauncherRecentEntry entry in entries)
        {
            RecentsList.Items.Add(new RecentProjectItem
            {
                Name = entry.Name,
                Path = entry.Path,
                Status = entry.Status,
            });
        }
        RecentCountText.Text = entries.Count == 1
            ? "1 PROJECT"
            : $"{entries.Count} PROJECTS";
        if (!_operationGate.IsExclusiveActive)
        {
            StatusText.Text = entries.Count == 0
                ? "No recent projects. Open an existing project or create a new one."
                : "Ready";
        }
    }

    private bool CanPublishRecentEntries(ProjectLauncherAsyncTicket ticket) =>
        IsLoaded &&
        !_lifetime.IsCancellationRequested &&
        _recentLoadGate.IsCurrent(ticket) &&
        !Dispatcher.HasShutdownStarted &&
        !Dispatcher.HasShutdownFinished;

    private void OnPickTemplate(object sender, RoutedEventArgs e)
    {
        if (sender is ToggleButton tb && tb.Tag is string tag)
            _template = tag;
        UpdateTemplateToggles();
    }

    private void UpdateTemplateToggles()
    {
        Tpl3D.IsChecked = _template == "3d";
        Tpl2D.IsChecked = _template == "2d";
    }

    private void OnBrowseLocation(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog
        {
            Title = "プロジェクトの作成先フォルダ",
            // LocationBox may contain a disconnected UNC path. Probing it (or
            // asking the shell to resolve it as the initial folder) would
            // block the Dispatcher, so start from a known local location.
            InitialDirectory = Environment.GetFolderPath(
                Environment.SpecialFolder.MyDocuments),
        };
        if (dlg.ShowDialog(this) == true) LocationBox.Text = dlg.FolderName;
    }

    private async void OnCreate(object sender, RoutedEventArgs e)
    {
        if (!_operationGate.TryBeginExclusive(
                out ProjectLauncherAsyncTicket ticket))
        {
            return;
        }
        string name = NameBox.Text;
        string location = LocationBox.Text;
        string template = _template;
        SetOperationBusy(
            busy: true,
            "Creating project and indexing initial assets...");
        try
        {
            Project proj =
                await ProjectLauncherBackgroundOperations.RunAsync(
                    () => ProjectManager.CreateNew(
                        name,
                        location,
                        template),
                    _lifetime.Token);
            if (!CanPublishOperation(ticket))
                return;
            CommitSuccessfulOperation(ticket, proj);
        }
        catch (OperationCanceledException)
            when (_lifetime.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            if (!CanPublishOperation(ticket))
                return;
            StatusText.Text = "作成失敗: " + ex.Message;
            MessageBox.Show(this, ex.Message, "プロジェクト作成に失敗", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally
        {
            CompleteOperation(ticket);
        }
    }

    private async void OnOpenExisting(object sender, RoutedEventArgs e)
    {
        if (_operationGate.IsExclusiveActive) return;
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
            !_operationGate.IsExclusiveActive &&
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
        if (!_operationGate.TryBeginExclusive(
                out ProjectLauncherAsyncTicket ticket))
        {
            return;
        }
        SetOperationBusy(
            busy: true,
            "Opening project and recovering interrupted asset operations...");
        try
        {
            Project project =
                await ProjectLauncherBackgroundOperations.RunAsync(
                    () => ProjectManager.Open(acsprojectPath),
                    _lifetime.Token);
            if (!CanPublishOperation(ticket))
                return;
            CommitSuccessfulOperation(ticket, project);
        }
        catch (OperationCanceledException)
            when (_lifetime.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            if (!CanPublishOperation(ticket))
                return;
            StatusText.Text = "オープン失敗: " + ex.Message;
            MessageBox.Show(this, ex.Message, "プロジェクトを開けませんでした", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally
        {
            CompleteOperation(ticket);
        }
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        if (_operationGate.IsExclusiveActive)
        {
            RequestCloseAfterOperation();
            return;
        }
        SelectedProject = null;
        DialogResult = false;
        Close();
    }

    private bool CanPublishOperation(ProjectLauncherAsyncTicket ticket) =>
        IsLoaded &&
        !_closeRequested &&
        !_lifetime.IsCancellationRequested &&
        _operationGate.IsCurrent(ticket) &&
        !Dispatcher.HasShutdownStarted &&
        !Dispatcher.HasShutdownFinished;

    private void CommitSuccessfulOperation(
        ProjectLauncherAsyncTicket ticket,
        Project project)
    {
        // Setting DialogResult synchronously begins WPF's modal close. Release
        // the exclusive operation and approve that close first; otherwise the
        // Closing guard would reinterpret a successful open as a deferred
        // user-cancel request.
        if (!CanPublishOperation(ticket) ||
            !_operationGate.TryCompleteExclusive(ticket))
        {
            return;
        }

        _allowClose = true;
        SelectedProject = project;
        try
        {
            DialogResult = true;
        }
        catch
        {
            // If a host presents this window outside the normal modal path,
            // keep it usable and surface the original error through the
            // operation handler.
            SelectedProject = null;
            _allowClose = false;
            SetOperationBusy(busy: false, status: null);
            throw;
        }
    }

    private void CompleteOperation(ProjectLauncherAsyncTicket ticket)
    {
        if (!_operationGate.TryCompleteExclusive(ticket) ||
            !IsLoaded ||
            _lifetime.IsCancellationRequested ||
            Dispatcher.HasShutdownStarted ||
            Dispatcher.HasShutdownFinished)
        {
            return;
        }
        if (_closeRequested)
        {
            _allowClose = true;
            SelectedProject = null;
            DialogResult = false;
            Close();
            return;
        }
        SetOperationBusy(busy: false, status: null);
    }

    private void SetOperationBusy(bool busy, string? status)
    {
        OpenExistingBtn.IsEnabled = !busy;
        RecentsList.IsEnabled = !busy;
        OpenRecentBtn.IsEnabled =
            !busy && RecentsList.SelectedItem is RecentProjectItem;
        NameBox.IsEnabled = !busy;
        LocationBox.IsEnabled = !busy;
        BrowseLocationBtn.IsEnabled = !busy;
        Tpl3D.IsEnabled = !busy;
        Tpl2D.IsEnabled = !busy;
        CreateBtn.IsEnabled = !busy;
        CreateBtn.Content = busy ? "Working..." : "Create Project";
        if (status != null)
            StatusText.Text = status;
    }

    private void RequestCloseAfterOperation()
    {
        if (_closeRequested)
            return;
        _closeRequested = true;
        CloseBtn.IsEnabled = false;
        StatusText.Text =
            "Finishing the active filesystem transaction before closing...";
    }

    private void OnLauncherClosing(object? sender, CancelEventArgs e)
    {
        if (ProjectLauncherClosePolicy.ShouldDeferClose(
                _operationGate.IsExclusiveActive,
                _allowClose))
        {
            e.Cancel = true;
            RequestCloseAfterOperation();
        }
    }

    private void OnLauncherClosed(object? sender, EventArgs e)
    {
        _lifetime.Cancel();
        _recentLoadGate.Dispose();
        _operationGate.Dispose();
        _lifetime.Dispose();
    }
}
