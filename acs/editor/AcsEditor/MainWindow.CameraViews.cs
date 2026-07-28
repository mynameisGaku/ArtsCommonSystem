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

    private sealed class CameraViewLease
    {
        internal required ulong SlotId { get; init; }
        internal required ulong NativeRequestId { get; init; }
        internal required int CameraNodeId { get; set; }
        internal required string StableCameraId { get; init; }
        internal required uint RequestedWidth { get; set; }
        internal required uint RequestedHeight { get; set; }
        internal required bool IsCameraAvailable { get; set; }
    }

    private CameraViewportWindow? _cameraViewportWindow;
    private bool _cameraViewRefreshQueued;
    private readonly List<CameraViewLease> _cameraViewLeases = new(
        CameraViewSlotPolicy.MaximumLogicalSlots);
    private ulong _nextCameraViewSlotId = 1;
    private ulong _selectedCameraViewSlotId;
    private bool _cameraViewUsesRequestContract;
    private uint _cameraViewSurfaceWidth = 1;
    private uint _cameraViewSurfaceHeight = 1;
    private CameraViewOpenLifecycleState _cameraViewOpenLifecycle;
    private CameraViewOpenSnapshot? _pendingCameraViewOpenSnapshot;
    private string _pendingCameraViewOpenFailure =
        "the window closed before its live surface attached";
    private bool _cameraViewOpenRecoveryRunning;

    private bool DetachedCameraViewOwnsLiveSurface =>
        _cameraViewportWindow?.HasLiveSurface == true;

    private bool HasCameraViewLease => _cameraViewLeases.Count != 0;

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
        if (_cameraViewportWindow == null &&
            _pendingCameraViewOpenSnapshot.HasValue)
        {
            RecoverFailedCameraViewOpen(
                window: null,
                failureDetail: _pendingCameraViewOpenFailure,
                windowAlreadyClosed: true);
            if (_cameraViewOpenLifecycle !=
                CameraViewOpenLifecycleState.None)
            {
                return;
            }
        }
        if (_cameraViewportWindow == null &&
            HasCameraViewLease &&
            !ReleaseCameraViewRequests())
        {
            Log(
                "Camera View cannot open because one or more previous native " +
                "requests could not be released safely.",
                "Camera",
                LogLevel.Error);
            return;
        }
        CameraViewBackendPlan backendPlan =
            CameraViewBackendPolicy.Resolve(
                CameraAuthoringAvailable,
                _viewport.SupportsCameraViewRequests);
        if (!backendPlan.CanOpen)
            return;
        System.Diagnostics.Debug.Assert(
            backendPlan.MaximumLivePresenters == 1 &&
            !backendPlan.HasDedicatedOffscreenTargets);
        if (_cameraViewportWindow is CameraViewportWindow existingWindow)
        {
            if (!existingWindow.PinCamera(
                    camera.NodeId,
                    camera.StableCameraId))
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
        BeginCameraViewOpen(snapshot);
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

            _cameraViewUsesRequestContract =
                backendPlan.UsesRequestContract;
            if (!TryCreateInitialCameraViewLease(
                    camera.NodeId,
                    camera.StableCameraId))
            {
                throw new InvalidOperationException(
                    "the initial logical Camera View request could not be created");
            }

            window = new CameraViewportWindow(
                this,
                _viewport,
                camera.StableCameraId,
                placement,
                GetCameraViewChoices,
                GetCameraViewSlots,
                TryPinCameraView,
                TryActivateCameraViewSlot,
                TryCloseCameraViewSlot,
                RefreshCameraViewLeasesFromScene,
                GetPreviewCameraNode,
                ClearCameraViewPreviewOverride,
                TryResizeCameraViewRequest,
                detail => Log(detail, "Camera", LogLevel.Warn));
            SubscribeCameraViewportWindow(window);
            _cameraViewportWindow = window;

            // Do not bind the preview camera or enter Game View until the
            // floating window owns the one physical surface. Otherwise this
            // request can redirect the main Game View during the asynchronous
            // HWND hand-off, including while Play is running.
            window.Show();
        }
        catch (Exception error)
        {
            RecoverFailedCameraViewOpen(window, error.Message);
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

    private void BeginCameraViewOpen(CameraViewOpenSnapshot snapshot)
    {
        System.Diagnostics.Debug.Assert(
            _cameraViewOpenLifecycle ==
                CameraViewOpenLifecycleState.None);
        _pendingCameraViewOpenSnapshot = snapshot;
        _pendingCameraViewOpenFailure =
            "the window closed before its live surface attached";
        _cameraViewOpenLifecycle = CameraViewOpenLifecycle.Transition(
            _cameraViewOpenLifecycle,
            CameraViewOpenLifecycleEvent.Begin);
    }

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
        string failureDetail,
        bool windowAlreadyClosed = false)
    {
        if (_pendingCameraViewOpenSnapshot is not
                CameraViewOpenSnapshot snapshot ||
            _cameraViewOpenLifecycle is
                CameraViewOpenLifecycleState.None or
                CameraViewOpenLifecycleState.Committed)
        {
            return;
        }
        _pendingCameraViewOpenFailure = failureDetail;
        _cameraViewOpenLifecycle = CameraViewOpenLifecycle.Transition(
            _cameraViewOpenLifecycle,
            CameraViewOpenLifecycleEvent.OpenFailed);
        if (_cameraViewOpenRecoveryRunning)
            return;

        _cameraViewOpenRecoveryRunning = true;
        try
        {
            bool failedWindowClosed =
                windowAlreadyClosed || window == null;
            if (window != null && !windowAlreadyClosed)
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
            _ = ReleaseCameraViewRequests();
            if (plan.RestorePreviewOverride)
                RestoreCameraViewPreviewOverride(snapshot.PreviewNodeId);
            if (plan.RestoreViewPresentation)
                RestoreCameraViewPresentation(snapshot.WasGameView);
            if (plan.RestoreViewportOverlay)
                RestoreCameraViewOverlay(snapshot);
            _pendingCameraViewOpenSnapshot = null;
            _pendingCameraViewOpenFailure = "";
            _cameraViewOpenLifecycle =
                CameraViewOpenLifecycle.Transition(
                    _cameraViewOpenLifecycle,
                    CameraViewOpenLifecycleEvent.RollbackCompleted);
            Log(
                "Camera View could not be opened; editor state was restored: " +
                failureDetail,
                "Camera",
                LogLevel.Error);
        }
        finally
        {
            _cameraViewOpenRecoveryRunning = false;
        }
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
        int previewNodeId = GetPreviewCameraNode() ?? -1;

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

    private bool TryPinCameraView(
        int cameraNodeId,
        string stableCameraId) =>
        TryPinCameraViewCore(
            cameraNodeId,
            stableCameraId,
            activateImmediately: true);

    private bool TryCreateInitialCameraViewLease(
        int cameraNodeId,
        string stableCameraId)
    {
        return TryPinCameraViewCore(
            cameraNodeId,
            stableCameraId,
            activateImmediately: false);
    }

    private bool TryPinCameraViewCore(
        int cameraNodeId,
        string stableCameraId,
        bool activateImmediately)
    {
        if (!CameraAuthoringAvailable ||
            Engine == IntPtr.Zero ||
            _viewport == null ||
            !TryGetCameraAuthoringState(
                cameraNodeId,
                out CameraAuthoringState camera) ||
            !string.Equals(
                camera.StableCameraId,
                stableCameraId,
                StringComparison.Ordinal))
        {
            return false;
        }

        CameraViewPreviewPlan previewPlan =
            CameraViewPreviewPolicy.Plan(
                CameraAuthoringAvailable,
                camera.IsEnabled);
        System.Diagnostics.Debug.Assert(
            !previewPlan.MutatesAuthoredCamera &&
            !previewPlan.RecordsSceneHistory);
        if (!previewPlan.CanApply ||
            !previewPlan.UsesTransientNativeOverride)
        {
            Log(
                "The selected Camera cannot drive the transient preview.",
                "Camera",
                LogLevel.Warn);
            return false;
        }

        var identities =
            new List<CameraViewSlotIdentity>(_cameraViewLeases.Count);
        foreach (CameraViewLease existingLease in _cameraViewLeases)
        {
            identities.Add(new CameraViewSlotIdentity(
                existingLease.SlotId,
                existingLease.StableCameraId,
                existingLease.IsCameraAvailable));
        }
        CameraViewSlotAddPlan addPlan = CameraViewSlotPolicy.PlanAdd(
            identities,
            camera.NodeId,
            camera.StableCameraId,
            _cameraViewUsesRequestContract);
        if (addPlan.Action == CameraViewSlotAddAction.SelectExisting)
        {
            if (activateImmediately)
                return TryActivateCameraViewSlot(addPlan.ExistingSlotId);
            _selectedCameraViewSlotId = addPlan.ExistingSlotId;
            return true;
        }
        if (addPlan.Action != CameraViewSlotAddAction.Add)
        {
            Log(
                $"Camera View supports at most {addPlan.Capacity} logical " +
                "camera slot(s) with the negotiated backend.",
                "Camera",
                LogLevel.Warn);
            return false;
        }

        ulong nativeRequestId = 0;
        if (_cameraViewUsesRequestContract &&
            !_viewport.TryCreateCameraViewRequest(
                camera.NodeId,
                camera.StableCameraId,
                _cameraViewSurfaceWidth,
                _cameraViewSurfaceHeight,
                out nativeRequestId))
        {
            Log(
                "The native logical Camera View request could not be created.",
                "Camera",
                LogLevel.Error);
            return false;
        }

        var lease = new CameraViewLease
        {
            SlotId = AllocateCameraViewSlotId(),
            NativeRequestId = nativeRequestId,
            CameraNodeId = camera.NodeId,
            StableCameraId = camera.StableCameraId,
            RequestedWidth = _cameraViewSurfaceWidth,
            RequestedHeight = _cameraViewSurfaceHeight,
            IsCameraAvailable = true,
        };
        _cameraViewLeases.Add(lease);
        if (!activateImmediately)
        {
            _selectedCameraViewSlotId = lease.SlotId;
            return true;
        }
        if (TryActivateCameraViewSlot(lease.SlotId))
            return true;

        if (nativeRequestId != 0 &&
            !_viewport.ReleaseCameraViewRequest(nativeRequestId))
        {
            lease.IsCameraAvailable = false;
            Log(
                "Camera View failed to activate a new logical slot and its " +
                "native request could not be released; the lease was retained " +
                "for a later retry.",
                "Camera",
                LogLevel.Error);
            return false;
        }
        _cameraViewLeases.Remove(lease);
        return false;
    }

    private ulong AllocateCameraViewSlotId()
    {
        for (int attempt = 0;
             attempt <= CameraViewSlotPolicy.MaximumLogicalSlots;
             ++attempt)
        {
            ulong candidate = _nextCameraViewSlotId++;
            if (_nextCameraViewSlotId == 0)
                _nextCameraViewSlotId = 1;
            if (candidate != 0 &&
                !_cameraViewLeases.Exists(
                    lease => lease.SlotId == candidate))
            {
                return candidate;
            }
        }
        throw new InvalidOperationException(
            "Camera View logical slot identity space is exhausted.");
    }

    private CameraViewLease? FindCameraViewLease(ulong slotId) =>
        _cameraViewLeases.Find(lease => lease.SlotId == slotId);

    private CameraViewLease? FindNativeCameraViewPresenter()
    {
        if (!_cameraViewUsesRequestContract || _viewport == null)
            return FindCameraViewLease(_selectedCameraViewSlotId);
        foreach (CameraViewLease lease in _cameraViewLeases)
        {
            if (lease.NativeRequestId != 0 &&
                _viewport.TryGetCameraViewRequest(
                    lease.NativeRequestId,
                    out CameraViewRequestSnapshot snapshot) &&
                snapshot.Flags.HasFlag(
                    CameraViewRequestFlags.Presenter) &&
                !snapshot.Flags.HasFlag(
                    CameraViewRequestFlags.CameraStale))
            {
                return lease;
            }
        }
        return null;
    }

    private bool TryActivateCameraViewSlot(ulong slotId)
    {
        CameraViewLease? target = FindCameraViewLease(slotId);
        if (target == null ||
            !RefreshCameraViewLeaseIdentity(
                target,
                GetCameraViewChoices()))
        {
            return false;
        }

        if (!_cameraViewUsesRequestContract)
        {
            if (Engine == IntPtr.Zero ||
                EngineInterop.acs_editor_game_camera_preview_set(
                    Engine,
                    target.CameraNodeId) == 0)
            {
                return false;
            }
            _selectedCameraViewSlotId = target.SlotId;
            return true;
        }
        if (_viewport == null ||
            target.NativeRequestId == 0 ||
            !_viewport.TryUpdateCameraViewRequest(
                target.NativeRequestId,
                target.CameraNodeId,
                target.StableCameraId,
                _cameraViewSurfaceWidth,
                _cameraViewSurfaceHeight))
        {
            return false;
        }
        target.RequestedWidth = _cameraViewSurfaceWidth;
        target.RequestedHeight = _cameraViewSurfaceHeight;

        CameraViewLease? current = FindNativeCameraViewPresenter();
        CameraViewPresenterTransferPlan transfer =
            CameraViewSlotPolicy.PlanTransfer(
                current?.SlotId ?? 0,
                target.SlotId);
        System.Diagnostics.Debug.Assert(
            !transfer.MutatesAuthoredCamera &&
            !transfer.RecordsSceneHistory);
        if (!transfer.CanApply)
            return false;
        if (transfer.UnbindCurrentPresenter &&
            (current == null ||
             current.NativeRequestId == 0 ||
             !_viewport.TryUnbindCameraViewPresenter(
                 current.NativeRequestId)))
        {
            return false;
        }
        if (transfer.BindTargetPresenter &&
            !_viewport.TryBindCameraViewPresenter(
                target.NativeRequestId))
        {
            if (transfer.UnbindCurrentPresenter &&
                current is { NativeRequestId: not 0 } &&
                current.IsCameraAvailable &&
                !_viewport.TryBindCameraViewPresenter(
                    current.NativeRequestId))
            {
                Log(
                    "Camera View presenter transfer failed and the previous " +
                    "logical request could not be restored.",
                    "Camera",
                    LogLevel.Error);
            }
            return false;
        }

        _selectedCameraViewSlotId = target.SlotId;
        return true;
    }

    private bool TryCloseCameraViewSlot(ulong slotId)
    {
        CameraViewLease? closing = FindCameraViewLease(slotId);
        if (closing == null)
            return false;
        var orderedIds = new List<ulong>(_cameraViewLeases.Count);
        foreach (CameraViewLease lease in _cameraViewLeases)
            orderedIds.Add(lease.SlotId);
        CameraViewSlotClosePlan closePlan =
            CameraViewSlotPolicy.PlanClose(
                orderedIds,
                _selectedCameraViewSlotId,
                slotId);
        if (!closePlan.CanClose)
            return false;

        if (closePlan.ClosesSelectedSlot &&
            closePlan.NextSelectedSlotId != 0 &&
            !TryActivateCameraViewSlot(
                closePlan.NextSelectedSlotId))
        {
            return false;
        }
        if (closePlan.ClosesSelectedSlot &&
            closePlan.NextSelectedSlotId == 0)
        {
            ClearCameraViewPreviewOverride();
        }

        if (closing.NativeRequestId != 0 &&
            _viewport != null &&
            !_viewport.ReleaseCameraViewRequest(
                closing.NativeRequestId))
        {
            if (closePlan.ClosesSelectedSlot &&
                closePlan.NextSelectedSlotId == 0)
            {
                _ = TryActivateCameraViewSlot(closing.SlotId);
            }
            Log(
                "Camera View could not close the logical slot because its " +
                "opaque native request remains live.",
                "Camera",
                LogLevel.Error);
            return false;
        }

        _cameraViewLeases.Remove(closing);
        if (_cameraViewLeases.Count == 0)
        {
            _selectedCameraViewSlotId = 0;
        }
        else if (closePlan.ClosesSelectedSlot)
        {
            _selectedCameraViewSlotId =
                closePlan.NextSelectedSlotId;
        }
        return true;
    }

    private IReadOnlyList<CameraViewSlotView> GetCameraViewSlots()
    {
        IReadOnlyList<CameraViewChoice> cameras =
            GetCameraViewChoices();
        var result =
            new List<CameraViewSlotView>(_cameraViewLeases.Count);
        foreach (CameraViewLease lease in _cameraViewLeases)
        {
            CameraViewChoice? camera =
                ResolveCameraViewChoice(cameras, lease);
            bool cameraAvailable =
                camera is { IsEnabled: true } &&
                lease.IsCameraAvailable;
            uint requestedWidth = lease.RequestedWidth;
            uint requestedHeight = lease.RequestedHeight;
            uint targetGeneration = 0;
            uint historyGeneration = 0;
            bool isPresenter =
                !_cameraViewUsesRequestContract &&
                lease.SlotId == _selectedCameraViewSlotId;
            bool cameraStale = !cameraAvailable;
            if (_cameraViewUsesRequestContract &&
                _viewport != null &&
                lease.NativeRequestId != 0 &&
                _viewport.TryGetCameraViewRequest(
                    lease.NativeRequestId,
                    out CameraViewRequestSnapshot snapshot))
            {
                requestedWidth = snapshot.Width;
                requestedHeight = snapshot.Height;
                targetGeneration = snapshot.TargetGeneration;
                historyGeneration = snapshot.HistoryGeneration;
                isPresenter = snapshot.Flags.HasFlag(
                    CameraViewRequestFlags.Presenter);
                cameraStale = snapshot.Flags.HasFlag(
                    CameraViewRequestFlags.CameraStale);
                lease.RequestedWidth = requestedWidth;
                lease.RequestedHeight = requestedHeight;
            }
            cameraAvailable &= !cameraStale;
            lease.IsCameraAvailable = cameraAvailable;
            result.Add(new CameraViewSlotView(
                lease.SlotId,
                lease.NativeRequestId,
                camera?.NodeId ?? lease.CameraNodeId,
                camera?.NodeName ?? "Missing Camera",
                lease.StableCameraId,
                cameraAvailable,
                camera?.IsEnabled == true,
                lease.SlotId == _selectedCameraViewSlotId,
                isPresenter,
                requestedWidth,
                requestedHeight,
                targetGeneration,
                historyGeneration));
        }
        return result;
    }

    private static CameraViewChoice? ResolveCameraViewChoice(
        IReadOnlyList<CameraViewChoice> cameras,
        CameraViewLease lease) =>
        CameraViewSlotPolicy.ResolveStableCamera(
            cameras,
            lease.StableCameraId,
            lease.CameraNodeId);

    private bool RefreshCameraViewLeaseIdentity(
        CameraViewLease lease,
        IReadOnlyList<CameraViewChoice> cameras)
    {
        CameraViewChoice? camera =
            ResolveCameraViewChoice(cameras, lease);
        if (camera is not { IsEnabled: true })
        {
            lease.IsCameraAvailable = false;
            return false;
        }
        if (_cameraViewUsesRequestContract &&
            (_viewport == null ||
             lease.NativeRequestId == 0 ||
             !_viewport.TryUpdateCameraViewRequest(
                 lease.NativeRequestId,
                 camera.NodeId,
                 camera.StableCameraId,
                 lease.RequestedWidth,
                 lease.RequestedHeight)))
        {
            lease.IsCameraAvailable = false;
            return false;
        }
        lease.CameraNodeId = camera.NodeId;
        lease.IsCameraAvailable = true;
        return true;
    }

    private bool RefreshCameraViewLeasesFromScene()
    {
        if (_cameraViewLeases.Count == 0)
            return true;
        IReadOnlyList<CameraViewChoice> cameras =
            GetCameraViewChoices();
        foreach (CameraViewLease lease in _cameraViewLeases)
            _ = RefreshCameraViewLeaseIdentity(lease, cameras);

        var identities =
            new List<CameraViewSlotIdentity>(_cameraViewLeases.Count);
        foreach (CameraViewLease lease in _cameraViewLeases)
        {
            identities.Add(new CameraViewSlotIdentity(
                lease.SlotId,
                lease.StableCameraId,
                lease.IsCameraAvailable));
        }
        ulong selected = CameraViewSlotPolicy.SelectAfterSceneRefresh(
            identities,
            _selectedCameraViewSlotId);
        if (selected == 0)
        {
            _selectedCameraViewSlotId = 0;
            ClearCameraViewPreviewOverride();
            return true;
        }
        return TryActivateCameraViewSlot(selected);
    }

    private int? GetPreviewCameraNode()
    {
        if (!CameraAuthoringAvailable || Engine == IntPtr.Zero)
            return null;
        if (_cameraViewUsesRequestContract)
        {
            CameraViewLease? presenter =
                FindNativeCameraViewPresenter();
            if (presenter != null &&
                _viewport != null &&
                _viewport.TryGetCameraViewRequest(
                    presenter.NativeRequestId,
                    out CameraViewRequestSnapshot snapshot) &&
                snapshot.Flags.HasFlag(
                    CameraViewRequestFlags.Presenter) &&
                !snapshot.Flags.HasFlag(
                    CameraViewRequestFlags.CameraStale))
            {
                return snapshot.CameraNodeId;
            }
            return null;
        }
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
        if (_cameraViewUsesRequestContract)
        {
            CameraViewLease? presenter =
                FindNativeCameraViewPresenter();
            if (presenter != null)
            {
                _ = _viewport?.TryUnbindCameraViewPresenter(
                    presenter.NativeRequestId);
            }
            return;
        }
        EngineInterop.acs_editor_game_camera_preview_clear(Engine);
    }

    private bool TryResizeCameraViewRequest(
        uint width,
        uint height)
    {
        if (!CameraViewRequestContract.IsValidExtent(width, height))
            return false;
        _cameraViewSurfaceWidth = width;
        _cameraViewSurfaceHeight = height;
        if (!_cameraViewUsesRequestContract ||
            _selectedCameraViewSlotId == 0)
        {
            return true;
        }

        CameraViewExtentUpdatePlan plan =
            CameraViewSlotPolicy.PlanExtentUpdate(
                _selectedCameraViewSlotId,
                width,
                height);
        System.Diagnostics.Debug.Assert(
            plan.MutatesOnlyTargetRequest &&
            plan.ResetsOnlyTargetHistory);
        CameraViewLease? target =
            FindCameraViewLease(plan.TargetSlotId);
        if (!plan.CanApply || target == null)
            return false;
        if (!target.IsCameraAvailable)
        {
            target.RequestedWidth = width;
            target.RequestedHeight = height;
            return true;
        }
        if (_viewport == null ||
            target.NativeRequestId == 0 ||
            !_viewport.TryUpdateCameraViewRequest(
                target.NativeRequestId,
                target.CameraNodeId,
                target.StableCameraId,
                width,
                height))
        {
            return false;
        }
        target.RequestedWidth = width;
        target.RequestedHeight = height;
        return true;
    }

    private bool ReleaseCameraViewRequests()
    {
        bool releasedAll = true;
        for (int index = _cameraViewLeases.Count - 1;
             index >= 0;
             --index)
        {
            CameraViewLease lease = _cameraViewLeases[index];
            if (lease.NativeRequestId != 0 &&
                _viewport != null &&
                !_viewport.ReleaseCameraViewRequest(
                    lease.NativeRequestId))
            {
                releasedAll = false;
                Log(
                    "Camera View native request release failed; the opaque " +
                    "logical lease was retained for a later retry.",
                    "Camera",
                    LogLevel.Error);
                continue;
            }
            _cameraViewLeases.RemoveAt(index);
        }
        if (_cameraViewLeases.Count == 0)
        {
            _selectedCameraViewSlotId = 0;
            _cameraViewUsesRequestContract = false;
            _cameraViewSurfaceWidth = 1;
            _cameraViewSurfaceHeight = 1;
        }
        else if (FindCameraViewLease(
                     _selectedCameraViewSlotId) == null)
        {
            _selectedCameraViewSlotId =
                _cameraViewLeases[0].SlotId;
        }
        return releasedAll;
    }

    private void OnCameraLiveSurfaceAttached(object? sender, EventArgs e)
    {
        if (sender is not CameraViewportWindow window ||
            !ReferenceEquals(window, _cameraViewportWindow))
            return;
        if (!CameraViewPresenterPublicationPolicy.CanBindPresenter(
                window.HasLiveSurface) ||
            _selectedCameraViewSlotId == 0 ||
            !TryActivateCameraViewSlot(_selectedCameraViewSlotId))
        {
            RecoverFailedCameraViewOpen(
                window,
                "the preview presenter could not bind after the Camera View acquired the live surface");
            return;
        }

        // Camera View is a presentation choice only. It neither starts nor
        // stops Play, and it never mutates the editor navigation camera.
        SetGameView(true);
        _cameraViewOpenLifecycle = CameraViewOpenLifecycle.Transition(
            _cameraViewOpenLifecycle,
            CameraViewOpenLifecycleEvent.LiveSurfaceAttached);
        if (_cameraViewOpenLifecycle ==
            CameraViewOpenLifecycleState.Committed)
        {
            _pendingCameraViewOpenSnapshot = null;
            _pendingCameraViewOpenFailure = "";
        }
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
        _pendingCameraViewOpenFailure =
            "the live renderer surface could not be attached";
        _cameraViewOpenLifecycle = CameraViewOpenLifecycle.Transition(
            _cameraViewOpenLifecycle,
            CameraViewOpenLifecycleEvent.OpenFailed);
    }

    private void OnCameraViewportWindowClosed(object? sender, EventArgs e)
    {
        if (sender is not CameraViewportWindow window)
            return;
        _cameraViewOpenLifecycle = CameraViewOpenLifecycle.Transition(
            _cameraViewOpenLifecycle,
            CameraViewOpenLifecycleEvent.WindowClosed);
        if (_cameraViewOpenLifecycle ==
            CameraViewOpenLifecycleState.RollbackRequired)
        {
            if (!_cameraViewOpenRecoveryRunning)
            {
                RecoverFailedCameraViewOpen(
                    window,
                    _pendingCameraViewOpenFailure,
                    windowAlreadyClosed: true);
            }
            return;
        }
        window.LiveSurfaceAttached -= OnCameraLiveSurfaceAttached;
        window.LiveSurfaceDocked -= OnCameraLiveSurfaceDocked;
        window.LiveSurfaceAttachFailed -= OnCameraLiveSurfaceAttachFailed;
        window.Closed -= OnCameraViewportWindowClosed;
        ClearCameraViewPreviewOverride();
        ReleaseCameraViewRequests();
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
        if (EditorCloseFinalizationPolicy.ShouldBypassAuxiliaryHandlers(
                _auxiliaryCloseApproved))
        {
            return;
        }

        if (e.Cancel)
        {
            if (ToolPanelOwnerCloseTransactionPolicy.MustRollbackPending(
                    _pendingToolPanelOwnerClose != null,
                    closeAlreadyCancelled: true,
                    auxiliaryCloseSucceeded: false))
            {
                _ = RollbackDockableToolPanelsOwnerClose();
                CancelApprovedEditorClose();
            }
            return;
        }

        bool cameraClosed = true;
        try
        {
            cameraClosed =
                _cameraViewportWindow == null ||
                _cameraViewportWindow.CloseForOwner();
        }
        catch (Exception error)
        {
            cameraClosed = false;
            Log(
                "Camera View owner-close failed: " + error.Message,
                "Camera",
                LogLevel.Error);
        }

        if (ToolPanelOwnerCloseTransactionPolicy.MayCommitPending(
                _pendingToolPanelOwnerClose != null,
                closeAlreadyCancelled: false,
                auxiliaryCloseSucceeded: cameraClosed))
        {
            CommitDockableToolPanelsOwnerClose();
        }
        if (cameraClosed)
        {
            FinalizeApprovedEditorCloseAfterAuxiliaryCommit(e);
            return;
        }

        e.Cancel = true;
        bool exactRollback = true;
        if (ToolPanelOwnerCloseTransactionPolicy.MustRollbackPending(
                _pendingToolPanelOwnerClose != null,
                closeAlreadyCancelled: false,
                auxiliaryCloseSucceeded: false))
        {
            exactRollback = RollbackDockableToolPanelsOwnerClose();
        }
        CancelApprovedEditorClose();
        Log(
            exactRollback
                ? "Editor close was cancelled because the live Camera View " +
                  "surface could not be safely re-docked; tool panels were restored."
                : "Editor close was cancelled because the live Camera View " +
                  "surface could not be safely re-docked; the recovered tool " +
                  "layout was persisted.",
            "Camera",
            exactRollback ? LogLevel.Warn : LogLevel.Error);
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
