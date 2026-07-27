// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;

namespace AcsEditor;

/// <summary>
/// Shared floating-window integration for editor tool panels. Camera View keeps
/// its separate native-HWND transfer path; WPF panels use this visual-owner
/// transfer host.
/// </summary>
public partial class MainWindow
{
    private DockableToolHost? _hierarchyToolHost;
    private DockableToolHost? _inspectorToolHost;
    private DockableToolHost? _bottomToolHost;
    private ToolPanelPlacementStore? _toolPanelPlacementStore;
    private bool _toolPanelsRestoreCompleted;
    private bool _suppressToolPanelPersistence;

    private void InitializeDockableToolPanels()
    {
        _toolPanelPlacementStore = new ToolPanelPlacementStore(
            CreateDefaultToolPanelPlacements(),
            message => Log(message, "Editor", LogLevel.Warn));
        _toolPanelPlacementStore.Load();

        _hierarchyToolHost = CreateToolPanelHost(
            ToolPanelDockingContract.HierarchyPanelId,
            "Scene Outliner",
            HierarchyPanel,
            () => HierarchyPanel.Visibility == Visibility.Visible,
            ApplyHierarchyDockVisibility);
        _inspectorToolHost = CreateToolPanelHost(
            ToolPanelDockingContract.InspectorPanelId,
            "Details",
            InspectorPanel,
            () => InspectorPanel.Visibility == Visibility.Visible,
            ApplyInspectorDockVisibility);
        _bottomToolHost = CreateToolPanelHost(
            ToolPanelDockingContract.BottomPanelId,
            "Console / Build / Assets / Profiler",
            BottomDockPanel,
            () => BottomDockPanel.Visibility == Visibility.Visible,
            ApplyBottomDockVisibility);

        UpdateToolPanelPresentation(
            ToolPanelDockingContract.HierarchyPanelId,
            _hierarchyToolHost.State);
        UpdateToolPanelPresentation(
            ToolPanelDockingContract.InspectorPanelId,
            _inspectorToolHost.State);
        UpdateToolPanelPresentation(
            ToolPanelDockingContract.BottomPanelId,
            _bottomToolHost.State);
    }

    private DockableToolHost CreateToolPanelHost(
        string panelId,
        string title,
        FrameworkElement content,
        Func<bool> dockVisibility,
        Action<bool> applyDockVisibility) =>
        new(
            this,
            panelId,
            title,
            content,
            SceneWorkspace,
            dockVisibility,
            applyDockVisibility,
            state => UpdateToolPanelPresentation(panelId, state),
            () => LoadToolPanelBounds(panelId),
            (bounds, floating) =>
                SaveToolPanelPlacement(panelId, bounds, floating),
            message => Log(message, "Editor", LogLevel.Warn));

    private IReadOnlyList<ToolPanelPlacementState>
        CreateDefaultToolPanelPlacements()
    {
        Rect workArea = SystemParameters.WorkArea;
        double left = workArea.Left + 48.0;
        double top = workArea.Top + 72.0;
        return new[]
        {
            CreateToolPanelPlacement(
                ToolPanelDockingContract.HierarchyPanelId,
                new Rect(left, top, 420.0, 680.0)),
            CreateToolPanelPlacement(
                ToolPanelDockingContract.InspectorPanelId,
                new Rect(
                    Math.Max(left, workArea.Right - 528.0),
                    top,
                    480.0,
                    680.0)),
            CreateToolPanelPlacement(
                ToolPanelDockingContract.BottomPanelId,
                new Rect(
                    workArea.Left + 180.0,
                    workArea.Top + 180.0,
                    960.0,
                    460.0)),
        };
    }

    private static ToolPanelPlacementState CreateToolPanelPlacement(
        string panelId,
        Rect bounds) =>
        new()
        {
            PanelId = panelId,
            State = ToolPanelDockState.Docked,
            Left = bounds.Left,
            Top = bounds.Top,
            Width = bounds.Width,
            Height = bounds.Height,
        };

    private void RestoreFloatingToolPanels()
    {
        if (_toolPanelsRestoreCompleted ||
            _toolPanelPlacementStore == null)
        {
            return;
        }

        foreach (ToolPanelDockingDescriptor descriptor in
                 ToolPanelDockingContract.RegisteredPanels)
        {
            ToolPanelPlacementState state =
                _toolPanelPlacementStore.Get(descriptor.PanelId);
            DockableToolHost? host = GetToolPanelHost(descriptor.PanelId);
            if (host == null)
                continue;
            switch (state.State)
            {
                case ToolPanelDockState.Floating:
                    if (!host.TryFloat())
                    {
                        Log(
                            $"{descriptor.AccessibleName} could not restore " +
                            "its floating placement and remains docked.",
                            "Editor",
                            LogLevel.Warn);
                    }
                    break;
                case ToolPanelDockState.Hidden:
                    ApplyToolPanelDockVisibility(
                        descriptor.PanelId,
                        visible: false);
                    UpdateToolPanelPresentation(
                        descriptor.PanelId,
                        ToolPanelDockState.Hidden);
                    break;
                default:
                    ApplyToolPanelDockVisibility(
                        descriptor.PanelId,
                        visible: true);
                    UpdateToolPanelPresentation(
                        descriptor.PanelId,
                        ToolPanelDockState.Docked);
                    break;
            }
        }
        _toolPanelsRestoreCompleted = true;
    }

    private Rect LoadToolPanelBounds(string panelId)
    {
        ToolPanelPlacementState? state =
            _toolPanelPlacementStore?.Get(panelId);
        if (state == null)
            return new Rect(96.0, 72.0, 720.0, 480.0);
        return new Rect(
            state.Left,
            state.Top,
            state.Width,
            state.Height);
    }

    private void SaveToolPanelPlacement(
        string panelId,
        Rect bounds,
        bool floating)
    {
        if (_toolPanelPlacementStore == null)
            return;
        bool dockVisible = panelId switch
        {
            ToolPanelDockingContract.HierarchyPanelId =>
                HierarchyPanel.Visibility == Visibility.Visible,
            ToolPanelDockingContract.InspectorPanelId =>
                InspectorPanel.Visibility == Visibility.Visible,
            ToolPanelDockingContract.BottomPanelId =>
                BottomDockPanel.Visibility == Visibility.Visible,
            _ => false,
        };
        _toolPanelPlacementStore.Update(
            panelId,
            bounds,
            ToolPanelDockingContract.ResolveState(floating, dockVisible));
        if (!_suppressToolPanelPersistence)
            _toolPanelPlacementStore.Save();
    }

    private void PersistDockedToolPanelState(
        string panelId,
        bool visible)
    {
        if (!_toolPanelsRestoreCompleted ||
            _suppressToolPanelPersistence ||
            _toolPanelPlacementStore == null)
        {
            return;
        }
        ToolPanelPlacementState current =
            _toolPanelPlacementStore.Get(panelId);
        _toolPanelPlacementStore.Update(
            panelId,
            new Rect(
                current.Left,
                current.Top,
                current.Width,
                current.Height),
            visible
                ? ToolPanelDockState.Docked
                : ToolPanelDockState.Hidden);
        _toolPanelPlacementStore.Save();
    }

    private void OnToggleHierarchyFloat(
        object sender,
        RoutedEventArgs e) =>
        ToggleToolPanelFloating(_hierarchyToolHost);

    private void OnToggleInspectorFloat(
        object sender,
        RoutedEventArgs e) =>
        ToggleToolPanelFloating(_inspectorToolHost);

    private void OnToggleBottomFloat(
        object sender,
        RoutedEventArgs e) =>
        ToggleToolPanelFloating(_bottomToolHost);

    private void ToggleToolPanelFloating(DockableToolHost? host)
    {
        if (host == null)
            return;
        if (!host.TryToggleFloating())
        {
            Log(
                $"{host.PanelId} could not change its dock state safely.",
                "Editor",
                LogLevel.Warn);
            return;
        }
        MarkWorkspaceCustomized();
    }

    private void UpdateToolPanelPresentation(
        string panelId,
        ToolPanelDockState state)
    {
        bool visible = state != ToolPanelDockState.Hidden;
        bool floating = state == ToolPanelDockState.Floating;
        string action = floating ? "Dock" : "Float";
        switch (panelId)
        {
            case ToolPanelDockingContract.HierarchyPanelId:
                MenuShowHierarchy.IsChecked = visible;
                HierarchyFloatButton.Content = action;
                HierarchyFloatButton.ToolTip = floating
                    ? "Return Scene Outliner to the main editor window"
                    : "Open Scene Outliner in an independent window";
                AutomationProperties.SetName(
                    HierarchyFloatButton,
                    floating
                        ? "Dock Scene Outliner"
                        : "Float Scene Outliner");
                break;
            case ToolPanelDockingContract.InspectorPanelId:
                MenuShowInspector.IsChecked = visible;
                InspectorFloatButton.Content = action;
                InspectorFloatButton.ToolTip = floating
                    ? "Return Details to the main editor window"
                    : "Open Details in an independent window";
                AutomationProperties.SetName(
                    InspectorFloatButton,
                    floating ? "Dock Details" : "Float Details");
                break;
            case ToolPanelDockingContract.BottomPanelId:
                MenuShowBottom.IsChecked = visible;
                BottomFloatButton.Content = action;
                BottomFloatButton.ToolTip = floating
                    ? "Return the bottom tools to the main editor window"
                    : "Open the bottom tools in an independent window";
                AutomationProperties.SetName(
                    BottomFloatButton,
                    floating
                        ? "Dock bottom tool panel"
                        : "Float bottom tool panel");
                BottomDockToggleBtn.Content = visible ? "Hide" : "Show";
                break;
        }
    }

    private DockableToolHost? GetToolPanelHost(string panelId) =>
        panelId switch
        {
            ToolPanelDockingContract.HierarchyPanelId =>
                _hierarchyToolHost,
            ToolPanelDockingContract.InspectorPanelId =>
                _inspectorToolHost,
            ToolPanelDockingContract.BottomPanelId =>
                _bottomToolHost,
            _ => null,
        };

    private void ApplyToolPanelDockVisibility(
        string panelId,
        bool visible)
    {
        switch (panelId)
        {
            case ToolPanelDockingContract.HierarchyPanelId:
                ApplyHierarchyDockVisibility(visible);
                break;
            case ToolPanelDockingContract.InspectorPanelId:
                ApplyInspectorDockVisibility(visible);
                break;
            case ToolPanelDockingContract.BottomPanelId:
                ApplyBottomDockVisibility(visible);
                break;
        }
    }

    private bool ResetDockableToolPanels()
    {
        if (_hierarchyToolHost == null ||
            _inspectorToolHost == null ||
            _bottomToolHost == null ||
            _toolPanelPlacementStore == null)
        {
            Log(
                "Layout reset was cancelled because tool panel hosts are " +
                "not initialized.",
                "Editor",
                LogLevel.Error);
            return false;
        }

        DockableToolHost[] hosts =
        {
            _hierarchyToolHost,
            _inspectorToolHost,
            _bottomToolHost,
        };
        DockableToolHostSnapshot[] initial;
        try
        {
            initial = hosts
                .Select(host => host.CaptureSnapshot())
                .ToArray();
        }
        catch (Exception error)
        {
            Log(
                "Layout reset was cancelled because the current tool layout " +
                "could not be captured: " + error.Message,
                "Editor",
                LogLevel.Error);
            return false;
        }

        bool previousSuppression = _suppressToolPanelPersistence;
        _suppressToolPanelPersistence = true;
        bool commit = false;
        bool exactRollback = true;
        try
        {
            var resetResults = new bool[hosts.Length];
            bool mayContinue = true;
            for (int index = 0; index < hosts.Length; index++)
            {
                resetResults[index] =
                    mayContinue && hosts[index].ResetToDock();
                mayContinue = resetResults[index];
            }

            commit =
                ToolPanelResetTransactionPolicy.CanCommitDefaults(
                    resetResults) &&
                _toolPanelPlacementStore.ResetAndDelete();
            if (commit)
                return true;

            _toolPanelPlacementStore.Restore(initial);
            for (int index = hosts.Length - 1; index >= 0; index--)
            {
                ToolPanelDockState desired =
                    ToolPanelResetTransactionPolicy.DesiredFinalState(
                        initial[index].State,
                        commitDefaults: false);
                bool restored = hosts[index].TryRestoreState(desired);
                bool truthful =
                    ToolPanelResetTransactionPolicy.HasRestoredInitialState(
                        initial[index].State,
                        hosts[index].State);
                exactRollback &= restored && truthful;
            }

            DockableToolHostSnapshot[] actual = hosts
                .Select(host => host.CaptureSnapshot())
                .ToArray();
            _toolPanelPlacementStore.Restore(actual);
        }
        catch (Exception error)
        {
            exactRollback = false;
            Log(
                "Tool panel reset rollback encountered an error: " +
                error.Message,
                "Editor",
                LogLevel.Error);
            try
            {
                DockableToolHostSnapshot[] actual = hosts
                    .Select(host => host.CaptureSnapshot())
                    .ToArray();
                _toolPanelPlacementStore.Restore(actual);
            }
            catch (Exception captureError)
            {
                Log(
                    "The post-rollback tool layout could not be captured: " +
                    captureError.Message,
                    "Editor",
                    LogLevel.Error);
            }
        }
        finally
        {
            _suppressToolPanelPersistence = previousSuppression;
        }

        if (!previousSuppression)
            _toolPanelPlacementStore.Save();
        Log(
            exactRollback
                ? "Layout reset was cancelled; the previous tool panel " +
                  "layout was restored."
                : "Layout reset was cancelled; one or more tool panels " +
                  "could not be restored to their previous state.",
            "Editor",
            exactRollback ? LogLevel.Warn : LogLevel.Error);
        return false;
    }

    private void OnDockableToolPanelsOwnerClosing(
        object? sender,
        CancelEventArgs e)
    {
        if (e.Cancel)
            return;
        DockableToolHost[] floating = new[]
            {
                _hierarchyToolHost,
                _inspectorToolHost,
                _bottomToolHost,
            }
            .OfType<DockableToolHost>()
            .Where(host => host.IsFloating)
            .ToArray();

        foreach (DockableToolHost host in floating)
        {
            if (host.CloseForOwner())
                continue;
            e.Cancel = true;
            foreach (DockableToolHost previous in floating)
            {
                if (!previous.IsFloating)
                    _ = previous.TryFloat();
            }
            Log(
                "Editor close was deferred because a floating tool panel " +
                "could not be safely re-docked.",
                "Editor",
                LogLevel.Error);
            return;
        }
    }
}

/// <summary>
/// Bounded, versioned and atomically replaced user-local placement storage.
/// Invalid or duplicate panel identities invalidate the complete snapshot.
/// </summary>
internal sealed class ToolPanelPlacementStore
{
    private const int StoreVersion = 1;
    private const int MaximumBytes = 16 * 1024;
    private readonly Dictionary<string, ToolPanelPlacementState> _defaults;
    private readonly Dictionary<string, ToolPanelPlacementState> _states;
    private readonly Action<string> _logWarning;

    private sealed class Snapshot
    {
        public int Version { get; set; } = StoreVersion;
        public List<ToolPanelPlacementState> Panels { get; set; } = new();
    }

    internal ToolPanelPlacementStore(
        IEnumerable<ToolPanelPlacementState> defaults,
        Action<string> logWarning)
    {
        ArgumentNullException.ThrowIfNull(defaults);
        ArgumentNullException.ThrowIfNull(logWarning);
        _logWarning = logWarning;
        _defaults = defaults.ToDictionary(
            state => state.PanelId,
            Clone,
            StringComparer.Ordinal);
        if (_defaults.Count !=
                ToolPanelDockingContract.RegisteredPanels.Count ||
            !ToolPanelPlacementPolicy.HasUniqueKnownPanelIds(
                _defaults.Values))
        {
            throw new ArgumentException(
                "Tool panel defaults must contain every known panel once.",
                nameof(defaults));
        }
        _states = _defaults.ToDictionary(
            pair => pair.Key,
            pair => Clone(pair.Value),
            StringComparer.Ordinal);
    }

    private static string StorePath => Path.Combine(
        Environment.GetFolderPath(
            Environment.SpecialFolder.LocalApplicationData),
        "AcsEditor",
        $"ToolPanels.v{StoreVersion}.json");

    internal void Load()
    {
        if (!File.Exists(StorePath))
            return;
        try
        {
            var info = new FileInfo(StorePath);
            if (info.Length is <= 0 or > MaximumBytes)
            {
                throw new InvalidDataException(
                    "tool panel layout file has an invalid size");
            }
            byte[] payload;
            using (var stream = new FileStream(
                       StorePath,
                       FileMode.Open,
                       FileAccess.Read,
                       FileShare.Read,
                       bufferSize: 4096,
                       FileOptions.SequentialScan))
            {
                if (stream.Length is <= 0 or > MaximumBytes)
                {
                    throw new InvalidDataException(
                        "tool panel layout file has an invalid size");
                }
                payload = new byte[checked((int)stream.Length)];
                stream.ReadExactly(payload);
                if (stream.ReadByte() != -1)
                {
                    throw new InvalidDataException(
                        "tool panel layout changed while it was read");
                }
            }
            Snapshot? snapshot =
                JsonSerializer.Deserialize<Snapshot>(payload);
            if (snapshot == null ||
                snapshot.Version != StoreVersion ||
                snapshot.Panels.Count !=
                    ToolPanelDockingContract.RegisteredPanels.Count ||
                !ToolPanelPlacementPolicy.HasUniqueKnownPanelIds(
                    snapshot.Panels))
            {
                throw new InvalidDataException(
                    "tool panel layout identities are invalid");
            }

            var normalized = new Dictionary<
                string,
                ToolPanelPlacementState>(StringComparer.Ordinal);
            ToolWindowDipBounds virtualScreen = new(
                SystemParameters.VirtualScreenLeft,
                SystemParameters.VirtualScreenTop,
                SystemParameters.VirtualScreenWidth,
                SystemParameters.VirtualScreenHeight);
            foreach (ToolPanelPlacementState candidate in snapshot.Panels)
            {
                ToolPanelPlacementState fallback = _defaults[
                    candidate.PanelId];
                if (!ToolPanelPlacementPolicy.TryNormalizeForRestore(
                        candidate,
                        virtualScreen,
                        new ToolWindowDipBounds(
                            fallback.Left,
                            fallback.Top,
                            fallback.Width,
                            fallback.Height),
                        out ToolPanelPlacementState valid))
                {
                    throw new InvalidDataException(
                        "tool panel layout geometry is invalid");
                }
                normalized.Add(valid.PanelId, valid);
            }
            if (normalized.Count != _defaults.Count)
            {
                throw new InvalidDataException(
                    "tool panel layout is incomplete");
            }
            _states.Clear();
            foreach ((string panelId, ToolPanelPlacementState state) in
                     normalized)
            {
                _states.Add(panelId, state);
            }
        }
        catch (Exception error)
        {
            RestoreDefaults();
            _logWarning(
                "Floating tool layout could not be restored; defaults were " +
                "used: " + error.Message);
        }
    }

    internal ToolPanelPlacementState Get(string panelId)
    {
        if (!_states.TryGetValue(panelId, out ToolPanelPlacementState? state))
            throw new ArgumentOutOfRangeException(nameof(panelId));
        return Clone(state);
    }

    internal void Update(
        string panelId,
        Rect bounds,
        ToolPanelDockState state)
    {
        if (!_states.TryGetValue(
                panelId,
                out ToolPanelPlacementState? previous) ||
            !Enum.IsDefined(state))
        {
            throw new ArgumentOutOfRangeException(nameof(panelId));
        }
        Rect normalized = ToolPanelDockingContract.NormalizePlacementRect(
            bounds,
            new Rect(
                previous.Left,
                previous.Top,
                previous.Width,
                previous.Height));
        _states[panelId] = new ToolPanelPlacementState
        {
            PanelId = panelId,
            State = state,
            Left = normalized.Left,
            Top = normalized.Top,
            Width = normalized.Width,
            Height = normalized.Height,
        };
    }

    internal void Restore(
        IReadOnlyList<DockableToolHostSnapshot> snapshots)
    {
        ArgumentNullException.ThrowIfNull(snapshots);
        if (snapshots.Count !=
            ToolPanelDockingContract.RegisteredPanels.Count)
        {
            throw new InvalidDataException(
                "tool panel snapshot is incomplete");
        }

        var restored = new Dictionary<
            string,
            ToolPanelPlacementState>(StringComparer.Ordinal);
        foreach (DockableToolHostSnapshot snapshot in snapshots)
        {
            if (!ToolPanelDockingContract.IsKnownPanelId(
                    snapshot.PanelId) ||
                !Enum.IsDefined(snapshot.State) ||
                !restored.TryAdd(
                    snapshot.PanelId,
                    CreateRestoredState(snapshot)))
            {
                throw new InvalidDataException(
                    "tool panel snapshot identities are invalid");
            }
        }
        _states.Clear();
        foreach ((string panelId, ToolPanelPlacementState state) in restored)
            _states.Add(panelId, state);
    }

    private ToolPanelPlacementState CreateRestoredState(
        DockableToolHostSnapshot snapshot)
    {
        ToolPanelPlacementState fallback = _defaults[snapshot.PanelId];
        Rect bounds = ToolPanelDockingContract.NormalizePlacementRect(
            snapshot.Placement,
            new Rect(
                fallback.Left,
                fallback.Top,
                fallback.Width,
                fallback.Height));
        return new ToolPanelPlacementState
        {
            PanelId = snapshot.PanelId,
            State = snapshot.State,
            Left = bounds.Left,
            Top = bounds.Top,
            Width = bounds.Width,
            Height = bounds.Height,
        };
    }

    internal void Save()
    {
        try
        {
            var snapshot = new Snapshot
            {
                Panels = ToolPanelDockingContract.RegisteredPanels
                    .Select(descriptor => Clone(_states[descriptor.PanelId]))
                    .ToList(),
            };
            byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(
                snapshot,
                new JsonSerializerOptions { WriteIndented = true });
            if (bytes.Length > MaximumBytes)
            {
                throw new InvalidDataException(
                    "tool panel layout exceeds its storage budget");
            }

            string path = StorePath;
            string directory = Path.GetDirectoryName(path)!;
            Directory.CreateDirectory(directory);
            string temporary =
                path + $".{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
            try
            {
                using (var stream = new FileStream(
                           temporary,
                           FileMode.CreateNew,
                           FileAccess.Write,
                           FileShare.None,
                           bufferSize: 4096,
                           FileOptions.WriteThrough))
                {
                    stream.Write(bytes);
                    stream.Flush(flushToDisk: true);
                }
                if (File.Exists(path))
                    File.Replace(temporary, path, null, ignoreMetadataErrors: true);
                else
                    File.Move(temporary, path);
            }
            finally
            {
                try
                {
                    if (File.Exists(temporary))
                        File.Delete(temporary);
                }
                catch
                {
                    // A stale temporary file is bounded and ignored on load.
                }
            }
        }
        catch (Exception error)
        {
            _logWarning(
                "Floating tool layout could not be saved: " + error.Message);
        }
    }

    internal bool ResetAndDelete()
    {
        RestoreDefaults();
        try
        {
            if (File.Exists(StorePath))
                File.Delete(StorePath);
            return true;
        }
        catch (Exception error)
        {
            _logWarning(
                "Floating tool layout could not be deleted: " + error.Message);
            return false;
        }
    }

    private void RestoreDefaults()
    {
        _states.Clear();
        foreach ((string panelId, ToolPanelPlacementState state) in _defaults)
            _states.Add(panelId, Clone(state));
    }

    private static ToolPanelPlacementState Clone(
        ToolPanelPlacementState state) =>
        new()
        {
            Version = state.Version,
            PanelId = state.PanelId,
            State = state.State,
            Left = state.Left,
            Top = state.Top,
            Width = state.Width,
            Height = state.Height,
        };
}
