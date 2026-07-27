// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal static class ToolPanelDockingSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passes = 0;
        int failures = 0;

        void Check(bool condition, string name)
        {
            if (condition)
            {
                passes++;
                output.WriteLine($"PASS: {name}");
            }
            else
            {
                failures++;
                output.WriteLine($"FAIL: {name}");
            }
        }

        ToolPanelDockingDescriptor[] registered =
            ToolPanelDockingContract.RegisteredPanels.ToArray();
        Check(
            registered.Select(panel => panel.PanelId).SequenceEqual(
                new[]
                {
                    ToolPanelDockingContract.HierarchyPanelId,
                    ToolPanelDockingContract.InspectorPanelId,
                    ToolPanelDockingContract.BottomPanelId,
                }),
            "tool-panel docking uses an explicit stable-ID registry");
        Check(
            registered.All(panel =>
                !string.IsNullOrWhiteSpace(panel.AccessibleName) &&
                !panel.AccessibleName.Any(char.IsControl)) &&
            registered.Select(panel => panel.AccessibleName).Distinct(
                StringComparer.Ordinal).Count() == registered.Length,
            "registered tool panels expose distinct accessible names");
        Check(
            ToolPanelDockingContract.IsKnownPanelId("hierarchy") &&
            !ToolPanelDockingContract.IsKnownPanelId("Hierarchy") &&
            !ToolPanelDockingContract.IsKnownPanelId(" hierarchy") &&
            !ToolPanelDockingContract.IsKnownPanelId("camera") &&
            !ToolPanelDockingContract.IsKnownPanelId(null),
            "unknown, aliased, and non-canonical panel IDs fail closed");
        Check(
            ToolPanelDockingContract.TryGetDescriptor(
                "inspector",
                out ToolPanelDockingDescriptor details) &&
            details.AccessibleName == "Details" &&
            !ToolPanelDockingContract.TryGetDescriptor("unknown", out _),
            "descriptor lookup is deterministic and rejects unknown IDs");

        Check(
            ToolPanelDockingContract.ResolveState(
                floating: false,
                dockVisible: true) == ToolPanelDockState.Docked &&
            ToolPanelDockingContract.ResolveState(
                floating: true,
                dockVisible: false) == ToolPanelDockState.Floating &&
            ToolPanelDockingContract.ResolveState(
                floating: false,
                dockVisible: false) == ToolPanelDockState.Hidden,
            "dock state resolves without ambiguous floating-hidden ownership");
        Check(
            ToolPanelDockingContract.CanOwnSingleVisual(true, false) &&
            ToolPanelDockingContract.CanOwnSingleVisual(false, true) &&
            !ToolPanelDockingContract.CanOwnSingleVisual(true, true) &&
            !ToolPanelDockingContract.CanOwnSingleVisual(false, false),
            "a committed tool panel has exactly one visual owner");
        Check(
            ToolPanelDockingContract.AfterFloatTransfer(true, true) ==
                ToolPanelDockState.Floating &&
            ToolPanelDockingContract.AfterFloatTransfer(true, false) ==
                ToolPanelDockState.Docked &&
            ToolPanelDockingContract.AfterFloatTransfer(false, true) ==
                ToolPanelDockState.Docked,
            "failed float transfer reports docked rollback");
        Check(
            ToolPanelDockingContract.AfterRedockTransfer(true, true) ==
                ToolPanelDockState.Docked &&
            ToolPanelDockingContract.AfterRedockTransfer(true, false) ==
                ToolPanelDockState.Floating &&
            ToolPanelDockingContract.AfterRedockTransfer(false, true) ==
                ToolPanelDockState.Floating,
            "failed re-dock transfer reports truthful floating ownership");

        ToolPanelDockTransition failedDetach =
            ToolPanelDockTransitionPolicy.AfterTransferAttempt(
                ToolPanelDockState.Docked,
                ToolPanelDockState.Floating,
                transferSucceeded: false);
        ToolPanelDockTransition failedRedock =
            ToolPanelDockTransitionPolicy.AfterTransferAttempt(
                ToolPanelDockState.Floating,
                ToolPanelDockState.Docked,
                transferSucceeded: false);
        Check(
            failedDetach is { State: ToolPanelDockState.Docked, Committed: false } &&
            failedRedock is { State: ToolPanelDockState.Floating, Committed: false },
            "generic transfer policy never lies after detach or re-dock failure");
        Check(
            ToolPanelDockTransitionPolicy.RequiresVisualTransfer(
                ToolPanelDockState.Floating,
                ToolPanelDockState.Hidden) &&
            !ToolPanelDockTransitionPolicy.RequiresVisualTransfer(
                ToolPanelDockState.Docked,
                ToolPanelDockState.Hidden),
            "hiding a floating panel re-docks it while docked hide stays local");
        Check(
            !ToolPanelWindowPolicy.ShowActivatedOnFloat &&
            !ToolPanelWindowPolicy.IsTopmost &&
            ToolPanelWindowPolicy.RequiresExplicitDockAction &&
            ToolPanelWindowPolicy.MayCompleteOwnerClose(
                redockSucceeded: true) &&
            !ToolPanelWindowPolicy.MayCompleteOwnerClose(
                redockSucceeded: false),
            "floating tools do not steal focus or topmost and block unsafe owner close");
        Check(
            ToolPanelResetTransactionPolicy.CanCommitDefaults(
                new[] { true, true, true }) &&
            !ToolPanelResetTransactionPolicy.CanCommitDefaults(
                new[] { true, false, true }) &&
            !ToolPanelResetTransactionPolicy.CanCommitDefaults(
                new[] { true, true }) &&
            !ToolPanelResetTransactionPolicy.CanCommitDefaults(
                Array.Empty<bool>()),
            "layout reset commits defaults only after every registered panel succeeds");
        Check(
            ToolPanelResetTransactionPolicy.DesiredFinalState(
                ToolPanelDockState.Floating,
                commitDefaults: false) == ToolPanelDockState.Floating &&
            ToolPanelResetTransactionPolicy.DesiredFinalState(
                ToolPanelDockState.Hidden,
                commitDefaults: false) == ToolPanelDockState.Hidden &&
            ToolPanelResetTransactionPolicy.DesiredFinalState(
                ToolPanelDockState.Floating,
                commitDefaults: true) == ToolPanelDockState.Docked,
            "failed reset targets each panel's exact starting state");
        Check(
            ToolPanelResetTransactionPolicy.HasRestoredInitialState(
                ToolPanelDockState.Floating,
                ToolPanelDockState.Floating) &&
            ToolPanelResetTransactionPolicy.HasRestoredInitialState(
                ToolPanelDockState.Hidden,
                ToolPanelDockState.Hidden) &&
            !ToolPanelResetTransactionPolicy.HasRestoredInitialState(
                ToolPanelDockState.Floating,
                ToolPanelDockState.Docked),
            "reset rollback completion is validated rather than assumed");

        Check(
            ToolPanelSnapPolicy.ThresholdPixels(96) == 12 &&
            ToolPanelSnapPolicy.ThresholdPixels(144) == 18 &&
            ToolPanelSnapPolicy.ThresholdPixels(192) == 24 &&
            ToolPanelSnapPolicy.ThresholdPixels(0) == 12,
            "snap distance is 12 DIP at per-monitor DPI");

        var owner = new ToolWindowPixelBounds(100, 100, 800, 600);
        var workArea = new ToolWindowPixelBounds(0, 0, 1920, 1040);
        var moving = new ToolWindowPixelBounds(108, 109, 320, 240);
        ToolWindowPixelBounds snapped =
            ToolPanelSnapPolicy.Snap(moving, owner, workArea, 12);
        Check(
            snapped.Left == owner.Left &&
            snapped.Top == owner.Top,
            "floating tool panel snaps to an owner corner");
        Check(
            ToolPanelSnapPolicy.Snap(
                new ToolWindowPixelBounds(
                    owner.Left - 320 + 9,
                    checked((int)(owner.Bottom + 11)),
                    320,
                    240),
                owner,
                workArea,
                12) is
                { Left: -220, Top: 700 },
            "floating tool panel snaps outside adjacent owner edges");
        Check(
            ToolPanelSnapPolicy.Snap(
                new ToolWindowPixelBounds(7, 793, 320, 240),
                owner,
                workArea,
                12) is
                { Left: 0, Top: 800 },
            "floating tool panel snaps to monitor work-area edges");
        Check(
            ToolPanelSnapPolicy.Snap(
                new ToolWindowPixelBounds(912, 712, 320, 240),
                owner,
                workArea,
                12) is
                { Left: 900, Top: 700 } &&
            ToolPanelSnapPolicy.Snap(
                new ToolWindowPixelBounds(913, 713, 320, 240),
                owner,
                workArea,
                12) is
                { Left: 913, Top: 713 },
            "snap threshold is inclusive and leaves just-outside placement unchanged");
        Check(
            ToolPanelSnapPolicy.Snap(
                new ToolWindowPixelBounds(-1912, -1191, 400, 300),
                new ToolWindowPixelBounds(-1920, -1200, 1920, 1200),
                new ToolWindowPixelBounds(-1920, -1200, 1920, 1160),
                12) is
                { Left: -1920, Top: -1200 },
            "snap supports monitors with negative desktop coordinates");
        var invalid = new ToolWindowPixelBounds(50, 60, 0, 240);
        Check(
            ToolPanelSnapPolicy.Snap(invalid, owner, workArea, 12) == invalid &&
            ToolPanelSnapPolicy.Snap(moving, owner, workArea, -1) == moving,
            "invalid geometry and threshold fail safely without movement");

        ToolWindowPixelBounds reachable = ToolPanelSnapPolicy.ClampReachable(
            new ToolWindowPixelBounds(4000, -900, 600, 400),
            new ToolWindowPixelBounds(1920, 0, 1920, 1040),
            minimumVisibleWidth: 96,
            minimumVisibleHeight: 48);
        Check(
            reachable.Left == 3744 &&
            reachable.Top == 0,
            "restore keeps a reachable title region on the nearest work area");
        ToolWindowPixelBounds reachableNegative =
            ToolPanelSnapPolicy.ClampReachable(
                new ToolWindowPixelBounds(-5000, 2000, 640, 480),
                new ToolWindowPixelBounds(-1920, -1080, 1920, 1040),
                minimumVisibleWidth: 96,
                minimumVisibleHeight: 48);
        Check(
            reachableNegative.Left == -2464 &&
            reachableNegative.Top == -88,
            "reachable clamp handles disconnected negative-coordinate monitors");
        var hostile = new ToolWindowPixelBounds(40, 50, 320, 240);
        Check(
            ToolPanelSnapPolicy.ClampReachable(
                hostile,
                new ToolWindowPixelBounds(int.MinValue, int.MinValue, 1, 1),
                minimumVisibleWidth: 96,
                minimumVisibleHeight: 48) == hostile &&
            ToolPanelSnapPolicy.ClampReachable(
                hostile,
                new ToolWindowPixelBounds(
                    int.MaxValue - 1,
                    int.MaxValue - 1,
                    int.MaxValue,
                    int.MaxValue),
                minimumVisibleWidth: 96,
                minimumVisibleHeight: 48).Left >= int.MaxValue - 321,
            "hostile integer work areas fail safely without checked-cast overflow");

        var fallback = new ToolWindowDipBounds(80, 60, 640, 420);
        bool normalizedOk = ToolPanelPlacementPolicy.TryNormalizeForRestore(
            new ToolPanelPlacementState
            {
                PanelId = "bottom",
                State = ToolPanelDockState.Floating,
                Left = double.NaN,
                Top = 5000,
                Width = double.PositiveInfinity,
                Height = 10,
            },
            new ToolWindowDipBounds(-1920, 0, 3840, 1080),
            fallback,
            out ToolPanelPlacementState normalized);
        Check(
            normalizedOk &&
            normalized.PanelId == "bottom" &&
            normalized.State == ToolPanelDockState.Floating &&
            normalized.Left == fallback.Left &&
            normalized.Top == fallback.Top &&
            normalized.Width == fallback.Width &&
            normalized.Height == 160,
            "restore normalizes non-finite, undersized, and unreachable geometry");
        Check(
            !ToolPanelPlacementPolicy.TryNormalizeForRestore(
                new ToolPanelPlacementState
                {
                    PanelId = "future-panel",
                    State = ToolPanelDockState.Floating,
                },
                new ToolWindowDipBounds(0, 0, 1920, 1080),
                fallback,
                out _) &&
            !ToolPanelPlacementPolicy.TryNormalizeForRestore(
                new ToolPanelPlacementState
                {
                    Version = ToolPanelPlacementState.CurrentVersion + 1,
                    PanelId = "hierarchy",
                },
                new ToolWindowDipBounds(0, 0, 1920, 1080),
                fallback,
                out _),
            "restore rejects unknown panel IDs and unsupported versions");
        Check(
            ToolPanelPlacementPolicy.HasUniqueKnownPanelIds(
                new ToolPanelPlacementState?[]
                {
                    new() { PanelId = "hierarchy" },
                    new() { PanelId = "inspector" },
                    new() { PanelId = "bottom" },
                }) &&
            !ToolPanelPlacementPolicy.HasUniqueKnownPanelIds(
                new ToolPanelPlacementState?[]
                {
                    new() { PanelId = "hierarchy" },
                    new() { PanelId = "hierarchy" },
                }) &&
            !ToolPanelPlacementPolicy.HasUniqueKnownPanelIds(
                new ToolPanelPlacementState?[]
                {
                    new() { PanelId = "hierarchy" },
                    new() { PanelId = "unknown" },
                }),
            "persisted layouts reject duplicate and unknown panel records");

        output.WriteLine(
            $"Tool-panel docking self-test: {passes} PASS / {failures} failures");
        return failures;
    }
}
