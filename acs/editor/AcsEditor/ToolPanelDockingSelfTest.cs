// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
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
        ToolPanelUserActionDescriptor[] registeredActions =
            ToolPanelDockingContract.RegisteredUserActions.ToArray();
        Check(
            registered.Select(panel => panel.PanelId).SequenceEqual(
                new[]
                {
                    ToolPanelDockingContract.HierarchyPanelId,
                    ToolPanelDockingContract.InspectorPanelId,
                    ToolPanelDockingContract.ConsolePanelId,
                    ToolPanelDockingContract.BuildPanelId,
                    ToolPanelDockingContract.AssetsPanelId,
                    ToolPanelDockingContract.ProfilerPanelId,
                }),
            "tool-panel docking uses an explicit stable-ID registry");
        string[] registeredCommandIds = registered
            .SelectMany(panel => registeredActions.Select(action =>
                ToolPanelDockingContract.PaletteCommandId(
                    panel.PanelId,
                    action.Action)))
            .ToArray();
        Check(
            registeredActions.Select(action => action.Action).SequenceEqual(
                new[]
                {
                    ToolPanelUserAction.Show,
                    ToolPanelUserAction.Hide,
                    ToolPanelUserAction.Float,
                    ToolPanelUserAction.Redock,
                }) &&
            registeredCommandIds.Length ==
                registered.Length * registeredActions.Length &&
            registeredCommandIds.Distinct(StringComparer.Ordinal).Count() ==
                registeredCommandIds.Length &&
            registeredCommandIds.All(commandId =>
                ToolPanelDockingContract.TryParseMenuActionTag(
                    commandId,
                    out string panelId,
                    out ToolPanelUserAction action) &&
                ToolPanelDockingContract.PaletteCommandId(
                    panelId,
                    action) == commandId),
            "every stable-ID tool exposes unique Show, Hide, Float, and Re-dock commands");
        Check(
            ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Hidden,
                ToolPanelUserAction.Show) &&
            ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Hidden,
                ToolPanelUserAction.Float) &&
            !ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Hidden,
                ToolPanelUserAction.Hide) &&
            !ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Hidden,
                ToolPanelUserAction.Redock) &&
            ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Docked,
                ToolPanelUserAction.Hide) &&
            ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Docked,
                ToolPanelUserAction.Float) &&
            !ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Docked,
                ToolPanelUserAction.Show) &&
            !ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Docked,
                ToolPanelUserAction.Redock) &&
            ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Floating,
                ToolPanelUserAction.Hide) &&
            ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Floating,
                ToolPanelUserAction.Redock) &&
            !ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Floating,
                ToolPanelUserAction.Show) &&
            !ToolPanelDockingContract.CanExecuteUserAction(
                ToolPanelDockState.Floating,
                ToolPanelUserAction.Float),
            "tool actions fail closed unless their source state can commit the requested transition");
        Check(
            ToolPanelDockingContract.BottomToolPanelIds.SequenceEqual(
                new[]
                {
                    ToolPanelDockingContract.ConsolePanelId,
                    ToolPanelDockingContract.BuildPanelId,
                    ToolPanelDockingContract.AssetsPanelId,
                    ToolPanelDockingContract.ProfilerPanelId,
                }) &&
            ToolPanelDockingContract.BottomToolPanelIds.All(
                ToolPanelDockingContract.IsBottomToolPanelId) &&
            !ToolPanelDockingContract.IsBottomToolPanelId(
                ToolPanelDockingContract.HierarchyPanelId),
            "bottom tools keep individual stable IDs in deterministic tab order");
        Check(
            BottomToolDockSelectionPolicy.ResolveActivePanelId(
                ToolPanelDockingContract.AssetsPanelId,
                new[] { "console", "assets", "profiler" }) == "assets" &&
            BottomToolDockSelectionPolicy.ResolveActivePanelId(
                ToolPanelDockingContract.AssetsPanelId,
                new[] { "profiler", "build" }) == "build" &&
            BottomToolDockSelectionPolicy.ResolveActivePanelId(
                ToolPanelDockingContract.ProfilerPanelId,
                Array.Empty<string>()) == "profiler" &&
            BottomToolDockSelectionPolicy.ResolveActivePanelId(
                "unknown",
                Array.Empty<string>()) == "console",
            "bottom tab selection survives independent float, hide, and empty-dock states");
        var bottomDockVisibility = new BottomToolDockVisibilityState();
        string[] dockedBeforeAggregateHide =
        {
            ToolPanelDockingContract.ConsolePanelId,
            ToolPanelDockingContract.AssetsPanelId,
        };
        string activeBeforeAggregateHide =
            ToolPanelDockingContract.AssetsPanelId;
        bottomDockVisibility.SetVisible(visible: false);
        Check(
            bottomDockVisibility.IsSuppressed &&
            !bottomDockVisibility.IsVisible(
                dockedBeforeAggregateHide.Length) &&
            dockedBeforeAggregateHide.SequenceEqual(
                new[]
                {
                    ToolPanelDockingContract.ConsolePanelId,
                    ToolPanelDockingContract.AssetsPanelId,
                }) &&
            activeBeforeAggregateHide ==
                ToolPanelDockingContract.AssetsPanelId,
            "aggregate bottom-dock hide preserves child states and active tab");
        bottomDockVisibility.SetVisible(visible: true);
        Check(
            !bottomDockVisibility.IsSuppressed &&
            bottomDockVisibility.IsVisible(
                dockedBeforeAggregateHide.Length) &&
            !bottomDockVisibility.IsVisible(dockedToolCount: 0) &&
            dockedBeforeAggregateHide.SequenceEqual(
                new[]
                {
                    ToolPanelDockingContract.ConsolePanelId,
                    ToolPanelDockingContract.AssetsPanelId,
                }) &&
            activeBeforeAggregateHide ==
                ToolPanelDockingContract.AssetsPanelId,
            "aggregate bottom-dock restore is presentation-only and reversible");
        Check(
            !ToolPanelWorkspaceMutationPolicy.ShouldMarkCustomized(
                restoreCompleted: false,
                persistenceSuppressed: false) &&
            !ToolPanelWorkspaceMutationPolicy.ShouldMarkCustomized(
                restoreCompleted: true,
                persistenceSuppressed: true) &&
            ToolPanelWorkspaceMutationPolicy.ShouldMarkCustomized(
                restoreCompleted: true,
                persistenceSuppressed: false),
            "user tool-state mutations mark named workspaces only after restore");
        Check(
            !ToolPanelWorkspaceMutationPolicy.ShouldMarkPublishedTransition(
                committedTransition: false,
                userMutationInProgress: false,
                restoreCompleted: true,
                persistenceSuppressed: false) &&
            !ToolPanelWorkspaceMutationPolicy.ShouldMarkPublishedTransition(
                committedTransition: true,
                userMutationInProgress: true,
                restoreCompleted: true,
                persistenceSuppressed: false) &&
            !ToolPanelWorkspaceMutationPolicy.ShouldMarkPublishedTransition(
                committedTransition: true,
                userMutationInProgress: false,
                restoreCompleted: false,
                persistenceSuppressed: false) &&
            !ToolPanelWorkspaceMutationPolicy.ShouldMarkPublishedTransition(
                committedTransition: true,
                userMutationInProgress: false,
                restoreCompleted: true,
                persistenceSuppressed: true) &&
            ToolPanelWorkspaceMutationPolicy.ShouldMarkPublishedTransition(
                committedTransition: true,
                userMutationInProgress: false,
                restoreCompleted: true,
                persistenceSuppressed: false),
            "only an external committed transition may customize a restored workspace");
        var workspaceMutationOrder = new List<string>();
        bool committedMutation =
            ToolPanelWorkspaceMutationPolicy.ApplyUserMutation(
                () =>
                {
                    workspaceMutationOrder.Add("visibility");
                    return true;
                },
                () => workspaceMutationOrder.Add("custom"));
        int rejectedMutationMarks = 0;
        bool rejectedMutation =
            ToolPanelWorkspaceMutationPolicy.ApplyUserMutation(
                () => false,
                () => rejectedMutationMarks++);
        int exceptionalMutationMarks = 0;
        try
        {
            ToolPanelWorkspaceMutationPolicy.ApplyUserMutation(
                () => throw new InvalidOperationException("mutation failed"),
                () => exceptionalMutationMarks++);
        }
        catch (InvalidOperationException)
        {
            // A failed visibility mutation must not relabel the workspace.
        }
        Check(
            committedMutation &&
            !rejectedMutation &&
            workspaceMutationOrder.SequenceEqual(
                new[] { "visibility", "custom" }) &&
            rejectedMutationMarks == 0 &&
            exceptionalMutationMarks == 0,
            "user mutation helpers mark Custom only after a true committed result");
        int successfulStateMarks = 0;
        bool dockedSuccess =
            ToolPanelWorkspaceMutationPolicy.ApplyUserMutation(
                () => ToolPanelDockingContract.CanExecuteUserAction(
                    ToolPanelDockState.Docked,
                    ToolPanelUserAction.Hide),
                () => successfulStateMarks++);
        bool floatingSuccess =
            ToolPanelWorkspaceMutationPolicy.ApplyUserMutation(
                () => ToolPanelDockingContract.CanExecuteUserAction(
                    ToolPanelDockState.Floating,
                    ToolPanelUserAction.Redock),
                () => successfulStateMarks++);
        int failedFloatingMarks = 0;
        bool failedFloatingTransition =
            ToolPanelWorkspaceMutationPolicy.ApplyUserMutation(
                () => false,
                () => failedFloatingMarks++);
        Check(
            dockedSuccess &&
            floatingSuccess &&
            !failedFloatingTransition &&
            successfulStateMarks == 2 &&
            failedFloatingMarks == 0,
            "docked and floating successes customize once while failed re-dock does not");
        Check(
            !ToolPanelDockingContract.TryParseMenuActionTag(
                "view.panel.hierarchy.teleport",
                out _,
                out _) &&
            !ToolPanelDockingContract.TryParseMenuActionTag(
                "view.panel.unknown.show",
                out _,
                out _) &&
            !ToolPanelDockingContract.TryParseMenuActionTag(
                "VIEW.PANEL.HIERARCHY.SHOW",
                out _,
                out _) &&
            !ToolPanelDockingContract.TryParseMenuActionTag(
                null,
                out _,
                out _),
            "tool action tags reject unknown IDs, verbs, casing, and null");
        Check(
            ToolPanelStartupRestorePolicy.ShouldSeedCurrentLayout(
                loadedPersistedSnapshot: false) &&
            !ToolPanelStartupRestorePolicy.ShouldSeedCurrentLayout(
                loadedPersistedSnapshot: true),
            "startup seeds the restored legacy layout unless a complete v2 snapshot loaded");
        Check(
            registered.All(panel =>
                !string.IsNullOrWhiteSpace(panel.AccessibleName) &&
                !panel.AccessibleName.Any(char.IsControl)) &&
            registered.Select(panel => panel.AccessibleName).Distinct(
                StringComparer.Ordinal).Count() == registered.Length,
            "registered tool panels expose distinct accessible names");
        Check(
            ToolPanelDockingContract.IsKnownPanelId("hierarchy") &&
            ToolPanelDockingContract.IsKnownPanelId("console") &&
            ToolPanelDockingContract.IsKnownPanelId("build") &&
            ToolPanelDockingContract.IsKnownPanelId("assets") &&
            ToolPanelDockingContract.IsKnownPanelId("profiler") &&
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
                Enumerable.Repeat(true, registered.Length).ToArray()) &&
            !ToolPanelResetTransactionPolicy.CanCommitDefaults(
                Enumerable.Range(0, registered.Length)
                    .Select(index => index != registered.Length / 2)
                    .ToArray()) &&
            !ToolPanelResetTransactionPolicy.CanCommitDefaults(
                Enumerable.Repeat(true, registered.Length - 1).ToArray()) &&
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
        var ownerCloseSnapshot = new DockableToolHostSnapshot(
            ToolPanelDockingContract.AssetsPanelId,
            ToolPanelDockState.Floating,
            new System.Windows.Rect(80.0, 60.0, 900.0, 600.0));
        Check(
            ToolPanelOwnerCloseTransactionPolicy.HasRestoredState(
                ownerCloseSnapshot,
                ToolPanelDockState.Floating) &&
            !ToolPanelOwnerCloseTransactionPolicy.HasRestoredState(
                ownerCloseSnapshot,
                ToolPanelDockState.Docked),
            "owner-close rollback validates each requested panel state");
        Check(
            ToolPanelOwnerCloseTransactionPolicy.HasRestoredSnapshot(
                ownerCloseSnapshot,
                ownerCloseSnapshot with
                {
                    Placement = new System.Windows.Rect(
                        80.5,
                        59.5,
                        900.5,
                        599.5),
                }) &&
            !ToolPanelOwnerCloseTransactionPolicy.HasRestoredSnapshot(
                ownerCloseSnapshot,
                ownerCloseSnapshot with
                {
                    Placement = new System.Windows.Rect(
                        81.0,
                        60.0,
                        900.0,
                        600.0),
                }),
            "owner-close rollback validates placement with bounded DIP rounding tolerance");
        Check(
            ToolPanelOwnerCloseTransactionPolicy.MustRollbackPending(
                hasPendingSnapshot: true,
                closeAlreadyCancelled: true,
                auxiliaryCloseSucceeded: true) &&
            ToolPanelOwnerCloseTransactionPolicy.MustRollbackPending(
                hasPendingSnapshot: true,
                closeAlreadyCancelled: false,
                auxiliaryCloseSucceeded: false) &&
            !ToolPanelOwnerCloseTransactionPolicy.MustRollbackPending(
                hasPendingSnapshot: false,
                closeAlreadyCancelled: true,
                auxiliaryCloseSucceeded: false),
            "pending owner-close layout rolls back on cancellation or auxiliary failure");
        Check(
            ToolPanelOwnerCloseTransactionPolicy.MayCommitPending(
                hasPendingSnapshot: true,
                closeAlreadyCancelled: false,
                auxiliaryCloseSucceeded: true) &&
            !ToolPanelOwnerCloseTransactionPolicy.MayCommitPending(
                hasPendingSnapshot: true,
                closeAlreadyCancelled: true,
                auxiliaryCloseSucceeded: true) &&
            !ToolPanelOwnerCloseTransactionPolicy.MayCommitPending(
                hasPendingSnapshot: true,
                closeAlreadyCancelled: false,
                auxiliaryCloseSucceeded: false),
            "pending owner-close layout commits only after the final auxiliary surface");
        Check(
            !EditorCloseFinalizationPolicy.ShouldCancelAtEditorGate(
                documentsApproved: true,
                auxiliarySurfacesApproved: false,
                finalizationCompleted: false) &&
            EditorCloseFinalizationPolicy.ShouldCancelAtEditorGate(
                documentsApproved: true,
                auxiliarySurfacesApproved: true,
                finalizationCompleted: false) &&
            !EditorCloseFinalizationPolicy.ShouldCancelAtEditorGate(
                documentsApproved: true,
                auxiliarySurfacesApproved: true,
                finalizationCompleted: true) &&
            !EditorCloseFinalizationPolicy.ShouldCancelAtEditorGate(
                documentsApproved: false,
                auxiliarySurfacesApproved: true,
                finalizationCompleted: false),
            "editor close reaches auxiliary handlers once and waits for final cleanup");
        Check(
            EditorCloseFinalizationPolicy.ShouldBypassAuxiliaryHandlers(
                auxiliarySurfacesApproved: true) &&
            !EditorCloseFinalizationPolicy.ShouldBypassAuxiliaryHandlers(
                auxiliarySurfacesApproved: false),
            "final close bypasses temporary auxiliary re-dock handlers");

        var enabledMaterial = new InputProbe(isEnabled: true);
        var disabledMaterial = new InputProbe(isEnabled: false);
        var lateMaterial = new InputProbe(isEnabled: true);
        var closedMaterial = new InputProbe(isEnabled: true);
        var materialInput =
            new ModelessOwnerCloseInputTransaction<InputProbe>(
                probe => probe.IsEnabled,
                (probe, enabled) => probe.SetEnabled(enabled));
        materialInput.Block(
            new[]
            {
                enabledMaterial,
                disabledMaterial,
                closedMaterial,
            });
        enabledMaterial.IsEnabled = true;
        materialInput.Block(
            new[]
            {
                enabledMaterial,
                disabledMaterial,
                lateMaterial,
                closedMaterial,
            });
        Check(
            materialInput.IsBlocked &&
            materialInput.CapturedCount == 4 &&
            !enabledMaterial.IsEnabled &&
            !disabledMaterial.IsEnabled &&
            !lateMaterial.IsEnabled &&
            !closedMaterial.IsEnabled,
            "owner-close preparation and finalization keep every modeless material inert");
        int closedWritesBeforeRestore = closedMaterial.WriteCount;
        materialInput.Restore(
            new[] { enabledMaterial, disabledMaterial, lateMaterial });
        Check(
            !materialInput.IsBlocked &&
            materialInput.CapturedCount == 0 &&
            enabledMaterial.IsEnabled &&
            !disabledMaterial.IsEnabled &&
            lateMaterial.IsEnabled &&
            !closedMaterial.IsEnabled &&
            closedMaterial.WriteCount == closedWritesBeforeRestore,
            "owner-close cancellation restores only live modeless materials to prior input state");

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
                PanelId = "profiler",
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
            normalized.PanelId == "profiler" &&
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
                    new() { PanelId = "console" },
                    new() { PanelId = "build" },
                    new() { PanelId = "assets" },
                    new() { PanelId = "profiler" },
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

    private sealed class InputProbe
    {
        internal InputProbe(bool isEnabled) => IsEnabled = isEnabled;
        internal bool IsEnabled { get; set; }
        internal int WriteCount { get; private set; }

        internal void SetEnabled(bool enabled)
        {
            IsEnabled = enabled;
            WriteCount++;
        }
    }
}
