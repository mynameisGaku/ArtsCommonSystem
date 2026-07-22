// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using AcsEditor.Packaging;
using Microsoft.Win32;

namespace AcsEditor;

public partial class PackageProjectDialog : Window
{
    internal const bool DisablesOwnerDuringPrompt = false;

    private readonly Project _project;
    private readonly Action<string> _externalLog;
    private CancellationTokenSource? _cancellation;
    private bool _busy;
    private bool _allowClose;
    private string? _resultZip;
    private TaskCompletionSource<bool>? _modelessCompletion;
    private Window? _modelessOwner;

    public bool PackageSucceeded { get; private set; }

    private sealed record IssueRow(
        string Label,
        string Message,
        string Detail,
        Brush Color);

    public PackageProjectDialog(Project project, Action<string> externalLog)
    {
        InitializeComponent();
        _project = project;
        _externalLog = externalLog;
        ProjectTitle.Text = project.Name;
        VersionBox.Text = "0.1.0";
        OutputBox.Text = Path.Combine(project.RootDir, "Build", "Packages");
        ProfileBox.SelectedIndex = (int)PackageProfile.Shipping;
        ValidateAndDisplay();
    }

    internal Task<bool> ShowModelessAsync(Window owner)
    {
        ArgumentNullException.ThrowIfNull(owner);
        owner.Dispatcher.VerifyAccess();
        if (_modelessCompletion != null || IsVisible)
            throw new InvalidOperationException(
                "Package Project window is already being presented.");

        _modelessCompletion = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _modelessOwner = owner;
        Owner = owner;
        ShowActivated = owner.IsActive;
        Closed += OnModelessClosed;
        owner.Closed += OnModelessOwnerClosed;
        try
        {
            Show();
        }
        catch
        {
            Closed -= OnModelessClosed;
            owner.Closed -= OnModelessOwnerClosed;
            _modelessOwner = null;
            TaskCompletionSource<bool> completion = _modelessCompletion;
            _modelessCompletion = null;
            completion.TrySetCanceled();
            throw;
        }
        return _modelessCompletion.Task;
    }

    private void OnModelessOwnerClosed(object? sender, EventArgs e)
    {
        _allowClose = true;
        _cancellation?.Cancel();
        if (IsVisible) Close();
    }

    private void OnModelessClosed(object? sender, EventArgs e)
    {
        Closed -= OnModelessClosed;
        if (_modelessOwner != null)
            _modelessOwner.Closed -= OnModelessOwnerClosed;
        _modelessOwner = null;
        TaskCompletionSource<bool>? completion = _modelessCompletion;
        _modelessCompletion = null;
        completion?.TrySetResult(PackageSucceeded);
    }

    private PackageOptions ReadOptions() => new(
        OutputDirectory: string.IsNullOrWhiteSpace(OutputBox.Text)
            ? Path.Combine(_project.RootDir, "Build", "Packages")
            : OutputBox.Text.Trim(),
        ProductVersion: VersionBox.Text.Trim(),
        IncludeDebugSymbols: IncludeSymbolsCheck.IsChecked == true,
        Profile: (PackageProfile)Math.Clamp(
            ProfileBox.SelectedIndex,
            (int)PackageProfile.Development,
            (int)PackageProfile.Shipping));

    private IReadOnlyList<PackageIssue> Preflight()
    {
        var issues = PackagingService.Validate(_project, ReadOptions()).ToList();
        if (BuildReleaseCheck.IsChecked == true)
        {
            int executableIndex = issues.FindIndex(issue => issue.Code == "EXECUTABLE_MISSING");
            if (executableIndex >= 0)
            {
                issues[executableIndex] = new(
                    PackageIssueSeverity.Info,
                    "EXECUTABLE_WILL_BUILD",
                    "Release実行ファイルはPackage開始時にビルドします。");
            }

            string cmake = Path.Combine(_project.SourceDir, "CMakeLists.txt");
            if (!File.Exists(cmake))
            {
                issues.Add(new(
                    PackageIssueSeverity.Info,
                    "BUILD_FILES_WILL_REPAIR",
                    "Source/CMakeLists.txt はRelease build開始時に現在のテンプレートから再生成します。",
                    cmake));
            }
        }
        return issues;
    }

    private void ValidateAndDisplay()
    {
        IReadOnlyList<PackageIssue> issues;
        try { issues = Preflight(); }
        catch (Exception error)
        {
            issues = [new(
                PackageIssueSeverity.Error,
                "PREFLIGHT_FAILED",
                error.Message)];
        }

        ValidationList.ItemsSource = issues.Select(ToRow).ToArray();
        int errors = issues.Count(issue => issue.Severity == PackageIssueSeverity.Error);
        int warnings = issues.Count(issue => issue.Severity == PackageIssueSeverity.Warning);
        ValidationSummary.Text = errors > 0
            ? $"{errors} ERROR  /  {warnings} WARNING"
            : warnings > 0 ? $"{warnings} WARNING" : "READY";
        PackageButton.IsEnabled = !_busy && errors == 0;
    }

    private static IssueRow ToRow(PackageIssue issue)
    {
        (string label, Brush color) = issue.Severity switch
        {
            PackageIssueSeverity.Error => ("ERROR", new SolidColorBrush(Color.FromRgb(236, 106, 102))),
            PackageIssueSeverity.Warning => ("WARNING", new SolidColorBrush(Color.FromRgb(232, 197, 106))),
            _ => ("INFO", new SolidColorBrush(Color.FromRgb(111, 168, 232))),
        };
        color.Freeze();
        return new(label + "  " + issue.Code, issue.Message, issue.Path ?? "", color);
    }

    private void OnOptionChanged(object sender, RoutedEventArgs e)
    {
        if (!IsInitialized)
            return;
        ValidateAndDisplay();
    }

    private void OnProfileChanged(
        object sender,
        System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (!IsInitialized)
            return;
        bool shipping =
            ProfileBox.SelectedIndex == (int)PackageProfile.Shipping;
        if (shipping)
            IncludeSymbolsCheck.IsChecked = false;
        IncludeSymbolsCheck.IsEnabled = !_busy && !shipping;
        ValidateAndDisplay();
    }

    private void OnValidate(object sender, RoutedEventArgs e)
    {
        ValidateAndDisplay();
        AppendLog("Preflight validation refreshed.");
    }

    private void OnBrowse(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog
        {
            Title = "Select package output directory",
            InitialDirectory = Directory.Exists(OutputBox.Text)
                ? OutputBox.Text
                : _project.RootDir,
            Multiselect = false,
        };
        if (dialog.ShowDialog(this) == true)
            OutputBox.Text = dialog.FolderName;
    }

    private async void OnPackage(object sender, RoutedEventArgs e)
    {
        ValidateAndDisplay();
        if (Preflight().Any(issue => issue.Severity == PackageIssueSeverity.Error))
            return;

        SetBusy(true);
        LogBox.Clear();
        _resultZip = null;
        PackageSucceeded = false;
        OpenResultButton.IsEnabled = false;
        ResultPathText.Text = "Packaging in progress…";
        _cancellation = new CancellationTokenSource();

        var progress = new Progress<PackageProgress>(item =>
        {
            StatusText.Text = item.Message;
            if (item.Total > 0)
            {
                PackageProgressBar.IsIndeterminate = false;
                PackageProgressBar.Maximum = item.Total;
                PackageProgressBar.Value = Math.Min(item.Completed, item.Total);
            }
            else
            {
                PackageProgressBar.IsIndeterminate = true;
            }
        });

        try
        {
            AppendLog("Package pipeline started.");
            PackageResult result = await PackagingService.PackageAsync(
                _project,
                ReadOptions(),
                buildRelease: BuildReleaseCheck.IsChecked == true,
                forceConfigure: true,
                Log,
                progress,
                _cancellation.Token);
            _resultZip = result.ZipPath;
            PackageSucceeded = true;
            ResultPathText.Text = result.ZipPath;
            StatusText.Text = $"Complete  /  {result.FileCount} files  /  {FormatBytes(result.UncompressedBytes)}";
            AppendLog($"Build ID: {result.BuildId}");
            AppendLog(
                $"Cooked assets: {result.CookedAssetCount}  /  " +
                $"game.acpak SHA-256: {result.AssetPackSha256}");
            AppendLog($"Package complete: {result.ZipPath}");
            OpenResultButton.IsEnabled = true;
        }
        catch (OperationCanceledException)
        {
            StatusText.Text = "Cancelled";
            ResultPathText.Text = "Package cancelled. Existing package was not replaced.";
            AppendLog("Package cancelled.");
        }
        catch (PackageValidationException error)
        {
            ValidationList.ItemsSource = error.Issues.Select(ToRow).ToArray();
            StatusText.Text = "Validation failed";
            ResultPathText.Text = "Package was not generated.";
            foreach (PackageIssue issue in error.Issues)
                AppendLog($"{issue.Severity} [{issue.Code}] {issue.Message} {issue.Path}");
        }
        catch (Exception error)
        {
            StatusText.Text = "Package failed";
            ResultPathText.Text = "Package was not generated.";
            AppendLog("ERROR: " + error.Message);
        }
        finally
        {
            _cancellation.Dispose();
            _cancellation = null;
            SetBusy(false);
            ValidateAndDisplay();
        }
    }

    private void Log(string message)
    {
        AppendLog(message);
        _externalLog(message);
    }

    private void AppendLog(string message)
    {
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.BeginInvoke(() => AppendLog(message));
            return;
        }
        LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        LogBox.ScrollToEnd();
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        VersionBox.IsEnabled = !busy;
        OutputBox.IsEnabled = !busy;
        ProfileBox.IsEnabled = !busy;
        BuildReleaseCheck.IsEnabled = !busy;
        IncludeSymbolsCheck.IsEnabled =
            !busy &&
            ProfileBox.SelectedIndex != (int)PackageProfile.Shipping;
        PackageButton.Content = busy ? "Packaging…" : "Package Project";
        PackageButton.IsEnabled = !busy;
        PackageProgressBar.IsIndeterminate = busy;
        if (!busy && !PackageSucceeded)
        {
            PackageProgressBar.IsIndeterminate = false;
            PackageProgressBar.Value = 0;
        }
    }

    private void OnOpenResult(object sender, RoutedEventArgs e)
    {
        if (_resultZip == null || !File.Exists(_resultZip))
            return;
        try
        {
            var start = new ProcessStartInfo
            {
                FileName = "explorer.exe",
                UseShellExecute = true,
            };
            start.ArgumentList.Add("/select," + _resultZip);
            Process.Start(start);
        }
        catch (Exception error)
        {
            AppendLog("Explorerを開けません: " + error.Message);
        }
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
        {
            if (e.ClickCount == 2)
                ToggleMaximize();
            else
                DragMove();
        }
    }

    private void OnMinimize(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void OnMaximizeRestore(object sender, RoutedEventArgs e) => ToggleMaximize();

    private void ToggleMaximize() =>
        WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal
            : WindowState.Maximized;

    private void OnClose(object sender, RoutedEventArgs e)
    {
        if (_busy)
        {
            _cancellation?.Cancel();
            StatusText.Text = "Cancelling after the active build step…";
            return;
        }
        _allowClose = true;
        Close();
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (_busy && !_allowClose)
        {
            _cancellation?.Cancel();
            StatusText.Text = "Cancelling after the active build step…";
            e.Cancel = true;
            return;
        }
        if (!_allowClose)
            _allowClose = true;
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = ["B", "KiB", "MiB", "GiB"];
        double value = bytes;
        int unit = 0;
        while (value >= 1024.0 && unit < units.Length - 1)
        {
            value /= 1024.0;
            unit++;
        }
        return $"{value:0.##} {units[unit]}";
    }
}
