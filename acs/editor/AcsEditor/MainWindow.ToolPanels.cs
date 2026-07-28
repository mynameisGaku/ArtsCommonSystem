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
    private readonly Dictionary<string, DockableToolHost> _toolPanelHosts =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, FrameworkElement> _bottomToolContents =
        new(StringComparer.Ordinal);
    private readonly HashSet<string> _bottomDockedToolIds =
        new(StringComparer.Ordinal);
    private readonly BottomToolDockVisibilityState _bottomDockVisibility =
        new();
    private string _activeBottomToolId =
        ToolPanelDockingContract.ConsolePanelId;
    private ToolPanelPlacementStore? _toolPanelPlacementStore;
    private bool _toolPanelsRestoreCompleted;
    private bool _suppressToolPanelPersistence;
    private int _toolPanelUserMutationDepth;
    private ToolPanelOwnerCloseSnapshot? _pendingToolPanelOwnerClose;

    private sealed record ToolPanelOwnerCloseSnapshot(
        DockableToolHostSnapshot[] Panels,
        string ActiveBottomToolId);

    private void InitializeDockableToolPanels()
    {
        _toolPanelPlacementStore = new ToolPanelPlacementStore(
            CreateDefaultToolPanelPlacements(),
            message => Log(message, "Editor", LogLevel.Warn));
        _toolPanelPlacementStore.Load();

        _toolPanelHosts.Clear();
        _bottomToolContents.Clear();
        _bottomDockedToolIds.Clear();

        RegisterToolPanelHost(
            ToolPanelDockingContract.HierarchyPanelId,
            "Scene Outliner",
            HierarchyPanel,
            () => HierarchyPanel.Visibility == Visibility.Visible,
            ApplyHierarchyDockVisibility);
        RegisterToolPanelHost(
            ToolPanelDockingContract.InspectorPanelId,
            "Details",
            InspectorPanel,
            () => InspectorPanel.Visibility == Visibility.Visible,
            ApplyInspectorDockVisibility);

        RegisterBottomToolPanel(
            ToolPanelDockingContract.ConsolePanelId,
            "Console",
            ConsoleToolPanel);
        RegisterBottomToolPanel(
            ToolPanelDockingContract.BuildPanelId,
            "Build",
            BuildToolPanel);
        RegisterBottomToolPanel(
            ToolPanelDockingContract.AssetsPanelId,
            "Assets",
            AssetsToolPanel);
        RegisterBottomToolPanel(
            ToolPanelDockingContract.ProfilerPanelId,
            "Profiler",
            ProfilerToolPanel);

        if (_toolPanelHosts.Count !=
            ToolPanelDockingContract.RegisteredPanels.Count)
        {
            throw new InvalidOperationException(
                "Every registered tool panel must have exactly one visual host.");
        }

        foreach (ToolPanelDockingDescriptor descriptor in
                 ToolPanelDockingContract.RegisteredPanels)
        {
            UpdateToolPanelPresentation(
                descriptor.PanelId,
                _toolPanelHosts[descriptor.PanelId].State);
        }
        RefreshBottomToolDock();
    }

    private void RegisterBottomToolPanel(
        string panelId,
        string title,
        FrameworkElement content)
    {
        if (!ToolPanelDockingContract.IsBottomToolPanelId(panelId) ||
            !_bottomToolContents.TryAdd(panelId, content))
        {
            throw new InvalidOperationException(
                $"Bottom tool panel '{panelId}' is not registered exactly once.");
        }
        _bottomDockedToolIds.Add(panelId);
        RegisterToolPanelHost(
            panelId,
            title,
            content,
            () => _bottomDockedToolIds.Contains(panelId),
            visible => ApplyBottomToolDockVisibility(panelId, visible));
    }

    private void RegisterToolPanelHost(
        string panelId,
        string title,
        FrameworkElement content,
        Func<bool> dockVisibility,
        Action<bool> applyDockVisibility)
    {
        if (_toolPanelHosts.ContainsKey(panelId))
        {
            throw new InvalidOperationException(
                $"Tool panel '{panelId}' was registered more than once.");
        }
        var host = new DockableToolHost(
            this,
            panelId,
            title,
            content,
            SceneWorkspace,
            dockVisibility,
            applyDockVisibility,
            (state, committedTransition) =>
            {
                UpdateToolPanelPresentation(panelId, state);
                if (ToolPanelWorkspaceMutationPolicy
                    .ShouldMarkPublishedTransition(
                        committedTransition,
                        _toolPanelUserMutationDepth > 0,
                        _toolPanelsRestoreCompleted,
                        _suppressToolPanelPersistence))
                {
                    MarkWorkspaceCustomized();
                }
            },
            () => LoadToolPanelBounds(panelId),
            (bounds, floating) =>
                SaveToolPanelPlacement(panelId, bounds, floating),
            message => Log(message, "Editor", LogLevel.Warn));
        _toolPanelHosts.Add(panelId, host);
    }

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
                ToolPanelDockingContract.ConsolePanelId,
                new Rect(
                    workArea.Left + 180.0,
                    workArea.Top + 180.0,
                    920.0,
                    440.0)),
            CreateToolPanelPlacement(
                ToolPanelDockingContract.BuildPanelId,
                new Rect(
                    workArea.Left + 220.0,
                    workArea.Top + 210.0,
                    860.0,
                    420.0)),
            CreateToolPanelPlacement(
                ToolPanelDockingContract.AssetsPanelId,
                new Rect(
                    workArea.Left + 160.0,
                    workArea.Top + 140.0,
                    1120.0,
                    680.0)),
            CreateToolPanelPlacement(
                ToolPanelDockingContract.ProfilerPanelId,
                new Rect(
                    workArea.Left + 260.0,
                    workArea.Top + 120.0,
                    960.0,
                    680.0)),
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

        if (ToolPanelStartupRestorePolicy.ShouldSeedCurrentLayout(
                _toolPanelPlacementStore.LoadedPersistedSnapshot))
        {
            string activeBottomToolId = _activeBottomToolId;
            try
            {
                DockableToolHostSnapshot[] current =
                    ToolPanelDockingContract.RegisteredPanels
                        .Select(descriptor =>
                            _toolPanelHosts[descriptor.PanelId]
                                .CaptureSnapshot())
                        .ToArray();
                _toolPanelPlacementStore.Restore(current);
                _toolPanelPlacementStore.Save();
            }
            catch (Exception error)
            {
                Log(
                    "The current editor layout could not seed tool-panel " +
                    "placement storage: " + error.Message,
                    "Editor",
                    LogLevel.Warn);
            }
            _activeBottomToolId = activeBottomToolId;
            _toolPanelsRestoreCompleted = true;
            return;
        }

        string requestedBottomToolId = _activeBottomToolId;
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
        _activeBottomToolId =
            BottomToolDockSelectionPolicy.ResolveActivePanelId(
                requestedBottomToolId,
                _bottomDockedToolIds);
        RefreshBottomToolDock();
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
        bool dockVisible = IsToolPanelDockVisible(panelId);
        _toolPanelPlacementStore.Update(
            panelId,
            bounds,
            ToolPanelDockingContract.ResolveState(floating, dockVisible));
        if (!_suppressToolPanelPersistence)
            _toolPanelPlacementStore.Save();
    }

    private bool IsToolPanelDockVisible(string panelId) =>
        panelId switch
        {
            ToolPanelDockingContract.HierarchyPanelId =>
                HierarchyPanel.Visibility == Visibility.Visible,
            ToolPanelDockingContract.InspectorPanelId =>
                InspectorPanel.Visibility == Visibility.Visible,
            _ when ToolPanelDockingContract.IsBottomToolPanelId(panelId) =>
                _bottomDockedToolIds.Contains(panelId),
            _ => false,
        };

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
        ToggleToolPanelFloatingFromUser(
            ToolPanelDockingContract.HierarchyPanelId);

    private void OnToggleInspectorFloat(
        object sender,
        RoutedEventArgs e) =>
        ToggleToolPanelFloatingFromUser(
            ToolPanelDockingContract.InspectorPanelId);

    private void OnToggleBottomFloat(
        object sender,
        RoutedEventArgs e) =>
        ToggleToolPanelFloatingFromUser(_activeBottomToolId);

    private void ToggleToolPanelFloatingFromUser(string panelId)
    {
        DockableToolHost? host = GetToolPanelHost(panelId);
        if (host == null)
            return;
        ToolPanelUserAction action = host.IsFloating
            ? ToolPanelUserAction.Redock
            : ToolPanelUserAction.Float;
        _ = ExecuteToolPanelUserAction(panelId, action);
    }

    private void OnExecuteToolPanelUserAction(
        object sender,
        RoutedEventArgs e)
    {
        if (sender is not MenuItem item ||
            item.Tag is not string tag ||
            !ToolPanelDockingContract.TryParseMenuActionTag(
                tag,
                out string panelId,
                out ToolPanelUserAction action))
        {
            Log(
                "The requested tool-panel menu action was invalid.",
                "Editor",
                LogLevel.Warn);
            return;
        }
        _ = ExecuteToolPanelUserAction(panelId, action);
    }

    private void OnToolPanelActionSubmenuOpened(
        object sender,
        RoutedEventArgs e)
    {
        if (sender is not MenuItem submenu)
            return;
        foreach (MenuItem item in submenu.Items.OfType<MenuItem>())
        {
            item.IsEnabled =
                item.Tag is string tag &&
                ToolPanelDockingContract.TryParseMenuActionTag(
                    tag,
                    out string panelId,
                    out ToolPanelUserAction action) &&
                CanExecuteToolPanelUserAction(panelId, action);
        }
    }

    private bool CanExecuteToolPanelUserAction(
        string panelId,
        ToolPanelUserAction action) =>
        GetToolPanelHost(panelId) is DockableToolHost host &&
        ToolPanelDockingContract.CanExecuteUserAction(host.State, action);

    private bool ExecuteToolPanelUserAction(
        string panelId,
        ToolPanelUserAction action) =>
        ExecuteToolPanelUserMutation(
            () => TryExecuteToolPanelAction(panelId, action));

    private bool ExecuteToolPanelUserMutation(Func<bool> mutation)
    {
        ArgumentNullException.ThrowIfNull(mutation);
        _toolPanelUserMutationDepth++;
        try
        {
            return ToolPanelWorkspaceMutationPolicy.ApplyUserMutation(
                mutation,
                MarkToolPanelWorkspaceCustomizedIfAllowed);
        }
        finally
        {
            _toolPanelUserMutationDepth--;
        }
    }

    private void MarkToolPanelWorkspaceCustomizedIfAllowed()
    {
        if (ToolPanelWorkspaceMutationPolicy.ShouldMarkCustomized(
                _toolPanelsRestoreCompleted,
                _suppressToolPanelPersistence))
        {
            MarkWorkspaceCustomized();
        }
    }

    private bool TryExecuteToolPanelAction(
        string panelId,
        ToolPanelUserAction action)
    {
        DockableToolHost? host = ToolPanelDockingContract.IsKnownPanelId(panelId)
            ? GetToolPanelHost(panelId)
            : null;
        if (host == null ||
            !ToolPanelDockingContract.CanExecuteUserAction(host.State, action))
        {
            if (host != null)
                UpdateToolPanelPresentation(panelId, host.State);
            Log(
                $"{panelId} cannot execute the {action} tool-panel action " +
                "from its current state.",
                "Editor",
                LogLevel.Warn);
            return false;
        }

        ToolPanelDockState requestedState = action switch
        {
            ToolPanelUserAction.Show => ToolPanelDockState.Docked,
            ToolPanelUserAction.Hide => ToolPanelDockState.Hidden,
            ToolPanelUserAction.Float => ToolPanelDockState.Floating,
            ToolPanelUserAction.Redock => ToolPanelDockState.Docked,
            _ => throw new ArgumentOutOfRangeException(nameof(action)),
        };
        if (!host.TryRestoreState(requestedState))
        {
            UpdateToolPanelPresentation(panelId, host.State);
            Log(
                $"{panelId} could not complete the {action} tool-panel " +
                "action safely.",
                "Editor",
                LogLevel.Warn);
            return false;
        }

        if (ToolPanelDockingContract.IsBottomToolPanelId(panelId) &&
            action is ToolPanelUserAction.Show or
                ToolPanelUserAction.Redock)
        {
            _bottomDockVisibility.SetVisible(visible: true);
            _activeBottomToolId = panelId;
            RefreshBottomToolDock();
        }
        if (host.State != ToolPanelDockState.Floating)
        {
            PersistDockedToolPanelState(
                panelId,
                visible: host.State == ToolPanelDockState.Docked);
        }
        return true;
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
            case ToolPanelDockingContract.ConsolePanelId:
                MenuShowConsole.IsChecked = visible;
                break;
            case ToolPanelDockingContract.BuildPanelId:
                MenuShowBuildPanel.IsChecked = visible;
                break;
            case ToolPanelDockingContract.AssetsPanelId:
                MenuShowAssets.IsChecked = visible;
                break;
            case ToolPanelDockingContract.ProfilerPanelId:
                MenuShowProfiler.IsChecked = visible;
                break;
        }
        if (ToolPanelDockingContract.IsBottomToolPanelId(panelId))
            RefreshBottomToolDock();
    }

    private DockableToolHost? GetToolPanelHost(string panelId) =>
        _toolPanelHosts.TryGetValue(
            panelId,
            out DockableToolHost? host)
            ? host
            : null;

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
            default:
                if (ToolPanelDockingContract.IsBottomToolPanelId(panelId))
                    ApplyBottomToolDockVisibility(panelId, visible);
                break;
        }
    }

    private void ApplyBottomToolDockVisibility(
        string panelId,
        bool visible)
    {
        if (!ToolPanelDockingContract.IsBottomToolPanelId(panelId))
            throw new ArgumentOutOfRangeException(nameof(panelId));

        if (visible)
        {
            _bottomDockedToolIds.Add(panelId);
            _activeBottomToolId = panelId;
        }
        else
        {
            _bottomDockedToolIds.Remove(panelId);
            if (string.Equals(
                    _activeBottomToolId,
                    panelId,
                    StringComparison.Ordinal))
            {
                _activeBottomToolId =
                    BottomToolDockSelectionPolicy.ResolveActivePanelId(
                        _activeBottomToolId,
                        _bottomDockedToolIds);
            }
        }
        RefreshBottomToolDock();
    }

    private void RefreshBottomToolDock()
    {
        if (_bottomToolContents.Count == 0)
            return;

        _activeBottomToolId =
            BottomToolDockSelectionPolicy.ResolveActivePanelId(
                _activeBottomToolId,
                _bottomDockedToolIds);

        bool hasDockedTools = _bottomDockedToolIds.Count > 0;
        bool bottomDockVisible =
            _bottomDockVisibility.IsVisible(_bottomDockedToolIds.Count);
        ApplyBottomDockVisibility(bottomDockVisible);
        foreach ((string panelId, FrameworkElement content) in
                 _bottomToolContents)
        {
            DockableToolHost? host = GetToolPanelHost(panelId);
            bool activeDocked =
                _bottomDockedToolIds.Contains(panelId) &&
                string.Equals(
                    _activeBottomToolId,
                    panelId,
                    StringComparison.Ordinal);
            content.Visibility =
                host?.IsFloating == true || activeDocked
                    ? Visibility.Visible
                    : Visibility.Collapsed;
        }

        TabConsole.IsChecked =
            _activeBottomToolId == ToolPanelDockingContract.ConsolePanelId &&
            _bottomDockedToolIds.Contains(
                ToolPanelDockingContract.ConsolePanelId);
        TabBuild.IsChecked =
            _activeBottomToolId == ToolPanelDockingContract.BuildPanelId &&
            _bottomDockedToolIds.Contains(
                ToolPanelDockingContract.BuildPanelId);
        TabAssets.IsChecked =
            _activeBottomToolId == ToolPanelDockingContract.AssetsPanelId &&
            _bottomDockedToolIds.Contains(
                ToolPanelDockingContract.AssetsPanelId);
        TabProfiler.IsChecked =
            _activeBottomToolId == ToolPanelDockingContract.ProfilerPanelId &&
            _bottomDockedToolIds.Contains(
                ToolPanelDockingContract.ProfilerPanelId);
        UpdateBottomToolTabPresentation(
            TabConsole,
            ToolPanelDockingContract.ConsolePanelId,
            "Console");
        UpdateBottomToolTabPresentation(
            TabBuild,
            ToolPanelDockingContract.BuildPanelId,
            "Build");
        UpdateBottomToolTabPresentation(
            TabAssets,
            ToolPanelDockingContract.AssetsPanelId,
            "Assets");
        UpdateBottomToolTabPresentation(
            TabProfiler,
            ToolPanelDockingContract.ProfilerPanelId,
            "Profiler");

        MenuShowBottom.IsChecked = bottomDockVisible;
        BottomFloatButton.IsEnabled = hasDockedTools;
        BottomDockToggleBtn.IsEnabled = hasDockedTools;
        if (hasDockedTools)
        {
            ToolPanelDockingDescriptor descriptor =
                ToolPanelDockingContract.RegisteredPanels.First(
                    candidate => candidate.PanelId == _activeBottomToolId);
            BottomFloatButton.Content = "Float";
            BottomFloatButton.ToolTip =
                $"Open {descriptor.AccessibleName} in an independent window";
            BottomDockToggleBtn.Content = "Hide";
            BottomDockToggleBtn.ToolTip =
                $"Hide {descriptor.AccessibleName}";
            AutomationProperties.SetName(
                BottomFloatButton,
                $"Float {descriptor.AccessibleName}");
            AutomationProperties.SetName(
                BottomDockToggleBtn,
                $"Hide {descriptor.AccessibleName}");
        }
    }

    private void UpdateBottomToolTabPresentation(
        System.Windows.Controls.Primitives.ToggleButton tab,
        string panelId,
        string title)
    {
        ToolPanelDockState state =
            GetToolPanelHost(panelId)?.State ??
            ToolPanelDockState.Hidden;
        tab.Opacity = state switch
        {
            ToolPanelDockState.Floating => 0.78,
            ToolPanelDockState.Hidden => 0.62,
            _ => 1.0,
        };
        tab.ToolTip = state switch
        {
            ToolPanelDockState.Floating =>
                $"{title} is floating; click to re-dock it",
            ToolPanelDockState.Hidden =>
                $"{title} is hidden; click to restore it",
            _ => $"Show {title}",
        };
        AutomationProperties.SetName(
            tab,
            state switch
            {
                ToolPanelDockState.Floating => $"Re-dock {title}",
                ToolPanelDockState.Hidden => $"Restore {title}",
                _ => $"Show {title}",
            });
    }

    private bool ActivateBottomTool(string panelId)
    {
        if (!ToolPanelDockingContract.IsBottomToolPanelId(panelId) ||
            GetToolPanelHost(panelId) is not DockableToolHost host)
        {
            return false;
        }
        if (!host.TryRestoreState(ToolPanelDockState.Docked))
            return false;
        _bottomDockVisibility.SetVisible(visible: true);
        _activeBottomToolId = panelId;
        RefreshBottomToolDock();
        PersistDockedToolPanelState(panelId, visible: true);
        return true;
    }

    private bool ResetDockableToolPanels()
    {
        if (_toolPanelHosts.Count !=
                ToolPanelDockingContract.RegisteredPanels.Count ||
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
            ToolPanelDockingContract.RegisteredPanels
                .Select(descriptor =>
                    _toolPanelHosts[descriptor.PanelId])
                .ToArray();
        string initialBottomToolId = _activeBottomToolId;
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
            {
                _activeBottomToolId =
                    ToolPanelDockingContract.ConsolePanelId;
                RefreshBottomToolDock();
                return true;
            }

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
            _activeBottomToolId = initialBottomToolId;
            RefreshBottomToolDock();

            DockableToolHostSnapshot[] actual = hosts
                .Select(host => host.CaptureSnapshot())
                .ToArray();
            _toolPanelPlacementStore.Restore(actual);
        }
        catch (Exception error)
        {
            exactRollback = false;
            _activeBottomToolId = initialBottomToolId;
            RefreshBottomToolDock();
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
        if (EditorCloseFinalizationPolicy.ShouldBypassAuxiliaryHandlers(
                _auxiliaryCloseApproved))
        {
            return;
        }

        if (e.Cancel)
        {
            if (_pendingToolPanelOwnerClose != null)
            {
                _ = RollbackDockableToolPanelsOwnerClose();
                CancelApprovedEditorClose();
            }
            return;
        }

        if (_pendingToolPanelOwnerClose != null)
        {
            e.Cancel = true;
            _ = RollbackDockableToolPanelsOwnerClose();
            CancelApprovedEditorClose();
            Log(
                "Editor close was cancelled because an earlier tool-panel " +
                "close transaction was still pending.",
                "Editor",
                LogLevel.Error);
            return;
        }

        DockableToolHost[] hosts;
        DockableToolHostSnapshot[] snapshots;
        try
        {
            hosts = ToolPanelDockingContract.RegisteredPanels
                .Select(descriptor => _toolPanelHosts[descriptor.PanelId])
                .ToArray();
            snapshots = hosts
                .Select(host => host.CaptureSnapshot())
                .ToArray();
        }
        catch (Exception error)
        {
            e.Cancel = true;
            CancelApprovedEditorClose();
            Log(
                "Editor close was cancelled because the complete tool-panel " +
                "layout could not be captured: " + error.Message,
                "Editor",
                LogLevel.Error);
            return;
        }

        _pendingToolPanelOwnerClose = new ToolPanelOwnerCloseSnapshot(
            snapshots,
            _activeBottomToolId);
        bool previousSuppression = _suppressToolPanelPersistence;
        _suppressToolPanelPersistence = true;
        bool redocked = true;
        try
        {
            foreach (DockableToolHost host in hosts)
            {
                if (host.CloseForOwner())
                    continue;
                redocked = false;
                break;
            }
        }
        catch (Exception error)
        {
            redocked = false;
            Log(
                "A tool-panel owner-close operation failed: " + error.Message,
                "Editor",
                LogLevel.Error);
        }
        finally
        {
            _suppressToolPanelPersistence = previousSuppression;
        }

        if (redocked)
            return;

        e.Cancel = true;
        bool exactRollback = RollbackDockableToolPanelsOwnerClose();
        CancelApprovedEditorClose();
        Log(
            exactRollback
                ? "Editor close was cancelled because a floating tool panel " +
                  "could not be safely re-docked; the previous layout was restored."
                : "Editor close was cancelled because a floating tool panel " +
                  "could not be safely re-docked; the actual recovered layout " +
                  "was persisted.",
            "Editor",
            exactRollback ? LogLevel.Warn : LogLevel.Error);
    }

    private bool RollbackDockableToolPanelsOwnerClose()
    {
        ToolPanelOwnerCloseSnapshot? transaction =
            _pendingToolPanelOwnerClose;
        if (transaction == null)
            return true;
        _pendingToolPanelOwnerClose = null;

        DockableToolHost[] hosts = ToolPanelDockingContract.RegisteredPanels
            .Select(descriptor => _toolPanelHosts[descriptor.PanelId])
            .ToArray();
        bool previousSuppression = _suppressToolPanelPersistence;
        _suppressToolPanelPersistence = true;
        bool exactRollback = true;
        try
        {
            try
            {
                _toolPanelPlacementStore?.Restore(transaction.Panels);
            }
            catch (Exception error)
            {
                exactRollback = false;
                Log(
                    "The captured tool-panel placements could not be staged " +
                    "for rollback: " + error.Message,
                    "Editor",
                    LogLevel.Error);
            }

            for (int index = hosts.Length - 1; index >= 0; index--)
            {
                DockableToolHostSnapshot requested =
                    transaction.Panels[index];
                try
                {
                    bool restored =
                        hosts[index].TryRestoreState(requested.State);
                    exactRollback &=
                        restored &&
                        ToolPanelOwnerCloseTransactionPolicy.HasRestoredState(
                            requested,
                            hosts[index].State);
                }
                catch (Exception error)
                {
                    exactRollback = false;
                    Log(
                        $"{requested.PanelId} could not restore its captured " +
                        "owner-close state: " + error.Message,
                        "Editor",
                        LogLevel.Error);
                }
            }
            try
            {
                _activeBottomToolId = transaction.ActiveBottomToolId;
                RefreshBottomToolDock();
            }
            catch (Exception error)
            {
                exactRollback = false;
                Log(
                    "Bottom-tool presentation could not refresh after close " +
                    "rollback: " + error.Message,
                    "Editor",
                    LogLevel.Error);
            }
            _activeBottomToolId = transaction.ActiveBottomToolId;
        }
        catch (Exception error)
        {
            exactRollback = false;
            Log(
                "Tool-panel close rollback encountered an error: " +
                error.Message,
                "Editor",
                LogLevel.Error);
        }
        finally
        {
            _suppressToolPanelPersistence = previousSuppression;
        }

        PersistActualToolPanelOwnerCloseState(
            hosts,
            transaction,
            ref exactRollback);
        return exactRollback;
    }

    private void CommitDockableToolPanelsOwnerClose()
    {
        ToolPanelOwnerCloseSnapshot? transaction =
            _pendingToolPanelOwnerClose;
        if (transaction == null)
            return;
        _pendingToolPanelOwnerClose = null;

        // Re-docking is only a shutdown transfer. Preserve the user's floating
        // placements and selected bottom tab for the next editor session.
        try
        {
            _toolPanelPlacementStore?.Restore(transaction.Panels);
            _toolPanelPlacementStore?.Save();
        }
        catch (Exception error)
        {
            Log(
                "Tool-panel layout could not be committed for editor close: " +
                error.Message,
                "Editor",
                LogLevel.Error);
        }
        _activeBottomToolId = transaction.ActiveBottomToolId;
        try
        {
            RefreshBottomToolDock();
        }
        catch (Exception error)
        {
            Log(
                "Bottom-tool presentation could not refresh during editor " +
                "close: " + error.Message,
                "Editor",
                LogLevel.Error);
        }
        _activeBottomToolId = transaction.ActiveBottomToolId;
    }

    private void PersistActualToolPanelOwnerCloseState(
        IReadOnlyList<DockableToolHost> hosts,
        ToolPanelOwnerCloseSnapshot transaction,
        ref bool exactRollback)
    {
        if (_toolPanelPlacementStore == null)
            return;
        var actual = new DockableToolHostSnapshot[hosts.Count];
        for (int index = 0; index < hosts.Count; index++)
        {
            try
            {
                actual[index] = hosts[index].CaptureSnapshot();
                exactRollback &=
                    ToolPanelOwnerCloseTransactionPolicy.HasRestoredSnapshot(
                        transaction.Panels[index],
                        actual[index]);
            }
            catch (Exception error)
            {
                exactRollback = false;
                ToolPanelDockState actualState =
                    transaction.Panels[index].State;
                try
                {
                    actualState = hosts[index].State;
                }
                catch (Exception stateError)
                {
                    Log(
                        $"{hosts[index].PanelId} state could not be read " +
                        "after close rollback: " + stateError.Message,
                        "Editor",
                        LogLevel.Error);
                }
                actual[index] = new DockableToolHostSnapshot(
                    hosts[index].PanelId,
                    actualState,
                    transaction.Panels[index].Placement);
                Log(
                    $"{hosts[index].PanelId} placement could not be captured " +
                    "after close rollback: " + error.Message,
                    "Editor",
                    LogLevel.Error);
            }
        }

        try
        {
            _toolPanelPlacementStore.Restore(actual);
            _toolPanelPlacementStore.Save();
        }
        catch (Exception error)
        {
            exactRollback = false;
            Log(
                "The recovered tool-panel layout could not be persisted: " +
                error.Message,
                "Editor",
                LogLevel.Error);
        }
    }
}

/// <summary>
/// Bounded, versioned and atomically replaced user-local placement storage.
/// Invalid or duplicate panel identities invalidate the complete snapshot.
/// </summary>
internal sealed class ToolPanelPlacementStore
{
    private const int StoreVersion = 2;
    private const int MaximumBytes = 16 * 1024;
    private readonly Dictionary<string, ToolPanelPlacementState> _defaults;
    private readonly Dictionary<string, ToolPanelPlacementState> _states;
    private readonly Action<string> _logWarning;

    internal bool LoadedPersistedSnapshot { get; private set; }

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
        LoadedPersistedSnapshot = false;
        RestoreDefaults();
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
            LoadedPersistedSnapshot = true;
        }
        catch (Exception error)
        {
            RestoreDefaults();
            _logWarning(
                "Floating tool layout could not be restored; the current " +
                "editor layout will be retained: " + error.Message);
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

internal static class ToolPanelStartupRestorePolicy
{
    internal static bool ShouldSeedCurrentLayout(
        bool loadedPersistedSnapshot) =>
        !loadedPersistedSnapshot;
}
