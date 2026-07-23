// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Input;

namespace AcsEditor;

/// <summary>Keyboard-first access to editor actions (Ctrl+Shift+P).</summary>
public partial class MainWindow
{
    private void OnCommandPalette(object sender, RoutedEventArgs e) => ShowCommandPalette();

    private void ShowCommandPalette()
    {
        var palette = new EditorCommandPaletteWindow(CreatePaletteCommands())
        {
            Owner = this,
        };
        if (palette.ShowDialog() != true || palette.SelectedCommand is not { } command)
            return;

        // Run after the modal window has completely unwound so commands that open another dialog
        // receive correct ownership/focus and never become hidden behind the palette.
        Dispatcher.BeginInvoke(() =>
        {
            try
            {
                if (!command.IsAvailable)
                {
                    Log(
                        $"Command unavailable while scene input is blocked: {command.Label}",
                        "Editor",
                        LogLevel.Warn);
                    return;
                }
                command.Execute();
                Log($"Command: {command.Label}", "Editor", LogLevel.Info);
            }
            catch (Exception ex)
            {
                Log($"Command failed ({command.Label}): {ex.Message}", "Editor", LogLevel.Error);
            }
        });
    }

    private IReadOnlyList<EditorPaletteCommand> CreatePaletteCommands()
    {
        var commands = new List<EditorPaletteCommand>();

        void Add(
            string id,
            string label,
            string category,
            string description,
            string shortcut,
            Action execute,
            Func<bool>? canExecute = null,
            params string[] keywords) =>
            commands.Add(new EditorPaletteCommand(
                id, label, category, description, shortcut, execute, keywords, canExecute));

        bool EngineReady() => Engine != IntPtr.Zero;
        bool HasProject() => EngineReady() && _project != null;
        bool SceneReady() => EngineReady() && !IsSceneEditingBlocked;
        bool SceneProjectReady() => SceneReady() && _project != null;

        Add("file.new-scene", "New Scene", "File",
            "Create an empty scene using the currently loaded source format.", "Ctrl+N",
            () => OnNewScene(this, new RoutedEventArgs()), SceneReady, "level document");
        Add("file.open-scene", "Open Scene…", "File",
            "Open an .acscene or .acs3d source in the scene editor.", "",
            () => OnOpenScene(this, new RoutedEventArgs()), SceneReady, "level load");
        Add("file.save-scene", "Save Scene", "File",
            "Save the active scene document.", "Ctrl+S",
            () => OnSaveScene(this, new RoutedEventArgs()), SceneReady, "level write");
        Add("file.save-all-scenes", "Save All", "File",
            "Save every dirty compatibility source for the current scene.", "Ctrl+Shift+S",
            () => OnSaveAllScenes(this, new RoutedEventArgs()), SceneReady,
            "level write documents all");
        Add("file.open-project-folder", "Open Project Folder", "File",
            "Reveal the current project root in Explorer.", "",
            () => OpenFolder(_project!.RootDir), HasProject, "explorer directory root");
        Add("file.open-assets-folder", "Open Assets Folder", "File",
            "Reveal the project Assets folder in Explorer.", "",
            () => OpenFolder(_project!.AssetsDir), HasProject, "content browser explorer");

        Add("edit.undo", "Undo", "Edit",
            "Undo the latest scene transaction.", "Ctrl+Z",
            () => ApplicationCommands.Undo.Execute(null, this),
            () => ApplicationCommands.Undo.CanExecute(null, this), "history revert");
        Add("edit.redo", "Redo", "Edit",
            "Redo the latest reverted scene transaction.", "Ctrl+Y",
            () => ApplicationCommands.Redo.Execute(null, this),
            () => ApplicationCommands.Redo.CanExecute(null, this), "history repeat");
        Add("edit.duplicate", "Duplicate Selection", "Edit",
            "Duplicate the selected scene object or subtree.", "Ctrl+D",
            () => OnDuplicateNode(this, new RoutedEventArgs()), SceneReady, "clone object");
        Add("edit.delete", "Delete Selection", "Edit",
            "Delete the selected scene object.", "Delete",
            () => ApplicationCommands.Delete.Execute(null, this),
            () => ApplicationCommands.Delete.CanExecute(null, this), "remove object");
        Add("edit.focus-selection", "Focus Selection", "Viewport",
            "Frame the selected object in the scene viewport.", "F",
            () => OnFocus(this, new RoutedEventArgs()), SceneReady, "frame camera");

        Add("scene.view-perspective", "Scene View: Perspective", "Scene",
            "Use a perspective camera without changing scene content or history.", "",
            () => SwitchSceneViewMode(EditorSceneViewMode.Perspective),
            () => SceneReady() &&
                  EditorSceneViewModePolicy.IsSupportedByLegacySource(
                      EditorSceneViewMode.Perspective,
                      _legacySceneSourceMode),
            "viewport projection 3d");
        Add("scene.view-2d", "Scene View: 2D (Orthographic)", "Scene",
            "Use the XY-front orthographic pan-navigation preset on the current scene.", "",
            () => SwitchSceneViewMode(EditorSceneViewMode.TwoD), SceneReady,
            "viewport xy sprite");
        Add("scene.create-empty", "Create Empty Object", "Scene",
            "Create an empty transform at the scene root.", "",
            () => OnCreateEmpty(this, new RoutedEventArgs()), SceneReady, "gameobject actor node");
        Add("scene.create-child", "Create Child of Selection", "Scene",
            "Create an empty transform beneath the selected object.", "",
            () => OnCreateChild(this, new RoutedEventArgs()), SceneReady, "gameobject actor node");
        Add("scene.add-cube", "Add 3D Cube", "Scene",
            "Create a cube mesh in the current scene (requires an .acs3d source).", "",
            () => OnAdd3DCube(this, new RoutedEventArgs()), SceneReady, "primitive mesh");
        Add("scene.add-sphere", "Add 3D Sphere", "Scene",
            "Create a sphere mesh in the current scene (requires an .acs3d source).", "",
            () => OnAdd3DSphere(this, new RoutedEventArgs()), SceneReady, "primitive mesh");
        Add("scene.add-plane", "Add 3D Plane", "Scene",
            "Create a plane mesh in the current scene (requires an .acs3d source).", "",
            () => OnAdd3DPlane(this, new RoutedEventArgs()), SceneReady, "primitive mesh");

        Add("play.toggle", "Play / Stop", "Play",
            "Start or stop Play In Editor and restore edit state on stop.", "",
            () => OnPlay(this, new RoutedEventArgs()), SceneReady, "pie simulate game");
        Add("play.pause", "Pause / Resume", "Play",
            "Pause or resume the running editor simulation.", "",
            () => OnPause(this, new RoutedEventArgs()),
            () => SceneReady() && EngineInterop.acs_editor_play_state(Engine) != 0,
            "simulation");
        Add("play.step", "Step One Frame", "Play",
            "Advance a paused simulation by one frame.", "",
            () => OnStep(this, new RoutedEventArgs()),
            () => SceneReady() && EngineInterop.acs_editor_play_state(Engine) == 2,
            "simulation frame");
        Add("play.game-view", "Show Game View", "Play",
            "Switch the center viewport to the game output.", "",
            () => SetGameView(true), SceneReady, "viewport runtime");
        Add("play.scene-view", "Show Scene View", "Play",
            "Switch the center viewport back to editing.", "",
            () => SetGameView(false), SceneReady, "viewport editor");

        Add("build.build", "Build Project", "Build",
            "Configure and compile the current project.", "F7",
            () => OnBuildProject(this, new RoutedEventArgs()), SceneProjectReady, "compile cmake");
        Add("build.run", "Run Standalone", "Build",
            "Launch the most recent standalone project build.", "Ctrl+F5",
            () => OnRunProject(this, new RoutedEventArgs()), SceneProjectReady, "launch game");
        Add("build.build-run", "Build & Run", "Build",
            "Build the project and launch it when compilation succeeds.", "F5",
            () => OnBuildAndRun(this, new RoutedEventArgs()), SceneProjectReady, "compile launch game");
        Add("build.package", "Package Project…", "Build",
            "Cook, stage, verify and package the project for distribution.", "",
            () => OnPackageProject(this, new RoutedEventArgs()), SceneProjectReady,
            "shipping zip distribute");
        Add("build.results", "Show Build Results", "Build",
            "Open the bottom dock on the Build tab.", "",
            () => ShowBottomTab("build"), EngineReady, "errors diagnostics output");

        Add("tools.blueprints", "Open Blueprint Editor", "Tools",
            "Open the visual scripting and component graph editor.", "",
            () => OnBlueprintTab(this, new RoutedEventArgs()), EngineReady, "visual script graph");
        Add("tools.project-settings", "Project Settings…", "Tools",
            "Configure rendering, gameplay, physics and editor project settings.", "",
            () => OnProjectSettings(this, new RoutedEventArgs()), EngineReady, "preferences config");

        Add("view.assets", "Show Asset Browser", "View",
            "Open the bottom dock on the Assets tab.", "",
            () => ShowBottomTab("assets"), EngineReady, "content browser files");
        Add("view.console", "Show Output Log", "View",
            "Open the bottom dock on the Console tab.", "Ctrl+J",
            () => ShowBottomTab("console"), EngineReady, "log output");
        Add("view.toggle-hierarchy", "Toggle Hierarchy", "View",
            "Show or hide the scene hierarchy panel.", "",
            () => SetHierarchyVisible(HierarchyPanel.Visibility != Visibility.Visible), EngineReady, "outliner");
        Add("view.toggle-inspector", "Toggle Inspector", "View",
            "Show or hide the details and components panel.", "",
            () => SetInspectorVisible(InspectorPanel.Visibility != Visibility.Visible), EngineReady, "details components");
        Add("view.toggle-bottom", "Toggle Bottom Dock", "View",
            "Show or hide Console, Build Results and Assets.", "Ctrl+J",
            () => SetBottomDockVisible(BottomDockPanel.Visibility != Visibility.Visible), EngineReady, "panel");
        Add("view.toggle-grid", "Toggle Viewport Grid", "View",
            "Show or hide the 3D reference grid.", "",
            () =>
            {
                ShowGridItem.IsChecked = !ShowGridItem.IsChecked;
                OnToggleGrid(ShowGridItem, new RoutedEventArgs());
            }, EngineReady, "scene");
        Add("view.reset-layout", "Reset Editor Layout", "View",
            "Restore the default panel sizes and visibility.", "",
            () => OnResetEditorLayout(this, new RoutedEventArgs()), EngineReady, "workspace panels");
        Add("view.manage-workspaces", "Manage Workspaces…", "View",
            "Capture, duplicate, rename, delete or activate named editor layouts.",
            "Ctrl+Alt+W",
            () => OnManageWorkspaces(this, new RoutedEventArgs()),
            null,
            "layout profile panes");

        InitializeWorkspaceProfiles();
        int workspaceIndex = 0;
        foreach (EditorWorkspaceProfile profile in WorkspaceStore.GetProfiles())
        {
            string workspaceName = profile.Name;
            Add(
                $"view.workspace.{workspaceIndex++}",
                $"Activate Workspace: {workspaceName}",
                "View",
                "Apply panel visibility and sizes without changing the scene, selection or camera.",
                "",
                () => ActivateWorkspaceByName(workspaceName),
                null,
                "layout profile panes");
        }

        return commands;
    }

    private static void OpenFolder(string path)
    {
        string fullPath = Path.GetFullPath(path);
        if (!Directory.Exists(fullPath))
            throw new DirectoryNotFoundException(fullPath);
        var startInfo = new ProcessStartInfo("explorer.exe")
        {
            UseShellExecute = true,
        };
        startInfo.ArgumentList.Add(fullPath);
        Process.Start(startInfo);
    }
}
