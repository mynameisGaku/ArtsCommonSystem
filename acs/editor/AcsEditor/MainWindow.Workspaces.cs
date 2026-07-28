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

    private bool InitializeWorkspaceProfiles()
    {
        if (_workspaceStore != null)
            return true;
        // The startup worker already owns the only persisted catalogue read.
        // A fast menu/shortcut must not start a second synchronous read on the
        // Dispatcher while that worker is still probing redirected storage.
        if (ShouldDeferWorkspaceInitialization(
                _layoutRestorePending,
                _workspaceStore != null))
        {
            if (WorkspaceStateText != null)
            {
                WorkspaceStateText.Text = "WORKSPACE: LOADING";
                WorkspaceStateText.ToolTip =
                    "Saved workspace profiles are loading without blocking editor input.";
            }
            return false;
        }
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
        return true;
    }

    internal static bool ShouldDeferWorkspaceInitialization(
        bool startupRestorePending,
        bool storeAlreadyPublished) =>
        startupRestorePending && !storeAlreadyPublished;

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
        => BottomToolDockSelectionPolicy.ResolveActivePanelId(
            _activeBottomToolId,
            Array.Empty<string>());

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
            _ = SaveEditorLayoutAsync();
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
        WorkspaceMenu.Items.Clear();
        if (!InitializeWorkspaceProfiles())
        {
            WorkspaceMenu.Items.Add(new MenuItem
            {
                Header = "Loading saved workspaces…",
                IsEnabled = false,
            });
            return;
        }
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
        if (!InitializeWorkspaceProfiles())
            return;
        if (!WorkspaceStore.TryGetProfile(name, out EditorWorkspaceProfile profile))
        {
            Log($"Workspace no longer exists: {name}", "Editor", LogLevel.Warn);
            return;
        }
        ApplyWorkspace(profile);
    }

    private void OnManageWorkspaces(object sender, RoutedEventArgs e)
    {
        if (!InitializeWorkspaceProfiles())
            return;
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
        if (!InitializeWorkspaceProfiles())
            return;
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
