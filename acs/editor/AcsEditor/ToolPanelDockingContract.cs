// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;

namespace AcsEditor;

internal enum ToolPanelDockState
{
    Docked,
    Floating,
    Hidden,
}

internal sealed record ToolPanelDockingDescriptor(
    string PanelId,
    string AccessibleName);

internal enum ToolPanelUserAction
{
    Show,
    Hide,
    Float,
    Redock,
}

internal sealed record ToolPanelUserActionDescriptor(
    ToolPanelUserAction Action,
    string CommandSuffix,
    string Verb);

/// <summary>
/// Explicit registry for editor tool panels that may leave the main window.
/// Adding a new panel is an intentional contract change; unknown IDs fail closed.
/// </summary>
internal static class ToolPanelDockingContract
{
    internal const string HierarchyPanelId = "hierarchy";
    internal const string InspectorPanelId = "inspector";
    internal const string ConsolePanelId = "console";
    internal const string BuildPanelId = "build";
    internal const string AssetsPanelId = "assets";
    internal const string ProfilerPanelId = "profiler";

    private static readonly IReadOnlyList<ToolPanelDockingDescriptor>
        Registered = Array.AsReadOnly(
            new ToolPanelDockingDescriptor[]
            {
                new(HierarchyPanelId, "Scene Outliner"),
                new(InspectorPanelId, "Details"),
                new(ConsolePanelId, "Console"),
                new(BuildPanelId, "Build"),
                new(AssetsPanelId, "Assets"),
                new(ProfilerPanelId, "Profiler"),
            });

    private static readonly IReadOnlyList<string> BottomTools =
        Array.AsReadOnly(
            new string[]
            {
                ConsolePanelId,
                BuildPanelId,
                AssetsPanelId,
                ProfilerPanelId,
            });

    private static readonly IReadOnlyList<ToolPanelUserActionDescriptor>
        UserActions = Array.AsReadOnly(
            new ToolPanelUserActionDescriptor[]
            {
                new(ToolPanelUserAction.Show, "show", "Show"),
                new(ToolPanelUserAction.Hide, "hide", "Hide"),
                new(ToolPanelUserAction.Float, "float", "Float"),
                new(ToolPanelUserAction.Redock, "redock", "Re-dock"),
            });

    internal static IReadOnlyList<ToolPanelDockingDescriptor> RegisteredPanels =>
        Registered;

    internal static IReadOnlyList<string> BottomToolPanelIds => BottomTools;

    internal static IReadOnlyList<ToolPanelUserActionDescriptor>
        RegisteredUserActions => UserActions;

    internal static bool IsBottomToolPanelId(string? panelId) =>
        panelId != null &&
        BottomTools.Contains(panelId, StringComparer.Ordinal);

    internal static bool IsKnownPanelId(string? panelId) =>
        panelId != null &&
        Registered.Any(panel =>
            string.Equals(panel.PanelId, panelId, StringComparison.Ordinal));

    internal static ToolPanelDockState ResolveState(
        bool floating,
        bool dockVisible) =>
        floating
            ? ToolPanelDockState.Floating
            : dockVisible
                ? ToolPanelDockState.Docked
                : ToolPanelDockState.Hidden;

    internal static bool CanOwnSingleVisual(
        bool contentInDock,
        bool contentInFloat) =>
        contentInDock ^ contentInFloat;

    internal static ToolPanelDockState AfterFloatTransfer(
        bool detachSucceeded,
        bool attachSucceeded) =>
        detachSucceeded && attachSucceeded
            ? ToolPanelDockState.Floating
            : ToolPanelDockState.Docked;

    internal static ToolPanelDockState AfterRedockTransfer(
        bool detachSucceeded,
        bool attachSucceeded) =>
        detachSucceeded && attachSucceeded
            ? ToolPanelDockState.Docked
            : ToolPanelDockState.Floating;

    internal static bool TryGetDescriptor(
        string? panelId,
        out ToolPanelDockingDescriptor descriptor)
    {
        descriptor = Registered.FirstOrDefault(panel =>
            string.Equals(panel.PanelId, panelId, StringComparison.Ordinal))!;
        return descriptor != null;
    }

    internal static bool CanExecuteUserAction(
        ToolPanelDockState state,
        ToolPanelUserAction action)
    {
        if (!Enum.IsDefined(state) || !Enum.IsDefined(action))
            return false;
        return action switch
        {
            ToolPanelUserAction.Show =>
                state == ToolPanelDockState.Hidden,
            ToolPanelUserAction.Hide =>
                state is ToolPanelDockState.Docked or
                    ToolPanelDockState.Floating,
            ToolPanelUserAction.Float =>
                state is ToolPanelDockState.Docked or
                    ToolPanelDockState.Hidden,
            ToolPanelUserAction.Redock =>
                state == ToolPanelDockState.Floating,
            _ => false,
        };
    }

    internal static string PaletteCommandId(
        string panelId,
        ToolPanelUserAction action)
    {
        if (!IsKnownPanelId(panelId))
            throw new ArgumentOutOfRangeException(nameof(panelId));
        ToolPanelUserActionDescriptor descriptor =
            UserActions.FirstOrDefault(candidate =>
                candidate.Action == action) ??
            throw new ArgumentOutOfRangeException(nameof(action));
        return $"view.panel.{panelId}.{descriptor.CommandSuffix}";
    }

    internal static bool TryParseMenuActionTag(
        string? tag,
        out string panelId,
        out ToolPanelUserAction action)
    {
        panelId = string.Empty;
        action = default;
        const string Prefix = "view.panel.";
        if (tag == null ||
            !tag.StartsWith(Prefix, StringComparison.Ordinal))
        {
            return false;
        }

        string payload = tag[Prefix.Length..];
        int separator = payload.LastIndexOf('.');
        if (separator <= 0 || separator == payload.Length - 1)
            return false;
        string candidatePanelId = payload[..separator];
        string suffix = payload[(separator + 1)..];
        ToolPanelUserActionDescriptor? descriptor =
            UserActions.FirstOrDefault(candidate =>
                string.Equals(
                    candidate.CommandSuffix,
                    suffix,
                    StringComparison.Ordinal));
        if (!IsKnownPanelId(candidatePanelId) || descriptor == null)
            return false;
        panelId = candidatePanelId;
        action = descriptor.Action;
        return true;
    }

    internal static Rect NormalizePlacementRect(
        Rect placement,
        Rect fallback)
    {
        fallback = IsValidRect(fallback)
            ? fallback
            : new Rect(96.0, 72.0, 720.0, 480.0);
        if (!IsValidRect(placement))
            placement = fallback;
        return new Rect(
            double.IsFinite(placement.Left) ? placement.Left : fallback.Left,
            double.IsFinite(placement.Top) ? placement.Top : fallback.Top,
            ClampFinite(placement.Width, 240.0, 7680.0, fallback.Width),
            ClampFinite(placement.Height, 160.0, 4320.0, fallback.Height));
    }

    private static bool IsValidRect(Rect value) =>
        !value.IsEmpty &&
        double.IsFinite(value.Left) &&
        double.IsFinite(value.Top) &&
        double.IsFinite(value.Width) &&
        double.IsFinite(value.Height) &&
        value.Width > 0.0 &&
        value.Height > 0.0;

    private static double ClampFinite(
        double value,
        double minimum,
        double maximum,
        double fallback) =>
        double.IsFinite(value)
            ? Math.Clamp(value, minimum, maximum)
            : Math.Clamp(fallback, minimum, maximum);
}

internal readonly record struct ToolPanelDockTransition(
    ToolPanelDockState State,
    bool Committed);

internal static class ToolPanelDockTransitionPolicy
{
    internal static bool RequiresVisualTransfer(
        ToolPanelDockState current,
        ToolPanelDockState requested) =>
        current != requested &&
        ((current == ToolPanelDockState.Floating) !=
         (requested == ToolPanelDockState.Floating));

    /// <summary>
    /// A failed detach remains docked; a failed re-dock remains floating.
    /// Hidden is owned by the main window and therefore re-docks first.
    /// </summary>
    internal static ToolPanelDockTransition AfterTransferAttempt(
        ToolPanelDockState current,
        ToolPanelDockState requested,
        bool transferSucceeded)
    {
        if (!Enum.IsDefined(current) || !Enum.IsDefined(requested))
            return new ToolPanelDockTransition(current, false);
        if (!RequiresVisualTransfer(current, requested) || transferSucceeded)
            return new ToolPanelDockTransition(requested, true);
        return new ToolPanelDockTransition(current, false);
    }
}

internal static class ToolPanelWindowPolicy
{
    // Floating diagnostics must not steal editor/game input when opened.
    internal const bool ShowActivatedOnFloat = false;
    internal const bool IsTopmost = false;
    internal const bool RequiresExplicitDockAction = true;

    internal static bool MayCompleteOwnerClose(bool redockSucceeded) =>
        redockSucceeded;
}

internal static class BottomToolDockSelectionPolicy
{
    /// <summary>
    /// Keeps the requested workspace tab when it is docked, otherwise chooses
    /// the first docked tool in contract order. When every tool is floating or
    /// hidden, the requested stable ID is retained for the next restore.
    /// </summary>
    internal static string ResolveActivePanelId(
        string? requestedPanelId,
        IReadOnlyCollection<string> dockedPanelIds)
    {
        ArgumentNullException.ThrowIfNull(dockedPanelIds);
        string requested =
            ToolPanelDockingContract.IsBottomToolPanelId(requestedPanelId)
                ? requestedPanelId!
                : ToolPanelDockingContract.ConsolePanelId;
        if (dockedPanelIds.Contains(requested, StringComparer.Ordinal))
            return requested;
        return ToolPanelDockingContract.BottomToolPanelIds.FirstOrDefault(
                   panelId =>
                       dockedPanelIds.Contains(
                           panelId,
                           StringComparer.Ordinal)) ??
               requested;
    }
}

/// <summary>
/// Presentation-only visibility for the aggregate bottom dock. Collapsing the
/// dock must not rewrite the Docked/Floating/Hidden state of any child tool.
/// </summary>
internal sealed class BottomToolDockVisibilityState
{
    internal bool IsSuppressed { get; private set; }

    internal bool IsVisible(int dockedToolCount) =>
        !IsSuppressed && dockedToolCount > 0;

    internal void SetVisible(bool visible) =>
        IsSuppressed = !visible;
}

internal static class ToolPanelWorkspaceMutationPolicy
{
    internal static bool ShouldMarkCustomized(
        bool restoreCompleted,
        bool persistenceSuppressed) =>
        restoreCompleted && !persistenceSuppressed;

    internal static bool ShouldMarkPublishedTransition(
        bool committedTransition,
        bool userMutationInProgress,
        bool restoreCompleted,
        bool persistenceSuppressed) =>
        committedTransition &&
        !userMutationInProgress &&
        ShouldMarkCustomized(restoreCompleted, persistenceSuppressed);

    internal static bool ApplyUserMutation(
        Func<bool> applyMutation,
        Action markCustomized)
    {
        ArgumentNullException.ThrowIfNull(applyMutation);
        ArgumentNullException.ThrowIfNull(markCustomized);
        if (!applyMutation())
            return false;
        markCustomized();
        return true;
    }
}

internal static class ToolPanelResetTransactionPolicy
{
    internal static bool CanCommitDefaults(
        IReadOnlyList<bool> resetResults)
    {
        ArgumentNullException.ThrowIfNull(resetResults);
        if (resetResults.Count !=
            ToolPanelDockingContract.RegisteredPanels.Count)
        {
            return false;
        }
        for (int index = 0; index < resetResults.Count; index++)
        {
            if (!resetResults[index])
                return false;
        }
        return true;
    }

    internal static ToolPanelDockState DesiredFinalState(
        ToolPanelDockState initialState,
        bool commitDefaults)
    {
        if (!Enum.IsDefined(initialState))
            return initialState;
        return commitDefaults
            ? ToolPanelDockState.Docked
            : initialState;
    }

    internal static bool HasRestoredInitialState(
        ToolPanelDockState initialState,
        ToolPanelDockState actualState) =>
        Enum.IsDefined(initialState) &&
        Enum.IsDefined(actualState) &&
        initialState == actualState;
}

internal static class ToolPanelOwnerCloseTransactionPolicy
{
    internal const double PlacementToleranceDip = 0.75;

    internal static bool HasRestoredState(
        DockableToolHostSnapshot requested,
        ToolPanelDockState actualState) =>
        ToolPanelDockingContract.IsKnownPanelId(requested.PanelId) &&
        Enum.IsDefined(requested.State) &&
        requested.State == actualState;

    internal static bool HasRestoredSnapshot(
        DockableToolHostSnapshot requested,
        DockableToolHostSnapshot actual) =>
        string.Equals(
            requested.PanelId,
            actual.PanelId,
            StringComparison.Ordinal) &&
        HasRestoredState(requested, actual.State) &&
        PlacementMatches(requested.Placement, actual.Placement);

    private static bool PlacementMatches(Rect requested, Rect actual) =>
        IsFinite(requested) &&
        IsFinite(actual) &&
        Math.Abs(requested.Left - actual.Left) <= PlacementToleranceDip &&
        Math.Abs(requested.Top - actual.Top) <= PlacementToleranceDip &&
        Math.Abs(requested.Width - actual.Width) <= PlacementToleranceDip &&
        Math.Abs(requested.Height - actual.Height) <= PlacementToleranceDip;

    private static bool IsFinite(Rect value) =>
        double.IsFinite(value.Left) &&
        double.IsFinite(value.Top) &&
        double.IsFinite(value.Width) &&
        double.IsFinite(value.Height) &&
        value.Width > 0.0 &&
        value.Height > 0.0;

    internal static bool MustRollbackPending(
        bool hasPendingSnapshot,
        bool closeAlreadyCancelled,
        bool auxiliaryCloseSucceeded) =>
        hasPendingSnapshot &&
        (closeAlreadyCancelled || !auxiliaryCloseSucceeded);

    internal static bool MayCommitPending(
        bool hasPendingSnapshot,
        bool closeAlreadyCancelled,
        bool auxiliaryCloseSucceeded) =>
        hasPendingSnapshot &&
        !closeAlreadyCancelled &&
        auxiliaryCloseSucceeded;
}

internal readonly record struct ToolWindowPixelBounds(
    int Left,
    int Top,
    int Width,
    int Height)
{
    internal bool IsValid => Width > 0 && Height > 0;
    internal long Right => (long)Left + Width;
    internal long Bottom => (long)Top + Height;
}

internal static class ToolPanelSnapPolicy
{
    internal const double SnapDistanceDip = 12.0;

    internal static int ThresholdPixels(uint dpi) =>
        Math.Max(1, checked((int)Math.Min(
            int.MaxValue,
            Math.Round(
                SnapDistanceDip * Math.Max(96u, dpi) / 96.0,
                MidpointRounding.AwayFromZero))));

    internal static ToolWindowPixelBounds Snap(
        ToolWindowPixelBounds moving,
        ToolWindowPixelBounds owner,
        ToolWindowPixelBounds workArea,
        int thresholdPixels)
    {
        if (!moving.IsValid || thresholdPixels < 0)
            return moving;

        int left = moving.Left;
        int top = moving.Top;
        long bestLeftDistance = (long)thresholdPixels + 1L;
        long bestTopDistance = (long)thresholdPixels + 1L;

        if (owner.IsValid)
        {
            Consider(moving.Left, thresholdPixels, owner.Left,
                ref left, ref bestLeftDistance);
            Consider(moving.Left, thresholdPixels, owner.Right - moving.Width,
                ref left, ref bestLeftDistance);
            Consider(moving.Left, thresholdPixels, (long)owner.Left - moving.Width,
                ref left, ref bestLeftDistance);
            Consider(moving.Left, thresholdPixels, owner.Right,
                ref left, ref bestLeftDistance);
            Consider(moving.Top, thresholdPixels, owner.Top,
                ref top, ref bestTopDistance);
            Consider(moving.Top, thresholdPixels, owner.Bottom - moving.Height,
                ref top, ref bestTopDistance);
            Consider(moving.Top, thresholdPixels, (long)owner.Top - moving.Height,
                ref top, ref bestTopDistance);
            Consider(moving.Top, thresholdPixels, owner.Bottom,
                ref top, ref bestTopDistance);
        }

        if (workArea.IsValid)
        {
            Consider(moving.Left, thresholdPixels, workArea.Left,
                ref left, ref bestLeftDistance);
            Consider(moving.Left, thresholdPixels, workArea.Right - moving.Width,
                ref left, ref bestLeftDistance);
            Consider(moving.Top, thresholdPixels, workArea.Top,
                ref top, ref bestTopDistance);
            Consider(moving.Top, thresholdPixels, workArea.Bottom - moving.Height,
                ref top, ref bestTopDistance);
        }

        return moving with { Left = left, Top = top };
    }

    internal static ToolWindowPixelBounds ClampReachable(
        ToolWindowPixelBounds moving,
        ToolWindowPixelBounds workArea,
        int minimumVisibleWidth,
        int minimumVisibleHeight)
    {
        if (!moving.IsValid || !workArea.IsValid ||
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
        if (!TryClampCoordinate(
                moving.Left,
                minimumLeft,
                maximumLeft,
                out int left) ||
            !TryClampCoordinate(
                moving.Top,
                minimumTop,
                maximumTop,
                out int top))
        {
            return moving;
        }
        return moving with
        {
            Left = left,
            Top = top,
        };
    }

    private static void Consider(
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

    private static bool TryClampCoordinate(
        int value,
        long minimum,
        long maximum,
        out int result)
    {
        minimum = Math.Max(minimum, int.MinValue);
        maximum = Math.Min(maximum, int.MaxValue);
        if (minimum > maximum)
        {
            result = value;
            return false;
        }
        result = (int)Math.Clamp((long)value, minimum, maximum);
        return true;
    }
}

internal readonly record struct ToolWindowDipBounds(
    double Left,
    double Top,
    double Width,
    double Height)
{
    internal bool IsValid =>
        double.IsFinite(Left) &&
        double.IsFinite(Top) &&
        double.IsFinite(Width) &&
        double.IsFinite(Height) &&
        Width > 0.0 &&
        Height > 0.0;
}

internal sealed class ToolPanelPlacementState
{
    internal const int CurrentVersion = 2;

    public int Version { get; set; } = CurrentVersion;
    public string PanelId { get; set; } = "";
    public ToolPanelDockState State { get; set; } = ToolPanelDockState.Docked;
    public double Left { get; set; }
    public double Top { get; set; }
    public double Width { get; set; } = 420.0;
    public double Height { get; set; } = 320.0;
}

internal static class ToolPanelPlacementPolicy
{
    private const double MinimumWidth = 240.0;
    private const double MinimumHeight = 160.0;
    private const double MaximumWidth = 7680.0;
    private const double MaximumHeight = 4320.0;

    internal static ToolWindowPixelBounds ClampRestoredToWorkArea(
        ToolWindowPixelBounds restored,
        ToolWindowPixelBounds nearestWorkArea,
        uint dpi) =>
        ToolPanelSnapPolicy.ClampReachable(
            restored,
            nearestWorkArea,
            ScaleDipToPixels(96.0, dpi),
            ScaleDipToPixels(48.0, dpi));

    internal static bool TryNormalizeForRestore(
        ToolPanelPlacementState? state,
        ToolWindowDipBounds virtualScreen,
        ToolWindowDipBounds fallback,
        out ToolPanelPlacementState normalized)
    {
        normalized = new ToolPanelPlacementState();
        if (state == null ||
            state.Version != ToolPanelPlacementState.CurrentVersion ||
            !ToolPanelDockingContract.IsKnownPanelId(state.PanelId) ||
            !Enum.IsDefined(state.State))
        {
            return false;
        }

        virtualScreen = virtualScreen.IsValid
            ? virtualScreen
            : new ToolWindowDipBounds(0.0, 0.0, 1920.0, 1080.0);
        fallback = fallback.IsValid
            ? fallback
            : new ToolWindowDipBounds(96.0, 72.0, 420.0, 320.0);
        double width = ClampFinite(
            state.Width, MinimumWidth, MaximumWidth,
            Math.Clamp(fallback.Width, MinimumWidth, MaximumWidth));
        double height = ClampFinite(
            state.Height, MinimumHeight, MaximumHeight,
            Math.Clamp(fallback.Height, MinimumHeight, MaximumHeight));
        double left = double.IsFinite(state.Left) ? state.Left : fallback.Left;
        double top = double.IsFinite(state.Top) ? state.Top : fallback.Top;

        if (!HasReachableTitle(
                new ToolWindowDipBounds(left, top, width, height),
                virtualScreen))
        {
            left = fallback.Left;
            top = fallback.Top;
        }

        normalized = new ToolPanelPlacementState
        {
            PanelId = state.PanelId,
            State = state.State,
            Left = left,
            Top = top,
            Width = width,
            Height = height,
        };
        return true;
    }

    internal static bool HasUniqueKnownPanelIds(
        IEnumerable<ToolPanelPlacementState?> states)
    {
        ArgumentNullException.ThrowIfNull(states);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        int count = 0;
        foreach (ToolPanelPlacementState? state in states)
        {
            if (state == null ||
                !ToolPanelDockingContract.IsKnownPanelId(state.PanelId) ||
                !seen.Add(state.PanelId) ||
                ++count > ToolPanelDockingContract.RegisteredPanels.Count)
            {
                return false;
            }
        }
        return true;
    }

    private static bool HasReachableTitle(
        ToolWindowDipBounds candidate,
        ToolWindowDipBounds virtualScreen)
    {
        double right = Math.Min(
            candidate.Left + candidate.Width,
            virtualScreen.Left + virtualScreen.Width);
        double bottom = Math.Min(
            candidate.Top + candidate.Height,
            virtualScreen.Top + virtualScreen.Height);
        double left = Math.Max(candidate.Left, virtualScreen.Left);
        double top = Math.Max(candidate.Top, virtualScreen.Top);
        return right - left >= 96.0 && bottom - top >= 48.0;
    }

    private static double ClampFinite(
        double value,
        double minimum,
        double maximum,
        double fallback) =>
        double.IsFinite(value)
            ? Math.Clamp(value, minimum, maximum)
            : fallback;

    private static int ScaleDipToPixels(double dip, uint dpi) =>
        checked((int)Math.Min(
            int.MaxValue,
            Math.Max(
                1.0,
                Math.Round(
                    dip * Math.Max(96u, dpi) / 96.0,
                    MidpointRounding.AwayFromZero))));
}
