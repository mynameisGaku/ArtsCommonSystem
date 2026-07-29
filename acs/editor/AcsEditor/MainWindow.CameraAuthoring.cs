// SPDX-License-Identifier: Apache-2.0

using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

public partial class MainWindow
{
    private const string CameraComponentDisplayName = "Camera";
    private const int StableCameraBufferCapacity =
        CameraAuthoringContract.MaximumStableCameraIdLength + 1;

    private bool CameraAuthoringAvailable =>
        _viewport?.AbiCapabilities.HasFlag(
            EditorAbiCapability.CameraAuthoringV1) == true;

    private bool EnsureCameraAuthoringAvailable()
    {
        if (CameraAuthoringAvailable)
            return true;
        Log(
            "The loaded editor runtime does not advertise camera-authoring-v1; " +
            "camera actions were disabled before calling the optional ABI.",
            "Camera",
            LogLevel.Warn);
        return false;
    }

    private bool TryGetCameraAuthoringState(
        int nodeId,
        out CameraAuthoringState state)
    {
        var stableId = new byte[StableCameraBufferCapacity];
        var values = new float[4];
        return TryGetCameraAuthoringState(
            nodeId,
            stableId,
            values,
            out state);
    }

    private bool TryGetCameraAuthoringState(
        int nodeId,
        byte[] stableId,
        float[] values,
        out CameraAuthoringState state)
    {
        state = default;
        if (!CameraAuthoringAvailable ||
            Engine == IntPtr.Zero ||
            nodeId < 0 ||
            stableId.Length < StableCameraBufferCapacity ||
            values.Length < 4)
        {
            return false;
        }

        if (EngineInterop.acs_editor_node3d_camera_get(
                Engine,
                nodeId,
                stableId,
                stableId.Length,
                out int projection,
                out int priority,
                out int active,
                values) == 0)
        {
            return false;
        }

        var candidate = new CameraAuthoringState(
            nodeId,
            EngineInterop.Utf8Z(stableId),
            active != 0,
            EngineInterop.acs_editor_node3d_get_enabled(Engine, nodeId) != 0,
            (CameraProjectionMode)projection,
            values[0],
            values[1],
            values[2],
            values[3],
            priority);
        if (!CameraAuthoringContract.TryNormalize(
                candidate,
                out state,
                out string detail))
        {
            Log(
                $"Camera {nodeId} was rejected by the editor contract: {detail}",
                "Camera",
                LogLevel.Error);
            return false;
        }
        return true;
    }

    private int GetDesignatedCameraNode()
    {
        if (!CameraAuthoringAvailable || Engine == IntPtr.Zero)
            return -1;
        var stableId = new byte[StableCameraBufferCapacity];
        var values = new float[4];
        return EngineInterop.acs_editor_scene3d_active_camera(
                   Engine,
                   out int nodeId,
                   stableId,
                   stableId.Length,
                   out _,
                   out _,
                   values) != 0
            ? nodeId
            : -1;
    }

    private FrameworkElement BuildCameraComponent(
        CameraAuthoringState initialState)
    {
        CameraAuthoringState state = initialState;
        var body = new StackPanel();
        bool showAll = _detailsFilter.Length == 0 ||
                       DetailsMatches("component", "native", "camera");

        bool Apply(
            CameraAuthoringState candidate,
            string label,
            bool refreshHierarchy = false)
        {
            if (!CameraAuthoringContract.TryNormalize(
                    candidate,
                    out CameraAuthoringState normalized,
                    out string detail))
            {
                Log(
                    $"{label} was rejected: {detail}",
                    "Camera",
                    LogLevel.Warn);
                return false;
            }
            if (!EnsureCameraAuthoringAvailable() ||
                EngineInterop.acs_editor_node3d_camera_set(
                    Engine,
                    normalized.NodeId,
                    normalized.StableCameraId,
                    (int)normalized.Projection,
                    normalized.Priority,
                    normalized.IsActive ? 1 : 0,
                    normalized.FieldOfViewDegrees,
                    normalized.OrthographicSize,
                    normalized.NearPlane,
                    normalized.FarPlane) == 0)
            {
                Log(
                    $"{label} failed because the authored camera changed or was removed.",
                    "Camera",
                    LogLevel.Error);
                return false;
            }

            state = normalized;
            RecordSceneDocumentChange(label);
            if (refreshHierarchy)
                Build3DHierarchy();
            return true;
        }

        if (showAll || DetailsMatches("stable id", "identity"))
        {
            body.Children.Add(
                LabeledValue3D("Stable ID", state.StableCameraId));
        }

        if (showAll || DetailsMatches("projection", "perspective", "orthographic"))
        {
            var row = CameraLabeledRow("Projection");
            var projection = new ComboBox
            {
                MinWidth = 126,
                SelectedIndex = (int)state.Projection,
            };
            projection.Items.Add("Perspective");
            projection.Items.Add("Orthographic");
            projection.SelectionChanged += (_, _) =>
            {
                if (_pop3d || projection.SelectedIndex is < 0 or > 1)
                    return;
                CameraProjectionMode selected =
                    (CameraProjectionMode)projection.SelectedIndex;
                if (selected == state.Projection)
                    return;
                if (!Apply(
                        state with { Projection = selected },
                        "Change Camera Projection"))
                {
                    projection.SelectedIndex = (int)state.Projection;
                }
            };
            row.Children.Add(projection);
            body.Children.Add(row);
        }

        if (showAll || DetailsMatches("field of view", "fov", "perspective"))
        {
            body.Children.Add(CameraFloatRow(
                "Field of View",
                state.FieldOfViewDegrees,
                value => Apply(
                        state with { FieldOfViewDegrees = value },
                        "Edit Camera Field of View")
                    ? state.FieldOfViewDegrees
                    : null,
                "\u00b0"));
        }
        if (showAll || DetailsMatches("orthographic", "ortho", "size", "height"))
        {
            body.Children.Add(CameraFloatRow(
                "Ortho Size",
                state.OrthographicSize,
                value => Apply(
                        state with { OrthographicSize = value },
                        "Edit Camera Ortho Size")
                    ? state.OrthographicSize
                    : null));
        }
        if (showAll || DetailsMatches("near", "near clip", "clipping"))
        {
            body.Children.Add(CameraFloatRow(
                "Near Clip",
                state.NearPlane,
                value => Apply(
                        state with { NearPlane = value },
                        "Edit Camera Near Clip")
                    ? state.NearPlane
                    : null));
        }
        if (showAll || DetailsMatches("far", "far clip", "clipping"))
        {
            body.Children.Add(CameraFloatRow(
                "Far Clip",
                state.FarPlane,
                value => Apply(
                        state with { FarPlane = value },
                        "Edit Camera Far Clip")
                    ? state.FarPlane
                    : null));
        }
        if (showAll || DetailsMatches("priority", "camera stack"))
        {
            body.Children.Add(CameraIntegerRow(
                "Priority",
                state.Priority,
                value => Apply(
                        state with { Priority = value },
                        "Edit Camera Priority",
                        refreshHierarchy: true)
                    ? state.Priority
                    : null));
        }
        if (showAll || DetailsMatches("enabled", "camera stack"))
        {
            body.Children.Add(CameraBoolRow(
                "Node Enabled",
                state.IsEnabled,
                enabled =>
                {
                    EngineInterop.acs_editor_node3d_set_enabled(
                        Engine,
                        state.NodeId,
                        enabled ? 1 : 0);
                    state = state with { IsEnabled = enabled };
                    RecordSceneDocumentChange("Edit Camera Enabled");
                    bool wasPopulating = _pop3d;
                    _pop3d = true;
                    InspEnabled.IsChecked = enabled;
                    _pop3d = wasPopulating;
                    Build3DHierarchy();
                    return true;
                }));
        }
        if (showAll || DetailsMatches("active", "camera stack", "game camera"))
        {
            body.Children.Add(CameraBoolRow(
                "Active",
                state.IsActive,
                active => Apply(
                    state with { IsActive = active },
                    active
                        ? "Set Active Camera"
                        : "Clear Active Camera",
                    refreshHierarchy: true)));
        }

        if (showAll ||
            DetailsMatches(
                "align",
                "snap",
                "scene view",
                "pilot",
                "preview",
                "active"))
            body.Children.Add(BuildCameraActionRow(state.NodeId));

        return ComponentCard(
            "Camera",
            body,
            native: true,
            remove: () => RemoveCameraFromNode(state.NodeId));
    }

    private DockPanel CameraLabeledRow(string label)
    {
        var row = new DockPanel { Margin = new Thickness(0, 3, 0, 2) };
        var text = new TextBlock
        {
            Text = label,
            Width = 88,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = (Brush)FindResource("TextDim"),
        };
        DockPanel.SetDock(text, Dock.Left);
        row.Children.Add(text);
        return row;
    }

    private FrameworkElement CameraFloatRow(
        string label,
        float initial,
        Func<float, float?> apply,
        string suffix = "")
    {
        var row = CameraLabeledRow(label);
        var value = new TextBox
        {
            Text = initial.ToString("0.####", CultureInfo.InvariantCulture),
            Style = (Style)FindResource("NumBox"),
            MinWidth = 86,
        };
        float committed = initial;
        void Commit()
        {
            if (!float.TryParse(
                    value.Text,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out float parsed) ||
                float.IsNaN(parsed) ||
                float.IsInfinity(parsed))
            {
                value.Text = committed.ToString(
                    "0.####",
                    CultureInfo.InvariantCulture);
                return;
            }
            if (parsed == committed)
                return;
            float? applied = apply(parsed);
            if (applied is float actual)
            {
                committed = actual;
                value.Text = committed.ToString(
                    "0.####",
                    CultureInfo.InvariantCulture);
            }
            else
            {
                value.Text = committed.ToString(
                    "0.####",
                    CultureInfo.InvariantCulture);
            }
        }
        value.LostKeyboardFocus += (_, _) => Commit();
        value.KeyDown += (_, e) =>
        {
            if (e.Key != Key.Enter)
                return;
            Commit();
            Keyboard.ClearFocus();
        };
        row.Children.Add(value);
        if (suffix.Length != 0)
        {
            row.Children.Add(new TextBlock
            {
                Text = suffix,
                Margin = new Thickness(5, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = (Brush)FindResource("TextDim"),
            });
        }
        return row;
    }

    private FrameworkElement CameraIntegerRow(
        string label,
        int initial,
        Func<int, int?> apply)
    {
        var row = CameraLabeledRow(label);
        var value = new TextBox
        {
            Text = initial.ToString(CultureInfo.InvariantCulture),
            Style = (Style)FindResource("NumBox"),
            MinWidth = 86,
        };
        int committed = initial;
        void Commit()
        {
            if (!int.TryParse(
                    value.Text,
                    NumberStyles.Integer,
                    CultureInfo.InvariantCulture,
                    out int parsed))
            {
                value.Text = committed.ToString(CultureInfo.InvariantCulture);
                return;
            }
            if (parsed == committed)
                return;
            int? applied = apply(parsed);
            if (applied is int actual)
            {
                committed = actual;
                value.Text = committed.ToString(CultureInfo.InvariantCulture);
            }
            else
                value.Text = committed.ToString(CultureInfo.InvariantCulture);
        }
        value.LostKeyboardFocus += (_, _) => Commit();
        value.KeyDown += (_, e) =>
        {
            if (e.Key != Key.Enter)
                return;
            Commit();
            Keyboard.ClearFocus();
        };
        row.Children.Add(value);
        return row;
    }

    private FrameworkElement CameraBoolRow(
        string label,
        bool initial,
        Func<bool, bool> apply)
    {
        var row = CameraLabeledRow(label);
        var value = new CheckBox
        {
            IsChecked = initial,
            VerticalAlignment = VerticalAlignment.Center,
        };
        bool committed = initial;
        void Commit(bool selected)
        {
            if (_pop3d || selected == committed)
                return;
            if (apply(selected))
            {
                committed = selected;
                return;
            }
            bool wasPopulating = _pop3d;
            _pop3d = true;
            value.IsChecked = committed;
            _pop3d = wasPopulating;
        }
        value.Checked += (_, _) => Commit(true);
        value.Unchecked += (_, _) => Commit(false);
        row.Children.Add(value);
        return row;
    }

    private FrameworkElement BuildCameraActionRow(int nodeId)
    {
        var row = new WrapPanel { Margin = new Thickness(0, 7, 0, 1) };
        var snapView = new Button
        {
            Content = "Snap View",
            Padding = new Thickness(8, 2, 8, 2),
            Margin = new Thickness(0, 0, 5, 0),
            ToolTip =
                "Move the editor Scene View to this Camera's resolved world " +
                "pose once. Authored Camera state and scene history are not changed.",
        };
        snapView.Click += (_, _) => SnapSceneViewToCamera(nodeId);
        var align = new Button
        {
            Content = "Align to View",
            Padding = new Thickness(8, 2, 8, 2),
            Margin = new Thickness(0, 0, 5, 0),
            ToolTip = "Move this Camera to the current Scene View pose.",
        };
        align.Click += (_, _) => AlignCameraToSceneView(nodeId);
        var active = new Button
        {
            Content = "Set Active",
            Padding = new Thickness(8, 2, 8, 2),
            Margin = new Thickness(0, 0, 5, 0),
            ToolTip = "Use this Camera as the explicitly active game camera.",
        };
        active.Click += (_, _) =>
        {
            if (!TryGetCameraAuthoringState(
                    nodeId,
                    out CameraAuthoringState state))
            {
                return;
            }
            if (!EnsureCameraAuthoringAvailable() ||
                EngineInterop.acs_editor_node3d_camera_set(
                    Engine,
                    nodeId,
                    state.StableCameraId,
                    (int)state.Projection,
                    state.Priority,
                    1,
                    state.FieldOfViewDegrees,
                    state.OrthographicSize,
                    state.NearPlane,
                    state.FarPlane) == 0)
            {
                Log("Set Active Camera failed.", "Camera", LogLevel.Error);
                return;
            }
            RecordSceneDocumentChange("Set Active Camera");
            Build3DHierarchy();
            Populate3DInspector(nodeId);
        };
        EditorOptionalServiceUiState requestService =
            GetCameraViewRequestServiceState();
        bool canOpenCameraPreview =
            EditorOptionalServiceActionPolicy.CanOpenCameraPreview(
                requestService);
        string cameraPreviewToolTip =
            "Preview this Camera in the one live floating viewport without " +
            "editing its Active flag. The renderer is transferred, not duplicated.";
        if (requestService.UsesExactNativeDiagnostic ||
            !requestService.CanInvoke)
        {
            cameraPreviewToolTip +=
                "\n\nRequest service: " +
                requestService.StatusText +
                "\n" +
                requestService.ToolTip;
            if (requestService.IsCapabilityNotAdvertised)
            {
                cameraPreviewToolTip +=
                    "\nA single legacy preview remains available; " +
                    "multi-slot request controls are disabled.";
            }
        }
        var cameraView = new Button
        {
            Content = "Float Preview",
            Padding = new Thickness(8, 2, 8, 2),
            IsEnabled = canOpenCameraPreview,
            ToolTip = cameraPreviewToolTip,
        };
        ToolTipService.SetShowOnDisabled(cameraView, true);
        cameraView.Click += (_, _) => OpenCameraView(nodeId);
        row.Children.Add(snapView);
        row.Children.Add(align);
        row.Children.Add(active);
        row.Children.Add(cameraView);
        return row;
    }

    private void AlignCameraToSceneView(int nodeId)
    {
        if (Engine == IntPtr.Zero ||
            IsSceneEditingBlocked ||
            !EnsureCameraAuthoringAvailable())
            return;
        if (EngineInterop.acs_editor_node3d_camera_align_to_view(
                Engine,
                nodeId) == 0)
        {
            Log(
                "Camera could not be aligned to the current Scene View.",
                "Camera",
                LogLevel.Error);
            return;
        }
        RecordSceneDocumentChange("Align Camera to Scene View");
        Populate3DInspector(nodeId);
        Log(
            $"Camera node {nodeId} was aligned to the current Scene View pose.",
            "Camera",
            LogLevel.Success);
    }

    private void OnAdd3DCamera(
        object sender,
        RoutedEventArgs e)
    {
        if (Engine == IntPtr.Zero ||
            IsSceneEditingBlocked ||
            !EnsureCameraAuthoringAvailable())
            return;
        if (!EnsureView3D())
            return;

        string stableId = NewStableCameraId();
        int id = EngineInterop.acs_editor_add_camera3d(
            Engine,
            "Camera",
            stableId);
        if (id < 0)
        {
            Log(
                "Camera creation failed. The scene may have reached its camera limit.",
                "Camera",
                LogLevel.Error);
            return;
        }
        BuildHierarchy();
        Select3DInHierarchy(id);
        Populate3DInspector(id);
        RecordSceneDocumentChange("Create Camera");
        Log(
            $"Camera '{stableId}' was created at the current Scene View pose (node {id}).",
            "Camera",
            LogLevel.Success);
    }

    private void AttachCameraToNode(int nodeId)
    {
        if (Engine == IntPtr.Zero ||
            !EnsureCameraAuthoringAvailable() ||
            TryGetCameraAuthoringState(nodeId, out _))
        {
            return;
        }

        string stableId = NewStableCameraId();
        if (EngineInterop.acs_editor_node3d_camera_set(
                Engine,
                nodeId,
                stableId,
                (int)CameraProjectionMode.Perspective,
                0,
                0,
                60.0f,
                10.0f,
                0.05f,
                1000.0f) == 0)
        {
            Log(
                "Camera could not be attached to the selected node.",
                "Camera",
                LogLevel.Error);
            return;
        }
        RecordSceneDocumentChange("Add Camera Component");
        Build3DHierarchy();
        Populate3DInspector(nodeId);
    }

    private void RemoveCameraFromNode(int nodeId)
    {
        if (Engine == IntPtr.Zero || !EnsureCameraAuthoringAvailable())
            return;
        if (EngineInterop.acs_editor_node3d_camera_clear(
                Engine,
                nodeId) == 0)
        {
            Log(
                "Camera component could not be removed.",
                "Camera",
                LogLevel.Error);
            return;
        }
        RecordSceneDocumentChange("Remove Camera Component");
        Build3DHierarchy();
        Populate3DInspector(nodeId);
    }

    private static string NewStableCameraId() =>
        "camera-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
}
