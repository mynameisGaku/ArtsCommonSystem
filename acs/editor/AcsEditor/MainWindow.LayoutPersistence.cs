// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;

namespace AcsEditor;

internal static class EditorLayoutFileStore
{
    internal static void WriteAtomically(
        string path,
        string source,
        Action<string>? beforeCommit = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(source);
        string fullPath = Path.GetFullPath(path);
        string directory = Path.GetDirectoryName(fullPath) ??
            throw new InvalidDataException(
                "The editor layout path has no parent directory.");
        Directory.CreateDirectory(directory);
        if (File.Exists(fullPath) &&
            (File.GetAttributes(fullPath) & FileAttributes.ReparsePoint) != 0)
        {
            throw new IOException(
                "The editor layout target is a reparse point.");
        }

        string temp = Path.Combine(
            directory,
            Path.GetFileName(fullPath) +
            $".{Environment.ProcessId}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllText(
                temp,
                source,
                new UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false,
                    throwOnInvalidBytes: true));
            beforeCommit?.Invoke(temp);
            File.Move(temp, fullPath, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temp))
                    File.Delete(temp);
            }
            catch
            {
                // A failed cleanup is non-fatal to close. The uniquely named
                // same-directory temp is never treated as a startup source.
            }
        }
    }
}

/// <summary>Per-user editor window and workspace layout persistence.</summary>
public partial class MainWindow
{
    private const int EditorLayoutVersion = 1;
    private const int EditorLayoutMaximumBytes = 64 * 1024;
    private bool _layoutRestored;
    private bool _layoutRestoreStarted;
    private bool _layoutRestorePending;
    private int _layoutPublicationVersion;
    private int _layoutSaveGeneration;
    private readonly SemaphoreSlim _layoutSaveGate = new(1, 1);

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

    private async void RestoreEditorLayout()
    {
        if (_layoutRestoreStarted) return;
        _layoutRestoreStarted = true;
        _layoutRestorePending = true;
        int publicationVersion = _layoutPublicationVersion;
        PreviewMouseDown += OnPendingLayoutUserInput;
        PreviewKeyDown += OnPendingLayoutUserInput;

        string path = EditorLayoutPath;
        using var cancellation = new CancellationTokenSource();
        EventHandler onClosed = (_, _) => cancellation.Cancel();
        Closed += onClosed;
        try
        {
            Task<EditorStartupTextSnapshot> layoutTask =
                EditorStartupFileSnapshot.ReadAsync(
                    path,
                    EditorLayoutMaximumBytes,
                    cancellation.Token);
            Task<EditorWorkspaceStore> workspaceTask = Task.Run(
                () => new EditorWorkspaceStore(),
                CancellationToken.None);

            EditorStartupTextSnapshot snapshot = await layoutTask;
            EditorWorkspaceStore workspace = await workspaceTask;
            bool dispatcherShuttingDown =
                Dispatcher.HasShutdownStarted ||
                Dispatcher.HasShutdownFinished;
            if (cancellation.IsCancellationRequested ||
                dispatcherShuttingDown)
            {
                return;
            }

            PublishStartupWorkspaceStore(workspace);
            _layoutRestored = true;
            if (snapshot.Missing)
                return;
            if (snapshot.Warning is { Length: > 0 } warning)
                throw new InvalidDataException(warning);

            EditorLayoutState? state = JsonSerializer.Deserialize<EditorLayoutState>(
                snapshot.Source ?? "");
            if (state == null || state.Version != EditorLayoutVersion)
                return;
            if (!ShouldPublishStartupLayout(
                    publicationVersion,
                    _layoutPublicationVersion,
                    cancelled: false,
                    dispatcherShuttingDown: false))
            {
                Log(
                    "Saved editor layout finished loading after user input; " +
                    "the live layout was preserved.",
                    "Editor",
                    LogLevel.Info);
                return;
            }
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
            if (!_startupMonitorPlacementRequested &&
                candidate.IntersectsWith(virtualScreen) &&
                Rect.Intersect(candidate, virtualScreen).Width >= 96 &&
                Rect.Intersect(candidate, virtualScreen).Height >= 64)
            {
                WindowStartupLocation = WindowStartupLocation.Manual;
                Left = left;
                Top = top;
                Width = width;
                Height = height;
            }
            else if (!_startupMonitorPlacementRequested)
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
            if (state.Maximized && !_startupMonitorPlacementRequested)
                WindowState = WindowState.Maximized;
            if (_showProfilerAtStartup)
                ShowBottomTab("profiler");
        }
        catch (OperationCanceledException)
            when (cancellation.IsCancellationRequested)
        {
            // Closing invalidates the pending result. No UI publication is
            // allowed after this point.
        }
        catch (Exception ex)
        {
            _layoutRestored = true;
            Log("Editor layout could not be restored; defaults were used: " + ex.Message,
                "Editor", LogLevel.Warn);
        }
        finally
        {
            Closed -= onClosed;
            PreviewMouseDown -= OnPendingLayoutUserInput;
            PreviewKeyDown -= OnPendingLayoutUserInput;
            _layoutRestorePending = false;
        }
    }

    private void PublishStartupWorkspaceStore(EditorWorkspaceStore workspace)
    {
        // A very fast user command may have initialized the store while the
        // worker was reading. Never replace that live instance or its writes
        // with the older startup snapshot.
        if (_workspaceStore != null)
            return;

        _workspaceStore = workspace;
        _activeWorkspaceName = workspace.LastActiveName;
        UpdateWorkspaceStatus();
        if (workspace.LoadWarning is { Length: > 0 } warning)
        {
            Log(
                "Named workspaces could not be restored; built-in layouts are available: " +
                warning,
                "Editor",
                LogLevel.Warn);
        }
    }

    private void OnPendingLayoutUserInput(
        object sender,
        InputEventArgs e)
    {
        if (_layoutRestorePending)
            _layoutPublicationVersion++;
    }

    private void ObservePendingLayoutWindowMove()
    {
        if (_layoutRestorePending)
            _layoutPublicationVersion++;
    }

    internal static bool ShouldPublishStartupLayout(
        int requestedVersion,
        int currentVersion,
        bool cancelled,
        bool dispatcherShuttingDown) =>
        !cancelled &&
        !dispatcherShuttingDown &&
        requestedVersion == currentVersion;

    internal static bool ShouldPublishLayoutSave(
        int requestedGeneration,
        int currentGeneration) =>
        requestedGeneration == currentGeneration;

    private async Task SaveEditorLayoutAsync()
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
            string source = JsonSerializer.Serialize(
                state,
                new JsonSerializerOptions
                {
                    WriteIndented = true,
                });
            int saveGeneration = ++_layoutSaveGeneration;
            // Capture WPF geometry above, then keep directory creation, write,
            // flush/close, replacement, and cleanup off the Dispatcher. Close
            // finalization awaits this owned task without blocking the UI.
            await _layoutSaveGate.WaitAsync();
            try
            {
                // A later workspace/layout command already captured a newer
                // immutable source while this save was waiting. Only the
                // latest queued generation may publish.
                if (!ShouldPublishLayoutSave(
                        saveGeneration,
                        _layoutSaveGeneration))
                    return;
                await Task.Run(
                    () => EditorLayoutFileStore.WriteAtomically(
                        path,
                        source));
            }
            finally
            {
                _layoutSaveGate.Release();
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
