// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;

namespace AcsEditor;

internal readonly record struct CameraViewBackendPlan(
    bool CanOpen,
    bool UsesRequestContract,
    bool UsesLegacyGlobalOverride,
    int MaximumLivePresenters,
    bool HasDedicatedOffscreenTargets);

/// <summary>
/// Negotiates the staged Camera View backend without inferring render surfaces
/// from request allocation. CameraViewRequestsV1 isolates logical identity,
/// extent and generations; this version still exposes exactly one physical
/// shared-swapchain presenter and no synchronous-readback fallback.
/// </summary>
internal static class CameraViewBackendPolicy
{
    internal static CameraViewBackendPlan Resolve(
        bool cameraAuthoringCapability,
        bool cameraViewRequestsCapability) =>
        !cameraAuthoringCapability
            ? default
            : cameraViewRequestsCapability
                ? new CameraViewBackendPlan(
                    CanOpen: true,
                    UsesRequestContract: true,
                    UsesLegacyGlobalOverride: false,
                    MaximumLivePresenters: 1,
                    HasDedicatedOffscreenTargets: false)
                : new CameraViewBackendPlan(
                    CanOpen: true,
                    UsesRequestContract: false,
                    UsesLegacyGlobalOverride: true,
                    MaximumLivePresenters: 1,
                    HasDedicatedOffscreenTargets: false);
}

/// <summary>
/// A Camera View may choose a preview camera only after its window owns the
/// one physical render surface. Binding earlier would temporarily redirect
/// the main Game View, including while Play is running.
/// </summary>
internal static class CameraViewPresenterPublicationPolicy
{
    internal static bool CanBindPresenter(bool liveSurfaceAttached) =>
        liveSurfaceAttached;
}

internal readonly record struct CameraViewSlotIdentity(
    ulong SlotId,
    string StableCameraId,
    bool IsCameraAvailable);

internal enum CameraViewSlotAddAction
{
    Reject,
    Add,
    SelectExisting,
}

internal readonly record struct CameraViewSlotAddPlan(
    CameraViewSlotAddAction Action,
    ulong ExistingSlotId,
    int Capacity);

internal readonly record struct CameraViewSlotClosePlan(
    bool CanClose,
    bool ClosesSelectedSlot,
    ulong NextSelectedSlotId,
    bool CloseWindow);

internal readonly record struct CameraViewPresenterTransferPlan(
    bool CanApply,
    bool UnbindCurrentPresenter,
    bool BindTargetPresenter,
    bool MutatesAuthoredCamera,
    bool RecordsSceneHistory);

internal readonly record struct CameraViewExtentUpdatePlan(
    bool CanApply,
    ulong TargetSlotId,
    bool MutatesOnlyTargetRequest,
    bool ResetsOnlyTargetHistory);

/// <summary>
/// Pure bounded lifecycle policy for the tabs hosted by the one physical
/// Camera View window. CameraViewRequestsV1 contributes up to eight logical
/// leases, but never more than one presenter. The legacy override remains a
/// truthful single-slot fallback.
/// </summary>
internal static class CameraViewSlotPolicy
{
    internal const int MaximumLogicalSlots = 8;
    internal const int MaximumLivePresenters = 1;

    internal static int Capacity(bool usesRequestContract) =>
        usesRequestContract ? MaximumLogicalSlots : 1;

    internal static CameraViewSlotAddPlan PlanAdd(
        IReadOnlyList<CameraViewSlotIdentity> slots,
        int cameraNodeId,
        string stableCameraId,
        bool usesRequestContract)
    {
        ArgumentNullException.ThrowIfNull(slots);
        int capacity = Capacity(usesRequestContract);
        if (cameraNodeId < 0 ||
            !CameraAuthoringContract.IsValidStableCameraId(stableCameraId))
        {
            return new CameraViewSlotAddPlan(
                CameraViewSlotAddAction.Reject,
                ExistingSlotId: 0,
                capacity);
        }

        for (int index = 0; index < slots.Count; ++index)
        {
            CameraViewSlotIdentity slot = slots[index];
            if (string.Equals(
                    slot.StableCameraId,
                    stableCameraId,
                    StringComparison.Ordinal))
            {
                return new CameraViewSlotAddPlan(
                    CameraViewSlotAddAction.SelectExisting,
                    slot.SlotId,
                    capacity);
            }
        }
        return slots.Count < capacity
            ? new CameraViewSlotAddPlan(
                CameraViewSlotAddAction.Add,
                ExistingSlotId: 0,
                capacity)
            : new CameraViewSlotAddPlan(
                CameraViewSlotAddAction.Reject,
                ExistingSlotId: 0,
                capacity);
    }

    internal static CameraViewSlotClosePlan PlanClose(
        IReadOnlyList<ulong> orderedSlotIds,
        ulong selectedSlotId,
        ulong closingSlotId)
    {
        ArgumentNullException.ThrowIfNull(orderedSlotIds);
        int closingIndex = -1;
        for (int index = 0; index < orderedSlotIds.Count; ++index)
        {
            if (orderedSlotIds[index] == closingSlotId)
            {
                closingIndex = index;
                break;
            }
        }
        if (closingIndex < 0 || closingSlotId == 0)
            return default;

        bool closesSelected = selectedSlotId == closingSlotId;
        if (orderedSlotIds.Count == 1)
        {
            return new CameraViewSlotClosePlan(
                CanClose: true,
                ClosesSelectedSlot: closesSelected,
                NextSelectedSlotId: 0,
                CloseWindow: true);
        }
        if (!closesSelected)
        {
            return new CameraViewSlotClosePlan(
                CanClose: true,
                ClosesSelectedSlot: false,
                NextSelectedSlotId: selectedSlotId,
                CloseWindow: false);
        }

        int nextIndex =
            closingIndex + 1 < orderedSlotIds.Count
                ? closingIndex + 1
                : closingIndex - 1;
        return new CameraViewSlotClosePlan(
            CanClose: true,
            ClosesSelectedSlot: true,
            NextSelectedSlotId: orderedSlotIds[nextIndex],
            CloseWindow: false);
    }

    internal static CameraViewPresenterTransferPlan PlanTransfer(
        ulong currentPresenterSlotId,
        ulong targetSlotId)
    {
        if (targetSlotId == 0)
            return default;
        bool alreadyPresented =
            currentPresenterSlotId == targetSlotId;
        return new CameraViewPresenterTransferPlan(
            CanApply: true,
            UnbindCurrentPresenter:
                currentPresenterSlotId != 0 && !alreadyPresented,
            BindTargetPresenter: !alreadyPresented,
            MutatesAuthoredCamera: false,
            RecordsSceneHistory: false);
    }

    internal static ulong SelectAfterSceneRefresh(
        IReadOnlyList<CameraViewSlotIdentity> orderedSlots,
        ulong selectedSlotId)
    {
        ArgumentNullException.ThrowIfNull(orderedSlots);
        ulong firstAvailable = 0;
        for (int index = 0; index < orderedSlots.Count; ++index)
        {
            CameraViewSlotIdentity slot = orderedSlots[index];
            if (!slot.IsCameraAvailable)
                continue;
            firstAvailable = firstAvailable == 0
                ? slot.SlotId
                : firstAvailable;
            if (slot.SlotId == selectedSlotId)
                return selectedSlotId;
        }
        return firstAvailable;
    }

    internal static CameraViewChoice? ResolveStableCamera(
        IReadOnlyList<CameraViewChoice> cameras,
        string stableCameraId,
        int previousNodeId)
    {
        ArgumentNullException.ThrowIfNull(cameras);
        if (!CameraAuthoringContract.IsValidStableCameraId(
                stableCameraId))
        {
            return null;
        }

        CameraViewChoice? stableMatch = null;
        int stableMatchCount = 0;
        for (int index = 0; index < cameras.Count; ++index)
        {
            CameraViewChoice camera = cameras[index];
            if (!string.Equals(
                    camera.StableCameraId,
                    stableCameraId,
                    StringComparison.Ordinal))
            {
                continue;
            }
            if (camera.NodeId == previousNodeId)
                return camera;
            stableMatch = camera;
            stableMatchCount++;
        }
        // A published scene replacement may assign a new transient node ID.
        // Stable identity is authoritative only when it resolves uniquely.
        return stableMatchCount == 1 ? stableMatch : null;
    }

    internal static CameraViewExtentUpdatePlan PlanExtentUpdate(
        ulong selectedSlotId,
        uint width,
        uint height) =>
        selectedSlotId != 0 &&
        CameraViewRequestContract.IsValidExtent(width, height)
            ? new CameraViewExtentUpdatePlan(
                CanApply: true,
                TargetSlotId: selectedSlotId,
                MutatesOnlyTargetRequest: true,
                ResetsOnlyTargetHistory: true)
            : default;
}

internal readonly record struct CameraViewPreviewPlan(
    bool CanApply,
    bool UsesTransientNativeOverride,
    bool MutatesAuthoredCamera,
    bool RecordsSceneHistory);

internal static class CameraViewPreviewPolicy
{
    internal static CameraViewPreviewPlan Plan(
        bool cameraAuthoringCapability,
        bool cameraEnabled) =>
        new(
            CanApply: cameraAuthoringCapability && cameraEnabled,
            UsesTransientNativeOverride:
                cameraAuthoringCapability && cameraEnabled,
            MutatesAuthoredCamera: false,
            RecordsSceneHistory: false);
}

internal readonly record struct CameraFrustumControlState(
    bool IsVisible,
    bool IsEnabled,
    bool IsChecked);

internal static class CameraFrustumControlPolicy
{
    internal static CameraFrustumControlState Resolve(
        bool isThreeDimensionalScene,
        bool engineAttached,
        bool cameraAuthoringCapability,
        bool nativeVisible)
    {
        bool available =
            isThreeDimensionalScene &&
            engineAttached &&
            cameraAuthoringCapability;
        return new CameraFrustumControlState(
            IsVisible: available,
            IsEnabled: available,
            IsChecked: available && nativeVisible);
    }
}

internal enum RenderSurfaceOwnership
{
    Embedded,
    External,
}

internal static class RenderSurfaceTransferPolicy
{
    internal static RenderSurfaceOwnership AfterFailedExternalPosition(
        bool rollbackSucceeded) =>
        rollbackSucceeded
            ? RenderSurfaceOwnership.Embedded
            : RenderSurfaceOwnership.External;
}

internal readonly record struct CameraViewOpenRecoveryPlan(
    bool DiscardFailedWindow,
    bool RestorePreviewOverride,
    bool RestoreViewPresentation,
    bool RestoreViewportOverlay);

/// <summary>
/// Pure commit/rollback contract for opening the one live Camera View. A failed
/// open may restore editor state only after the renderer surface is known to be
/// embedded again. If re-docking fails, keeping the visible floating owner is
/// safer than publishing a false embedded state or losing the renderer HWND.
/// </summary>
internal static class CameraViewOpenRecoveryPolicy
{
    internal static CameraViewOpenRecoveryPlan Resolve(
        bool openCommitted,
        bool renderSurfaceRestored,
        bool failedWindowClosed)
    {
        if (openCommitted)
            return default;
        if (!renderSurfaceRestored || !failedWindowClosed)
            return default;
        return new CameraViewOpenRecoveryPlan(
            DiscardFailedWindow: true,
            RestorePreviewOverride: true,
            RestoreViewPresentation: true,
            RestoreViewportOverlay: true);
    }
}

internal enum CameraViewOpenLifecycleState
{
    None,
    Pending,
    Committed,
    RollbackRequired,
}

internal enum CameraViewOpenLifecycleEvent
{
    Begin,
    LiveSurfaceAttached,
    OpenFailed,
    WindowClosed,
    RollbackCompleted,
}

/// <summary>
/// Pure lifecycle for the asynchronous portion of Camera View open. The
/// pre-open snapshot stays pending until the live surface actually attaches;
/// merely returning from Window.Show is not a commit.
/// </summary>
internal static class CameraViewOpenLifecycle
{
    internal static CameraViewOpenLifecycleState Transition(
        CameraViewOpenLifecycleState state,
        CameraViewOpenLifecycleEvent lifecycleEvent) =>
        (state, lifecycleEvent) switch
        {
            (CameraViewOpenLifecycleState.None,
                CameraViewOpenLifecycleEvent.Begin) =>
                CameraViewOpenLifecycleState.Pending,
            (CameraViewOpenLifecycleState.Pending,
                CameraViewOpenLifecycleEvent.LiveSurfaceAttached) =>
                CameraViewOpenLifecycleState.Committed,
            (CameraViewOpenLifecycleState.Pending,
                CameraViewOpenLifecycleEvent.OpenFailed) =>
                CameraViewOpenLifecycleState.RollbackRequired,
            (CameraViewOpenLifecycleState.RollbackRequired,
                CameraViewOpenLifecycleEvent.OpenFailed) =>
                CameraViewOpenLifecycleState.RollbackRequired,
            (CameraViewOpenLifecycleState.Pending,
                CameraViewOpenLifecycleEvent.WindowClosed) =>
                CameraViewOpenLifecycleState.RollbackRequired,
            (CameraViewOpenLifecycleState.RollbackRequired,
                CameraViewOpenLifecycleEvent.WindowClosed) =>
                CameraViewOpenLifecycleState.RollbackRequired,
            (CameraViewOpenLifecycleState.Committed,
                CameraViewOpenLifecycleEvent.WindowClosed) =>
                CameraViewOpenLifecycleState.None,
            (CameraViewOpenLifecycleState.RollbackRequired,
                CameraViewOpenLifecycleEvent.RollbackCompleted) =>
                CameraViewOpenLifecycleState.None,
            _ => state,
        };
}

/// <summary>
/// A native scene-document load invalidates every Camera View identity. Refresh
/// only after that replacement (or its successful canonical rollback) is the
/// scene generation actually published by the asynchronous load gate.
/// </summary>
internal static class CameraViewScenePublicationPolicy
{
    internal static bool ShouldRefresh(
        bool publishedCurrentScene,
        bool nativeReplacementAttempted) =>
        publishedCurrentScene && nativeReplacementAttempted;
}

internal readonly record struct CameraViewPixelBounds(
    int Left,
    int Top,
    int Width,
    int Height)
{
    internal long Right => (long)Left + Width;
    internal long Bottom => (long)Top + Height;
}

/// <summary>
/// Pure Win32-pixel snap policy. The caller converts the 12-DIP affordance to
/// pixels using the floating window's current monitor DPI before invoking it.
/// </summary>
internal static class CameraViewSnapPolicy
{
    internal const double SnapDistanceDip = 12.0;

    internal static int ThresholdPixels(uint dpi) =>
        Math.Max(1, (int)Math.Round(
            SnapDistanceDip * Math.Max(96u, dpi) / 96.0,
            MidpointRounding.AwayFromZero));

    internal static CameraViewPixelBounds Snap(
        CameraViewPixelBounds moving,
        CameraViewPixelBounds owner,
        int thresholdPixels)
    {
        if (moving.Width <= 0 || moving.Height <= 0 ||
            owner.Width <= 0 || owner.Height <= 0 ||
            thresholdPixels < 0)
        {
            return moving;
        }

        int left = NearestWithin(
            moving.Left,
            thresholdPixels,
            owner.Left,
            owner.Right - moving.Width,
            owner.Left - moving.Width,
            owner.Right);
        int top = NearestWithin(
            moving.Top,
            thresholdPixels,
            owner.Top,
            owner.Bottom - moving.Height,
            owner.Top - moving.Height,
            owner.Bottom);
        return moving with { Left = left, Top = top };
    }

    internal static CameraViewPixelBounds ClampReachable(
        CameraViewPixelBounds moving,
        CameraViewPixelBounds workArea,
        int minimumVisibleWidth,
        int minimumVisibleHeight)
    {
        if (moving.Width <= 0 || moving.Height <= 0 ||
            workArea.Width <= 0 || workArea.Height <= 0 ||
            minimumVisibleWidth <= 0 || minimumVisibleHeight <= 0)
        {
            return moving;
        }

        int visibleWidth = Math.Min(minimumVisibleWidth, moving.Width);
        int visibleHeight = Math.Min(minimumVisibleHeight, moving.Height);
        long minimumLeft =
            (long)workArea.Left - moving.Width + visibleWidth;
        long maximumLeft = workArea.Right - visibleWidth;
        long minimumTop = workArea.Top;
        long maximumTop = workArea.Bottom - visibleHeight;
        int left = checked((int)Math.Clamp(
            (long)moving.Left,
            minimumLeft,
            maximumLeft));
        int top = checked((int)Math.Clamp(
            (long)moving.Top,
            minimumTop,
            maximumTop));
        return moving with { Left = left, Top = top };
    }

    private static int NearestWithin(
        int value,
        int threshold,
        long first,
        long second,
        long third,
        long fourth)
    {
        int best = value;
        long bestDistance = (long)threshold + 1L;
        ConsiderCandidate(value, threshold, first, ref best, ref bestDistance);
        ConsiderCandidate(value, threshold, second, ref best, ref bestDistance);
        ConsiderCandidate(value, threshold, third, ref best, ref bestDistance);
        ConsiderCandidate(value, threshold, fourth, ref best, ref bestDistance);
        return best;
    }

    private static void ConsiderCandidate(
        int value,
        int threshold,
        long candidate,
        ref int best,
        ref long bestDistance)
    {
        if (candidate is < int.MinValue or > int.MaxValue)
            return;
        long distance = Math.Abs((long)value - candidate);
        if (distance <= threshold && distance < bestDistance)
        {
            best = (int)candidate;
            bestDistance = distance;
        }
    }
}

internal static class CameraViewPlacementPolicy
{
    internal static CameraViewPixelBounds ClampRestoredToWorkArea(
        CameraViewPixelBounds restored,
        CameraViewPixelBounds nearestWorkArea,
        uint dpi)
    {
        dpi = Math.Max(96u, dpi);
        return CameraViewSnapPolicy.ClampReachable(
            restored,
            nearestWorkArea,
            ScaleDipToPixels(96.0, dpi),
            ScaleDipToPixels(48.0, dpi));
    }

    private static int ScaleDipToPixels(double dip, uint dpi) =>
        checked((int)Math.Min(
            int.MaxValue,
            Math.Max(
                1.0,
                Math.Round(
                    dip * dpi / 96.0,
                    MidpointRounding.AwayFromZero))));
}

internal sealed class CameraViewPlacementState
{
    internal const int CurrentVersion = 1;

    public int Version { get; set; } = CurrentVersion;
    public double Left { get; set; }
    public double Top { get; set; }
    public double Width { get; set; } = 720.0;
    public double Height { get; set; } = 480.0;
    public string StableCameraId { get; set; } = "";
}

/// <summary>
/// Bounded, versioned, replace-atomic persistence for the one live detached
/// camera viewport. Scene state and active-camera state are deliberately absent.
/// </summary>
internal static class CameraViewPlacementStore
{
    private const long MaximumFileBytes = 16 * 1024;
    private const double MinimumWidth = 420.0;
    private const double MinimumHeight = 280.0;
    private const double MaximumWidth = 7680.0;
    private const double MaximumHeight = 4320.0;

    private static string PlacementPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AcsEditor",
        $"CameraViewLayout.v{CameraViewPlacementState.CurrentVersion}.json");

    internal static CameraViewPlacementState Load(
        Rect virtualScreen,
        Rect fallback,
        out string warning)
    {
        warning = "";
        try
        {
            if (!File.Exists(PlacementPath))
                return Normalize(new CameraViewPlacementState(), virtualScreen, fallback);
            if (new FileInfo(PlacementPath).Length > MaximumFileBytes)
                throw new InvalidDataException("camera-view layout file is too large");
            CameraViewPlacementState? state =
                JsonSerializer.Deserialize<CameraViewPlacementState>(
                    File.ReadAllText(PlacementPath));
            if (state == null ||
                state.Version != CameraViewPlacementState.CurrentVersion)
            {
                return Normalize(new CameraViewPlacementState(), virtualScreen, fallback);
            }
            return Normalize(state, virtualScreen, fallback);
        }
        catch (Exception error)
        {
            warning = error.Message;
            return Normalize(new CameraViewPlacementState(), virtualScreen, fallback);
        }
    }

    internal static bool TrySave(
        CameraViewPlacementState state,
        out string warning)
    {
        ArgumentNullException.ThrowIfNull(state);
        warning = "";
        string? temporary = null;
        try
        {
            string directory = Path.GetDirectoryName(PlacementPath)!;
            Directory.CreateDirectory(directory);
            temporary =
                PlacementPath +
                $".{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
            string json = JsonSerializer.Serialize(
                state,
                new JsonSerializerOptions { WriteIndented = true });
            byte[] payload = new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false,
                throwOnInvalidBytes: true).GetBytes(json);
            if (payload.LongLength > MaximumFileBytes)
                throw new InvalidDataException(
                    "camera-view layout payload is too large");

            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       bufferSize: 4096,
                       FileOptions.WriteThrough))
            {
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }

            // The temporary file lives beside the destination, so both
            // File.Replace and the first-create rename stay on one volume.
            if (File.Exists(PlacementPath))
                File.Replace(temporary, PlacementPath, null);
            else
                File.Move(temporary, PlacementPath);
            temporary = null;
            return true;
        }
        catch (Exception error)
        {
            warning = error.Message;
            return false;
        }
        finally
        {
            try
            {
                if (temporary != null && File.Exists(temporary))
                    File.Delete(temporary);
            }
            catch
            {
                // A stale temp file is harmless and never read as committed state.
            }
        }
    }

    internal static CameraViewPlacementState Normalize(
        CameraViewPlacementState state,
        Rect virtualScreen,
        Rect fallback)
    {
        ArgumentNullException.ThrowIfNull(state);
        virtualScreen = ValidRect(virtualScreen)
            ? virtualScreen
            : new Rect(0.0, 0.0, 1920.0, 1080.0);
        fallback = ValidRect(fallback)
            ? fallback
            : new Rect(96.0, 72.0, 720.0, 480.0);
        double fallbackWidth =
            ClampFinite(fallback.Width, MinimumWidth, MaximumWidth, 720.0);
        double fallbackHeight =
            ClampFinite(fallback.Height, MinimumHeight, MaximumHeight, 480.0);
        double width =
            ClampFinite(state.Width, MinimumWidth, MaximumWidth, fallbackWidth);
        double height =
            ClampFinite(state.Height, MinimumHeight, MaximumHeight, fallbackHeight);
        double left = double.IsFinite(state.Left) ? state.Left : fallback.Left;
        double top = double.IsFinite(state.Top) ? state.Top : fallback.Top;

        var candidate = new Rect(left, top, width, height);
        Rect visible = Rect.Intersect(candidate, virtualScreen);
        if (virtualScreen.IsEmpty ||
            visible.IsEmpty ||
            visible.Width < 96.0 ||
            visible.Height < 48.0)
        {
            left = fallback.Left;
            top = fallback.Top;
        }

        return new CameraViewPlacementState
        {
            Left = left,
            Top = top,
            Width = width,
            Height = height,
            StableCameraId =
                CameraAuthoringContract.IsValidStableCameraId(
                    state.StableCameraId)
                    ? state.StableCameraId
                    : "",
        };
    }

    private static double ClampFinite(
        double value,
        double minimum,
        double maximum,
        double fallback) =>
        double.IsFinite(value)
            ? Math.Clamp(value, minimum, maximum)
            : fallback;

    private static bool ValidRect(Rect value) =>
        !value.IsEmpty &&
        double.IsFinite(value.Left) &&
        double.IsFinite(value.Top) &&
        double.IsFinite(value.Width) &&
        double.IsFinite(value.Height) &&
        value.Width > 0.0 &&
        value.Height > 0.0;
}
