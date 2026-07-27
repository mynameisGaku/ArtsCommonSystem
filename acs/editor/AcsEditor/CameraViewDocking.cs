// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;

namespace AcsEditor;

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
