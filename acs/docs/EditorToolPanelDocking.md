# Editor tool-panel docking

ACS Editor treats each detachable tool as one registered visual with a stable
identity. The initial registry contains:

- `hierarchy` — Scene Outliner
- `inspector` — Details
- `console` — Console
- `build` — Build
- `assets` — Assets
- `profiler` — Profiler

Console, Build, Assets, and Profiler share the bottom dock while docked, but
each can be floated, hidden, restored, and persisted independently. Selecting a
tab for a floating or hidden tool safely re-docks or restores that tool. Several
tools may be floating at the same time. `Ctrl+J` and **View > Bottom Dock**
collapse or restore only the aggregate dock presentation; they do not rewrite
any child's Docked/Floating/Hidden state or the active tab. The dock's **Hide**
button is intentionally different: it hides only the selected tool.

## Ownership and transitions

`DockableToolHost` transfers the original WPF `FrameworkElement`; it never
clones a panel. A committed panel has exactly one visual owner: its original
dock parent or one `DockableToolWindow`. Failed float and re-dock operations
roll back to the last truthful owner. Layout reset captures every registered
host first, applies defaults only after all hosts re-dock, and otherwise
restores the captured states.

Editor shutdown uses one auxiliary-surface transaction. It captures all six
tool hosts and the active bottom tab before any temporary re-dock, then commits
only after Camera View also returns its native surface. A tool or Camera View
failure restores the six requested states in reverse order, persists the
truthful recovered layout if exact restoration is impossible, revokes hosted
material-window close approval, resumes asset operations, and re-enables editor
input. Successful shutdown retains the original floating placements and bottom
tab instead of persisting its temporary re-dock state. Autosave workers are
joined and session-recovery deletion is attempted only after all auxiliary
surfaces commit; while that asynchronous cleanup runs, re-entrant close
attempts remain cancelled. A cleanup failure is diagnosed, but after autosave
has begun its irreversible stop the final close proceeds instead of trying to
resume a partially stopped session. The final close bypasses the temporary
re-dock handlers, so a failed auxiliary return cannot leave a reopened editor
without autosave or recovery.
All open modeless Material Editor windows are input-disabled from the beginning
of close preparation through final recovery cleanup. Cancellation or auxiliary
rollback restores each still-open window to its exact pre-close `IsEnabled`
state; successful final close never re-enables those windows.

Floating windows are editor-owned tool windows with `ShowActivated=false` and
`Topmost=false`. Their moving bounds use physical pixels for per-monitor-DPI
snapping, while persisted bounds remain WPF DIPs.

## Persistence

Tool state and placement are stored under the current user's local application
data, not in a scene or project. The snapshot is:

- schema-versioned and rejected as a unit if incomplete or duplicated;
- bounded to 16 KiB;
- normalized to finite, reachable multi-monitor geometry;
- written to a unique temporary file and atomically replaced.

The six-panel registry uses placement schema version 2. `ToolPanels.v2.json`
owns only each panel's `Docked`/`Floating`/`Hidden` state and bounded floating
placement. The workspace layout store separately owns aggregate Bottom Dock
visibility, active bottom tab, and row/column sizes. Only a complete valid
version-2 panel snapshot is applied. If that file is missing or invalid, the
editor retains the workspace's aggregate visibility and active bottom tab,
then atomically seeds only the six panel states and placements from the
current hosts.

## Registering another tool

1. Add one canonical stable ID and accessible descriptor to
   `ToolPanelDockingContract`.
2. Place the tool's single root `FrameworkElement` in a WPF `Panel`.
3. Register it once through `RegisterToolPanelHost`, providing dock membership
   and visibility callbacks.
4. Add a bounded default placement and update the docking self-test's expected
   registry.
5. Keep selection state separate from dock membership when multiple tools share
   one tab slot.

Document tabs and native render views have separate lifetime requirements and
must not be moved into this WPF host without an explicit ownership adapter.

## Validation

Run:

```powershell
dotnet build .\acs\editor\AcsEditor\AcsEditor.csproj -c Release
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --workspace-selftest
```

The workspace self-test includes stable registry ordering, independent bottom
tool selection, single-visual ownership, transactional reset and owner-close
decisions, aggregate bottom-dock round trips, modeless close-input restoration,
focus policy, DPI snapping, negative-coordinate monitors, hostile geometry, and
persisted snapshot validation.
