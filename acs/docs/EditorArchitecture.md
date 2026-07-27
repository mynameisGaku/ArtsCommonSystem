<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS Editor: architecture and workflows

This document describes the production ACS Editor under
`editor/AcsEditor`, as implemented on 2026-07-26. It is the authoritative
overview for the desktop Editor, asset authoring, profiling, packaging, and
verification entry points.

The native ImGui authoring panels under `src/gameframework/tools` are a
separate in-engine tools library. They can be embedded by samples, but they
are not the WPF desktop Editor described here.

## Supported host

- Windows 10 or 11, x64.
- .NET 10 Windows Desktop for the WPF shell.
- The native `acs_editor_abi` library and an ACS RHI backend for the rendered
  viewport.
- A project rooted by one `.acsproject` manifest and an `Assets` directory.

The Editor is not binary- or project-compatible with Unreal Editor or Unity.
Its UI and workflows may use familiar conventions, but ACS assets and scene
formats are ACS contracts.

## Runtime architecture

```text
AcsEditor.exe (WPF, UI Dispatcher)
  |
  +-- MainWindow and document services
  |     project, scene transactions, autosave, build, package
  |
  +-- AssetBrowserPanel and asset services
  |     index, create/import/reimport, references, Trash, DDC
  |
  +-- ProfilerPanel
  |     native snapshots, Editor log-pump and native-call diagnostics
  |
  +-- EngineViewport (HwndHost child HWND)
        |
        +-- EngineInterop (P/Invoke contract)
              |
              +-- acs_editor_abi
                    |
                    +-- FRenderer
                          |
                          +-- raw DX12 or configured Diligent RHI
```

### Managed shell

`App.xaml.cs` owns process startup and the headless self-test switches.
`MainWindow.xaml.cs` and its partial files own the desktop workspace:

- scene hierarchy and details;
- scene and game views;
- document dirty/save state;
- startup, build, run, package, and shutdown coordination;
- workspace layout and command routing;
- Editor log ingestion and profiling integration.

Shared stateful workflows are kept out of button handlers where possible.
Examples include `EditorDocumentHost`, `SceneSaveAllPlanner`,
`SceneAutosaveStore`, `AssetDatabase`, `AssetImportWorkflow`,
`AssetManagementWorkflow`, and `PackageCore`.

### Native viewport boundary

`EngineViewport` is a WPF `HwndHost`. The child HWND and all
`acs_editor_abi` renderer calls remain on the owning UI thread. The boundary
is deliberately C-compatible; managed code does not own native scene or GPU
objects.

The native boundary is negotiated before the Editor creates a native host.
`acs_editor_abi_query` accepts the oldest contract version understood by the
managed host plus a required capability mask, and always reports the
provider's version and capability mask. Contract versions are additive;
optional surfaces are represented by bits and are never inferred from the
product-version label. The current required host set is:

- result-bearing render frames;
- incremental startup;
- result-bearing resize.

Profiler v3, the independent volumetric-cloud workload v1 snapshot, unified
scene documents, high-quality material previews, Substrate graphs, and
interactive 3D water are advertised independently.
An older DLL without the query export, a missing required capability, a bad
binary architecture, or a rejected query leaves the viewport disabled and
publishes a stable startup diagnostic instead of calling further entry
points. **Help > About ACS Editor** shows the product version, actual render
backend, ABI version, negotiated capabilities, unknown future bits, and the
compatibility result.

Startup is incremental:

1. Create the child HWND and native host.
2. Attach the renderer after the physical viewport size stabilizes.
3. Keep the HWND out of normal composition while native warm-up advances.
4. Present deterministic neutral frames during warm-up and scene
   transactions.
5. Publish the complete scene, then enter the Ready state and enable normal
   input.

Rendering uses one private Win32 pump message in bounded bursts rather than
`CompositionTarget.Rendering`. A cooperative native frame returns:

- `1`: command submission produced a valid completion fence and the
  swapchain accepted the presentation;
- `0`: GPU frame-slot backpressure; yield without consuming frame time;
- `-1`: submit/present failure, device removal/loss, or an incompatible render
  contract; suspend the pump.

Profiler snapshots and the managed timestamp advance only after result `1`.
Busy attempts and fatal submit/present failures never publish a completed
frame. Excess time is consumed in bounded later deltas instead of being
discarded.

The frame lifecycle is result-bearing end to end:

```text
TryBegin -> record -> RHI Submit -> completion fence -> swapchain Present
    0                      failure ---------------------------> -1
    |                                                           |
    +-- busy, no simulation/profiler commit     success --------+--> 1
```

Raw DX12 propagates both queue-signal failure and the `Present` HRESULT,
including `GetDeviceRemovedReason`. Diligent validates backend device health
before and after Flush/Present and requires its frame-completion fence before
reporting success. `FRenderer::EndFrame*` returns false on either submit or
present failure; `acs_editor_render_try` maps that to `-1`.

Move/size modal loops pause the render pump. After `WM_EXITSIZEMOVE`, the
managed host waits for two identical physical-size observations and applies
only that final size. `FRenderer::OnResize` owns the single GPU-idle boundary;
swapchain backends only recreate buffers. If a resize and MSAA resource rebuild
occur in the same owner-thread turn, the MSAA path reuses that idle boundary
instead of draining the queue twice.

`acs_editor_resize` returns success only after swapchain and depth recreation.
On failure, managed `_w/_h` remains unchanged, so the next pump retries the
same final dimensions. That pump still invokes `acs_editor_render_try`: a
transient depth rebuild failure may continue on the recreated color buffers,
while missing backbuffers or device loss fail submit/present, become render
result `-1`, and stop the pump instead of retrying forever. The standalone
`FApplication` and Easy loops latch resize failure during event dispatch and
exit before recording another frame.

This design improves UI fairness, but it does not make every native operation
asynchronous. See [Known constraints](#known-constraints).

## Projects, documents, and scenes

### Project identity

The `.acsproject` manifest records the project name, schema and engine
versions, template, `initialScene`, and the persistent
`canonicalSceneAssetId`. The asset ID is the durable scene identity; the path
is still validated and journalled so a rename or interrupted publication
cannot silently package a different file.

Schema version 1 is the current project contract; an omitted version is the
only legacy form migrated to version 1. Reads are bounded and strict UTF-8,
reject duplicate property names case-insensitively, reject unsupported
versions and malformed or zero Asset IDs, and keep field-type failures behind
one `InvalidDataException` boundary. Project strings reject control, Unicode
format, and line/paragraph separator characters before they reach logs or UI.
Manifest and `Game.DefaultScene` updates publish flushed sibling files under
the project-wide `AssetMutationLock`; a missing destination uses a no-overwrite
rename. The lease is the cooperative multi-editor serialization boundary.
Non-cooperating external file writers are detected by snapshot validation and
leave the operation or recovery journal fail-closed; the editor does not claim
filesystem compare-and-swap semantics that Windows path replacement cannot
provide.

Legacy manifests with an empty `canonicalSceneAssetId` are upgraded during
project open, after import/reimport and interrupted scene-move recovery have
settled under that same lease. The editor performs one content-verified Asset
Database refresh, requires the normalized startup path and non-zero GUID to
resolve to the same unique scene record, then atomically rewrites only the
identity field while preserving unknown manifest properties. The authoritative
`.acsmeta` sidecar is durable before manifest publication, so an interruption
is idempotently retried. Projects that already carry an Asset ID skip this
migration scan; full dependency/content validation remains a Cook boundary.
For the migration itself, the refreshed Scene and its adjacent `.acsmeta` are
captured as bounded ordinary-file byte snapshots. The Scene bytes must retain
the refreshed content hash, and the canonical GUID and `scene` kind are read
from the captured sidecar and matched back to the unique database record.
Both snapshots are compared again immediately before manifest publication.
This detects non-cooperating edits completed before the final comparison; like
the other project transactions, it does not claim filesystem compare-and-swap
against a hostile path replacement after that last read.

Editor-only preferences such as recent projects and workspace layouts are not
project source assets. Verification runs isolate `APPDATA` and
`LOCALAPPDATA`, so they never update a developer's normal preferences.

### One world, projection as a view

New 2D and 3D projects both create one `Assets/main.acs3d` world. The 2D
template is ordinary 3D scene data opened with an XY orthographic view
preset. Perspective versus orthographic projection is camera/editor state,
not a second scene asset type.

Compatibility remains explicit:

- `.acs3d` (`ACS3D v2`) is the source created by new projects and by the
  Content Browser.
- Legacy `.acscene` (`ACSCENE v1`) can still be opened and packaged through
  its adapter.
- `SceneDocumentMode` and the reversible compatibility envelope still exist
  while both legacy native serializers are supported. Their presence must
  not be interpreted as two scene documents for a new project.

`CanonicalSceneAdapter` validates the supported bootstrap subset and rewrites
portable references only in isolated cook copies. Unsupported directives or
external resources fail closed rather than being silently dropped.

### Save and recovery

`EditorDocumentHost` supplies common identity, dirty, save, and transaction
state. The Scene adapter and the authored Substrate graph in each open Material
Editor are connected to this host today. Blueprint, Prefab, and Settings
adapters are migration targets for the same stable `(kind, ID)` contract.
Material documents prefer the adjacent Asset Database GUID, so rename/move
does not replace their identity; a loose material uses a canonical absolute
path and is rebound after a successful path mutation. A supplied malformed
Asset ID is rejected instead of silently changing identity domains. Project
materials read and validate their authoritative sidecar before window/host
registration, so an Inspector open does not wait for the Asset View's
asynchronous index and never silently falls back to path identity. New Material
creation passes the GUID from its authoritative refresh directly to the editor
window.
After the common host is initialized, a registration failure aborts opening
the window rather than leaving an unhosted editor that can bypass Save All.
Delayed host initialization also requires every already-open Material Editor
to register; one failure rolls back the complete host initialization and keeps
editor startup fail-closed.
Duplicate IDs fail closed. Registry snapshots and Save All use explicit save
priority followed by ordinal kind/ID ordering, so results do not depend on
hash-table or window activation order. Dirty documents are captured and saved
sequentially through asynchronous contracts. A failed or unsupported writer is
diagnosed per document while independent later writers still run;
cancellation stops before the next writer. A batch is not successful while any
registered document remains dirty, including a document edited during its own
asynchronous save. Suspended documents, open transactions, and capture
failures are explicit per-document batch failures even when their last cached
fingerprint was clean.

Close preparation captures its own deterministic dirty snapshot instead of
depending on a UI refresh or a Scene-only boolean. Cancel performs no source
writes, Discard is explicit, and Save authorizes close only after the complete
dirty set succeeds. If any registered document cannot be inspected because it
is suspended, has an open transaction, or its capture contract fails, Save
blocks before invoking any writer. Normal unregister/registry clear refuses
dirty, suspended, or open-transaction documents unless its caller explicitly
chooses discard.

Material graph gestures bridge their existing semantic history scope into the
host transaction state. Save All therefore refuses to call the native writer
while a graph gesture is open, and asset rename/move suspends both the window
and hosted document. The material save callback validates and commits the
combined closure/expression graph through the existing native writer and only
publishes its saved fingerprint after success. Main-window close uses the
common Save/Discard/Cancel result, while closing a Material Editor by itself
retains its local confirmation. Layout-only graph changes are intentionally
excluded from the source fingerprint but the sidecar is still written on every
non-Discard close.

This vertical slice does not claim that all legacy material controls are
transactional: the legacy PBR/effect panels still use their existing immediate
native writers. Common material autosave/recovery, cross-window Undo routing,
and Blueprint/Prefab/Settings registration remain follow-up work.

Recovery state is composed as the complete 2D/3D compatibility envelope,
replacing only the recovered source subsystem and preserving the other graph.
It is applied through an atomic document boundary: the restored live canonical
fingerprint must match the recovery fingerprint; parser rejection, restore
failure, or verification failure rolls both native graphs and history metadata
back, while success clears stale undo/redo and deliberately remains dirty.

The current Scene adapter still owns validation of the active project scene
reference and compatibility-source publication. Source writes use same-volume
temporary files and atomic replacement where the underlying workflow supports
it. Other document types should keep their format-specific serializer inside
their registered save callback, not add another Save All or close path.

Autosave and recovery are separate from the authoritative source file.
Generation gates prevent an older asynchronous operation from publishing over
a newer document. Build, Run, and Package request a coherent saved scene and
abort when that contract cannot be met.

## Asset workflow

### Storage and identity

The Content Browser treats the project `Assets` directory as its security and
portability boundary:

- existing reparse points, symlinks, and junctions are rejected instead of
  intentionally traversed;
- an adjacent `<asset>.acsmeta` sidecar is the authoritative persistent Asset
  ID, importer, source, dependency, and import-settings record;
- `Assets/.acsdb/index.v1.json` is only a deterministic acceleration and
  recovery cache;
- mutation workflows share a project asset lease and revalidate path
  containment before publication;
- Cook requests a content-verified, read-only database snapshot and does not
  synthesize missing authoritative sidecars.

### Creating ACS assets

The **New** menu creates:

| UI item | Source contract |
|---|---|
| Folder | ordinary directory below `Assets` |
| Material | `.acsmat`, serialized by the native canonical material writer |
| Scene | `.acs3d` with an `ACS3D v2` header |
| Blueprint | `.acsbp` with an `ACSBP 1` header |
| Prefab | `.acsprefab` containing an ACS3D prefab root |

Creation writes a complete same-directory temporary asset and publishes it
with a no-overwrite move. Collision-safe suffixes are selected without
overwriting an existing file.

The material editor provides ACS closure and expression graphs, preview, and
canonical ACSMAT persistence. It is Substrate-inspired ACS functionality, not
an implementation of Unreal's serialized Substrate asset format.

### Browsing and management

The current Asset View provides:

- folder history, favourites, collections, explicit current-folder/subfolder/
  all-assets search scopes, type filters, list/tile presentation, thumbnails,
  and a preview pane;
- query tokens including `name:`, `path:`, `type:`, `id:`, and exclusions;
- open/place, rename, duplicate, copy/cut/paste, migrate, and reveal/copy-path
  actions;
- import and transactional reimport from recorded source metadata;
- Reference Viewer and same-kind Replace References preview/commit;
- redirector cleanup;
- recoverable Trash, restore-last-delete, retention, and explicit emptying.

All-assets search reads the immutable Asset Database snapshot on a worker task;
it does not walk the project filesystem from the UI thread. Scope evaluation
normalizes every candidate and fails closed unless both the current folder and
candidate remain inside the active project's `Assets` root. Folder tiles are
intentionally omitted from the all-assets result set so the result represents
indexed assets rather than a second, potentially expensive directory crawl.

Rename, move, duplicate, replace-reference, and delete paths inspect or
rewrite supported references transactionally. Referenced assets are refused
when an operation cannot preserve the graph safely. Interrupted import,
reimport, and Trash transactions leave recovery information under
`Assets/.acsdb` instead of pretending that publication succeeded.

The browser authors source assets. It does not currently mount a packaged
`.acpak` as a writable authoring layer.

## Profiler

`ProfilerPanel` samples a versioned native snapshot and combines it with
managed Editor diagnostics.

Native metrics include:

- submitted frame index, frame delta, FPS, CPU frame and submit time;
- asynchronous GPU frame timings and per-pass timings;
- current, average, and rolling-window peak values;
- draw, dispatch, triangle, and resource counts;
- cloud CPU/GPU cost, trace resolution, render scale, and march/light steps;
- viewport dimensions and active feature flags.

Profiler v3 remains a packed 208-byte version-3 contract. Exact volumetric
cloud accounting is an optional, separate packed 168-byte version-1 contract
(`cloud-workload-v1`), so adding it does not reinterpret or resize an existing
Profiler snapshot. The managed host calls its export only when the capability
was advertised. The query distinguishes an available attempt, runtime
unavailability, and an ABI error; an unattached/warming renderer, inactive
cloud pass, or uninitialized cloud renderer is shown as unavailable rather
than as a zero-cost frame.

For an available attempt the Cloud panel displays the exact steady,
one-time-bake, shadow-cache, and total compute-dispatch counts; the composite
draw count; logical invocations and padded launched-thread counts for trace,
resolve, bake, shadow, and totals; temporal-history state; and conservative
view/light sample ceilings. A skipped attempt retains its explicit native
reason. The ceilings are not measured samples; GPU timestamps remain the
authoritative elapsed-cost metric. This diagnostic surface does not change
cloud resolution, quality, or march counts.

Managed metrics include:

- Editor log drain/apply/retention cost and queue peaks;
- native attach, resize, and render call duration;
- slow native-call and GPU-backpressure yield counts;
- Dispatcher heartbeat gap, detected UI-stall count and longest stall;
- Profiler Dispatcher-callback CPU cost and history-graph `OnRender` command
  generation CPU cost (not compositor/GPU time).

The history records each native frame index once and can be paused or reset.
The visible dock samples at 10 Hz. A collapsed dock keeps the status summary
current at 2 Hz without updating the detailed metric grid or invalidating the
graph, so profiling does not become a significant hidden UI workload.
When the native frame index stops, visible managed diagnostics continue to
refresh; only native history insertion and graph invalidation wait for a new
frame. This keeps the watchdog, log-pump, and native-call evidence useful while
investigating a renderer stall.
GPU timestamp results arrive after a backend-dependent number of frames.
Unavailable or warming-up data is shown as such; it is not replaced with a
CPU estimate. This profiler is an Editor performance overview, not a GPU
capture debugger, allocation tracker, or platform telemetry service.

### UI-stall evidence

`EditorDispatcherWatchdog` is driven by a ThreadPool timer, independent of the
WPF Dispatcher. A minimal `DispatcherPriority.Input` heartbeat records startup
phase and normal interaction health every 500 ms, so starvation is measured at
the same scheduling class as user input rather than a hidden background panel.
If no heartbeat arrives for two seconds, the watchdog
writes `DISPATCHER_STALL`; the first later heartbeat writes
`DISPATCHER_RECOVERED` with the measured duration and phase. This distinction
is important because a `DispatcherTimer` cannot produce evidence while the
Dispatcher itself is blocked.

Each Editor process writes a session-scoped
`%LOCALAPPDATA%\ACS\Editor\Diagnostics\interaction-health-<UTC>-<PID>.log`.
A single FIFO worker preserves transition order, rotates the current session
at 2 MiB to `.previous`, and performs console/file I/O away from the UI thread.
Reliability-soak completion awaits its final diagnostic asynchronously; normal
window close performs one bounded 500 ms drain after stopping the watchdog.
Lines are bounded and control-, bidi-, and line-injection safe; the phase token
additionally removes key/value separators. The watchdog diagnoses a stall; it
does not abort a native call or attempt unsafe UI-thread recovery.

## Build, package, and distribution

The WPF package flow uses `PackagingService` and `PackageCore`. The
`tools/acspackage` project exposes the same non-UI package core for automation.

Current Windows x64 packaging performs:

1. project, scene identity, asset graph, and portable-path validation;
2. optional Release game build;
3. deterministic Cook in an isolated staging tree;
4. `.acpak` creation and native verification;
5. non-system runtime DLL resolution;
6. manifest generation with payload hashes and content build identity;
7. deterministic ZIP publication through atomic replacement.

Profiles are `Development`, `Test`, and `Shipping`. Test and Shipping use the
verified compressed pack without redundant loose source assets. Development
also retains loose Cooked assets for inspection. Shipping refuses PDB
inclusion; Editor-only reflection DLLs are excluded by default.

The canonical root scene is cooked to the runtime bootstrap path
`main.acscene`; manifest metadata records the original adapter format.
Detailed CLI syntax and the supported scene subset are documented in
[`tools/acspackage/README.md`](../tools/acspackage/README.md).

Local packaging does not currently provide executable signing, product
publisher/icon metadata, an installer, store upload, patch hosting, or a
cross-platform package.

## Known constraints

- The desktop Editor and package pipeline are Windows x64 only.
- Legacy `.acscene` remains supported, but new authoring uses one `.acs3d`
  world and projection presets.
- The runtime/package adapter supports a defined subset of Editor-authored 3D
  directives. Unsupported content fails closed; consult the package CLI
  documentation before promising standalone parity for a feature.
- Renderer attach, final resize, present, selected resource rebuilds, and
  shutdown still cross the native boundary synchronously on the HWND owner
  thread. Cooperative frame-slot backpressure does not bound those calls.
- GPU profiler data is asynchronous and may be unavailable on a backend or
  during warm-up.
- Asset collections and browser layout are Editor conveniences; authoritative
  asset identity lives in `.acsmeta`.
- The project mutation lease coordinates ACS processes. It is not an
  operating-system sandbox against a hostile same-user process replacing a
  validated directory between filesystem calls.
- Packaged asset packs are runtime/cook products, not editable Content Browser
  mounts.
- Signing, installers, store deployment, source-control integration, and
  multi-platform distribution remain outside the current local Editor flow.

The prioritized implementation gaps are maintained in
[`EditorProductionRoadmap.md`](EditorProductionRoadmap.md). That roadmap is
planning material; this document describes only current contracts.

## Verification

Use `scripts/verify_editor.ps1` from the `acs` directory. It redirects build
outputs, logs, NuGet state, temporary files, and application profile folders
to one unique directory below the system temp root. By default that directory
is removed before the final result summary. Repository `Binaries`,
`Intermediate`, and `Saved` trees and normal Editor user settings are not
cleaned or overwritten.

```powershell
# Print and validate the plan; launch nothing and create nothing.
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_editor.ps1 -Mode fast -DryRun

# Isolated Editor build and high-signal managed smoke tests.
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_editor.ps1 -Mode fast

# Every registered Editor CLI self-test plus package CLI self-test.
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_editor.ps1 -Mode managed

# Managed verification plus isolated native configure/build/CTest.
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_editor.ps1 -Mode full
```

| Mode | Verification surface |
|---|---|
| `fast` | isolated Editor Release build; ABI negotiation, document host, Asset Browser, profiler, and package-responsiveness self-tests |
| `managed` | isolated Editor build; every public self-test switch registered in `App.xaml.cs`; Blueprint self-test; isolated `acspackage --self-test` |
| `full` | managed mode; isolated native CMake build with samples/tools disabled; complete CTest registration |

The runner is intentionally not fail-fast. Independent suites continue after
a failure; dependent steps are marked `SKIP`. Any `FAIL` or `SKIP` produces
exit code `1`; an explicit repository preflight failure or a trapped fatal
runner error produces `2`; and a clean run produces `0`. PowerShell
parameter-binding errors happen before the script starts and use the host's
nonzero status. Use `-KeepArtifacts` to retain the unique temp directory and
per-step logs.

When a new public Editor self-test switch is added to `App.xaml.cs`, add it to
the explicit safe list in `verify_editor.ps1`. The list is not discovered and
executed dynamically because an unknown process switch must never gain CI
execution authority merely by matching a name pattern.

## Implementation map

| Area | Primary implementation |
|---|---|
| process and self-test dispatch | `editor/AcsEditor/App.xaml.cs` |
| workspace shell | `editor/AcsEditor/MainWindow*.cs`, `MainWindow.xaml` |
| rendered viewport | `editor/AcsEditor/EngineViewport.cs`, `EngineInterop.cs` |
| native bridge | `src/editor_abi/EditorAbi.cpp` |
| renderer/RHI | `src/render/Renderer.*`, `src/render/Dx12`, `src/render/Diligent` |
| document/save/autosave | `EditorDocumentHost.cs`, `MainWindow.Documents.cs`, `MaterialDocumentHostRegistration.cs`, `SceneSaveAllPlanner.cs`, `SceneAutosaveStore.cs` |
| scene compatibility | `SceneDocumentMode.cs`, `SceneWorldDocumentEnvelope.cs`, `CanonicalSceneAdapter.cs` |
| Asset View | `AssetBrowserPanel*`, `AssetDatabase.cs`, asset workflow classes |
| profiler | `ProfilerPanel*`, `EditorProfilerModel.cs`, `EditorCloudWorkload.cs`, native profiler/workload snapshots |
| package | `PackagingService.cs`, `PackageCore.cs`, `tools/acspackage` |
| aggregate verification | `scripts/verify_editor.ps1` |
