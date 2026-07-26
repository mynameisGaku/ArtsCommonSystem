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
using System.Windows.Threading;
using AcsEditor.Packaging;
using Microsoft.Win32;

namespace AcsEditor;

public partial class PackageProjectDialog : Window
{
    internal const bool DisablesOwnerDuringPrompt = false;
    internal static readonly TimeSpan OwnerShutdownDrainTimeout =
        TimeSpan.FromSeconds(15);

    private readonly Project _project;
    private readonly Action<string> _externalLog;
    private readonly PackageShutdownCoordinator _shutdown = new();
    private CancellationTokenSource? _cancellation;
    private TaskCompletionSource<bool>? _activePackageCompletion;
    private bool _busy;
    private bool _allowClose;
    private bool _ownerShutdownRequested;
    private string? _resultZip;
    private TaskCompletionSource<bool>? _modelessCompletion;
    private Window? _modelessOwner;
    private readonly DispatcherTimer _validationDebounce = new()
    {
        Interval = TimeSpan.FromMilliseconds(180),
    };
    private readonly PackageValidationCoordinator _validation = new();
    private readonly CancellationTokenSource _dialogLifetime = new();
    private int _validationGeneration;
    private bool _lifetimeEnded;

    public bool PackageSucceeded { get; private set; }
    private bool IsCloseRequested =>
        _allowClose || _ownerShutdownRequested;

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
        _validationDebounce.Tick += OnValidationDebounceTick;
        ValidationSummary.Text = "CHECKING...";
        PackageButton.IsEnabled = false;
        ScheduleValidation();
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

    private async void OnModelessOwnerClosed(object? sender, EventArgs e)
    {
        // MainWindow normally drains this workflow from its cancellable Closing
        // path, before WPF reaches Closed.  Keep this fallback safe for any
        // future owner that does not implement that contract: never tear down
        // the dialog while its package child/process-output drain is active.
        bool drained;
        try
        {
            drained = await RequestOwnerShutdownAsync(
                OwnerShutdownDrainTimeout);
        }
        catch (Exception error)
        {
            drained = false;
            try
            {
                _externalLog(
                    "Package shutdown failed while its owner was closing: " +
                    error.Message);
            }
            catch
            {
            }
        }
        if (!drained)
        {
            try
            {
                _externalLog(
                    "Package cancellation did not drain within the shutdown " +
                    "deadline; the package window remains alive to protect the " +
                    "active child process.");
            }
            catch
            {
            }
        }
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

    internal Task<bool> RequestOwnerShutdownAsync(TimeSpan timeout)
    {
        Dispatcher.VerifyAccess();
        return _shutdown.RunOnceAsync(
            () => RequestOwnerShutdownCoreAsync(timeout));
    }

    private async Task<bool> RequestOwnerShutdownCoreAsync(TimeSpan timeout)
    {
        if (_allowClose || _lifetimeEnded)
            return true;

        _ownerShutdownRequested = true;
        _validationDebounce.Stop();
        _validation.CancelLatest();
        checked { _validationGeneration++; }
        if (_busy)
            StatusText.Text = "Cancelling package before editor shutdown...";

        Task? activeOperation = _activePackageCompletion?.Task;
        bool drained = await PackageShutdownCoordinator.CancelAndDrainAsync(
            activeOperation,
            () => _cancellation?.Cancel(),
            timeout);
        if (!drained)
        {
            _ownerShutdownRequested = false;
            if (!Dispatcher.HasShutdownStarted &&
                !Dispatcher.HasShutdownFinished)
            {
                // The timeout and operation completion can become runnable on
                // the dispatcher in either order.  If the operation already
                // skipped its normal UI reset while shutdown was requested,
                // restore the prompt now that editor close has been deferred.
                if (_activePackageCompletion == null && _busy)
                {
                    SetBusy(false);
                    ScheduleValidation(immediate: true);
                }
                StatusText.Text =
                    "Package cancellation is still draining; editor close was deferred.";
            }
            return false;
        }

        _allowClose = true;
        if (IsVisible)
            Close();
        else
            EndDialogLifetime();
        return true;
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

    private static IReadOnlyList<PackageIssue> Preflight(
        Project project,
        PackageOptions options,
        bool buildRelease,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var issues = PackagingService.Validate(
                project,
                options,
                cancellationToken: cancellationToken)
            .ToList();
        if (buildRelease)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int executableIndex = issues.FindIndex(issue => issue.Code == "EXECUTABLE_MISSING");
            if (executableIndex >= 0)
            {
                issues[executableIndex] = new(
                    PackageIssueSeverity.Info,
                    "EXECUTABLE_WILL_BUILD",
                    "Release実行ファイルはPackage開始時にビルドします。");
            }

            string cmake = Path.Combine(project.SourceDir, "CMakeLists.txt");
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

    private Project SnapshotProject() => new()
    {
        Version = _project.Version,
        Name = _project.Name,
        EngineVersion = _project.EngineVersion,
        Template = _project.Template,
        InitialScene = _project.InitialScene,
        CanonicalSceneAssetId = _project.CanonicalSceneAssetId,
        ProjectFilePath = _project.ProjectFilePath,
    };

    private void ScheduleValidation(bool immediate = false)
    {
        if (_busy || IsCloseRequested) return;
        _validationDebounce.Stop();
        _validation.CancelLatest();
        checked { _validationGeneration++; }
        ValidationSummary.Text = "CHECKING...";
        PackageButton.IsEnabled = false;
        if (immediate)
            _ = ValidateAndDisplayAsync(_validationGeneration);
        else
            _validationDebounce.Start();
    }

    private void OnValidationDebounceTick(object? sender, EventArgs e)
    {
        _validationDebounce.Stop();
        _ = ValidateAndDisplayAsync(_validationGeneration);
    }

    private async Task<IReadOnlyList<PackageIssue>> RunPreflightAsync(
        Project project,
        PackageOptions options,
        bool buildRelease,
        CancellationToken cancellationToken = default)
    {
        using PackageValidationOperation operation =
            _validation.BeginLatest(cancellationToken);
        return await _validation.RunAsync(
            operation,
            token =>
            {
                try
                {
                    return Preflight(project, options, buildRelease, token);
                }
                catch (OperationCanceledException)
                {
                    throw;
                }
                catch (Exception error)
                {
                    return
                    [
                        new PackageIssue(
                            PackageIssueSeverity.Error,
                            "PREFLIGHT_FAILED",
                            error.Message),
                    ];
                }
            });
    }

    private async Task ValidateAndDisplayAsync(int generation)
    {
        Project project = SnapshotProject();
        PackageOptions options = ReadOptions();
        bool buildRelease = BuildReleaseCheck.IsChecked == true;
        IReadOnlyList<PackageIssue> issues;
        try
        {
            issues = await RunPreflightAsync(
                project,
                options,
                buildRelease);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        if (generation != _validationGeneration ||
            _busy ||
            IsCloseRequested ||
            _dialogLifetime.IsCancellationRequested ||
            Dispatcher.HasShutdownStarted ||
            Dispatcher.HasShutdownFinished)
        {
            return;
        }
        DisplayValidation(issues);
    }

    private void DisplayValidation(IReadOnlyList<PackageIssue> issues)
    {
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
        ScheduleValidation();
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
        ScheduleValidation();
    }

    private void OnValidate(object sender, RoutedEventArgs e)
    {
        ScheduleValidation(immediate: true);
        AppendLog("Preflight validation started.");
    }

    private void OnBrowse(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog
        {
            Title = "Select package output directory",
            // OutputBox can contain an offline UNC path. Probing it on the
            // dispatcher can freeze the complete editor before the shell
            // dialog even opens, so seed from the known-local project root.
            InitialDirectory = _project.RootDir,
            Multiselect = false,
        };
        if (dialog.ShowDialog(this) == true)
            OutputBox.Text = dialog.FolderName;
    }

    private async void OnPackage(object sender, RoutedEventArgs e)
    {
        if (_busy || IsCloseRequested)
            return;
        _validationDebounce.Stop();
        _validation.CancelLatest();
        checked { _validationGeneration++; }
        Project project = SnapshotProject();
        PackageOptions options = ReadOptions();
        bool buildRelease = BuildReleaseCheck.IsChecked == true;
        var operationCompletion = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _activePackageCompletion = operationCompletion;
        SetBusy(true);
        StatusText.Text = "Validating package inputs...";
        LogBox.Clear();
        _resultZip = null;
        PackageSucceeded = false;
        OpenResultButton.IsEnabled = false;
        ResultPathText.Text = "Packaging in progress…";
        _cancellation = CancellationTokenSource.CreateLinkedTokenSource(
            _dialogLifetime.Token);

        var progress = new Progress<PackageProgress>(item =>
        {
            if (IsCloseRequested ||
                _dialogLifetime.IsCancellationRequested ||
                Dispatcher.HasShutdownStarted ||
                Dispatcher.HasShutdownFinished)
            {
                return;
            }
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
            IReadOnlyList<PackageIssue> preflight =
                await RunPreflightAsync(
                    project,
                    options,
                    buildRelease,
                    _cancellation.Token);
            _cancellation.Token.ThrowIfCancellationRequested();
            if (IsCloseRequested)
                return;
            DisplayValidation(preflight);
            if (preflight.Any(
                    issue => issue.Severity == PackageIssueSeverity.Error))
            {
                StatusText.Text = "Validation failed";
                ResultPathText.Text = "Package was not generated.";
                foreach (PackageIssue issue in preflight)
                {
                    AppendLog(
                        $"{issue.Severity} [{issue.Code}] {issue.Message} {issue.Path}");
                }
                return;
            }

            AppendLog("Package pipeline started.");
            PackageResult result = await PackagingService.PackageAsync(
                project,
                options,
                buildRelease,
                forceConfigure: true,
                Log,
                progress,
                _cancellation.Token);
            _cancellation.Token.ThrowIfCancellationRequested();
            if (IsCloseRequested ||
                Dispatcher.HasShutdownStarted ||
                Dispatcher.HasShutdownFinished)
            {
                return;
            }
            _resultZip = result.ZipPath;
            PackageSucceeded = true;
            ResultPathText.Text = result.ZipPath;
            StatusText.Text =
                $"{(result.ArchiveVerified ? "Verified" : "Complete")}  /  " +
                $"{result.FileCount} files  /  {FormatBytes(result.UncompressedBytes)}";
            AppendLog($"Build ID: {result.BuildId}");
            AppendLog(
                $"Cooked assets: {result.CookedAssetCount}  /  " +
                $"game.acpak SHA-256: {result.AssetPackSha256}");
            if (result.ArchiveVerified)
            {
                AppendLog(
                    $"Archive verification: PASS  /  " +
                    $"{result.FileCount} payload SHA-256 hashes.");
            }
            AppendLog($"Package complete: {result.ZipPath}");
            OpenResultButton.IsEnabled = true;
        }
        catch (OperationCanceledException)
        {
            if (IsCloseRequested ||
                _dialogLifetime.IsCancellationRequested ||
                Dispatcher.HasShutdownStarted ||
                Dispatcher.HasShutdownFinished)
            {
                return;
            }
            StatusText.Text = "Cancelled";
            ResultPathText.Text = "Package cancelled. Existing package was not replaced.";
            AppendLog("Package cancelled.");
        }
        catch (PackageValidationException error)
        {
            if (IsCloseRequested ||
                _dialogLifetime.IsCancellationRequested ||
                Dispatcher.HasShutdownStarted ||
                Dispatcher.HasShutdownFinished)
            {
                return;
            }
            ValidationList.ItemsSource = error.Issues.Select(ToRow).ToArray();
            StatusText.Text = "Validation failed";
            ResultPathText.Text = "Package was not generated.";
            foreach (PackageIssue issue in error.Issues)
                AppendLog($"{issue.Severity} [{issue.Code}] {issue.Message} {issue.Path}");
        }
        catch (Exception error)
        {
            if (IsCloseRequested ||
                _dialogLifetime.IsCancellationRequested ||
                Dispatcher.HasShutdownStarted ||
                Dispatcher.HasShutdownFinished)
            {
                return;
            }
            StatusText.Text = "Package failed";
            ResultPathText.Text = "Package was not generated.";
            AppendLog("ERROR: " + error.Message);
        }
        finally
        {
            _cancellation?.Dispose();
            _cancellation = null;
            if (!IsCloseRequested &&
                !_dialogLifetime.IsCancellationRequested &&
                !Dispatcher.HasShutdownStarted &&
                !Dispatcher.HasShutdownFinished)
            {
                SetBusy(false);
                ScheduleValidation(immediate: true);
            }
            if (ReferenceEquals(_activePackageCompletion, operationCompletion))
                _activePackageCompletion = null;
            operationCompletion.TrySetResult(true);
        }
    }

    private void Log(string message)
    {
        if (IsCloseRequested ||
            _dialogLifetime.IsCancellationRequested ||
            Dispatcher.HasShutdownStarted ||
            Dispatcher.HasShutdownFinished)
        {
            return;
        }
        AppendLog(message);
        try
        {
            _externalLog(message);
        }
        catch
        {
        }
    }

    private void AppendLog(string message)
    {
        if (IsCloseRequested ||
            _dialogLifetime.IsCancellationRequested ||
            Dispatcher.HasShutdownStarted ||
            Dispatcher.HasShutdownFinished)
        {
            return;
        }
        if (!Dispatcher.CheckAccess())
        {
            try
            {
                _ = Dispatcher.BeginInvoke(
                    DispatcherPriority.Background,
                    new Action(() => AppendLog(message)));
            }
            catch
            {
            }
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
        // The successful PackageResult already proved publication. Avoid a
        // dispatcher-thread File.Exists probe because output may be a slow or
        // disconnected network location.
        if (_resultZip == null)
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
        _validationDebounce.Stop();
        checked { _validationGeneration++; }
        if (!_allowClose)
            _allowClose = true;
        EndDialogLifetime();
    }

    private void EndDialogLifetime()
    {
        if (_lifetimeEnded)
            return;
        _lifetimeEnded = true;
        _dialogLifetime.Cancel();
        _validation.Dispose();
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
