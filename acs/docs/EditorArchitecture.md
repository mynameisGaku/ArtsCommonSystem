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
state. Scene Save All planning validates the active document and project
scene reference before publication. Source writes use same-volume temporary
files and atomic replacement where the underlying workflow supports it.

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

- folder history, favourites, collections, recursive search, type filters,
  list/tile presentation, thumbnails, and a preview pane;
- query tokens including `name:`, `path:`, `type:`, `id:`, and exclusions;
- open/place, rename, duplicate, copy/cut/paste, migrate, and reveal/copy-path
  actions;
- import and transactional reimport from recorded source metadata;
- Reference Viewer and same-kind Replace References preview/commit;
- redirector cleanup;
- recoverable Trash, restore-last-delete, retention, and explicit emptying.

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

Managed metrics include:

- Editor log drain/apply/retention cost and queue peaks;
- native attach, resize, and render call duration;
- slow native-call and GPU-backpressure yield counts.

The history records each native frame index once and can be paused or reset.
GPU timestamp results arrive after a backend-dependent number of frames.
Unavailable or warming-up data is shown as such; it is not replaced with a
CPU estimate. This profiler is an Editor performance overview, not a GPU
capture debugger, allocation tracker, or platform telemetry service.

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
| `fast` | isolated Editor Release build; document host, Asset Browser, profiler, and package-responsiveness self-tests |
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
| document/save/autosave | `EditorDocumentHost.cs`, `SceneSaveAllPlanner.cs`, `SceneAutosaveStore.cs` |
| scene compatibility | `SceneDocumentMode.cs`, `SceneWorldDocumentEnvelope.cs`, `CanonicalSceneAdapter.cs` |
| Asset View | `AssetBrowserPanel*`, `AssetDatabase.cs`, asset workflow classes |
| profiler | `ProfilerPanel*`, `EditorProfilerModel.cs`, native profiler snapshot |
| package | `PackagingService.cs`, `PackageCore.cs`, `tools/acspackage` |
| aggregate verification | `scripts/verify_editor.ps1` |
