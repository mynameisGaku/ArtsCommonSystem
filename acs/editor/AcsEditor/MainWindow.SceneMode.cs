// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;

namespace AcsEditor;

/// <summary>
/// Selects exactly one scene-load presentation outcome. Keeping this branch executable outside
/// WPF lets the regression suite prove that a restored/current scene cannot share the
/// unavailable presentation path.
/// </summary>
internal static class SceneLoadPublicationBranch
{
    internal static void Run(
        bool publishScene,
        Action publishCurrentScene,
        Action publishUnavailable)
    {
        ArgumentNullException.ThrowIfNull(publishCurrentScene);
        ArgumentNullException.ThrowIfNull(publishUnavailable);
        if (publishScene)
        {
            publishCurrentScene();
            return;
        }
        publishUnavailable();
    }
}

/// <summary>
/// One editor scene document with Perspective and 2D (XY Orthographic) view presets. The current
/// native editor still has separate legacy 2D and 3D serializers, so this class keeps both as
/// reversible compatibility adapters behind the single managed document identity.
/// </summary>
public partial class MainWindow
{
    // The unpublished bootstrap state follows the canonical editor model: one 3D world viewed
    // in Perspective. Project/source loading may then choose Orthographic or a legacy adapter.
    private EditorSceneViewMode _sceneViewMode = EditorSceneViewMode.Perspective;
    private SceneDocumentMode _legacySceneSourceMode = SceneDocumentMode.ThreeD;
    private string? _scene2DPath;
    private string? _scene3DDocumentPath;
    private bool _scene2DInitialized;
    private bool _scene3DInitialized;

    private string? _scene2DSavedSnapshot;
    private string? _scene3DSavedSnapshot;
    private bool _scene2DDirty;
    private bool _scene3DDirty;
    private readonly SceneLoadGenerationState _sceneLoadGeneration = new();
    private CancellationTokenSource? _sceneLoadCancellation;

    private sealed class ActiveSceneLoad : IDisposable
    {
        private IDisposable? _inputLease;

        internal ActiveSceneLoad(
            SceneLoadTicket ticket,
            CancellationTokenSource cancellation,
            IDisposable inputLease)
        {
            Ticket = ticket;
            Cancellation = cancellation;
            _inputLease = inputLease;
        }

        internal SceneLoadTicket Ticket { get; }
        internal CancellationTokenSource Cancellation { get; }

        public void Dispose()
        {
            IDisposable? inputLease =
                Interlocked.Exchange(ref _inputLease, null);
            try
            {
                Cancellation.Dispose();
            }
            finally
            {
                // Input availability is more important than CTS cleanup. Even a disposal
                // failure must not leave the live editor looking healthy while every scene
                // command remains disabled.
                inputLease?.Dispose();
            }
        }
    }

    /// <summary>
    /// File/Open spans a native parser plus managed document metadata. Preserve both compatibility
    /// payloads and every managed durability/history alias so a post-parse exception cannot publish
    /// a mixed old/new world.
    /// </summary>
    private sealed record SceneOpenRollbackSnapshot(
        string Legacy2D,
        string World3D,
        SceneDocumentMode LegacySourceMode,
        EditorSceneViewMode SceneViewMode,
        bool View3D,
        string? CurrentScenePath,
        string? Scene2DPath,
        string? Scene3DPath,
        bool Scene2DInitialized,
        bool Scene3DInitialized,
        string? Scene2DSavedSnapshot,
        string? Scene3DSavedSnapshot,
        bool Scene2DDirty,
        bool Scene3DDirty,
        string? SavedSceneSnapshot,
        bool SceneDirty,
        bool SnapshotCaptureFailed,
        string? HostSavedSubsystem2D,
        string? HostSavedSubsystem3D,
        bool HistorySimulationSuspended,
        string PendingHistoryLabel,
        string? PendingHistoryMergeKey,
        bool MergeSelectionInitialized,
        bool MergeSelection3D,
        int MergeSelectionNodeId,
        int MergeSelectionCount,
        ulong MergeSelectionEpoch,
        EditorDocument? Document,
        EditorDocument.Checkpoint? DocumentCheckpoint,
        SceneMutationRevisionGate.Checkpoint RevisionCheckpoint);

    private SceneOpenRollbackSnapshot CaptureSceneOpenRollbackSnapshot()
    {
        if (Engine == IntPtr.Zero)
            throw new InvalidOperationException("The editor engine is unavailable.");
        EditorDocument? document = _documentHostInitialized
            ? _documentHost.ActiveDocument
            : null;
        return new SceneOpenRollbackSnapshot(
            EngineInterop.SceneText(Engine),
            EngineInterop.Scene3DText(Engine),
            _legacySceneSourceMode,
            _sceneViewMode,
            _view3d,
            _currentScenePath,
            _scene2DPath,
            _scene3DDocumentPath,
            _scene2DInitialized,
            _scene3DInitialized,
            _scene2DSavedSnapshot,
            _scene3DSavedSnapshot,
            _scene2DDirty,
            _scene3DDirty,
            _savedSceneSnapshot,
            _sceneDirty,
            _snapshotCaptureFailed,
            _hostSavedSubsystem2D,
            _hostSavedSubsystem3D,
            _sceneHistorySimulationSuspended,
            _pendingSceneHistoryLabel,
            _pendingSceneHistoryMergeKey,
            _sceneMergeSelectionInitialized,
            _sceneMergeSelection3D,
            _sceneMergeSelectionNodeId,
            _sceneMergeSelectionCount,
            _sceneMergeSelectionEpoch,
            document,
            document?.CaptureCheckpoint(),
            _sceneMutationRevision.CaptureCheckpoint());
    }

    private bool RestoreSceneOpenRollbackSnapshot(
        SceneOpenRollbackSnapshot snapshot)
    {
        if (Engine == IntPtr.Zero) return false;
        int restored = EngineInterop.acs_editor_scene_document_load_text(
            Engine,
            snapshot.Legacy2D,
            snapshot.World3D);
        if (restored == 0) return false;

        _legacySceneSourceMode = snapshot.LegacySourceMode;
        _sceneViewMode = snapshot.SceneViewMode;
        _view3d = snapshot.View3D;
        _currentScenePath = snapshot.CurrentScenePath;
        _scene2DPath = snapshot.Scene2DPath;
        _scene3DDocumentPath = snapshot.Scene3DPath;
        _scene2DInitialized = snapshot.Scene2DInitialized;
        _scene3DInitialized = snapshot.Scene3DInitialized;
        _scene2DSavedSnapshot = snapshot.Scene2DSavedSnapshot;
        _scene3DSavedSnapshot = snapshot.Scene3DSavedSnapshot;
        _scene2DDirty = snapshot.Scene2DDirty;
        _scene3DDirty = snapshot.Scene3DDirty;
        SetActiveSavedSceneSnapshot(snapshot.SavedSceneSnapshot);
        _sceneDirty = snapshot.SceneDirty;
        _snapshotCaptureFailed = snapshot.SnapshotCaptureFailed;
        _hostSavedSubsystem2D = snapshot.HostSavedSubsystem2D;
        _hostSavedSubsystem3D = snapshot.HostSavedSubsystem3D;
        _pendingSceneHistoryLabel = snapshot.PendingHistoryLabel;
        _pendingSceneHistoryMergeKey = snapshot.PendingHistoryMergeKey;
        _sceneMergeSelectionInitialized = snapshot.MergeSelectionInitialized;
        _sceneMergeSelection3D = snapshot.MergeSelection3D;
        _sceneMergeSelectionNodeId = snapshot.MergeSelectionNodeId;
        _sceneMergeSelectionCount = snapshot.MergeSelectionCount;
        _sceneMergeSelectionEpoch = snapshot.MergeSelectionEpoch;
        _sceneMutationRevision.RestoreCheckpoint(snapshot.RevisionCheckpoint);

        // UI refresh must not generate a transaction while rebuilding the restored scene.
        _sceneHistorySimulationSuspended = true;
        try
        {
            EngineInterop.acs_editor_set_view3d(
                Engine,
                snapshot.View3D ? 1 : 0);
            EditorSceneViewDescriptor restoredView =
                EditorSceneViewModePolicy.Describe(snapshot.SceneViewMode);
            EngineInterop.acs_editor_set_ortho3d(
                Engine,
                snapshot.View3D && restoredView.IsOrthographic ? 1 : 0);
            ApplySceneViewModePresentation();
            UpdateSceneName();
            BuildHierarchy();
            SyncSelectionUi();
            SetSceneDirty(snapshot.SceneDirty);
        }
        catch
        {
            // The native world and managed durability aliases are already restored. Presentation
            // will be rebuilt by the next ordinary editor refresh.
        }
        finally
        {
            if (snapshot.Document != null &&
                snapshot.DocumentCheckpoint != null)
            {
                try
                {
                    snapshot.Document.RestoreCheckpoint(
                        snapshot.DocumentCheckpoint);
                }
                catch
                {
                    // Checkpoint fields are installed before the notification callback runs.
                }
            }
            _sceneMutationRevision.RestoreCheckpoint(snapshot.RevisionCheckpoint);
            _sceneHistorySimulationSuspended =
                snapshot.HistorySimulationSuspended;
        }
        return true;
    }

    private static SceneDocumentMode LegacySceneModeFromPath(string path)
    {
        string extension = Path.GetExtension(path);
        if (string.Equals(extension, ".acscene", StringComparison.OrdinalIgnoreCase))
            return SceneDocumentMode.TwoD;
        if (string.Equals(extension, ".acs3d", StringComparison.OrdinalIgnoreCase))
            return SceneDocumentMode.ThreeD;
        throw new InvalidDataException(
            "Scene sources must use the .acscene or .acs3d extension.");
    }

    private string ValidateSceneDocumentPath(string path, bool use3D)
    {
        SceneDocumentMode mode = use3D
            ? SceneDocumentMode.ThreeD
            : SceneDocumentMode.TwoD;
        return _project == null
            ? SceneSourceFile.ValidateScenePath(path, mode)
            : SceneSourceFile.ValidateProjectScenePathForProject(
                path,
                _project.RootDir,
                _project.AssetsDir,
                mode);
    }

    private string ValidateLegacySceneDocumentPath(
        string path,
        out SceneDocumentMode sourceMode)
    {
        sourceMode = LegacySceneModeFromPath(path);
        return ValidateSceneDocumentPath(
            path,
            sourceMode == SceneDocumentMode.ThreeD);
    }

    /// <summary>
    /// Stable presentation path for the singular scene document. Switching a view preset must not
    /// make the tab look like a different file was activated.
    /// </summary>
    private string? SceneDocumentPresentationPath()
    {
        return _currentScenePath ?? _scene2DPath ?? _scene3DDocumentPath;
    }

    /// <summary>Updates the active path and its mode-specific slot together.</summary>
    private void SetCurrentScenePath(string? path)
    {
        _currentScenePath = path;
        if (_view3d)
            _scene3DDocumentPath = path;
        else
            _scene2DPath = path;
        UpdateSceneName();
    }

    /// <summary>Copies the active aliases into the mode-specific session slot before switching.</summary>
    private void RememberActiveSceneDocumentState()
    {
        if (_view3d)
        {
            _scene3DDocumentPath = _currentScenePath;
            _scene3DSavedSnapshot = _savedSceneSnapshot;
            _scene3DDirty = _sceneDirty;
        }
        else
        {
            _scene2DPath = _currentScenePath;
            _scene2DSavedSnapshot = _savedSceneSnapshot;
            _scene2DDirty = _sceneDirty;
        }
    }

    /// <summary>Restores the active aliases from the selected mode's session slot.</summary>
    private void RestoreActiveSceneDocumentState()
    {
        if (_view3d)
        {
            _currentScenePath = _scene3DDocumentPath;
            SetActiveSavedSceneSnapshot(_scene3DSavedSnapshot);
            _sceneDirty = _scene3DDirty;
        }
        else
        {
            _currentScenePath = _scene2DPath;
            SetActiveSavedSceneSnapshot(_scene2DSavedSnapshot);
            _sceneDirty = _scene2DDirty;
        }
        SetSceneDirty(_sceneDirty);
    }

    /// <summary>Persists the active snapshot/dirty aliases after MarkSceneClean or a dirty update.</summary>
    private void RememberActiveSceneTrackingState()
    {
        if (_view3d)
        {
            _scene3DSavedSnapshot = _savedSceneSnapshot;
            _scene3DDirty = _sceneDirty;
        }
        else
        {
            _scene2DSavedSnapshot = _savedSceneSnapshot;
            _scene2DDirty = _sceneDirty;
        }
    }

    private string ResolveConfiguredProjectScenePath()
    {
        if (_project == null) return "";

        string scenePath = _project.InitialScenePath;
        var buffer = new byte[256];
        if (EngineInterop.acs_editor_settings_get_value(
                Engine, "Game", "DefaultScene", buffer, buffer.Length) != 0)
        {
            string configured = EngineInterop.Utf8Z(buffer).Trim();
            if (configured.Length > 0)
            {
                try
                {
                    scenePath = SceneSourceFile.ResolveProjectSceneReference(
                        _project.RootDir,
                        _project.AssetsDir,
                        configured);
                }
                catch (Exception ex)
                {
                    Log(
                        "Game.DefaultScene was rejected; using the validated project " +
                        $"InitialScene instead: {ex.Message}");
                }
            }
        }
        return scenePath;
    }

    private ActiveSceneLoad BeginSceneLoad(string detail)
    {
        Dispatcher.VerifyAccess();
        _sceneLoadCancellation?.Cancel();

        var cancellation = new CancellationTokenSource();
        _sceneLoadCancellation = cancellation;
        SceneLoadTicket ticket = _sceneLoadGeneration.Begin();
        IDisposable inputLease = _sceneEditingBlock.Enter();

        IntPtr engine = RawEngine;
        if (engine != IntPtr.Zero)
            EngineInterop.acs_editor_set_scene_presentation_suppressed(engine, 1);

        ViewportLoadingTitle.Text = "Loading scene…";
        ViewportLoadingDetail.Text = detail;
        SceneModeText.Text = "VIEW: LOADING";
        SceneModeText.ToolTip =
            "No scene view is published while the replacement document is loading.";
        ViewportLoadingOverlay.Visibility = Visibility.Visible;
        ViewportHost.IsHitTestVisible = false;
        // Renderer warm-up is already complete whenever a scene load begins. Hiding the
        // HwndHost makes the WPF loading surface visible without stalling warm-up.
        ViewportHost.Visibility = Visibility.Hidden;
        return new ActiveSceneLoad(ticket, cancellation, inputLease);
    }

    private bool IsCurrentSceneLoad(ActiveSceneLoad load) =>
        _sceneLoadGeneration.IsCurrent(load.Ticket) &&
        ReferenceEquals(_sceneLoadCancellation, load.Cancellation);

    private void CompleteSceneLoad(
        ActiveSceneLoad load,
        bool publishScene,
        Action<bool>? completion = null)
    {
        Dispatcher.VerifyAccess();
        bool publishedCurrentScene = false;
        SceneLoadCompletionGuard.Run(
            () =>
            {
                bool current =
                    _sceneLoadGeneration.TryComplete(load.Ticket) &&
                    ReferenceEquals(
                        _sceneLoadCancellation,
                        load.Cancellation);
                if (!current) return;

                _sceneLoadCancellation = null;
                SceneLoadPublicationBranch.Run(
                    publishScene,
                    () =>
                    {
                        // This path also publishes the pre-existing scene after a manual Open
                        // fails before native loading begins. Restore its descriptor before the
                        // HWND is revealed so VIEW: LOADING can never survive publication.
                        ApplySceneViewModePresentation();
                        IntPtr engine = RawEngine;
                        if (engine != IntPtr.Zero)
                        {
                            EngineInterop.acs_editor_set_scene_presentation_suppressed(engine, 0);
                        }
                        ViewportHost.Visibility = Visibility.Visible;
                        ViewportLoadingOverlay.Visibility = Visibility.Collapsed;
                        publishedCurrentScene = true;
                        _ = Dispatcher.BeginInvoke(
                            DispatcherPriority.Loaded,
                            new Action(
                                () => _viewport?.ResumeRenderingAfterSceneLoad()));
                    },
                    () =>
                    {
                        SceneModeText.Text = "VIEW: UNAVAILABLE";
                        SceneModeText.ToolTip =
                            "No scene view was published by the failed or cancelled load.";
                        ViewportLoadingTitle.Text = "Scene loading stopped";
                        ViewportLoadingDetail.Text =
                            "The viewport remains blank because this load did not publish a scene.";
                    });
            },
            load,
            UpdateEditorInputEnabled);
        completion?.Invoke(publishedCurrentScene);
    }

    private void InvalidateSceneLoad(string detail)
    {
        Dispatcher.VerifyAccess();
        _sceneLoadCancellation?.Cancel();
        _sceneLoadCancellation = null;
        _sceneLoadGeneration.Invalidate();
        IntPtr engine = RawEngine;
        if (engine != IntPtr.Zero)
            EngineInterop.acs_editor_set_scene_presentation_suppressed(engine, 1);
        ViewportLoadingOverlay.Visibility = Visibility.Visible;
        SceneModeText.Text = "VIEW: UNAVAILABLE";
        SceneModeText.ToolTip =
            "No scene view is published while the viewport is unavailable.";
        ViewportLoadingTitle.Text = "Viewport unavailable";
        ViewportLoadingDetail.Text = detail;
        ViewportHost.IsHitTestVisible = false;
        ViewportHost.Visibility = Visibility.Hidden;
    }

    private static bool LoadLegacySceneSourceAsDocument(
        IntPtr engine,
        bool sourceUses3D,
        string sourceText)
    {
        string scene2D = sourceUses3D
            ? EngineInterop.EmptyScene2DText
            : sourceText;
        string scene3D = sourceUses3D
            ? sourceText
            : EngineInterop.EmptyScene3DText;
        return EngineInterop.acs_editor_scene_document_load_text(
            engine, scene2D, scene3D) != 0;
    }

    private void ConfigureSceneDocumentAdapter(
        bool use3D,
        string? sourcePath,
        bool keepSourcePath,
        EditorSceneViewMode? initialView = null)
    {
        IntPtr engine = Engine;
        if (engine == IntPtr.Zero)
            throw new InvalidOperationException(
                "The editor engine was lost while configuring the scene document.");
        EngineInterop.acs_editor_set_view3d(engine, use3D ? 1 : 0);
        _sceneViewMode =
            initialView ??
            EditorSceneViewModePolicy.InitialForLegacySource(sourcePath);
        EditorSceneViewDescriptor view =
            EditorSceneViewModePolicy.Describe(_sceneViewMode);
        EngineInterop.acs_editor_set_ortho3d(
            engine,
            use3D && view.IsOrthographic ? 1 : 0);

        _scene2DPath = !use3D && keepSourcePath ? sourcePath : null;
        _scene3DDocumentPath = use3D && keepSourcePath ? sourcePath : null;
        _scene2DInitialized = !use3D;
        _scene3DInitialized = use3D;
        _scene2DDirty = false;
        _scene3DDirty = false;
        SetCurrentScenePath(keepSourcePath ? sourcePath : null);
    }

    private void EstablishEmptySceneDocument(
        bool use3D,
        string? sourcePath,
        bool keepSourcePath,
        EditorSceneViewMode? initialView = null)
    {
        IntPtr engine = Engine;
        if (engine == IntPtr.Zero)
            throw new InvalidOperationException(
                "The editor engine was lost while establishing an empty scene.");

        // Both compatibility payloads are cleared in one native retirement
        // transaction so no retired compatibility payload can become a fallback.
        EngineInterop.acs_editor_scene_document_new(engine);
        ConfigureSceneDocumentAdapter(
            use3D, sourcePath, keepSourcePath, initialView);
    }

    /// <summary>
    /// Loads the configured initial document asynchronously with a generation check. Existing
    /// Assets/scene3d.acs3d is deliberately not preferred over an explicit 2D initial scene.
    /// </summary>
    private async Task<bool> InitializeProjectSceneDocument(int startupGeneration)
    {
        if (RawEngine == IntPtr.Zero) return false;

        string? scenePath = null;
        if (_project != null)
        {
            RunWithStartupEngineAccess(
                () => scenePath = ResolveConfiguredProjectScenePath());
        }

        EditorSceneStartupPlan startupPlan =
            EditorSceneStartupPolicy.Resolve(
                scenePath,
                _project?.Template);
        bool initialIs3D = startupPlan.Uses3D;
        EditorSceneViewMode initialProjectView = startupPlan.ViewMode;
        string initialSourceFormat = startupPlan.SourceExtension;
        ActiveSceneLoad load = BeginSceneLoad(
            scenePath == null
                ? "Preparing an empty scene"
                : $"Reading {Path.GetFileName(scenePath)}");
        bool publishScene = false;
        try
        {
            SceneSourceFile.ReadResult source =
                scenePath == null
                    ? new SceneSourceFile.ReadResult(false, null)
                    : await SceneSourceFile.ReadBoundedTextAsync(
                        scenePath,
                        load.Cancellation.Token);
            bool exists = source.Exists;
            string? text = source.Text;

            if (!IsCurrentSceneLoad(load) ||
                startupGeneration != _engineStartupGeneration ||
                _engineStartupState != EditorEngineStartupState.FinalizingEditor)
            {
                return false;
            }

            bool loaded = false;
            RunWithStartupEngineAccess(() =>
            {
                _legacySceneSourceMode = startupPlan.SourceMode;
                _view3d = initialIs3D;
                if (exists)
                {
                    // The combined loader validates both payloads before
                    // retiring the old world. Do not precede a valid startup
                    // load with document_new: one source load owns one GPU
                    // retirement transaction.
                    ConfigureSceneDocumentAdapter(
                        initialIs3D,
                        scenePath,
                        keepSourcePath: false,
                        initialView: initialProjectView);
                }
                else
                {
                    EstablishEmptySceneDocument(
                        initialIs3D,
                        scenePath,
                        keepSourcePath: true,
                        initialView: initialProjectView);
                }
                if (exists)
                {
                    loaded = LoadLegacySceneSourceAsDocument(
                        Engine, initialIs3D, text!);
                    if (!loaded)
                    {
                        // Native parsers can mutate before reporting failure. Re-clear both
                        // graphs and detach the rejected source rather than rolling back.
                        EstablishEmptySceneDocument(
                            initialIs3D,
                            sourcePath: null,
                            keepSourcePath: false,
                            initialView: initialProjectView);
                    }
                    else
                    {
                        _scene2DPath = initialIs3D ? null : scenePath;
                        _scene3DDocumentPath = initialIs3D ? scenePath : null;
                        SetCurrentScenePath(scenePath);
                    }
                }
                RestoreActiveSceneDocumentState();
                EngineInterop.acs_editor_camera_frame_all(Engine);
                ApplySceneViewModePresentation();
            });

            if (_project != null)
                Log($"Project: {_project.Name}  ({_project.RootDir})");
            if (scenePath == null)
            {
                Log("No project scene was configured — started with an empty scene.");
            }
            else if (!exists)
            {
                Log(
                    $"No initial scene source ({initialSourceFormat}) file — started empty.");
            }
            else if (loaded)
            {
                Log(
                    $"Loaded initial scene source ({initialSourceFormat}) ← {scenePath}");
            }
            else
            {
                Log(
                    $"Initial scene source ({initialSourceFormat}) was rejected; " +
                    "the startup viewport remains blank.",
                    "Scene",
                    LogLevel.Error);
                return false;
            }

            publishScene = true;
            return true;
        }
        catch (OperationCanceledException)
            when (!IsCurrentSceneLoad(load) ||
                  load.Cancellation.IsCancellationRequested)
        {
            return false;
        }
        catch (Exception ex)
        {
            if (!IsCurrentSceneLoad(load))
                return false;

            RunWithStartupEngineAccess(() =>
            {
                _legacySceneSourceMode = startupPlan.SourceMode;
                _view3d = initialIs3D;
                EstablishEmptySceneDocument(
                    initialIs3D,
                    sourcePath: null,
                    keepSourcePath: false,
                    initialView: initialProjectView);
                RestoreActiveSceneDocumentState();
                ApplySceneViewModePresentation();
            });
            Log(
                $"Initial scene source ({initialSourceFormat}) load error; " +
                $"the startup viewport remains blank: {ex.Message}",
                "Scene",
                LogLevel.Error);
            return false;
        }
        finally
        {
            CompleteSceneLoad(load, publishScene);
        }
    }

    private void SwitchSceneViewMode(
        EditorSceneViewMode mode,
        bool logChange = true)
    {
        if (Engine == IntPtr.Zero) return;
        if (EngineInterop.acs_editor_play_state(Engine) != 0 || PreviewBtn.IsChecked == true)
        {
            Log("Stop Play/Preview before changing the scene view.");
            ApplySceneViewModePresentation();
            return;
        }

        var currentView = new EditorSceneViewState(
            _legacySceneSourceMode,
            _sceneViewMode);
        if (!currentView.TryChangeView(mode, out EditorSceneViewState nextView))
        {
            Log(
                "Perspective view is unavailable for an unconverted .acscene source. " +
                "The current source and scene content were left unchanged.",
                "Scene",
                LogLevel.Warn);
            ApplySceneViewModePresentation();
            return;
        }

        _sceneViewMode = nextView.ViewMode;
        EditorSceneViewDescriptor view =
            EditorSceneViewModePolicy.Describe(_sceneViewMode);
        if (_legacySceneSourceMode == SceneDocumentMode.ThreeD)
        {
            // The native orthographic switch also aligns the camera to the XY plane and constrains
            // transforms to that plane, which is the 2D editing preset required here.
            EngineInterop.acs_editor_set_ortho3d(
                Engine,
                view.IsOrthographic ? 1 : 0);
        }
        ApplySceneViewModePresentation();
        if (logChange)
            Log($"Scene view: {view.ToolbarLabel}. {view.Description}");
    }

    private void ApplySceneViewModePresentation()
    {
        EditorSceneViewDescriptor view =
            EditorSceneViewModePolicy.Describe(_sceneViewMode);
        PerspectiveViewBtn.IsChecked =
            _sceneViewMode == EditorSceneViewMode.Perspective;
        Scene2DViewBtn.IsChecked =
            _sceneViewMode == EditorSceneViewMode.Orthographic;
        bool editable = Engine == IntPtr.Zero ||
                        (EngineInterop.acs_editor_play_state(Engine) == 0 &&
                         PreviewBtn.IsChecked != true);
        PerspectiveViewBtn.IsEnabled =
            editable &&
            EditorSceneViewModePolicy.IsSupportedByLegacySource(
                EditorSceneViewMode.Perspective,
                _legacySceneSourceMode);
        Scene2DViewBtn.IsEnabled = editable;
        SceneModeText.Text = $"VIEW: {view.StatusLabel}";
        SceneModeText.ToolTip =
            view.Description +
            " View presets do not change the scene document, active legacy source, save state, " +
            "or undo history." +
            (_legacySceneSourceMode == SceneDocumentMode.TwoD
                ? " Perspective requires an explicit source conversion and is disabled."
                : "");
        UpdateCameraFrustumControl();
        UpdateSceneName();
    }

    // Transitional alias for initialization call sites outside this focused partial class.
    private void ApplySceneDocumentModePresentation() =>
        ApplySceneViewModePresentation();

    private void OnSceneViewPerspective(object sender, RoutedEventArgs e) =>
        SwitchSceneViewMode(EditorSceneViewMode.Perspective);

    private void OnSceneView2D(object sender, RoutedEventArgs e) =>
        SwitchSceneViewMode(EditorSceneViewMode.Orthographic);
}
