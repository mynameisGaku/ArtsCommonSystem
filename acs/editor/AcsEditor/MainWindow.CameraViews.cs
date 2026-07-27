// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Windows;

namespace AcsEditor;

public partial class MainWindow
{
    private readonly record struct CameraViewOpenSnapshot(
        bool WasGameView,
        int? PreviewNodeId,
        Visibility OverlayVisibility,
        bool OverlayIsHitTestVisible,
        bool ViewportIsHitTestVisible,
        string OverlayTitle,
        string OverlayDetail);

    private CameraViewportWindow? _cameraViewportWindow;
    private bool _cameraViewRefreshQueued;

    private bool DetachedCameraViewOwnsLiveSurface =>
        _cameraViewportWindow?.HasLiveSurface == true;

    private void OpenCameraView(int nodeId)
    {
        if (Engine == IntPtr.Zero ||
            _viewport == null ||
            !EnsureCameraAuthoringAvailable() ||
            !TryGetCameraAuthoringState(
                nodeId,
                out CameraAuthoringState camera))
        {
            return;
        }
        if (!camera.IsEnabled)
        {
            Log(
                "Disabled cameras cannot drive the live Camera View.",
                "Camera",
                LogLevel.Warn);
            return;
        }
        if (_cameraViewportWindow is CameraViewportWindow existingWindow)
        {
            if (!existingWindow.PinCamera(camera.StableCameraId))
            {
                Log(
                    "The existing Camera View could not pin the selected camera.",
                    "Camera",
                    LogLevel.Warn);
                return;
            }
            // Camera View is a presentation choice only. It neither starts nor
            // stops Play, and it never creates a second native editor host.
            SetGameView(true);
            return;
        }
        if (_viewport.IsRenderSurfaceFloating)
        {
            Log(
                "Camera View cannot open because the live renderer surface " +
                "already has an external owner.",
                "Camera",
                LogLevel.Error);
            return;
        }

        CameraViewOpenSnapshot snapshot = CaptureCameraViewOpenSnapshot();
        CameraViewportWindow? window = null;
        try
        {
            Rect virtualScreen = new(
                SystemParameters.VirtualScreenLeft,
                SystemParameters.VirtualScreenTop,
                SystemParameters.VirtualScreenWidth,
                SystemParameters.VirtualScreenHeight);
            Rect ownerBounds = WindowState == WindowState.Normal
                ? new Rect(Left, Top, ActualWidth, ActualHeight)
                : RestoreBounds;
            var fallback = new Rect(
                ownerBounds.Left + Math.Min(96.0, ownerBounds.Width * 0.08),
                ownerBounds.Top + Math.Min(72.0, ownerBounds.Height * 0.08),
                Math.Clamp(ownerBounds.Width * 0.62, 560.0, 1080.0),
                Math.Clamp(ownerBounds.Height * 0.62, 360.0, 720.0));
            CameraViewPlacementState placement =
                CameraViewPlacementStore.Load(
                    virtualScreen,
                    fallback,
                    out string placementWarning);
            if (placementWarning.Length != 0)
            {
                Log(
                    "Camera View placement could not be restored; defaults were used: " +
                    placementWarning,
                    "Camera",
                    LogLevel.Warn);
            }
            placement.StableCameraId = camera.StableCameraId;

            window = new CameraViewportWindow(
                this,
                _viewport,
                camera.StableCameraId,
                placement,
                GetCameraViewChoices,
                TryPreviewCameraView,
                GetPreviewCameraNode,
                ClearCameraViewPreviewOverride,
                detail => Log(detail, "Camera", LogLevel.Warn));
            SubscribeCameraViewportWindow(window);
            _cameraViewportWindow = window;

            if (!TryPreviewCameraView(camera.StableCameraId))
            {
                RecoverFailedCameraViewOpen(
                    window,
                    snapshot,
                    "the transient camera override could not be applied");
                return;
            }

            // Camera View is a presentation choice only. It neither starts nor
            // stops Play, and it never creates a second native editor host.
            SetGameView(true);
            window.Show();
        }
        catch (Exception error)
        {
            RecoverFailedCameraViewOpen(window, snapshot, error.Message);
        }
    }

    private CameraViewOpenSnapshot CaptureCameraViewOpenSnapshot() =>
        new(
            WasGameView:
                Engine != IntPtr.Zero &&
                EngineInterop.acs_editor_is_game_view(Engine) != 0,
            PreviewNodeId: GetPreviewCameraNode(),
            OverlayVisibility: ViewportLoadingOverlay.Visibility,
            OverlayIsHitTestVisible:
                ViewportLoadingOverlay.IsHitTestVisible,
            ViewportIsHitTestVisible: ViewportHost.IsHitTestVisible,
            OverlayTitle: ViewportLoadingTitle.Text,
            OverlayDetail: ViewportLoadingDetail.Text);

    private void SubscribeCameraViewportWindow(
        CameraViewportWindow window)
    {
        window.LiveSurfaceAttached += OnCameraLiveSurfaceAttached;
        window.LiveSurfaceDocked += OnCameraLiveSurfaceDocked;
        window.LiveSurfaceAttachFailed += OnCameraLiveSurfaceAttachFailed;
        window.Closed += OnCameraViewportWindowClosed;
    }

    private void UnsubscribeCameraViewportWindow(
        CameraViewportWindow window)
    {
        window.LiveSurfaceAttached -= OnCameraLiveSurfaceAttached;
        window.LiveSurfaceDocked -= OnCameraLiveSurfaceDocked;
        window.LiveSurfaceAttachFailed -= OnCameraLiveSurfaceAttachFailed;
        window.Closed -= OnCameraViewportWindowClosed;
    }

    private void RecoverFailedCameraViewOpen(
        CameraViewportWindow? window,
        CameraViewOpenSnapshot snapshot,
        string failureDetail)
    {
        bool failedWindowClosed = window == null;
        if (window != null)
        {
            try
            {
                failedWindowClosed = window.TryRollbackFailedOpen();
            }
            catch (Exception cleanupError)
            {
                failedWindowClosed =
                    !window.IsVisible && !window.HasLiveSurface;
                Log(
                    "Camera View failed-open cleanup reported an error: " +
                    cleanupError.Message,
                    "Camera",
                    LogLevel.Warn);
            }
        }

        bool renderSurfaceRestored =
            _viewport?.IsRenderSurfaceFloating != true &&
            window?.HasLiveSurface != true;
        CameraViewOpenRecoveryPlan plan =
            CameraViewOpenRecoveryPolicy.Resolve(
                openCommitted: false,
                renderSurfaceRestored,
                failedWindowClosed);
        if (!plan.DiscardFailedWindow)
        {
            Log(
                "Camera View could not finish opening and could not be " +
                "rolled back safely; its current renderer ownership was " +
                "preserved: " + failureDetail,
                "Camera",
                LogLevel.Error);
            return;
        }

        if (window != null)
        {
            UnsubscribeCameraViewportWindow(window);
            if (ReferenceEquals(_cameraViewportWindow, window))
                _cameraViewportWindow = null;
        }
        if (plan.RestorePreviewOverride)
            RestoreCameraViewPreviewOverride(snapshot.PreviewNodeId);
        if (plan.RestoreViewPresentation)
            RestoreCameraViewPresentation(snapshot.WasGameView);
        if (plan.RestoreViewportOverlay)
            RestoreCameraViewOverlay(snapshot);
        Log(
            "Camera View could not be opened; editor state was restored: " +
            failureDetail,
            "Camera",
            LogLevel.Error);
    }

    private void RestoreCameraViewPreviewOverride(int? previewNodeId)
    {
        if (!CameraAuthoringAvailable || Engine == IntPtr.Zero)
            return;
        try
        {
            if (previewNodeId is int nodeId &&
                EngineInterop.acs_editor_game_camera_preview_set(
                    Engine,
                    nodeId) != 0)
            {
                return;
            }
            EngineInterop.acs_editor_game_camera_preview_clear(Engine);
            if (previewNodeId.HasValue)
            {
                Log(
                    "The previous Camera View preview override was no longer " +
                    "valid and was cleared during rollback.",
                    "Camera",
                    LogLevel.Warn);
            }
        }
        catch (Exception error)
        {
            Log(
                "Camera View preview rollback failed: " + error.Message,
                "Camera",
                LogLevel.Error);
        }
    }

    private void RestoreCameraViewPresentation(bool wasGameView)
    {
        try
        {
            SetGameView(wasGameView);
        }
        catch (Exception error)
        {
            Log(
                "Camera View presentation rollback failed: " + error.Message,
                "Camera",
                LogLevel.Error);
        }
    }

    private void RestoreCameraViewOverlay(
        CameraViewOpenSnapshot snapshot)
    {
        ViewportLoadingTitle.Text = snapshot.OverlayTitle;
        ViewportLoadingDetail.Text = snapshot.OverlayDetail;
        ViewportLoadingOverlay.Visibility =
            snapshot.OverlayVisibility;
        ViewportLoadingOverlay.IsHitTestVisible =
            snapshot.OverlayIsHitTestVisible;
        ViewportHost.IsHitTestVisible =
            snapshot.ViewportIsHitTestVisible;
    }

    private IReadOnlyList<CameraViewChoice> GetCameraViewChoices()
    {
        if (!CameraAuthoringAvailable || Engine == IntPtr.Zero)
            return Array.Empty<CameraViewChoice>();

        int count = EngineInterop.acs_editor_camera3d_count(Engine);
        if (count <= 0)
            return Array.Empty<CameraViewChoice>();
        count = Math.Min(
            count,
            CameraAuthoringContract.MaximumCameraCount);
        int previewNodeId = -1;
        _ = EngineInterop.acs_editor_game_camera_preview_get(
            Engine,
            out previewNodeId);

        var cameras = new List<CameraViewChoice>(count);
        var stableIdScratch = new byte[StableCameraBufferCapacity];
        var projectionScratch = new float[4];
        var nameScratch = new byte[256];
        for (int index = 0; index < count; ++index)
        {
            int nodeId =
                EngineInterop.acs_editor_camera3d_node_id_at(
                    Engine,
                    index);
            if (nodeId < 0 ||
                !TryGetCameraAuthoringState(
                    nodeId,
                    stableIdScratch,
                    projectionScratch,
                    out CameraAuthoringState state))
            {
                continue;
            }
            string name =
                EngineInterop.acs_editor_node3d_name(
                    Engine,
                    nodeId,
                    nameScratch,
                    nameScratch.Length) != 0
                    ? EngineInterop.Utf8Z(nameScratch)
                    : $"Node {nodeId}";
            cameras.Add(new CameraViewChoice(
                state.NodeId,
                name,
                state.StableCameraId,
                state.IsEnabled,
                state.IsActive,
                state.NodeId == previewNodeId,
                state.Priority));
        }

        cameras.Sort(static (left, right) =>
        {
            int activeOrder =
                right.IsAuthoredActive.CompareTo(left.IsAuthoredActive);
            if (activeOrder != 0)
                return activeOrder;
            int priorityOrder = right.Priority.CompareTo(left.Priority);
            if (priorityOrder != 0)
                return priorityOrder;
            int stableOrder = string.CompareOrdinal(
                left.StableCameraId,
                right.StableCameraId);
            return stableOrder != 0
                ? stableOrder
                : left.NodeId.CompareTo(right.NodeId);
        });
        return cameras;
    }

    private bool TryPreviewCameraView(string stableCameraId)
    {
        if (!CameraAuthoringAvailable ||
            Engine == IntPtr.Zero ||
            !CameraAuthoringContract.IsValidStableCameraId(stableCameraId))
        {
            return false;
        }

        CameraAuthoringState? match = null;
        int count = Math.Min(
            Math.Max(0, EngineInterop.acs_editor_camera3d_count(Engine)),
            CameraAuthoringContract.MaximumCameraCount);
        var stableIdScratch = new byte[StableCameraBufferCapacity];
        var projectionScratch = new float[4];
        for (int index = 0; index < count; ++index)
        {
            int nodeId =
                EngineInterop.acs_editor_camera3d_node_id_at(
                    Engine,
                    index);
            if (nodeId >= 0 &&
                TryGetCameraAuthoringState(
                    nodeId,
                    stableIdScratch,
                    projectionScratch,
                    out CameraAuthoringState state) &&
                string.Equals(
                    state.StableCameraId,
                    stableCameraId,
                    StringComparison.Ordinal))
            {
                match = state;
                break;
            }
        }
        if (match is not { } camera)
            return false;
        CameraViewPreviewPlan plan = CameraViewPreviewPolicy.Plan(
            CameraAuthoringAvailable,
            camera.IsEnabled);
        System.Diagnostics.Debug.Assert(
            !plan.MutatesAuthoredCamera &&
            !plan.RecordsSceneHistory);
        if (!plan.CanApply || !plan.UsesTransientNativeOverride)
        {
            Log(
                "The selected Camera cannot drive the transient preview.",
                "Camera",
                LogLevel.Warn);
            return false;
        }

        return EngineInterop.acs_editor_game_camera_preview_set(
                   Engine,
                   camera.NodeId) != 0;
    }

    private int? GetPreviewCameraNode()
    {
        if (!CameraAuthoringAvailable || Engine == IntPtr.Zero)
            return null;
        return EngineInterop.acs_editor_game_camera_preview_get(
                   Engine,
                   out int nodeId) != 0
            ? nodeId
            : null;
    }

    private void ClearCameraViewPreviewOverride()
    {
        if (!CameraAuthoringAvailable || Engine == IntPtr.Zero)
            return;
        EngineInterop.acs_editor_game_camera_preview_clear(Engine);
    }

    private void OnCameraLiveSurfaceAttached(object? sender, EventArgs e)
    {
        if (!ReferenceEquals(sender, _cameraViewportWindow))
            return;
        ViewportHost.IsHitTestVisible = false;
        ViewportLoadingTitle.Text = "Camera View is floating";
        ViewportLoadingDetail.Text =
            "Use Re-dock in the Camera View window to return the live renderer.";
        ViewportLoadingOverlay.IsHitTestVisible = false;
        ViewportLoadingOverlay.Visibility = Visibility.Visible;
    }

    private void OnCameraLiveSurfaceDocked(object? sender, EventArgs e)
    {
        if (!ReferenceEquals(sender, _cameraViewportWindow))
            return;
        ClearCameraViewPreviewOverride();
        ViewportLoadingOverlay.Visibility = Visibility.Collapsed;
        ViewportLoadingOverlay.IsHitTestVisible = true;
        ViewportHost.IsHitTestVisible = !IsSceneEditingBlocked;
    }

    private void NotifyCameraViewSceneChanged()
    {
        if (_cameraViewportWindow == null || _cameraViewRefreshQueued)
            return;
        _cameraViewRefreshQueued = true;
        _ = Dispatcher.BeginInvoke(
            System.Windows.Threading.DispatcherPriority.Background,
            new Action(() =>
            {
                _cameraViewRefreshQueued = false;
                _cameraViewportWindow?.RefreshFromScene();
            }));
    }

    private void OnCameraLiveSurfaceAttachFailed(object? sender, EventArgs e)
    {
        if (!ReferenceEquals(sender, _cameraViewportWindow))
            return;
        ClearCameraViewPreviewOverride();
    }

    private void OnCameraViewportWindowClosed(object? sender, EventArgs e)
    {
        if (sender is not CameraViewportWindow window)
            return;
        window.LiveSurfaceAttached -= OnCameraLiveSurfaceAttached;
        window.LiveSurfaceDocked -= OnCameraLiveSurfaceDocked;
        window.LiveSurfaceAttachFailed -= OnCameraLiveSurfaceAttachFailed;
        window.Closed -= OnCameraViewportWindowClosed;
        ClearCameraViewPreviewOverride();
        if (ReferenceEquals(_cameraViewportWindow, window))
            _cameraViewportWindow = null;
        ViewportLoadingOverlay.Visibility = Visibility.Collapsed;
        ViewportLoadingOverlay.IsHitTestVisible = true;
        ViewportHost.IsHitTestVisible = !IsSceneEditingBlocked;
    }

    private void OnCameraViewOwnerClosing(
        object? sender,
        CancelEventArgs e)
    {
        if (e.Cancel)
            return;
        if (_cameraViewportWindow != null &&
            !_cameraViewportWindow.CloseForOwner())
        {
            e.Cancel = true;
            Log(
                "Editor close was deferred because the live Camera View " +
                "surface could not be safely re-docked.",
                "Camera",
                LogLevel.Error);
        }
    }

    private void OnCameraFrustumsToggle(
        object sender,
        RoutedEventArgs e)
    {
        if (!_view3d ||
            !CameraAuthoringAvailable ||
            Engine == IntPtr.Zero)
        {
            UpdateCameraFrustumControl();
            return;
        }
        EngineInterop.acs_editor_camera_frustum_set_visible(
            Engine,
            CameraFrustumsCheck.IsChecked == true ? 1 : 0);
        UpdateCameraFrustumControl();
    }

    private void UpdateCameraFrustumControl()
    {
        bool engineAttached = Engine != IntPtr.Zero;
        bool capability = CameraAuthoringAvailable;
        bool nativeVisible =
            _view3d &&
            engineAttached &&
            capability &&
            EngineInterop.acs_editor_camera_frustum_get_visible(Engine) != 0;
        CameraFrustumControlState state =
            CameraFrustumControlPolicy.Resolve(
                _view3d,
                engineAttached,
                capability,
                nativeVisible);
        CameraFrustumsCheck.Visibility =
            state.IsVisible ? Visibility.Visible : Visibility.Collapsed;
        CameraFrustumsCheck.IsEnabled = state.IsEnabled;
        CameraFrustumsCheck.IsChecked = state.IsChecked;
    }
}
