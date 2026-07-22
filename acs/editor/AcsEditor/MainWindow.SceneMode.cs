// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Windows;

namespace AcsEditor;

/// <summary>
/// One editor scene document with Perspective and 2D (XY Orthographic) view presets. The current
/// native editor still has separate legacy 2D and 3D serializers, so this class keeps both as
/// reversible compatibility adapters behind the single managed document identity.
/// </summary>
public partial class MainWindow
{
    private EditorSceneViewMode _sceneViewMode = EditorSceneViewMode.TwoD;
    private SceneDocumentMode _legacySceneSourceMode = SceneDocumentMode.TwoD;
    private string? _scene2DPath;
    private string? _scene3DDocumentPath;
    private bool _scene2DInitialized;
    private bool _scene3DInitialized;

    private string? _scene2DSavedSnapshot;
    private string? _scene3DSavedSnapshot;
    private bool _scene2DDirty;
    private bool _scene3DDirty;

    private static bool Is3DScenePath(string? path) =>
        string.Equals(Path.GetExtension(path), ".acs3d", StringComparison.OrdinalIgnoreCase);

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
            _savedSceneSnapshot = _scene3DSavedSnapshot;
            _sceneDirty = _scene3DDirty;
        }
        else
        {
            _currentScenePath = _scene2DPath;
            _savedSceneSnapshot = _scene2DSavedSnapshot;
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

    /// <summary>
    /// Loads the configured initial document with the parser selected by its extension. Existing
    /// Assets/scene3d.acs3d is deliberately not preferred over an explicit 2D initial scene.
    /// </summary>
    private void InitializeProjectSceneDocument()
    {
        if (Engine == IntPtr.Zero || _project == null) return;

        string scenePath = ResolveConfiguredProjectScenePath();
        _legacySceneSourceMode = Is3DScenePath(scenePath)
            ? SceneDocumentMode.ThreeD
            : SceneDocumentMode.TwoD;
        _sceneViewMode = EditorSceneViewModePolicy.InitialForLegacySource(scenePath);
        EditorSceneViewDescriptor initialView =
            EditorSceneViewModePolicy.Describe(_sceneViewMode);
        bool initialIs3D =
            _legacySceneSourceMode == SceneDocumentMode.ThreeD;
        string initialSourceFormat = initialIs3D ? ".acs3d" : ".acscene";

        _view3d = initialIs3D;
        EngineInterop.acs_editor_set_view3d(Engine, initialIs3D ? 1 : 0);
        EngineInterop.acs_editor_set_ortho3d(
            Engine,
            initialView.IsOrthographic ? 1 : 0);
        RestoreActiveSceneDocumentState();

        Log($"Project: {_project.Name}  ({_project.RootDir})");
        try
        {
            bool loaded = false;
            if (File.Exists(scenePath))
            {
                string text = File.ReadAllText(scenePath, System.Text.Encoding.UTF8);
                if (initialIs3D)
                {
                    loaded = EngineInterop.acs_editor_scene3d_load_text(Engine, text) != 0;
                    _scene3DInitialized = true;
                }
                else
                {
                    EngineInterop.acs_editor_scene_new(Engine);
                    loaded = EngineInterop.acs_editor_scene_load_text(Engine, text) != 0;
                    _scene2DInitialized = true;
                }

                Log(loaded
                    ? $"Loaded initial scene source ({initialSourceFormat}) ← {scenePath}"
                    : $"Initial scene source ({initialSourceFormat}) load failed (format).");
            }
            else
            {
                if (initialIs3D)
                {
                    // set_view3d seeds a useful empty-project preview on first entry.
                    _scene3DInitialized = true;
                }
                else
                {
                    EngineInterop.acs_editor_scene_new(Engine);
                    _scene2DInitialized = true;
                }
                Log(
                    $"No initial scene source ({initialSourceFormat}) file — started empty.");
            }

            SetCurrentScenePath(scenePath);
            EngineInterop.acs_editor_camera_frame_all(Engine);
        }
        catch (Exception ex)
        {
            Log($"Initial scene source ({initialSourceFormat}) load error: " + ex.Message);
            if (initialIs3D) _scene3DInitialized = true;
            else _scene2DInitialized = true;
            SetCurrentScenePath(scenePath);
        }

        ApplySceneViewModePresentation();
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
            _sceneViewMode == EditorSceneViewMode.TwoD;
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
        UpdateSceneName();
    }

    // Transitional alias for initialization call sites outside this focused partial class.
    private void ApplySceneDocumentModePresentation() =>
        ApplySceneViewModePresentation();

    private void OnSceneViewPerspective(object sender, RoutedEventArgs e) =>
        SwitchSceneViewMode(EditorSceneViewMode.Perspective);

    private void OnSceneView2D(object sender, RoutedEventArgs e) =>
        SwitchSceneViewMode(EditorSceneViewMode.TwoD);
}
