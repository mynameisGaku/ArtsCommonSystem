using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Workspace chrome and editor-session state. Kept separate from scene editing code so layout
/// controls never become coupled to the native renderer.
/// </summary>
public partial class MainWindow
{
    private readonly DispatcherTimer _sceneStateTimer = new() {
        Interval = TimeSpan.FromMilliseconds(750)
    };
    private string? _savedSceneSnapshot;
    private bool _sceneDirty;
    private bool _snapshotCaptureFailed;
    private double _hierarchyWidth = 260;
    private double _inspectorWidth = 348;
    private double _bottomDockHeight = 210;
    private float _snapMove = 1.0f;
    private float _snapRotate = 15.0f;
    private float _snapScale = 0.25f;
    private bool _closeApproved;
    private bool _closePreparationRunning;
    private bool _closePreparationInputBlocked;
    private bool _replacementAutosaveSuppressed;
    private bool _replacementSuppressedMode3D;

    /// <summary>Starts status tracking once the native scene is attached and fully loaded.</summary>
    private void InitializeWorkspaceState()
    {
        SynchronizeSnapSettingsFromProject();
        SnapCheck.IsChecked = EngineInterop.acs_editor_get_snap(Engine) != 0;
        SnapPresetBox.IsEnabled = SnapCheck.IsChecked == true;
        MarkSceneClean();
        UpdatePlayStatePresentation();

        _sceneStateTimer.Tick -= OnSceneStateTick;
        _sceneStateTimer.Tick += OnSceneStateTick;
        _sceneStateTimer.Start();
    }

    /// <summary>
    /// Mirrors the increments already applied by ProjectSettings into the managed toolbar state.
    /// This intentionally does not call acs_editor_set_snap: doing so here used to overwrite the
    /// loaded project values with the WPF field defaults.
    /// </summary>
    private void SynchronizeSnapSettingsFromProject()
    {
        if (Engine == IntPtr.Zero) return;
        _snapMove = ReadPositiveSnapSetting("SnapMove", _snapMove);
        _snapRotate = ReadPositiveSnapSetting("SnapRotateDeg", _snapRotate);
        _snapScale = ReadPositiveSnapSetting("SnapScale", _snapScale);
        SnapPresetBox.ToolTip =
            $"Current: move {_snapMove:0.###}, rotate {_snapRotate:0.###}°, scale {_snapScale:0.###}. " +
            "Selecting a preset overrides these increments.";
    }

    private float ReadPositiveSnapSetting(string key, float fallback)
    {
        var buffer = new byte[64];
        if (EngineInterop.acs_editor_settings_get_value(
                Engine, "Editor", key, buffer, buffer.Length) == 0)
            return fallback;

        string text = EngineInterop.Utf8Z(buffer);
        return float.TryParse(
                   text,
                   System.Globalization.NumberStyles.Float,
                   System.Globalization.CultureInfo.InvariantCulture,
                   out float value)
               && float.IsFinite(value)
               && value > 0.0f
            ? value
            : fallback;
    }

    private void OnSceneStateTick(object? sender, EventArgs e)
    {
        if (Engine == IntPtr.Zero || _savedSceneSnapshot == null) return;
        if (!_sceneMutationRevision.WorkspaceCaptureRequired) return;
        // Runtime simulation is intentionally non-destructive and is restored on Stop.
        if (EngineInterop.acs_editor_play_state(Engine) != 0 || PreviewBtn.IsChecked == true) return;
        RefreshDirtyStateFromNativeScene();
    }

    private string CaptureEditableSceneSnapshot()
    {
        if (Engine == IntPtr.Zero) return "";
        string scene;
        if (_view3d)
        {
            scene = EngineInterop.Scene3DText(Engine);
        }
        else
        {
            scene = EngineInterop.SceneText(Engine);
        }

        // Selection belongs to the editor session, not scene content. Native serializers include it
        // so undo can restore selection; excluding it prevents clicking an object from marking dirty.
        return string.Join('\n', scene.Split('\n')
            .Where(line => !line.StartsWith("SEL ", StringComparison.Ordinal)
                        && !line.StartsWith("SEL3D ", StringComparison.Ordinal)));
    }

    private bool TryCaptureEditableSceneSnapshot(out string snapshot)
    {
        snapshot = "";
        if (Engine == IntPtr.Zero)
            return false;

        try
        {
            snapshot = CaptureEditableSceneSnapshot();
            if (_snapshotCaptureFailed)
                Log("Scene state tracking recovered.");
            _snapshotCaptureFailed = false;
            return true;
        }
        catch (Exception ex)
        {
            // Never compare or mark clean from an incomplete serialization. Keep the scene dirty
            // so close/new/open continue to protect the user's in-memory work.
            if (!_snapshotCaptureFailed)
                Log("Scene state tracking failed; the scene will remain unsaved: " + ex.Message);
            _snapshotCaptureFailed = true;
            SetSceneDirty(true);
            return false;
        }
    }

    private bool RefreshDirtyStateFromNativeScene()
    {
        if (_savedSceneSnapshot == null)
            return false;
        if (TryCaptureEditableSceneSnapshot(out string snapshot))
        {
            SetSceneDirty(!string.Equals(snapshot, _savedSceneSnapshot, StringComparison.Ordinal));
            _sceneMutationRevision.AcknowledgeWorkspace();
            return true;
        }
        return false;
    }

    private void MarkSceneClean(string? serializedScene = null)
    {
        if (serializedScene == null)
        {
            if (!TryCaptureEditableSceneSnapshot(out string snapshot))
            {
                _savedSceneSnapshot = null;
                return;
            }
            _savedSceneSnapshot = snapshot;
        }
        else
        {
            _savedSceneSnapshot = NormalizeSceneSnapshot(serializedScene);
            _snapshotCaptureFailed = false;
        }
        SetSceneDirty(false);
        _sceneMutationRevision.AcknowledgeWorkspace();
        RememberActiveSceneTrackingState();
    }

    private static string NormalizeSceneSnapshot(string scene) =>
        string.Join('\n', scene.Split('\n')
            .Where(line => !line.StartsWith("SEL ", StringComparison.Ordinal)
                        && !line.StartsWith("SEL3D ", StringComparison.Ordinal)));

    private void MarkSceneDirty()
    {
        SetSceneDirty(true);
        NotifySceneMutationPending();
    }

    private void SetSceneDirty(bool dirty)
    {
        _sceneDirty = dirty;
        if (_view3d) _scene3DDirty = dirty;
        else _scene2DDirty = dirty;
        bool worldDirty = _scene2DDirty || _scene3DDirty;
        string baseTitle = _project == null ? "ACS Editor" : $"ACS Editor — {_project.Name}";
        Title = worldDirty ? baseTitle + " *" : baseTitle;
        SaveStateText.Text = worldDirty ? "UNSAVED" : "SAVED";
        SaveStateText.Foreground = (Brush)FindResource(worldDirty ? "WarnFg" : "LiveFg");
        UpdateSceneName();
    }

    private void UpdateSceneName()
    {
        string? path = SceneDocumentPresentationPath();
        SceneNameText.Text = string.IsNullOrWhiteSpace(path) ? "Untitled Scene" : Path.GetFileName(path);
        if (string.IsNullOrWhiteSpace(path))
        {
            SceneNameText.ToolTip = "Scene has not been saved";
            return;
        }

        string? activeCompatibilitySource = _currentScenePath;
        SceneNameText.ToolTip =
            string.IsNullOrWhiteSpace(activeCompatibilitySource) ||
            SceneSourceFile.PathsEqual(path, activeCompatibilitySource)
                ? path
                : $"{path}\nActive legacy compatibility source: {activeCompatibilitySource}";
    }

    private async void OnEditorClosing(object? sender, CancelEventArgs e)
    {
        if (_closeApproved) return;
        e.Cancel = true;
        if (_closePreparationRunning) return;
        _closePreparationRunning = true;
        bool shouldClose = false;
        try
        {
            shouldClose = await PrepareEditorCloseAsync();
        }
        catch (Exception ex)
        {
            Log("Close preparation failed: " + ex.Message, "Editor", LogLevel.Error);
        }
        finally
        {
            _closePreparationRunning = false;
            if (!shouldClose)
            {
                AssetBrowser.ResumeOperations();
                SetClosePreparationInputBlocked(blocked: false);
            }
        }

        if (!shouldClose) return;
        _closeApproved = true;
        _ = Dispatcher.BeginInvoke(new Action(Close));
    }

    private async Task<bool> PrepareEditorCloseAsync()
    {
        // Asset copy/move/delete jobs are transactional only while the process stays alive.
        // Stop accepting new jobs and drain the current one before any close path can proceed.
        SetClosePreparationInputBlocked(blocked: true);
        // Build/Run/Package own external compiler/cook processes; Package also owns a
        // staging transaction. The editor and its modeless package window must not
        // disappear until cancellation has killed/drained every child and continuation.
        if (!await DrainActiveBuildForEditorCloseAsync())
            return false;
        await AssetBrowser.SuspendOperationsAndWaitAsync();
        // Asset Browser's lifecycle ends after it publishes completion, while the completion
        // handler may still be draining an old scene-autosave generation. Wait for that managed
        // document reconciliation before dirty detection or Save All can observe a suspended
        // scene document or an in-flight old path.
        await WaitForAssetDocumentMutationsAsync();

        if (_savedSceneSnapshot != null && Engine != IntPtr.Zero
            && EngineInterop.acs_editor_play_state(Engine) == 0 && PreviewBtn.IsChecked != true)
            RefreshDirtyStateFromNativeScene();
        RememberActiveSceneDocumentState();
        IReadOnlyList<EditorDocument>? hostedDirtyDocuments = null;
        if (_documentHostInitialized &&
            !TryRefreshHostedDirtyDocuments(
                out hostedDirtyDocuments,
                out string documentRefreshError))
        {
            Log(
                "Close preparation could not verify document state: " +
                documentRefreshError,
                "Document",
                LogLevel.Error);
            return false;
        }

        bool hasDirtyDocuments = _documentHostInitialized
            ? hostedDirtyDocuments!.Count > 0
            : _scene2DDirty || _scene3DDirty;
        if (!hasDirtyDocuments)
        {
            SetClosePreparationInputBlocked(blocked: true);
            if (!await DrainStandaloneGameForEditorCloseAsync())
                return false;
            await StopAndDiscardSessionRecoveriesAsync();
            ApproveHostedMaterialWindowsForOwnerClose(
                discardUnsavedChanges: false);
            return true;
        }

        string dirtySummary = _documentHostInitialized
            ? string.Join(
                "\n",
                hostedDirtyDocuments!
                    .Take(6)
                    .Select(document => "  • " + document.DisplayName))
            : "  • Scene";
        if (_documentHostInitialized && hostedDirtyDocuments!.Count > 6)
            dirtySummary += $"\n  • …and {hostedDirtyDocuments.Count - 6} more";

        var result = MessageBox.Show(this,
            "There are unsaved documents:\n\n" + dirtySummary +
            "\n\nSave before closing?",
            "Unsaved Documents", MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
        if (result == MessageBoxResult.Cancel) {
            return false;
        }
        if (result != MessageBoxResult.Yes)
        {
            SetClosePreparationInputBlocked(blocked: true);
            if (_documentHostInitialized)
            {
                EditorDocumentCloseResult discardResult =
                    await PrepareHostedDocumentsForCloseAsync(
                        EditorDocumentCloseChoice.Discard);
                if (!discardResult.CanClose)
                    return false;
            }
            if (!await DrainStandaloneGameForEditorCloseAsync())
                return false;
            await StopAndDiscardSessionRecoveriesAsync();
            ApproveHostedMaterialWindowsForOwnerClose(
                discardUnsavedChanges: true);
            return true;
        }

        // Prevent edits in the await windows between source commit, worker drain, recovery discard,
        // and final close. Save dialogs remain interactive as separate owned windows.
        SetClosePreparationInputBlocked(blocked: true);
        if (_documentHostInitialized)
        {
            EditorDocumentCloseResult closeResult =
                await PrepareHostedDocumentsForCloseAsync(EditorDocumentCloseChoice.Save);
            if (!closeResult.CanClose)
            {
                if (closeResult.SaveResult != null)
                    ReportHostedSaveAllResult(
                        closeResult.SaveResult,
                        operation: "Close save");
                else
                    Log(closeResult.Detail, "Document", LogLevel.Error);
                return false;
            }
            if (!await DrainStandaloneGameForEditorCloseAsync())
                return false;
            await StopAndDiscardSessionRecoveriesAsync();
            ApproveHostedMaterialWindowsForOwnerClose(
                discardUnsavedChanges: false);
            return true;
        }

        SaveAllResult saveResult = await SaveAllInitializedSceneDocumentsAsync();
        if (saveResult.Completion != SaveAllCompletion.Success)
        {
            // Save All never switches viewport mode, so cancellation/failure also leaves the
            // active document, camera, path alias, and selection exactly where the user was.
            ReportSaveAllResult(saveResult);
            return false;
        }
        if (!await DrainStandaloneGameForEditorCloseAsync())
            return false;
        await StopAndDiscardSessionRecoveriesAsync();
        return true;
    }

    private void SetClosePreparationInputBlocked(bool blocked)
    {
        _closePreparationInputBlocked = blocked;
        UpdateEditorInputEnabled();
    }

    private bool IsSceneEditingBlocked => _sceneEditingBlock?.IsBlocked == true;

    private void UpdateEditorInputEnabled()
    {
        if (!Dispatcher.CheckAccess())
        {
            _ = Dispatcher.BeginInvoke(new Action(UpdateEditorInputEnabled));
            return;
        }

        // Close preparation owns the whole content tree so owned Save dialogs remain usable while
        // the editor itself is inert. Scene-path mutation uses a narrower block: the title bar and
        // diagnostics stay responsive, but every scene editing surface and command menu is inert.
        if (Content is UIElement content)
            content.IsEnabled = !_closePreparationInputBlocked;
        bool sceneInputEnabled =
            !_closePreparationInputBlocked &&
            !IsSceneEditingBlocked &&
            EngineCommandsReady;
        ViewportHost.IsHitTestVisible =
            SceneLoadCompletionGuard.ShouldEnableViewportInput(
                sceneInputEnabled,
                ViewportHost.Visibility == Visibility.Visible);
        if (!sceneInputEnabled && IsSceneEditingBlocked)
            _viewport?.CancelPointerInteraction();
        SceneWorkspace.IsEnabled = sceneInputEnabled;
        SceneDocumentActions.IsEnabled = sceneInputEnabled;
        ScenePlaybackActions.IsEnabled = sceneInputEnabled;
        PreviewBtn.IsEnabled = sceneInputEnabled;
        FileNewSceneMenu.IsEnabled = sceneInputEnabled;
        FileOpenSceneMenu.IsEnabled = sceneInputEnabled;
        FileSaveSceneMenu.IsEnabled = sceneInputEnabled;
        FileSaveAllScenesMenu.IsEnabled = sceneInputEnabled;
        SceneEditMenu.IsEnabled = sceneInputEnabled;
        GameObjectMenu.IsEnabled = sceneInputEnabled;
        bool buildInputEnabled = sceneInputEnabled && !_building;
        BuildBtn.IsEnabled = buildInputEnabled;
        RunBtn.IsEnabled = buildInputEnabled;
        MenuBuild.IsEnabled = buildInputEnabled;
        MenuRun.IsEnabled = buildInputEnabled;
        MenuBuildRun.IsEnabled = buildInputEnabled;
        MenuPackage.IsEnabled = buildInputEnabled;
        BpRunBtn.IsEnabled = sceneInputEnabled;
        HotReloadBtn.IsEnabled = sceneInputEnabled;
        CommandManager.InvalidateRequerySuggested();
    }

    /// <summary>Protects New/Open from silently discarding edits.</summary>
    private async Task<bool> ConfirmSceneReplacementAsync()
    {
        if (_savedSceneSnapshot != null && Engine != IntPtr.Zero
            && EngineInterop.acs_editor_play_state(Engine) == 0 && PreviewBtn.IsChecked != true)
        {
            if (!TryRefreshAllSceneDirtyStates(out string refreshError))
            {
                Log(
                    "Scene state could not be refreshed before replacement: " + refreshError,
                    "Scene",
                    LogLevel.Warn);
            }
        }
        RememberActiveSceneDocumentState();
        if (!_scene2DDirty && !_scene3DDirty) return true;

        var result = MessageBox.Show(this,
            "Save changes to the current scene before continuing?",
            "Unsaved Scene", MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
        if (result == MessageBoxResult.Cancel) return false;
        if (result == MessageBoxResult.No)
        {
            if (_autosaveStore != null && _project != null)
            {
                _replacementSuppressedMode3D = _view3d;
                await DiscardRecoveryAsync(
                    AutosaveIdentity(_view3d),
                    resumeAfter: false);
                _replacementAutosaveSuppressed = true;
            }
            return true;
        }
        return await SaveHostedSceneDocumentAsync();
    }

    private void CompleteSceneReplacementAutosave()
    {
        if (!_replacementAutosaveSuppressed) return;
        _replacementAutosaveSuppressed = false;
        ResumeAutosaveAfterReplacement(_replacementSuppressedMode3D);
    }

    private void OnClearHierarchySearch(object sender, RoutedEventArgs e)
    {
        HierSearchBox.Clear();
        HierSearchBox.Focus();
    }

    private void OnSnapPresetChanged(object sender, SelectionChangedEventArgs e)
    {
        switch (SnapPresetBox.SelectedIndex)
        {
            case 0: _snapMove = 0.1f; _snapRotate = 5.0f;  _snapScale = 0.05f; break;
            case 2: _snapMove = 10.0f; _snapRotate = 45.0f; _snapScale = 0.5f; break;
            default: _snapMove = 1.0f; _snapRotate = 15.0f; _snapScale = 0.25f; break;
        }
        ApplySnapSettings(logChange: IsLoaded);
    }

    private void ApplySnapSettings(bool logChange)
    {
        if (Engine == IntPtr.Zero) return;
        bool enabled = SnapCheck.IsChecked == true;
        EngineInterop.acs_editor_set_snap(Engine, enabled ? 1 : 0, _snapMove, _snapRotate, _snapScale);
        SnapPresetBox.IsEnabled = enabled;
        if (logChange)
            Log(enabled
                ? $"Snap: move {_snapMove:0.##}, rotate {_snapRotate:0.#}°, scale {_snapScale:0.##}"
                : "Snap disabled");
    }

    private void OnTogglePanelVisibility(object sender, RoutedEventArgs e)
    {
        if (sender is not MenuItem item || item.Tag is not string panel) return;
        switch (panel)
        {
            case "hierarchy": SetHierarchyVisible(item.IsChecked); break;
            case "inspector": SetInspectorVisible(item.IsChecked); break;
            case "bottom": SetBottomDockVisible(item.IsChecked); break;
        }
        MarkWorkspaceCustomized();
    }

    private void OnToggleBottomDock(object sender, RoutedEventArgs e)
    {
        SetBottomDockVisible(
            _bottomToolHost?.State == ToolPanelDockState.Hidden);
        MarkWorkspaceCustomized();
    }

    private void SetHierarchyVisible(bool visible)
    {
        if (_hierarchyToolHost?.IsFloating == true)
        {
            _ = _hierarchyToolHost.HandleVisibilityRequest(visible);
            return;
        }
        ApplyHierarchyDockVisibility(visible);
        UpdateToolPanelPresentation(
            ToolPanelDockingContract.HierarchyPanelId,
            visible
                ? ToolPanelDockState.Docked
                : ToolPanelDockState.Hidden);
        PersistDockedToolPanelState(
            ToolPanelDockingContract.HierarchyPanelId,
            visible);
    }

    private void ApplyHierarchyDockVisibility(bool visible)
    {
        if (!visible && HierarchyColumn.ActualWidth > 0) _hierarchyWidth = HierarchyColumn.ActualWidth;
        HierarchyColumn.Width = visible ? new GridLength(Math.Max(210, _hierarchyWidth)) : new GridLength(0);
        HierarchyPanel.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        HierarchySplitter.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
    }

    private void SetInspectorVisible(bool visible)
    {
        if (_inspectorToolHost?.IsFloating == true)
        {
            _ = _inspectorToolHost.HandleVisibilityRequest(visible);
            return;
        }
        ApplyInspectorDockVisibility(visible);
        UpdateToolPanelPresentation(
            ToolPanelDockingContract.InspectorPanelId,
            visible
                ? ToolPanelDockState.Docked
                : ToolPanelDockState.Hidden);
        PersistDockedToolPanelState(
            ToolPanelDockingContract.InspectorPanelId,
            visible);
    }

    private void ApplyInspectorDockVisibility(bool visible)
    {
        if (!visible && InspectorColumn.ActualWidth > 0) _inspectorWidth = InspectorColumn.ActualWidth;
        InspectorColumn.Width = visible ? new GridLength(Math.Max(280, _inspectorWidth)) : new GridLength(0);
        InspectorPanel.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        InspectorSplitter.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
    }

    private void SetBottomDockVisible(bool visible)
    {
        if (_bottomToolHost?.IsFloating == true)
        {
            _ = _bottomToolHost.HandleVisibilityRequest(visible);
            return;
        }
        ApplyBottomDockVisibility(visible);
        UpdateToolPanelPresentation(
            ToolPanelDockingContract.BottomPanelId,
            visible
                ? ToolPanelDockState.Docked
                : ToolPanelDockState.Hidden);
        PersistDockedToolPanelState(
            ToolPanelDockingContract.BottomPanelId,
            visible);
    }

    private void ApplyBottomDockVisibility(bool visible)
    {
        if (!visible && BottomDockRow.ActualHeight > 0) _bottomDockHeight = BottomDockRow.ActualHeight;
        BottomDockRow.Height = visible ? new GridLength(Math.Max(140, _bottomDockHeight)) : new GridLength(0);
        BottomDockPanel.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        BottomDockSplitter.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnResetEditorLayout(object sender, RoutedEventArgs e)
    {
        if (!ResetDockableToolPanels())
            return;
        DeleteSavedEditorLayout();
        _suppressToolPanelPersistence = true;
        try
        {
            ActivateWorkspaceByName(EditorWorkspaceStore.DefaultWorkspaceName);
        }
        finally
        {
            _suppressToolPanelPersistence = false;
        }
        Log("Editor layout reset.");
    }

    private void OnWorkspaceSplitterDragCompleted(
        object sender,
        System.Windows.Controls.Primitives.DragCompletedEventArgs e) =>
        MarkWorkspaceCustomized();

    private void UpdatePlayStatePresentation()
    {
        int state = Engine == IntPtr.Zero ? 0 : EngineInterop.acs_editor_play_state(Engine);
        PlayStatusText.Text = state switch { 1 => "PLAYING", 2 => "PAUSED", _ => "EDIT" };
        PlayStatusText.Foreground = (Brush)FindResource(state == 0 ? "TextDim" : "LiveFg");
        ApplySceneViewModePresentation();
    }
}
