# Editor startup responsiveness

The WPF owner thread must remain available for first paint, caption
move/resize, input, and diagnostics while the editor is opening. Startup uses
two explicit worker boundaries; neither changes render quality or scene data.
Recent-project discovery, availability probing, and serialized launcher
open/create operations use the complementary contract in
[Project Launcher Responsiveness](ProjectLauncherResponsiveness.md).

## Native host bootstrap

`EngineViewport.BuildWindowCore` creates and subclasses only the lightweight
Win32 child window. ABI DLL discovery, capability query, and
`acs_editor_create` run through `EditorNativeBootstrap` on a worker. This keeps
the first initialization of the native logger, allocator, and thread pool off
the WPF Dispatcher.

The result is still unpublished and has no HWND attachment. It becomes the
viewport engine only when all of the following remain true on the Dispatcher:

- the originating HwndHost generation is current;
- its cancellation source has not been cancelled;
- the owner and child HWND are still alive; and
- ABI query, host creation, and presentation suppression all succeeded.

A close, HwndHost rebuild, incompatible ABI, or failed bootstrap never
publishes a partial handle. A successfully created but stale handle is
destroyed on a worker before the continuation completes. D3D device/swapchain
attachment and rendering remain on the HWND owner thread.

Cold create, queued publication, the adopted host, and final destroy share one
process-local lifetime lease. If WPF rebuilds the HwndHost while an earlier
native call is still returning, the newer generation cannot create a second
logger, allocator, or worker pool until the older handle has actually been
destroyed. Waiting for that slot is cancellation-aware, so a queued stale
generation exits promptly without consuming a worker for the older host's
remaining lifetime or creating another host. A worker result keeps owning the
slot while its Dispatcher continuation is queued; this closes
the create-to-publication race as well as overlapping create calls.

## Layout and workspace snapshot

Editor layout and named-workspace JSON are loaded on workers after `Loaded`.
The layout reader accepts at most 64 KiB of strict UTF-8 from an ordinary,
non-reparse file. It compares length and last-write time before and after the
read, so a concurrent replacement is rejected rather than parsed as a torn
snapshot.

Mouse, keyboard, or native caption move/resize input increments the live
layout publication version. A delayed result whose version is stale is not
allowed to move or resize the window, change dock visibility, or replace a
workspace store that a user command already initialized. The current layout
is subsequently saved normally.

If the Workspace menu or Command Palette opens before the catalogue worker has
finished, it reports the loading state and does not start a second synchronous
catalogue read on the Dispatcher. Reopening it after publication exposes the
saved profiles. This keeps a fast user command from reintroducing the same
redirected-storage stall that startup moved off-thread.

## Monitor placement and non-activating validation

`--secondary-monitor` starts the launcher or editor on the first active
non-primary display. `--monitor N` selects a stable primary-first display
index (`0` is the primary display). An explicit monitor selection overrides
only the saved top-level window bounds and maximized state; the saved dock,
workspace, and panel layout still load normally. Invalid indices and a
single-monitor `--secondary-monitor` request are reported instead of silently
moving the window off-screen.

Combine either selector with `--no-activate` for an interactive editor that
does not interrupt the current foreground application, or with `--unattended`
for visible render validation that rejects keyboard, mouse, touch, pointer,
gesture, and file-drop input:

```powershell
AcsEditor.exe .\Game.acsproject --secondary-monitor --unattended --show-profiler
```

Placement uses the target monitor's Win32 work area and `SWP_NOACTIVATE`, so
mixed-DPI coordinates are resolved in physical pixels and the taskbar is not
covered. The ordinary launch path remains governed by the saved editor layout.

## Ongoing interaction recovery

Viewport gestures use Win32 capture so gizmo and camera drags can leave the
child HWND. `WM_CAPTURECHANGED` remains the single gesture teardown path. A
lost button-up previously depended on another child-HWND message to begin and
complete stale-capture recovery; the viewport could therefore keep receiving
title-bar clicks while WPF timers and the profiler continued to advance.

The input-priority dispatcher heartbeat now also maintains capture. It releases
only when the viewport still owns the same HWND in the same generation, the
initiating button is physically up, the mismatch has survived the grace
period, no normal button-up finalization is in progress, and neither
move/resize, owner close, nor viewport destruction is active. A successful
repair is written as `STALE_VIEWPORT_CAPTURE_RECOVERED` in the interaction
health log. Real drags and held buttons are never cancelled by the timer.

Scene mutation history still captures the canonical 2D+3D compatibility
envelope on the Dispatcher because native scene serializers are
owner-thread-bound. That immutable Undo capture now also supplies the active
scene's normalized dirty comparison. Publication is rejected if scene
identity, mutation revision, clean-baseline generation, active view, active
document, or simulation state changed. The 750 ms workspace timer remains a
fallback, but an ordinary edit no longer performs a second native
serialization after its 350 ms history capture.

## Shutdown persistence

Final close captures WPF layout geometry on the Dispatcher and writes it on an
owned worker before the approved close is issued. The same-directory UTF-8
temp file is atomically moved into place and is deleted on every pre-commit
failure; an existing layout remains intact. The `Closed` interaction
diagnostic is explicitly best-effort: its bounded background worker is retained
and has a terminal exception boundary, but the Dispatcher never waits for the
last line and process exit may omit it.

## Verification and remaining boundary

Run:

```powershell
dotnet build .\acs\editor\AcsEditor\AcsEditor.csproj -c Release
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --editor-reliability-selftest
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --profiler-selftest
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --scene-editor-migration-selftest
```

The reliability test covers slow worker creation, cancellation cleanup,
prompt cancellation while another host owns the lifetime slot,
overlapping-generation serialization, incompatible ABI rejection,
strict/bounded layout input, reparse rejection, stale layout publication,
atomic close-layout failure cleanup, canonical-capture generation rejection,
and the no-wait close-diagnostic policy. The profiler test covers every
stale-capture exclusion, including held buttons, normal finalization,
move/resize, close, destruction, and HWND generation change.

The first D3D attach, a native render frame, resize, selected GPU resource
rebuilds, and published-host destruction still cross the ABI synchronously on
the HWND owner thread. The independent dispatcher watchdog and native-call
timings identify those remaining stalls; moving them requires an explicit
render-thread ownership protocol rather than dispatching existing calls to an
arbitrary worker.
