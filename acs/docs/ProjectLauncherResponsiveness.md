# Project Launcher Responsiveness Contract

The project launcher is part of the editor's availability boundary. It must
paint and remain movable even when a recent project or a requested destination
is on a slow, disconnected, or hostile filesystem.

## Dispatcher rules

- The WPF Dispatcher may capture textbox values, publish immutable results,
  update controls, and display errors.
- Reading the recent-project file, probing project availability, opening a
  project, and creating/indexing a project run through
  `ProjectLauncherBackgroundOperations`.
- A data-binding getter must be presentation-only. `RecentProjectItem.Status`
  receives the worker's availability snapshot and never calls `File.Exists`.
- Folder browsing starts from the local Documents directory. It does not probe
  a user-entered UNC path on the Dispatcher before showing the shell dialog.
- Busy state disables only controls that can submit or mutate the active
  operation. The top-level window and Close affordance remain responsive.

## Lifetime and close rules

`ProjectLauncherAsyncGate` admits one Open/Create operation, assigns a
monotonic generation, and rejects duplicate click, Enter, or double-click
submissions. Every result must still own the current ticket before touching
WPF state.

Open/Create currently contain legacy filesystem operations that cannot be
cancelled safely once publication has started. A close request therefore marks
the result non-publishable and defers the actual close until the worker drains;
the window remains movable and paintable during that interval. Window lifetime
disposal invalidates every outstanding recent-list and operation ticket.

For a successful modal result, the launcher commits the exclusive gate and
approves close before assigning `DialogResult = true`. WPF synchronously begins
Closing from that property setter, so reversing this order would incorrectly
turn success into a deferred cancel.

## Input bounds and verification

The recent-project file is captured as an ordinary, non-reparse file with a
512 KiB maximum, strict UTF-8 decoding, a maximum of 10 distinct entries, and
change-during-read detection. Invalid, oversized, or unstable input fails to
an empty recent list without delaying first paint.

Run the focused regression suite:

```powershell
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --project-launcher-responsiveness-selftest
```

The same suite is registered in both the `fast` and complete managed sets of
`acs/scripts/verify_editor.ps1`.
