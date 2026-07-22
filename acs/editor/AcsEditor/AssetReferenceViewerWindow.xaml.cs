using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// Read-only explorer for the persistent asset dependency graph.
/// It intentionally queries only the current in-memory snapshot: opening or
/// navigating this window never refreshes metadata or writes project files.
/// </summary>
public partial class AssetReferenceViewerWindow : Window
{
    private readonly AssetDatabase _database;
    private readonly List<string> _history = new();
    private int _historyIndex = -1;
    private string _rootAssetId;
    private int _maxDepth = 4;

    public AssetReferenceViewerWindow(AssetDatabase database, string rootAssetId)
    {
        _database = database ?? throw new ArgumentNullException(nameof(database));
        _rootAssetId = rootAssetId ?? throw new ArgumentNullException(nameof(rootAssetId));
        InitializeComponent();
        NavigateTo(rootAssetId, addHistory: true);
    }

    private void NavigateTo(string assetId, bool addHistory)
    {
        if (!_database.TryGetByAssetId(assetId, out AssetRecord? record) || record == null)
            return;

        _rootAssetId = record.AssetId;
        if (addHistory)
        {
            if (_historyIndex + 1 < _history.Count)
                _history.RemoveRange(_historyIndex + 1, _history.Count - _historyIndex - 1);
            if (_history.Count == 0 ||
                !string.Equals(_history[^1], record.AssetId, StringComparison.OrdinalIgnoreCase))
                _history.Add(record.AssetId);
            _historyIndex = _history.Count - 1;
        }

        AssetReferenceAnalysis analysis = _database.AnalyzeReferences(
            record.AssetId,
            _maxDepth);
        ReferenceRow[] dependencies = analysis.TransitiveDependencies
            .Select(ToRow)
            .ToArray();
        ReferenceRow[] referencers = analysis.TransitiveReferencers
            .Select(ToRow)
            .ToArray();

        DependenciesGrid.ItemsSource = dependencies;
        ReferencersGrid.ItemsSource = referencers;
        RootPathText.Text = record.RelativePath;
        RootIdText.Text = record.AssetId;
        RootKindText.Text = record.Kind.ToUpperInvariant();
        RootCardPathText.Text = record.RelativePath;
        RootCardKindText.Text = record.Kind.ToUpperInvariant();
        RootGlyphText.Text = GlyphFor(record.Kind);
        DependencyCountText.Text = dependencies.Length.ToString();
        ReferencerCountText.Text = referencers.Length.ToString();
        SummaryText.Text =
            $"Direct: {analysis.DirectDependencies.Count} dependencies, " +
            $"{analysis.DirectReferencers.Count} referencers   •   " +
            $"Closure depth {_maxDepth}: {dependencies.Length + referencers.Length} assets";
        Title = $"Reference Viewer — {record.RelativePath}";

        ShowDiagnostics(analysis);
        UpdateNavigationButtons();
    }

    private ReferenceRow ToRow(AssetReferenceNode node)
    {
        string via = node.ReachedFromAssetId;
        if (_database.TryGetByAssetId(node.ReachedFromAssetId, out AssetRecord? source) &&
            source != null)
            via = source.RelativePath;
        return new ReferenceRow(
            node.AssetId,
            node.RelativePath,
            node.Kind.ToUpperInvariant(),
            node.Depth,
            via,
            node.IsMissing);
    }

    private void ShowDiagnostics(AssetReferenceAnalysis analysis)
    {
        var diagnostics = new List<DiagnosticRow>();
        foreach (string missing in analysis.MissingAssetIds)
        {
            string[] owners = analysis.TransitiveDependencies
                .Where(node => node.IsMissing && node.AssetId == missing)
                .Select(node =>
                {
                    if (_database.TryGetByAssetId(
                        node.ReachedFromAssetId,
                        out AssetRecord? owner) && owner != null)
                        return owner.RelativePath;
                    return node.ReachedFromAssetId;
                })
                .Distinct(StringComparer.Ordinal)
                .OrderBy(static path => path, StringComparer.Ordinal)
                .ToArray();
            diagnostics.Add(new DiagnosticRow(
                "MISSING",
                $"{missing}  referenced by  {string.Join(", ", owners)}",
                (Brush)FindResource("ReferenceAmber"),
                new FontFamily("Consolas")));
        }

        foreach (AssetDependencyCycle cycle in analysis.Cycles)
        {
            string[] paths = cycle.AssetIds.Select(id =>
            {
                if (_database.TryGetByAssetId(id, out AssetRecord? asset) && asset != null)
                    return asset.RelativePath;
                return id;
            }).ToArray();
            diagnostics.Add(new DiagnosticRow(
                "CYCLE",
                string.Join("  →  ", paths),
                (Brush)FindResource("ReferenceError"),
                new FontFamily("Segoe UI")));
        }

        DiagnosticsItems.ItemsSource = diagnostics;
        DiagnosticsPanel.Visibility = diagnostics.Count == 0
            ? Visibility.Collapsed
            : Visibility.Visible;

        bool hasError = analysis.Cycles.Count != 0;
        bool hasWarning = analysis.MissingAssetIds.Count != 0;
        Brush health = (Brush)FindResource(
            hasError ? "ReferenceError" :
            hasWarning ? "ReferenceAmber" :
            "ReferenceHealthy");
        HealthBadge.BorderBrush = health;
        HealthBadge.Background = new SolidColorBrush(Color.FromArgb(
            0x18,
            ((SolidColorBrush)health).Color.R,
            ((SolidColorBrush)health).Color.G,
            ((SolidColorBrush)health).Color.B));
        HealthText.Foreground = health;
        HealthText.Text = hasError
            ? "DEPENDENCY CYCLE"
            : hasWarning
                ? "MISSING REFERENCES"
                : "GRAPH HEALTHY";
    }

    private void OnReferenceDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (sender is not DataGrid grid ||
            grid.SelectedItem is not ReferenceRow row ||
            row.IsMissing)
            return;
        NavigateTo(row.AssetId, addHistory: true);
    }

    private void OnDepthChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded ||
            DepthBox.SelectedItem is not ComboBoxItem item ||
            !int.TryParse(item.Tag?.ToString(), out int depth))
            return;
        _maxDepth = depth;
        NavigateTo(_rootAssetId, addHistory: false);
    }

    private void OnRequery(object sender, RoutedEventArgs e) =>
        NavigateTo(_rootAssetId, addHistory: false);

    private void OnCopyId(object sender, RoutedEventArgs e)
    {
        try { Clipboard.SetText(_rootAssetId); }
        catch { }
    }

    private void OnBack(object sender, RoutedEventArgs e)
    {
        if (_historyIndex <= 0)
            return;
        _historyIndex--;
        NavigateTo(_history[_historyIndex], addHistory: false);
    }

    private void OnForward(object sender, RoutedEventArgs e)
    {
        if (_historyIndex + 1 >= _history.Count)
            return;
        _historyIndex++;
        NavigateTo(_history[_historyIndex], addHistory: false);
    }

    private void UpdateNavigationButtons()
    {
        BackButton.IsEnabled = _historyIndex > 0;
        ForwardButton.IsEnabled = _historyIndex + 1 < _history.Count;
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left)
            return;
        if (e.ClickCount == 2)
            WindowState = WindowState == WindowState.Maximized
                ? WindowState.Normal
                : WindowState.Maximized;
        else
            DragMove();
    }

    private void OnMinimize(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void OnMaximizeRestore(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal
            : WindowState.Maximized;

    private void OnClose(object sender, RoutedEventArgs e) => Close();

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape)
        {
            Close();
            e.Handled = true;
        }
        else if (e.Key == Key.F5)
        {
            NavigateTo(_rootAssetId, addHistory: false);
            e.Handled = true;
        }
    }

    private static string GlyphFor(string kind) => kind switch
    {
        "image" => "IMG",
        "audio" => "AUD",
        "mesh" => "MESH",
        "scene" => "SCN",
        "material" => "MAT",
        "blueprint" => "BP",
        "prefab" => "PREF",
        _ => "ASSET",
    };

    private sealed record ReferenceRow(
        string AssetId,
        string Path,
        string Kind,
        int Depth,
        string Via,
        bool IsMissing);

    private sealed record DiagnosticRow(
        string Label,
        string Message,
        Brush Accent,
        FontFamily FontFamily);
}
