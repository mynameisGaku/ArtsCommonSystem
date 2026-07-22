// SPDX-License-Identifier: Apache-2.0

using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;

namespace AcsEditor;

/// <summary>Named UE-style workspace profiles layered over the last-session layout.</summary>
public partial class MainWindow
{
    private EditorWorkspaceStore? _workspaceStore;
    private string _activeWorkspaceName = EditorWorkspaceStore.DefaultWorkspaceName;
    private bool _applyingWorkspace;

    private EditorWorkspaceStore WorkspaceStore =>
        _workspaceStore ??= new EditorWorkspaceStore();

    private void InitializeWorkspaceProfiles()
    {
        if (_workspaceStore != null)
            return;
        _workspaceStore = new EditorWorkspaceStore();
        _activeWorkspaceName = _workspaceStore.LastActiveName;
        UpdateWorkspaceStatus();
        if (_workspaceStore.LoadWarning is { Length: > 0 } warning)
        {
            Log(
                "Named workspaces could not be restored; built-in layouts are available: " +
                warning,
                "Editor",
                LogLevel.Warn);
        }
    }

    private EditorWorkspaceLayout CaptureWorkspaceLayout()
    {
        UpdateLayout();
        return EditorWorkspaceStore.NormalizeLayout(new EditorWorkspaceLayout
        {
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
        });
    }

    private string CurrentBottomTab()
    {
        if (TabBuild.IsChecked == true)
            return "build";
        if (TabAssets.IsChecked == true)
            return "assets";
        if (TabProfiler.IsChecked == true)
            return "profiler";
        return "console";
    }

    private void ApplyWorkspace(EditorWorkspaceProfile profile)
    {
        EditorWorkspaceLayout layout =
            EditorWorkspaceStore.NormalizeLayout(profile.Layout);
        _applyingWorkspace = true;
        try
        {
            _hierarchyWidth = layout.HierarchyWidth;
            _inspectorWidth = layout.InspectorWidth;
            _bottomDockHeight = layout.BottomDockHeight;
            SetHierarchyVisible(layout.HierarchyVisible);
            SetInspectorVisible(layout.InspectorVisible);
            ShowBottomTab(layout.BottomTab);
            SetBottomDockVisible(layout.BottomDockVisible);
            _activeWorkspaceName = profile.Name;
            WorkspaceStore.MarkActive(profile.Name);
            UpdateWorkspaceStatus();
            SaveEditorLayout();
            Log($"Workspace activated: {profile.Name}", "Editor", LogLevel.Info);
        }
        finally
        {
            _applyingWorkspace = false;
        }
    }

    private void MarkWorkspaceCustomized()
    {
        if (_applyingWorkspace)
            return;
        _activeWorkspaceName = "Custom";
        UpdateWorkspaceStatus();
    }

    private void UpdateWorkspaceStatus()
    {
        if (WorkspaceStateText == null)
            return;
        WorkspaceStateText.Text = $"WORKSPACE: {_activeWorkspaceName.ToUpperInvariant()}";
        WorkspaceStateText.ToolTip =
            "Named workspace layouts never change the active scene, selection or camera.";
    }

    private void OnWorkspaceMenuOpened(object sender, RoutedEventArgs e)
    {
        InitializeWorkspaceProfiles();
        WorkspaceMenu.Items.Clear();
        foreach (EditorWorkspaceProfile profile in WorkspaceStore.GetProfiles())
        {
            var item = new MenuItem
            {
                Header = profile.Name,
                IsCheckable = true,
                IsChecked = string.Equals(
                    profile.Name,
                    _activeWorkspaceName,
                    StringComparison.OrdinalIgnoreCase),
                Tag = profile.Name,
            };
            item.Click += OnActivateWorkspaceMenuItem;
            WorkspaceMenu.Items.Add(item);
        }

        WorkspaceMenu.Items.Add(new Separator());
        var manage = new MenuItem
        {
            Header = "Manage Workspaces…",
            InputGestureText = "Ctrl+Alt+W",
        };
        manage.Click += OnManageWorkspaces;
        WorkspaceMenu.Items.Add(manage);
    }

    private void OnActivateWorkspaceMenuItem(object sender, RoutedEventArgs e)
    {
        if (sender is not MenuItem { Tag: string name })
            return;
        ActivateWorkspaceByName(name);
    }

    private void ActivateWorkspaceByName(string name)
    {
        InitializeWorkspaceProfiles();
        if (!WorkspaceStore.TryGetProfile(name, out EditorWorkspaceProfile profile))
        {
            Log($"Workspace no longer exists: {name}", "Editor", LogLevel.Warn);
            return;
        }
        ApplyWorkspace(profile);
    }

    private void OnManageWorkspaces(object sender, RoutedEventArgs e)
    {
        InitializeWorkspaceProfiles();
        var dialog = new WorkspaceManagerWindow(WorkspaceStore, CaptureWorkspaceLayout())
        {
            Owner = this,
        };
        if (dialog.ShowDialog() == true && dialog.SelectedProfile is { } profile)
            ApplyWorkspace(profile);
        else
        {
            _activeWorkspaceName = WorkspaceStore.LastActiveName;
            UpdateWorkspaceStatus();
        }
    }

    private void RestoreWorkspaceIdentity(string? workspaceName)
    {
        InitializeWorkspaceProfiles();
        string desired = string.IsNullOrWhiteSpace(workspaceName)
            ? WorkspaceStore.LastActiveName
            : workspaceName;
        _activeWorkspaceName = WorkspaceStore.GetProfiles().Any(profile =>
            string.Equals(profile.Name, desired, StringComparison.OrdinalIgnoreCase))
            ? WorkspaceStore.GetProfiles().First(profile =>
                string.Equals(profile.Name, desired, StringComparison.OrdinalIgnoreCase)).Name
            : "Custom";
        UpdateWorkspaceStatus();
    }
}
