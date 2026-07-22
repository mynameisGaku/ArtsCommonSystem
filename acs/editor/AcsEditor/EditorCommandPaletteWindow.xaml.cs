// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace AcsEditor;

public sealed record EditorPaletteCommand(
    string Id,
    string Label,
    string Category,
    string Description,
    string Shortcut,
    Action Execute,
    string[]? Keywords = null,
    Func<bool>? CanExecute = null)
{
    public bool IsAvailable => CanExecute?.Invoke() != false;
}

/// <summary>
/// Searchable, keyboard-first editor action launcher. The palette owns no editor state; it returns
/// the selected action and lets MainWindow execute it after the modal window has closed.
/// </summary>
public partial class EditorCommandPaletteWindow : Window
{
    private const double HeaderDragHeight = 38.0;
    private const double HeaderButtonReserve = 44.0;
    private readonly IReadOnlyList<EditorPaletteCommand> _commands;
    private readonly ObservableCollection<EditorPaletteCommand> _results = [];

    public EditorPaletteCommand? SelectedCommand { get; private set; }

    public EditorCommandPaletteWindow(IEnumerable<EditorPaletteCommand> commands)
    {
        InitializeComponent();
        PreviewMouseLeftButtonDown += OnHeaderMouseLeftButtonDown;
        _commands = commands
            .GroupBy(c => c.Id, StringComparer.Ordinal)
            .Select(group => group.First())
            .ToArray();
        ResultsList.ItemsSource = _results;

        Loaded += (_, _) =>
        {
            RefreshResults();
            QueryBox.Focus();
            Keyboard.Focus(QueryBox);
        };
    }

    internal static bool ShouldBeginHeaderDrag(
        double x,
        double y,
        double width) =>
        double.IsFinite(x) && double.IsFinite(y) && double.IsFinite(width) &&
        width > HeaderButtonReserve &&
        x >= 0.0 && x < width - HeaderButtonReserve &&
        y >= 0.0 && y < HeaderDragHeight;

    private void OnHeaderMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        Point position = e.GetPosition(this);
        if (!ShouldBeginHeaderDrag(position.X, position.Y, ActualWidth))
            return;

        try
        {
            DragMove();
            e.Handled = true;
        }
        catch (InvalidOperationException)
        {
            // Capture may have changed between the preview event and DragMove.
        }
    }

    internal void SetQueryForTest(string query)
    {
        QueryBox.Text = query;
        RefreshResults();
    }

    internal static IReadOnlyList<EditorPaletteCommand> CreateVisualTestCommands()
    {
        static void NoOp() { }
        return
        [
            new("build.package", "Package Project…", "Build",
                "Cook, stage, verify and package the project for distribution.", "",
                NoOp, ["shipping", "zip", "distribute"]),
            new("build.build", "Build Project", "Build",
                "Configure and compile the current project.", "F7", NoOp, ["compile"]),
            new("tools.project-settings", "Project Settings…", "Tools",
                "Configure rendering, gameplay, physics and packaging.", "", NoOp, ["preferences"]),
            new("tools.blueprints", "Open Blueprint Editor", "Tools",
                "Open the visual scripting and component graph editor.", "", NoOp, ["graph"]),
            new("view.assets", "Show Asset Browser", "View",
                "Open the bottom dock on the Assets tab.", "", NoOp, ["content"]),
            new("scene.view-perspective", "Scene View: Perspective", "Scene",
                "Use a perspective viewport for the current scene.", "", NoOp, ["level"]),
            new("file.save-scene", "Save Scene", "File",
                "Save the active scene document.", "Ctrl+S", NoOp, ["write"]),
        ];
    }

    internal static bool RunSearchSelfTest()
    {
        IReadOnlyList<EditorPaletteCommand> commands = CreateVisualTestCommands();
        EditorPaletteCommand package = commands.Single(c => c.Id == "build.package");
        EditorPaletteCommand settings = commands.Single(c => c.Id == "tools.project-settings");
        return Score(package, "package") > Score(settings, "package") &&
               Score(package, "ship zip") >= 0 &&
               Score(package, "pkg impossible token") < 0 &&
               Score(settings, "prj set") >= 0;
    }

    private void OnQueryChanged(object sender, TextChangedEventArgs e) => RefreshResults();

    private void RefreshResults()
    {
        string query = QueryBox?.Text ?? "";
        var ranked = _commands
            .Select(command => (Command: command, Score: Score(command, query)))
            .Where(item => item.Score >= 0)
            .OrderByDescending(item => item.Score)
            .ThenBy(item => item.Command.Category, StringComparer.OrdinalIgnoreCase)
            .ThenBy(item => item.Command.Label, StringComparer.OrdinalIgnoreCase)
            .Take(80)
            .Select(item => item.Command)
            .ToArray();

        _results.Clear();
        foreach (EditorPaletteCommand command in ranked)
            _results.Add(command);

        EmptyState.Visibility = _results.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        ResultsList.Visibility = _results.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        ResultCountText.Text = $"{_results.Count} action{(_results.Count == 1 ? "" : "s")}";
        if (_results.Count > 0)
            ResultsList.SelectedIndex = 0;
    }

    internal static int Score(EditorPaletteCommand command, string query)
    {
        string normalized = query.Trim().ToLowerInvariant();
        if (normalized.Length == 0)
            return command.CanExecute?.Invoke() == false ? 0 : 1;

        string label = command.Label.ToLowerInvariant();
        string haystack = string.Join(' ', new[]
        {
            command.Label,
            command.Category,
            command.Description,
            command.Shortcut,
            command.Id,
            command.Keywords == null ? "" : string.Join(' ', command.Keywords),
        }).ToLowerInvariant();

        string[] tokens = normalized.Split(
            [' ', '\t'],
            StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        int score = 0;
        foreach (string token in tokens)
        {
            int tokenScore;
            if (label == token) tokenScore = 400;
            else if (label.StartsWith(token, StringComparison.Ordinal)) tokenScore = 260;
            else if (label.Contains(token, StringComparison.Ordinal)) tokenScore = 180;
            else if (haystack.Contains(token, StringComparison.Ordinal)) tokenScore = 100;
            else
            {
                int subsequence = SubsequenceScore(haystack, token);
                if (subsequence < 0) return -1;
                tokenScore = subsequence;
            }
            score += tokenScore;
        }

        if (command.CanExecute?.Invoke() == false)
            score -= 10;
        return score;
    }

    private static int SubsequenceScore(string value, string token)
    {
        int cursor = 0;
        int first = -1;
        int last = -1;
        foreach (char ch in token)
        {
            int index = value.IndexOf(ch, cursor);
            if (index < 0) return -1;
            if (first < 0) first = index;
            last = index;
            cursor = index + 1;
        }

        int span = Math.Max(1, last - first + 1);
        return Math.Max(10, 80 - span - first / 4);
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape)
        {
            Close();
            e.Handled = true;
            return;
        }
        if (e.Key == Key.Down)
        {
            MoveSelection(1);
            e.Handled = true;
            return;
        }
        if (e.Key == Key.Up)
        {
            MoveSelection(-1);
            e.Handled = true;
            return;
        }
        if (e.Key == Key.Enter)
        {
            AcceptSelection();
            e.Handled = true;
        }
    }

    private void MoveSelection(int delta)
    {
        if (_results.Count == 0) return;
        int current = Math.Max(0, ResultsList.SelectedIndex);
        int next = Math.Clamp(current + delta, 0, _results.Count - 1);
        ResultsList.SelectedIndex = next;
        ResultsList.ScrollIntoView(ResultsList.SelectedItem);
    }

    private void OnResultDoubleClick(object sender, MouseButtonEventArgs e) => AcceptSelection();

    private void AcceptSelection()
    {
        if (ResultsList.SelectedItem is not EditorPaletteCommand command) return;
        if (command.CanExecute?.Invoke() == false)
        {
            System.Media.SystemSounds.Beep.Play();
            return;
        }
        SelectedCommand = command;
        DialogResult = true;
    }

    private void OnClose(object sender, RoutedEventArgs e) => Close();
}
