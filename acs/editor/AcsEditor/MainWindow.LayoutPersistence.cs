// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Text.Json;
using System.Windows;

namespace AcsEditor;

/// <summary>Per-user editor window and workspace layout persistence.</summary>
public partial class MainWindow
{
    private const int EditorLayoutVersion = 1;
    private bool _layoutRestored;

    private sealed class EditorLayoutState
    {
        public int Version { get; set; } = EditorLayoutVersion;
        public double Left { get; set; }
        public double Top { get; set; }
        public double Width { get; set; }
        public double Height { get; set; }
        public bool Maximized { get; set; }
        public double HierarchyWidth { get; set; }
        public double InspectorWidth { get; set; }
        public double BottomDockHeight { get; set; }
        public bool HierarchyVisible { get; set; } = true;
        public bool InspectorVisible { get; set; } = true;
        public bool BottomDockVisible { get; set; } = true;
        public string BottomTab { get; set; } = "console";
        public string ActiveWorkspaceName { get; set; } =
            EditorWorkspaceStore.DefaultWorkspaceName;
    }

    private static string EditorLayoutPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AcsEditor",
        $"EditorLayout.v{EditorLayoutVersion}.json");

    private void RestoreEditorLayout()
    {
        if (_layoutRestored) return;
        _layoutRestored = true;
        InitializeWorkspaceProfiles();

        string path = EditorLayoutPath;
        if (!File.Exists(path)) return;
        try
        {
            if (new FileInfo(path).Length > 64 * 1024)
                throw new InvalidDataException("layout file is too large");
            EditorLayoutState? state = JsonSerializer.Deserialize<EditorLayoutState>(
                File.ReadAllText(path));
            if (state == null || state.Version != EditorLayoutVersion)
                return;

            double width = ClampFinite(state.Width, MinWidth, 7680, Width);
            double height = ClampFinite(state.Height, MinHeight, 4320, Height);
            double left = ClampFinite(
                state.Left,
                SystemParameters.VirtualScreenLeft - width + 96,
                SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth - 96,
                Left);
            double top = ClampFinite(
                state.Top,
                SystemParameters.VirtualScreenTop,
                SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight - 64,
                Top);

            // Require a useful portion of the title bar to remain reachable. Monitor topology can
            // change between sessions (laptop dock/RDP); invalid bounds fall back to CenterScreen.
            var candidate = new Rect(left, top, width, height);
            var virtualScreen = new Rect(
                SystemParameters.VirtualScreenLeft,
                SystemParameters.VirtualScreenTop,
                SystemParameters.VirtualScreenWidth,
                SystemParameters.VirtualScreenHeight);
            if (candidate.IntersectsWith(virtualScreen) &&
                Rect.Intersect(candidate, virtualScreen).Width >= 96 &&
                Rect.Intersect(candidate, virtualScreen).Height >= 64)
            {
                WindowStartupLocation = WindowStartupLocation.Manual;
                Left = left;
                Top = top;
                Width = width;
                Height = height;
            }
            else
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen;
            }

            _hierarchyWidth = ClampFinite(state.HierarchyWidth, 210, 960, _hierarchyWidth);
            _inspectorWidth = ClampFinite(state.InspectorWidth, 280, 1200, _inspectorWidth);
            _bottomDockHeight = ClampFinite(state.BottomDockHeight, 140, 800, _bottomDockHeight);
            SetHierarchyVisible(state.HierarchyVisible);
            SetInspectorVisible(state.InspectorVisible);
            ShowBottomTab(EditorWorkspaceStore.NormalizeBottomTab(state.BottomTab));
            SetBottomDockVisible(state.BottomDockVisible);
            RestoreWorkspaceIdentity(state.ActiveWorkspaceName);
            if (state.Maximized)
                WindowState = WindowState.Maximized;
        }
        catch (Exception ex)
        {
            Log("Editor layout could not be restored; defaults were used: " + ex.Message,
                "Editor", LogLevel.Warn);
        }
    }

    private void SaveEditorLayout()
    {
        if (!_layoutRestored) return;
        try
        {
            Rect bounds = WindowState == WindowState.Normal
                ? new Rect(Left, Top, ActualWidth, ActualHeight)
                : RestoreBounds;
            if (!IsFiniteRect(bounds))
                return;

            var state = new EditorLayoutState
            {
                Left = bounds.Left,
                Top = bounds.Top,
                Width = bounds.Width,
                Height = bounds.Height,
                Maximized = WindowState == WindowState.Maximized,
                HierarchyWidth = HierarchyColumn.ActualWidth > 0
                    ? HierarchyColumn.ActualWidth : _hierarchyWidth,
                InspectorWidth = InspectorColumn.ActualWidth > 0
                    ? InspectorColumn.ActualWidth : _inspectorWidth,
                BottomDockHeight = BottomDockRow.ActualHeight > 0
                    ? BottomDockRow.ActualHeight : _bottomDockHeight,
                HierarchyVisible = HierarchyPanel.Visibility == Visibility.Visible,
                InspectorVisible = InspectorPanel.Visibility == Visibility.Visible,
                BottomDockVisible = BottomDockPanel.Visibility == Visibility.Visible,
                BottomTab = CurrentBottomTab(),
                ActiveWorkspaceName = _activeWorkspaceName,
            };

            string path = EditorLayoutPath;
            string directory = Path.GetDirectoryName(path)!;
            Directory.CreateDirectory(directory);
            string temp = path + $".{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
            try
            {
                File.WriteAllText(temp, JsonSerializer.Serialize(state, new JsonSerializerOptions
                {
                    WriteIndented = true,
                }));
                File.Move(temp, path, overwrite: true);
            }
            finally
            {
                try { if (File.Exists(temp)) File.Delete(temp); } catch { }
            }
        }
        catch (Exception ex)
        {
            Log("Editor layout could not be saved: " + ex.Message, "Editor", LogLevel.Warn);
        }
    }

    private static bool IsFiniteRect(Rect value) =>
        double.IsFinite(value.Left) &&
        double.IsFinite(value.Top) &&
        double.IsFinite(value.Width) &&
        double.IsFinite(value.Height) &&
        value.Width > 0 &&
        value.Height > 0;

    private static double ClampFinite(double value, double minimum, double maximum, double fallback) =>
        double.IsFinite(value) ? Math.Clamp(value, minimum, maximum) : fallback;

    private static void DeleteSavedEditorLayout()
    {
        try
        {
            if (File.Exists(EditorLayoutPath))
                File.Delete(EditorLayoutPath);
        }
        catch
        {
            // Reset still applies to this session even if persistence storage is unavailable.
        }
    }
}
