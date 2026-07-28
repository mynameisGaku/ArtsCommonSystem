// SPDX-License-Identifier: Apache-2.0

using System;
using System.ComponentModel;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

public sealed class AssetPackageReadinessNavigateEventArgs : EventArgs
{
    public string AssetPath { get; }
    public string AssetId { get; }

    public AssetPackageReadinessNavigateEventArgs(
        string assetPath,
        string assetId)
    {
        AssetPath = assetPath;
        AssetId = assetId;
    }
}

public partial class AssetPackageReadinessWindow : Window
{
    private readonly AssetPackageReadinessRequest _request;
    private readonly AssetPackageReadinessReport? _fixtureReport;
    private CancellationTokenSource? _analysisCancellation;
    private AssetPackageReadinessReport? _report;
    private int _generation;
    private bool _running;

    public event EventHandler<AssetPackageReadinessNavigateEventArgs>?
        LocateRequested;
    public event EventHandler<AssetPackageReadinessNavigateEventArgs>?
        ReferenceViewerRequested;

    public AssetPackageReadinessWindow(
        AssetPackageReadinessRequest request)
        : this(request, null)
    {
    }

    internal AssetPackageReadinessWindow(
        AssetPackageReadinessRequest request,
        AssetPackageReadinessReport? fixtureReport)
    {
        _request = request ?? throw new ArgumentNullException(nameof(request));
        _fixtureReport = fixtureReport;
        InitializeComponent();
        ProjectNameText.Text = request.ProjectName;
        RootSceneText.Text =
            "Canonical Scene ID  " + request.CanonicalSceneAssetId;
        DiagnosticsGrid.SelectionChanged += OnDiagnosticSelectionChanged;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (_fixtureReport != null)
        {
            _report = _fixtureReport;
            PublishReport(_fixtureReport);
            return;
        }
        await RunAnalysisAsync();
    }

    private async Task RunAnalysisAsync()
    {
        CancelAnalysis();
        int generation = ++_generation;
        var cancellation = new CancellationTokenSource();
        _analysisCancellation = cancellation;
        SetRunning(true);
        _report = null;
        SaveButton.IsEnabled = false;
        DiagnosticsGrid.ItemsSource = null;
        AssetsGrid.ItemsSource = null;
        HealthText.Text = "ANALYZING";
        HealthText.Foreground = (Brush)FindResource("InfoFg");
        SummaryText.Text =
            "Refreshing authoritative metadata and building the canonical Cook closure…";
        StatusText.Text = "Read-only audit in progress. Project files are not modified.";
        RequiredCountText.Text = "—";
        ErrorCountText.Text = "—";
        WarningCountText.Text = "—";

        try
        {
            AssetPackageReadinessReport report =
                await AssetPackageReadiness.AnalyzeAsync(
                    _request,
                    cancellation.Token);
            if (generation != _generation ||
                cancellation.IsCancellationRequested)
            {
                return;
            }
            _report = report;
            PublishReport(report);
        }
        catch (OperationCanceledException)
        {
            if (generation != _generation)
                return;
            HealthText.Text = "CANCELLED";
            HealthText.Foreground = (Brush)FindResource("ReadinessWarning");
            SummaryText.Text = "Package Readiness was cancelled without changing project files.";
            StatusText.Text = "Press Run Again or F5 to restart.";
        }
        catch (Exception error)
        {
            if (generation != _generation)
                return;
            HealthText.Text = "AUDIT FAILED";
            HealthText.Foreground = (Brush)FindResource("ReadinessError");
            SummaryText.Text = error.Message;
            StatusText.Text =
                "The audit failed closed. Repair the reported path or metadata and retry.";
        }
        finally
        {
            if (generation == _generation)
            {
                SetRunning(false);
                if (ReferenceEquals(_analysisCancellation, cancellation))
                    _analysisCancellation = null;
            }
            cancellation.Dispose();
        }
    }

    private void PublishReport(AssetPackageReadinessReport report)
    {
        DiagnosticsGrid.ItemsSource = report.Diagnostics;
        AssetsGrid.ItemsSource = report.Assets;
        RequiredCountText.Text = report.RequiredAssetCount.ToString();
        ErrorCountText.Text = report.ErrorCount.ToString();
        WarningCountText.Text = report.WarningCount.ToString();
        RootSceneText.Text = report.RootAssetPath.Length == 0
            ? "Canonical Scene ID  " + report.CanonicalSceneAssetId
            : report.RootAssetPath + "   ·   " + report.CanonicalSceneAssetId;
        HealthText.Text = report.Ready
            ? "READY TO COOK"
            : "BLOCKED";
        Brush health = (Brush)FindResource(
            report.Ready ? "ReadinessReady" : "ReadinessError");
        HealthText.Foreground = health;
        HealthBadge.BorderBrush = health;
        HealthBadge.Background = new SolidColorBrush(
            Color.FromArgb(
                0x18,
                ((SolidColorBrush)health).Color.R,
                ((SolidColorBrush)health).Color.G,
                ((SolidColorBrush)health).Color.B));
        SummaryText.Text = report.Ready
            ? $"Canonical closure is package-ready. Graph {ShortHash(report.GraphHash)}"
            : $"{report.ErrorCount} blocking issue(s) must be repaired before Package.";
        StatusText.Text =
            $"{report.RequiredAssetCount} required asset(s) · " +
            $"graph SHA-256 {report.GraphHash}";
        SaveButton.IsEnabled = true;
        if (report.Diagnostics.Count != 0)
            DiagnosticsGrid.SelectedIndex = 0;
    }

    private async void OnRunAgain(object sender, RoutedEventArgs e) =>
        await RunAnalysisAsync();

    private void OnCancel(object sender, RoutedEventArgs e) =>
        CancelAnalysis();

    private void CancelAnalysis()
    {
        CancellationTokenSource? cancellation = _analysisCancellation;
        _analysisCancellation = null;
        cancellation?.Cancel();
    }

    private void SetRunning(bool running)
    {
        _running = running;
        Progress.Visibility = running
            ? Visibility.Visible
            : Visibility.Collapsed;
        CancelButton.IsEnabled = running;
        RunButton.IsEnabled = !running;
        if (running)
        {
            LocateButton.IsEnabled = false;
            ReferenceButton.IsEnabled = false;
        }
    }

    private void OnDiagnosticSelectionChanged(
        object? sender,
        SelectionChangedEventArgs e)
    {
        bool idle = !_running;
        AssetPackageReadinessDiagnostic? diagnostic =
            DiagnosticsGrid.SelectedItem as AssetPackageReadinessDiagnostic;
        LocateButton.IsEnabled =
            idle && diagnostic is { CanLocate: true };
        ReferenceButton.IsEnabled =
            idle && diagnostic is { AssetId.Length: > 0 };
    }

    private void OnDiagnosticDoubleClick(
        object sender,
        MouseButtonEventArgs e)
    {
        if (LocateButton.IsEnabled)
            OnLocate(sender, new RoutedEventArgs());
    }

    private void OnLocate(object sender, RoutedEventArgs e)
    {
        if (DiagnosticsGrid.SelectedItem is not
            AssetPackageReadinessDiagnostic diagnostic ||
            !diagnostic.CanLocate)
        {
            return;
        }
        LocateRequested?.Invoke(
            this,
            new(
                diagnostic.AssetPath,
                diagnostic.AssetId));
    }

    private void OnOpenReferenceViewer(object sender, RoutedEventArgs e)
    {
        if (DiagnosticsGrid.SelectedItem is not
            AssetPackageReadinessDiagnostic diagnostic ||
            diagnostic.AssetId.Length == 0)
        {
            return;
        }
        ReferenceViewerRequested?.Invoke(
            this,
            new(
                diagnostic.AssetPath,
                diagnostic.AssetId));
    }

    private async void OnSaveJson(object sender, RoutedEventArgs e)
    {
        AssetPackageReadinessReport? report = _report;
        if (report == null || _running)
            return;
        var dialog = new Microsoft.Win32.SaveFileDialog
        {
            Title = "Save Package Readiness report",
            Filter = "JSON report (*.json)|*.json",
            DefaultExt = ".json",
            AddExtension = true,
            FileName = "package-readiness.json",
            OverwritePrompt = false,
        };
        if (dialog.ShowDialog(this) != true)
            return;
        try
        {
            await AssetPackageReadiness.WriteNewJsonAsync(
                dialog.FileName,
                report);
            StatusText.Text = "JSON report saved: " + dialog.FileName;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                ArgumentException or InvalidOperationException)
        {
            MessageBox.Show(
                this,
                error.Message,
                "Package Readiness report",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
        }
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        _generation++;
        CancelAnalysis();
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape)
        {
            if (_running)
                CancelAnalysis();
            else
                Close();
            e.Handled = true;
        }
        else if (e.Key == Key.F5)
        {
            if (!_running)
                _ = RunAnalysisAsync();
            e.Handled = true;
        }
        else if (e.Key == Key.S &&
                 Keyboard.Modifiers == ModifierKeys.Control &&
                 SaveButton.IsEnabled)
        {
            OnSaveJson(sender, new RoutedEventArgs());
            e.Handled = true;
        }
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left)
            return;
        if (e.ClickCount == 2)
        {
            WindowState = WindowState == WindowState.Maximized
                ? WindowState.Normal
                : WindowState.Maximized;
        }
        else
        {
            DragMove();
        }
    }

    private void OnMinimize(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void OnMaximizeRestore(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal
            : WindowState.Maximized;

    private void OnClose(object sender, RoutedEventArgs e) => Close();

    private static string ShortHash(string hash) =>
        hash.Length <= 12 ? hash : hash[..12];

    internal bool ValidateVisualFixtureLayout(out string error)
    {
        error = "";
        if (ActualWidth < 1000 ||
            ActualHeight < 650 ||
            DiagnosticsGrid.ActualWidth < 700 ||
            DiagnosticsGrid.ActualHeight < 300)
        {
            error =
                "Package Readiness did not receive a usable diagnostic layout.";
            return false;
        }
        if (HealthText.Text != "BLOCKED" ||
            RequiredCountText.Text != "4" ||
            ErrorCountText.Text != "3" ||
            DiagnosticsGrid.Items.Count != 4 ||
            AssetsGrid.Items.Count != 4 ||
            !SaveButton.IsEnabled)
        {
            error =
                "Fixture summary, diagnostics, closure, or report action is incomplete.";
            return false;
        }

        Rect save = SaveButton
            .TransformToAncestor(this)
            .TransformBounds(new Rect(SaveButton.RenderSize));
        Rect cancel = CancelButton
            .TransformToAncestor(this)
            .TransformBounds(new Rect(CancelButton.RenderSize));
        if (save.IntersectsWith(cancel) ||
            save.Right > ActualWidth ||
            cancel.Right > ActualWidth ||
            LocateButton.ActualWidth < 90)
        {
            error =
                "Package Readiness actions overlap or are clipped.";
            return false;
        }
        return true;
    }

    internal void ShowVisualFixtureClosure()
    {
        ReadinessTabs.SelectedIndex = 1;
        UpdateLayout();
    }
}
