<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS Editor: architecture and workflows

This document describes the production ACS Editor under `editor/AcsEditor`.
It is the authoritative overview for the desktop Editor, asset authoring,
profiling, packaging, and verification entry points.

The native ImGui authoring panels under `src/gameframework/tools` are a
separate in-engine tools library. Game and editor hosts can embed them, but
they are not the WPF desktop Editor described here.

## Supported host

- Windows 10 or 11, x64.
- .NET 10 Windows Desktop for the WPF shell.
- The native `acs_editor_abi` library and an ACS RHI backend for the rendered
  viewport.
- A project rooted by one `.acsproject` manifest and an `Assets` directory.

ACS assets, project manifests, scene formats, and native ABI are defined only
by the ACS contracts documented in this repository.

The scene identity and 2D-authoring policy is fixed by
[ADR 0001: Single 3D scene document with Orthographic 2D authoring](adr/0001-single-scene-document.md).

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
- result-bearing resize;
- sparse Transform mutation, so mixed-value Details edits cannot silently
  overwrite untouched components;
- transactional 3D Prefab instance refresh, so Apply/Revert cannot call a
  missing export after startup or retire the old subtree on failure;
- explicit 3D Prefab root-property overrides, so source refresh preserves only
  marked Visible/Enabled/Color values while the compatibility Revert path
  restores all source values;
- persistent 3D Prefab root-component property overrides and selective Apply/Revert,
  so a missing native export cannot leave managed UI ahead of the scene ABI.

Profiler v5 (with its version-4 compatibility prefix), the independent
volumetric-cloud workload v1 snapshot, unified scene documents, high-quality
material previews, Substrate graphs, interactive 3D water, camera authoring,
Camera View requests, and optional-service diagnostics v2 are advertised
independently.
The renderer-facing quality and interaction contracts are detailed in
[Interactive 3D Water](InteractiveWater3D.md) and
[Render Effects Quality](RenderEffectsQuality.md).
An older DLL without the query export, a missing required capability, a bad
binary architecture, or a rejected query leaves the viewport disabled and
publishes a stable startup diagnostic instead of calling further entry
points. **Help > About ACS Editor** shows the product version, actual render
backend, ABI version, negotiated capabilities, unknown future bits, and the
compatibility result.

`optional-service-diagnostics-v2` adds a typed, query-only status boundary
without making it a required host capability. A caller initializes
`acs_editor_optional_service_diagnostic_get` with a version/size header. Version
1 receives exactly the 192-byte readable prefix: service, enabled/disabled/
pending/inactive/failed state, reason, flags, process-local host generation,
and a NUL-terminated UTF-8 message bounded to 160 bytes. Version 2 appends a
64-byte typed tail containing error domain/code, a monotonic diagnostic
generation, and a NUL-terminated stable code bounded to 48 bytes. The provider
accepts either complete prefix and rejects unknown versions, undersized
buffers, and inconsistent declared sizes before reading a host.

A structurally valid query returns a status payload even for a null, stale, or
unregistered host, or an unknown service. Profiler, volumetric-cloud workload,
and Camera View requests therefore expose exact reasons such as capability
missing, invalid host, startup pending, startup failed, or scene feature
inactive instead of forcing managed code to infer state from a missing symbol
or empty snapshot. A synchronized live-host registry validates the opaque
identity before native dereference and excludes destruction while the bounded
snapshot is copied. The three queried runtime state bits are atomic. Each
native host receives a unique nonzero generation. Managed decoding uses that
generation to reject allocator address reuse, applies strict bounded UTF-8,
verifies the requested service and typed state/reason/error relationships, and
can supply its expected generation; a result from a destroyed HwndHost
generation is discarded before publication. The v1 prefix, typed v2 tail, malformed
headers, stale-handle and query/destroy races, unique host generations, and
startup-pending service state are fixed by `ACS.EditorAbiLifecycle` and
`--abi-contract-selftest`.

The managed `EditorOptionalServiceUiSession` is the publication boundary
between that query contract and WPF controls. It binds accepted results to both
the current `EngineViewport` generation and the native host generation, keeps a
monotonic diagnostic generation per service, and rejects a result if the
managed identity changes during the query. When diagnostics v2 is not
advertised, the editor preserves the previous capability-optional behavior.
When it is advertised, malformed, stale, pending, disabled, or failed results
fail closed for only the affected native service. Profiler Reset and Camera
View request controls show the native message, stable code, typed error, and
generations in their status/tooltip. Pause, capture export, the local Cloud
workload visibility filter, Camera View Re-dock/close, and unrelated editor
actions remain available.
Missing `camera-view-requests-v1` retains the explicitly supported single
legacy preview while disabling only multi-slot request mutation. The pure
headless `--optional-service-ui-selftest` fixes compatibility fallback,
per-service gating, exact reason presentation, host replacement, late result,
and diagnostic-generation regression behavior.

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

Successful frames run in input-aware bursts of at most 64 frames or 64
milliseconds.
At every burst boundary, the next frame must pass through one FIFO
`DispatcherPriority.Input` checkpoint before another private render message
can be posted. Current keyboard/pointer input forces that checkpoint before the
deadline. The checkpoint is also mandatory in unattended mode: continuously
reposting a private child-HWND message can otherwise keep the Win32 queue busy
while starving Dispatcher timers indefinitely.

The profiler sampler, interaction-health timer, and Dispatcher heartbeat share
the checkpoint's FIFO Input priority. Therefore their due work must make
progress between bounded render bursts without outranking real input at the
same priority. Input checkpoints alone do not guarantee that WPF promotes due
timers or completes lower-priority startup work: their callback can post the
next private message before the queue drains below Input. A Background drain
therefore owns continuation during hidden renderer startup and on a 500 ms
wall-clock cadence. Its measured queue wait is an explicit rendering gap and
exposes excess lower-priority backlog rather than hiding it.

A `0` backpressure result may retry through the private HWND pump only inside
its bounded epoch. At 256 attempts or eight milliseconds it must pass through
the same Dispatcher checkpoint before starting another epoch, including in
unattended mode. Busy attempts do not advance simulation time.
Renderer/editor startup keeps its explicit staged Background operations; the
startup drain guarantees that those stages can complete. Invalid maintenance
counters or timestamps select a fail-closed Background drain; shutdown and
HwndHost generation changes invalidate queued callbacks.

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

After startup recovery completes, `ProjectManager.Open` re-reads both durable
records and refuses to publish the Project when `.acsproject.initialScene` and
`Game.DefaultScene` identify different paths. This final coherence gate prevents
the window layer from silently loading a stale settings override or presenting
an unrelated legacy scene while the intended startup document is still opening.

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
The supported 3D subset includes inline `PLY3D` geometry, `SPR3D` texture
references, non-executing `PFAB3D` instance-source links, and `PINS3D` stable
instance identities plus `POVR3D` root overrides and `PCOVR3D` root-component
property overrides. Cook preserves
the geometry and rewrites both dependency kinds into the package asset namespace.

### Details multi-selection transactions

The 3D Details panel enumerates the complete native selection set rather than
editing only its primary node. Shared Transform components and Mesh Renderer
Color are editable per component; `—` represents mixed values and an untouched
mixed component preserves each node's authored value. Enabled uses the same
contract through a three-state header control. Mesh Renderer remains an
ordinary native component card after Transform.

Common reflected component properties are also multi-editable when every
selected node contains exactly one component of the same type and the property
schemas match exactly. Bool uses a tri-state value; integer, unsigned, enum,
object-reference, float, and vector values preserve per-component mixed state.
Sparse vector edits write only entered axes. Reset is exposed only when the
native schema supplies a validated default. Duplicate component types, schema
drift, unsupported strings, non-finite or inexact numeric input, and stale
selection fail before mutation.

Every batch captures all targets before writing, verifies native readback, and
restores the failing target plus prior writes in reverse order when any write
fails. One batch is enclosed by one canonical Scene document transaction, so
Undo/Redo observes a single edit rather than one entry per node. See
`docs/DetailsMultiEdit.md` for UI behavior, rollback boundaries, and the
headless verification switches.

### Editor camera and authored game cameras

The Scene View navigation camera is editor preference/state. It is never
serialized as a game camera, never becomes a `CAM3D` record implicitly, and
gameplay camera updates do not overwrite it. Play and Stop own simulation
lifetime and restoration. Switching between Scene and Game tabs changes only
which camera is presented: Play continues, the Scene View remains navigable,
and the Game View routes gameplay input only while gameplay code is active.
Focus loss or a view switch releases every forwarded key and mouse button so
input cannot remain latched.

Game cameras are explicit Camera components on ordinary 3D nodes. The node
hierarchy supplies their live world position and orientation; the component
supplies a stable camera ID, projection, priority, active preference, field of
view or orthographic height, and near/far clip planes. More than one camera is
supported. Among effectively enabled camera nodes, selection is deterministic:
active-preferred first, priority descending, stable ID ascending, then node ID
ascending. Multiple active-preferred records therefore produce a warning but
not an order-dependent result. A scene without `CAM3D` remains compatible and
uses a deterministic scene-bounds fallback that is independent of the current
Editor camera.

The Details Components stack can add or remove Camera, edit its validated
projection settings, designate it active, and align its transform to the
current Scene View. The Outliner identifies camera nodes and marks the
resolved active camera. Camera frusta are editor visualization, not scene
radiance: they are drawn only in Scene View and never in Game View. Their
toolbar control is exposed only when the negotiated runtime advertises
`camera-authoring-v1`, and its initial state is read from the runtime.

`Snap View` is the inverse one-shot navigation workflow: it resolves the
selected Camera's parent-aware world pose through the transient game-camera
resolver, restores the previous preview override in a `finally` boundary, and
then converts the result into the Scene View orbit representation. It changes
only editor navigation and projection; Camera components, `Active`, scene
dirty state, and undo history remain untouched. The operation is fail-closed
during Play/Preview or while Camera View owns a render-surface request, and it
rolls the editor projection and pose back if native application fails.
Non-finite/clipping-invalid samples, stale node identity, unrepresentable roll,
and orbit-pole poses are rejected instead of silently showing a different
view. Scene View has a fixed perspective FOV and bounded orthographic range,
so the command reports when it preserves pose/projection but must approximate
authored framing.

`Float Preview` transfers the existing native renderer child window into one
owned WPF Camera View; it does not create a second editor host, renderer, or
engine. Camera selection in that window is non-persistent, so floating
preview never changes authored `CAM3D Active` state, scene dirty state, or
undo history.

Owner shutdown returns floating WPF tools and Camera View through one
all-or-nothing auxiliary-surface transaction. Camera return is the commit
boundary: failure restores the captured six-tool layout, cancels the approved
close, revokes hosted material close approvals, resumes asset operations, and
re-enables editor input. The successful shutdown path persists the stored
pre-shutdown floating placements and active bottom tab, not the temporary
re-docked shutdown geometry. Only after every auxiliary surface commits does
the editor join autosave workers and attempt to discard recovery entries.
Cleanup failures are diagnosed, but once autosave has begun its irreversible
stop the editor does not try to resume a partially stopped run; it proceeds
to the final close, which bypasses the temporary re-dock handlers. A failed
auxiliary return occurs before that boundary and therefore leaves autosave and
recovery fully usable.

When the provider advertises `camera-view-requests-v1`, the managed window
owns a bounded set of native request leases instead of mutating one
process-global preview override. The Camera View tab strip can add, focus, and
close up to eight logical camera slots inside the same detached window. One
native host accepts at most eight logical requests.
Their opaque IDs encode both slot and generation, preventing a stale
close/reopen lease from mutating a reused slot. The registry binds each request
to a camera node plus stable camera identity. Its packed 60-byte v1 snapshot
publishes that node, requested and presented extents, latest presented frame
metadata, and independent target and history generations. Changing the camera
or extent advances the appropriate generation and requires temporal history to
be reset. Replacing the Scene marks every surviving request's camera stale and
releases the presenter; create, update, snapshot, and bind paths revalidate the
node/stable-ID pair against the current Scene before the request can present
again.
The managed publication boundary queues one coalesced stable-ID refresh after
New, the current successful Open, canonical Undo/Redo or recovery restore, and
a successful canonical rollback. Superseded, unpublished, and pre-replacement
failures do not refresh against a graph that was never published.
Every surviving tab is then re-resolved by stable camera ID. A changed
transient node ID is accepted only for one unambiguous stable-ID match;
malformed duplicate matches fail closed. Missing/disabled tabs retain their
logical identity so a later Undo or replacement can restore them. If the
selected tab is unavailable, the first available retained tab becomes the
presenter.

Exactly one request may bind the existing shared swapchain. Binding refuses
to steal it from another request; managed tab transfer explicitly unbinds the
old logical owner before binding the new one while the same detached HWND
remains the physical surface owner. Other logical requests retain only
identity, requested extent, and generation metadata. A resize updates only
the selected request, and switching later reconciles only that target with the
current window extent, so inactive target/history generations stay isolated.
`camera-view-requests-v1` does not advertise a dedicated offscreen target,
asynchronous readback, per-view GPU post-effect history, or simultaneous live
Camera View/PIP rendering. Those require a separate capability and
render-resource implementation.

The Camera View opens without activation, follows owner minimize/close
lifetime, snaps in physical pixels with per-monitor DPI, and clamps restored
placement to the nearest monitor work area after display-topology changes.
Only window geometry and the last stable camera ID are persisted; scene and
active-camera state are deliberately excluded. Adding, switching, refreshing,
and closing logical slots are presentation operations: none writes authored
`Active`, marks the Scene dirty, or records Scene Undo.

### Dockable tool panels

Floating and re-docking are editor-shell services, not Camera-only behavior.
[The tool-panel docking contract](EditorToolPanelDocking.md) documents the
stable IDs, persistence schema, transaction boundaries, and verification
entry points.
The explicit registry contains six stable IDs: `hierarchy` (Scene Outliner),
`inspector` (Details), `console`, `build`, `assets`, and `profiler`.
Unknown IDs are rejected rather than being silently assigned a layout slot;
future panels must be added deliberately with a stable ID and accessible name.
Camera View remains a specialized consumer because it transfers the native
render child and carries a transient request lease or legacy preview override,
while ordinary tool panels transfer one managed visual between their original
dock slot and one owned floating window.

Each registered panel has one committed visual owner. A failed float operation
rolls back to `Docked`; a failed re-dock remains truthfully `Floating`, so the
panel cannot disappear or be duplicated. `Hidden` is a main-window-owned
state: hiding a floating panel safely re-docks it first. Owner shutdown closes
floating tools through the same re-dock path. The explicit Dock action and
stable accessible window names keep the operation available without relying
on pointer-only title-bar gestures.

Console, Build, Assets, and Profiler share one bottom tab slot while docked;
when at least one bottom tool is docked, exactly one is active at a time. Each
of the four can be floated, hidden, restored, and persisted independently, and
several tools may be floating simultaneously. Activating a floating or hidden
bottom tool restores it through the same ownership transition rather than
creating a second visual. `Ctrl+J` and **View > Bottom Dock** only suppress or
restore the aggregate dock presentation. They preserve every child tool's
`Docked`, `Floating`, or `Hidden` state and the active tab; the dock-local
**Hide** action instead hides only the selected tool.

Floating tool windows snap to the owner and current monitor work-area edges
using a 12-DIP threshold converted with the floating window's per-monitor DPI.
Restoration permits negative desktop coordinates, clamps a reachable title
region to the nearest work area after monitor-topology changes, and rejects
non-finite geometry, unknown IDs, duplicate panel records, and unsupported
versions. The six-panel placement snapshot uses schema version 2. The layout
is per-account UI state; it never mutates Scene, Project, undo, dirty, or
gameplay-camera state.

Layout reset is transactional across the registered tool panels. It captures
all six panels' initial `Docked`, `Floating`, or `Hidden` states and the active
bottom tab before changing any host. It commits the default layout only after
every re-dock succeeds and restores the complete starting state if an
intermediate transfer fails. A failed rollback is reported and the persisted
layout is not deleted.

### Save and recovery

`EditorDocumentHost` supplies common identity, dirty, save, and transaction
state. The Scene adapter, the authored Substrate graph in each open Material
Editor, and the project-owned `ProjectSettings.ini` document are connected to
this host. Blueprint and Prefab documents are not registered in this contract.
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

Legacy PBR/effect panels still use their immediate native writers and are not
part of the common material transaction. Common material autosave/recovery,
cross-window Undo routing, and Blueprint/Prefab registration are not provided.

3D Prefab/Blueprint instance refresh nevertheless has a bounded native scene
transaction. The adapter validates the complete subtree and source-link byte
limit before mutation, snapshots the combined 2D+3D scene, removes the old
subtree before loading the replacement so authored camera stable IDs remain
unchanged, and publishes one Undo record only after parent, transform, and
source-link restoration succeeds. Any failure restores the prior scene and
leaves Undo history unchanged. This is an instance-edit safety boundary, not
Blueprint/Prefab document registration in `EditorDocumentHost`.

The root override adapter keeps calculation and mutation boundaries explicit.
Managed setters mark a bit only after the complete edit succeeds; native refresh
captures only bits already present on the stable instance, recreates the subtree,
and reapplies those values before publishing Undo. `POVR3D` stores the mask after
`PINS3D`; the ordinary refresh export remains a full Revert. A Color override
cannot be preserved onto an Empty root, so that refresh rolls back atomically.
Selective root Revert computes `remaining = current & ~requested` without scene
mutation, rejects unknown or already-cleared bits, and then uses the same native
refresh transaction to restore only the requested source values.
Selective root Apply uses a side-effect-free managed calculation to replace only
the selected root `N3D` Color or `FLG3D` flag in the Prefab/Blueprint component
text. The UI adapter reads the instance value, writes the source atomically,
clears only the selected mask bit, and refreshes other instances while retaining
their explicit root overrides. The initiating instance remains selected after
those replacement transactions. Invalid source, stale source text, or write
failure leaves both the source and instance mask unchanged.

Root reflected component edits use the same explicit boundary. Managed code
marks `PCOVR3D` metadata only after a verified property write or successful
atomic multi-edit. Refresh captures component type ID, property mask, and value,
then resolves that type in the replacement subtree so source component reorder
does not redirect an override. A removed component/property fails closed and
restores the old scene and Undo history. Full Apply clears the selected
instance's component markers; refresh of other instances preserves theirs.
Selective component Revert removes only the chosen type/property from the
captured snapshot, reloads that value from source, and retains every other root
and component override in the same native transaction.
Selective component Apply resolves the source component by type, changes only
the chosen `CPROP3D` value through a pure calculation and atomic file adapter,
then clears only that instance marker. Other instances refresh transactionally
and preserve their own explicit root/component overrides.

Project Settings uses the canonical settings-file path as its stable identity
and participates in common dirty, Undo/Redo transaction, Save All, and
Save/Discard/Cancel owner-close behavior. The managed adapter preflights the
same bounded INI grammar as the native parser and verifies that every input
entry, including unknown/custom keys, survives native load and canonical
serialization. Parse failures leave the source untouched and register an
unsaved, read-only settings document. Persistence uses the existing
cross-process lease and atomic publisher on a worker task; a failed write stays
dirty. If history restore fails and its native rollback cannot be verified, a
one-way safety latch blocks both editing and persistence until a complete
source reload succeeds.

Startup captures and parses `Config/ProjectSettings.ini` on a worker, then
applies the immutable result to native state only on the Dispatcher. A
generation/cancellation gate discards results from a superseded attach, project,
startup failure, or closed window. Reads require the exact
`project root/Config/ProjectSettings.ini` chain and reject reparse points,
malformed UTF-8, parser-limit violations, and files over the persistence limit.

Build, Run, Standalone, and Package must cross a Project Settings durability
gate before saving the Scene or starting downstream work. The gate invokes the
hosted save contract for both clean and dirty Settings through the common save
exclusion, so external disk/manifest drift cannot pass through the clean
shortcut; cancelled, suspended, transactional, or failed persistence aborts
the operation. After the Package confirmation dialog returns, Package runs the
Settings gate again before taking the project snapshot used by preflight and
publication. If a concurrent editor made the
manifest's `Game.DefaultScene` authoritative, the save callback applies that
durable canonical rewrite only when live state still equals its pre-write
snapshot, then reconciles `Project.InitialScene` before Scene/build gates run.
Otherwise it preserves the newer live state and remains dirty.

The Package action also carries the SHA-256 of the exact durable UTF-8 Settings
bytes into `PackageCore`. The Config Stage must contain exactly one
case-canonical `ProjectSettings.ini` whose captured hash matches that
checkpoint; a missing file, malformed checkpoint, or gate-to-Stage edit fails
closed as `CONFIG_CHANGED_DURING_PACKAGE`. The existing final source-snapshot
validation independently rejects edits that occur after Stage.

Initial-Scene path-follow transactions return the exact Settings source they
published. A clean editor applies and round-trip-verifies that complete
snapshot without rereading on the UI thread, so unrelated keys committed by a
different editor cannot be silently replaced by a DefaultScene-only refresh.

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
canonical ACSMAT persistence. This is an ACS-specific graph and does not define
compatibility with another serialized graph format.

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

Manual import opens a mixed-selection settings surface before publication.
Texture, mesh, and audio intent is normalized into an ordinal canonical recipe
containing importer identity, importer version, settings schema, and a
destination-independent SHA-256 recipe hash. The immutable recipe is written
through the existing import journal into authoritative `.acsmeta`; its complete
settings dictionary and importer version already participate in derived-data
cache identity. The last accepted profile is strict, bounded Editor state under
`Saved/Editor`, not an Asset. Folder drag-and-drop reuses that project-local
profile so large directory imports do not open one modal per file. See
`docs/AssetImporterSettings.md` for validation and transaction details.

The first processed-import worker is source-preserving: it reads and validates
the complete staged source bytes before any payload or `.acsmeta` publication.
Its derived-data key is path independent and combines source content with the
canonical importer recipe. Strict, bounded, hash-verified envelopes are
published atomically under
`Temp/DerivedDataCache/AssetImports/v1`; a malformed, truncated, mismatched, or
stale entry is ignored and rebuilt. Manual Import and Reimport use the same
worker and cache path, so metadata and payload identity cannot diverge between
the two flows. Native texture/mesh/audio transcoding and richer per-stage
progress are intentionally later processors; v1 does not claim that copied
source bytes are GPU-ready derived formats.

Image and Material tiles retain the bounded decoded-image LRU for the current
browser lifetime and add a project-local persistent thumbnail DDC under
`Temp/DerivedDataCache/AssetBrowserThumbnails`. Its key is derived from the
Asset Database content SHA-256, thumbnail generator version, asset kind, and
requested edge; source paths and timestamps are not cache identity, so a safe
rename can reuse identical derived data while a content, quality, or generator
change cannot. Only records whose current size and write timestamp still match
the immutable view snapshot are allowed to use or publish persistent entries;
a persistent hit re-stats the source before it can enter the in-memory cache or
current UI generation. This is a normal filesystem-race guard, not proof
against a non-cooperating process that replaces bytes while deliberately
preserving both size and timestamp.
Schema-v2 payloads are canonical lossless Pbgra32 pixels with an embedded key,
byte length, and SHA-256 checksum in the cache envelope. The fixed little-
endian raw header declares and validates its version, pixel format, width,
height, stride, and exact byte count. Persistent cache bytes never enter an
in-process compressed-image codec: after all bounds agree, the Editor creates
a frozen surface from the already-sized pixel buffer. Publication uses a
flushed private sibling file and atomic replacement. A malformed envelope, checksum
failure, legacy/compressed payload at a v2 key, noncanonical stride, or
unexpected dimensions removes that entry and falls back to the same full-
quality generator. The schema, generator, outer magic, and key encoding were
all advanced together, so schema-v1 PNG entries cannot be interpreted as raw
surfaces and simply age out under the normal cache budget. Cache failure never
replaces the source asset or lowers preview quality.

The managed cache is bounded to 256 MiB and 4096 entries. Its worker-side
initial reconciliation builds byte/count accounting and deterministic
timestamp/path eviction order. Normal hits and publications update that
bounded ledger incrementally instead of enumerating and sorting the complete
cache after every thumbnail. A full filesystem reconciliation runs on the
first cache request after the periodic deadline, once when entering the
high-water band, on explicit maintenance, or after an
unknown/missing/size-mismatched/corrupt entry exposes cross-process drift.
Reconciliation has explicit directory and entry-inspection ceilings; a
process under the same local account that externally inflates the cache beyond that bounded scan
disables the persistent layer instead of causing an unbounded allocation or
loop. It removes only canonical private temporary files older than the stale
threshold, leaving a second Editor's fresh atomic write untouched. Publication
and any required oldest-first eviction complete under the same in-process
gate, so the published local entry set is back within its configured
byte/count limit before Store returns; the flushed private publication may
transiently consume at most one additional bounded payload. Deterministic
injected filesystem counters keep the self-test contract that steady-state N
publications, including eviction, perform N atomic writes with only bounded
high-water reconciliation rather than N full scans or accumulated entry
stats.

Cook DDC and thumbnail DDC each own one bounded canonical-path pool. A pool
retains at most 512 SHA-256 keys and 256 Ki UTF-16 code units for the key,
prefix directory, and complete entry path; least-recently-used entries are
released first. An individually oversized path is returned without retention.
The immutable returned strings remain valid after eviction, and the pool
lifetime ends with its package operation or Asset Browser cache owner. Cook
uses a dedicated path gate and thumbnail operations reuse the existing cache
gate. The pools expose request, hit, miss, eviction, bypass, retained-entry,
and retained-code-unit snapshots. Canonical lowercase SHA-256 validation,
project containment, and reparse checks remain fail-closed in Release.
The source-import pipeline remains outside this optimization because its
static worker has no equivalent owner lifetime; no process-global DDC path
table is introduced.

Project-root containment, ordinary-directory traversal, and entry reparse
checks run before reads, writes, cleanup, and publication. Cache lookup,
encoding, reconciliation, and cleanup stay inside the existing background
image generation, cancellation, and generation gates; an obsolete request may
leave only valid content-addressed reusable data and cannot publish an image
into the current view.

The thumbnail DDC has no cross-process cache lease. Two Editor processes may
redundantly generate, replace, or evict the same content-addressed entry. A
complete atomic entry is equivalent regardless of the winner; an I/O,
sharing, or path-safety race disables the persistent layer for that browser
lifetime and falls back to the bounded memory cache and full-quality generator.
Source assets and Asset Database identity never depend on cache success. This
is a consistency fallback, not a filesystem compare-and-swap claim against a
non-cooperating process under the same local account replacing a validated cache path.

Rename, move, duplicate, replace-reference, and delete paths inspect or
rewrite supported references transactionally. Referenced assets are refused
when an operation cannot preserve the graph safely. Interrupted import,
reimport, and Trash transactions leave recovery information under
`Assets/.acsdb` instead of pretending that publication succeeded.

The browser authors source assets. It does not currently mount a packaged
`.acpak` as a writable authoring layer.

## Managed operation diagnostics

Long-running managed Editor work has a version-1 typed diagnostic contract in
`EditorOperationDiagnostics.cs`. Every diagnostic carries a nonzero canonical
operation GUID, contiguous per-operation sequence, service (`Build` or
`Package`), severity, stable `ACS.*` code, message, and optional Asset ID and
path. Codes and required fields are validated before publication; unknown or
lowercase codes and empty operation IDs fail closed.

`EditorOperationSession` owns the lifecycle. Success, failure, and cancellation
append exactly one terminal diagnostic, freeze the ordered aggregate, and are
idempotent under repeated completion. Leaving an operation scope without a
terminal outcome produces `ACS.OPERATION.INCOMPLETE` instead of silently
claiming success. Each operation retains at most 256 diagnostics: the final
slot is reserved for terminal completion and overflow produces one
`ACS.OPERATION.DIAGNOSTICS_TRUNCATED` warning. `EditorOperationJournal` removes
completed operations from its active set atomically, retains a bounded result
history, and returns retained results in operation-start order even when
completion order differs.

Build, Build and Run, and Standalone Build publish typed start and terminal
diagnostics around their existing cancellation lease. The Package dialog opens
a fresh operation for each Package invocation, maps existing stable preflight
issue codes into `ACS.PACKAGE.*`, and completes only after success, validation
failure, cancellation, or an exception. The same events are formatted into the
existing Build log with their operation ID, so this foundation does not remove
or reinterpret legacy process output. Native optional-service state and typed
errors use the independent ABI payload above. Correlating native errors with
managed operation GUIDs and cooperative cancellation of long-running native
jobs are not part of the service-status query contract.
`--operation-diagnostics-selftest` fixes contract validation, deterministic
ordering and eviction, observer isolation, overflow truncation, and
cancellation-safe terminal publication.

## Temporal rendering continuity

TAA, SSR, and SSGI use the shared `TemporalHistory.h` cold-start policy. Frame
zero always uses the current view-projection matrix, current-frame weight
`1.0`, and no motion-vector reprojection. Warm frames alone may consume the
caller's previous matrix, blend weight, and available motion texture.

The Editor invalidates motion, TAA, SSR, SSGI, and volumetric-cloud history as
one operation when the Scene is replaced, the logical camera/request owner or
Perspective/Orthographic projection changes, 2D/3D view mode changes, Play
restores the Editor camera, or an explicit reset/focus/frame camera cut occurs.
TAA/SSR/SSGI setting transitions are invalidated when settings are applied,
including an off/on pair between presented frames. A skipped SSR or SSGI pass
also invalidates its private history when the effect is disabled, unready, or
missing its G-buffer prerequisite. Interactive pan/orbit/zoom remains
continuous and keeps temporal accumulation.

## Profiler

`ProfilerPanel` samples a versioned native snapshot and combines it with
managed Editor diagnostics.

Native metrics include:

- submitted frame index, frame delta, FPS, CPU frame, GPU-ready native
  active-render time, and submit/Present time;
- asynchronous GPU frame timings and per-pass timings;
- current, average, and rolling-window peak values;
- draw, dispatch, triangle, and resource counts;
- cloud CPU/GPU cost, trace resolution, render scale, and march/light steps;
- viewport dimensions and active feature flags;
- exact main-view frustum-tested, visible, and culled node counts, plus the
  resolved game-camera node when an authored camera drives Game View.

Profiler v5 is a packed 256-byte version-5 contract advertised through the
`profiler-v5` capability. Its first 224 bytes remain the version-4 layout:
native providers accept an explicit v4 request and return only that prefix, so
an older host can poll a newer DLL safely. Versions 3 and 4 remain known
historical capabilities. A request smaller than the two-word negotiation
header is rejected before either word is read. At an explicit capture reset,
the provider invalidates current CPU/GPU timing payloads, GPU validity,
smoothed FPS, and cloud workload, advances the reset serial, and publishes zero
presented frames. Managed capture accepts no sample until a later presented
frame carries that exact serial. Exact
volumetric-cloud accounting remains an optional, separate packed 168-byte
version-1 contract (`cloud-workload-v1`), so it is negotiated and queried
independently. The managed host calls an optional export only when its
capability was advertised. The cloud query distinguishes an available attempt,
runtime unavailability, and an ABI error; an unattached/warming renderer,
inactive cloud pass, or uninitialized cloud renderer is shown as unavailable
rather than as a zero-cost frame.

Before either snapshot call, the panel consumes its independent optional
service diagnostic. A pending/failed Profiler disables native Reset but not
local Pause or export of already retained history. A pending/failed Cloud
service suppresses only its native snapshot query; the local visibility filter
and remaining Profiler controls stay live. `Inactive` remains callable and
reports the exact scene-feature reason rather than being treated as a contract
failure.

Frustum-culling counts describe the decision actually reused by the main-view
draw paths. The UI reports culling disabled when the aggregate compatibility
fallback cannot honor per-node decisions; it never presents visibility tests
as saved draw work unless those nodes were excluded from rendering.

The production submission-mask traversal is shared by the normal/depth
G-buffer pass, motion-vector pass, opaque PBR count and draw paths,
interactive-water specialized/fallback draws, and refraction preflight and
draw paths. The normal/depth pass preserves one aggregate draw when culling is
disabled or rejects nothing; a partial rejection coalesces adjacent visible
vertex ranges. Shadow casters deliberately use light-space coverage and VXGI
uses the complete world-space scene, so neither consumes the active camera
mask. A disabled, missing, or shorter-than-node mask fails open for decisions
it cannot safely represent. Native coverage fixes perspective and orthographic
plane extraction, default profiler publication, the shared traversal's
recorded command forms through a fake RHI, range coalescing and overflow, and
the real `DrawScene3D` profiler path when a DX12 adapter is available.

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
Capture rows retain the native active/Present peaks, reset serial, and
presented-frame count. Validation requires one reset generation across every
row, a monotonically increasing presented count, and each row's rolling peak
to cover only that row's current value. Summary peaks are the maximum rolling
peak observed among retained rows, not the latest rolling value, because an
old maximum may leave the native 120-frame window during a capture. Validation
also rejects a cloud-workload profiler frame outside the accepted first/last
frame range.
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

The frame-budget strip defaults to 300 FPS (3.33 ms) and can analyze other
targets without changing render quality or cadence. CPU and individual
completed-GPU-query p95 values retain their bounded 10 Hz sample counts and
exact budget-violation rates, so a missing GPU timestamp remains `N/A` instead
of looking artificially fast. `Export CSV` snapshots the current history,
writes it asynchronously to a unique sibling temporary file, and atomically
moves it over the selected destination. See
[`EditorPerformanceProfiler.md`](EditorPerformanceProfiler.md) for
interpretation and capture guidance.

### UI-stall evidence

`EditorDispatcherWatchdog` is driven by a ThreadPool timer, independent of the
WPF Dispatcher. A minimal `DispatcherPriority.Input` heartbeat records startup
phase and normal interaction health every 500 ms, so starvation is measured at
the same scheduling class as input dispatch rather than a hidden background panel.
If no heartbeat arrives for two seconds, the watchdog
writes `DISPATCHER_STALL`; the first later heartbeat writes
`DISPATCHER_RECOVERED` with the measured duration and phase. This distinction
is important because a `DispatcherTimer` cannot produce evidence while the
Dispatcher itself is blocked.
An explicit profiler capture reset rebases the watchdog's heartbeat origin,
heartbeat/stall counts, active-stall state, and extrema so startup evidence
cannot leak into the capture interval.

Each Editor process writes a process-scoped
`%LOCALAPPDATA%\ACS\Editor\Diagnostics\interaction-health-<UTC>-<PID>.log`.
A single FIFO worker preserves transition order, rotates the current log lifetime
at 2 MiB to `.previous`, and performs console/file I/O away from the UI thread.
Reliability-soak completion awaits its final diagnostic asynchronously. Normal
window close queues only a best-effort terminal line and never waits on the WPF
Dispatcher; the retained worker task has a terminal exception boundary, and
process exit is allowed to omit that final diagnostic.
Lines are bounded and control-, bidi-, and line-injection safe; the phase token
additionally removes key/value separators. The watchdog diagnoses a stall; it
does not abort a native call or attempt unsafe UI-thread recovery.

Cold ABI/subsystem creation and per-account layout/workspace reads are also
kept off the Dispatcher. Native host publication is generation-gated and
cleans up every unpublished handle; delayed layout results cannot overwrite
mouse-, keyboard-, or caption-move changes made after the read began. The
contracts, failure boundaries, and focused checks are documented in
[`EditorStartupResponsiveness.md`](EditorStartupResponsiveness.md).

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
7. deterministic PE product metadata/application-manifest publication and
   ZIP publication through atomic replacement;
8. complete verification of a fixed private ZIP copy, bounded private
   extraction, and a hidden authenticated first-frame runtime launch; and
9. atomic package-report publication with stable gate diagnostics.

`PackageLaunchSmoke` never executes the ZIP or the original Release output.
It verifies the copied archive before extraction, takes executable identity
only from that verified manifest, launches without shell/focus/visible window,
and requires one nonce-authenticated readiness marker after a successful
submit/present plus exit code zero. Its 45-second default deadline and external
cancellation both terminate the process tree and drain bounded output before
cleanup. Successful report JSON deliberately excludes observational values
(wall time, measured duration, nonce, TEMP path, output) so repeated runs of
the same package and limits are byte-identical. Package and smoke durations
remain UI-only Build Results telemetry.
The private ZIP is pinned without write/delete sharing across hash, verify,
and extraction; the extracted executable is re-hashed to its manifest and
similarly pinned across PE inspection and process creation. The child
environment is rebuilt from an explicit Windows/runtime allowlist. Credential,
CI, signing, cloud, and inherited tool/PATH state never crosses into packaged
project code; writable profile and TEMP roots are isolated below smoke staging.

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
- Camera View requests currently share one swapchain presenter. Dedicated
  offscreen targets, asynchronous readback, independent per-view post-effect
  histories, and multiple simultaneous live PIPs are not implemented or
  advertised.
- Asset collections and browser layout are Editor conveniences; authoritative
  asset identity lives in `.acsmeta`.
- The project mutation lease coordinates ACS processes. It is not an
  operating-system sandbox against a hostile process under the same local account replacing a
  validated directory between filesystem calls.
- Packaged asset packs are runtime/cook products, not editable Content Browser
  mounts.
- Signing, installers, store deployment, source-control integration, and
  multi-platform distribution remain outside the current local Editor flow.

## Verification

Use `scripts/verify_editor.ps1` from the `acs` directory. It redirects build
outputs, logs, NuGet state, temporary files, and application profile folders
to one unique directory below the system temp root. By default that directory
is removed before the final result summary. Repository `Binaries`,
`Intermediate`, and `Saved` trees and normal Editor settings are not
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
# Run this command from a Visual Studio x64 Native Tools prompt (or after
# calling that installation's vcvars64.bat) so CMake can resolve cl/nmake.
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_editor.ps1 -Mode full
```

`fast` and `managed` need the .NET SDK only. `full` additionally requires an
x64 MSVC developer environment; the verifier does not guess a Visual Studio
installation or mutate the caller's toolchain environment. If `cl`/`nmake`
are absent, native configure fails explicitly while the independent managed
steps still report their results.

| Mode | Verification surface |
|---|---|
| `fast` | isolated Editor Release build; ABI negotiation, document host (including Project Settings), Material Preview, Asset Browser (including thumbnail DDC), profiler, managed operation diagnostics, and package-responsiveness self-tests |
| `managed` | isolated Editor build; every public self-test switch registered in `App.xaml.cs`; Blueprint self-test; isolated `acspackage --self-test` |
| `full` | managed mode; isolated native CMake build with tools disabled; complete CTest registration |

Native unit coverage anchors the temporal contract in
`src/render/TemporalHistory.h` and
`tests/post_effect_quality_tests.cpp`, and the camera-mask command policy in
`src/editor_abi/EditorFrustumCulling.h` and
`tests/rhi_command_statistics_tests.cpp`.

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
| tool-panel docking | `DockableToolWindow.cs`, `ToolPanelDockingContract.cs`, `ToolPanelDockingSelfTest.cs` |
| rendered viewport | `editor/AcsEditor/EngineViewport.cs`, `EngineInterop.cs` |
| native bridge | `src/editor_abi/EditorAbi.cpp`, `EditorCameraViewRequests.h`, `EditorFrustumCulling.h` |
| renderer/RHI | `src/render/Renderer.*`, `src/render/Dx12`, `src/render/Diligent` |
| document/save/autosave | `EditorDocumentHost.cs`, `MainWindow.Documents.cs`, `MaterialDocumentHostRegistration.cs`, `SceneSaveAllPlanner.cs`, `SceneAutosaveStore.cs` |
| scene compatibility | `SceneDocumentMode.cs`, `SceneWorldDocumentEnvelope.cs`, `CanonicalSceneAdapter.cs` |
| Asset View | `AssetBrowserPanel*`, `AssetDatabase.cs`, asset workflow classes |
| profiler | `ProfilerPanel*`, `EditorProfilerModel.cs`, `EditorCloudWorkload.cs`, native profiler/workload snapshots |
| package | `PackagingService.cs`, `PackageCore.cs`, `tools/acspackage` |
| aggregate verification | `scripts/verify_editor.ps1` |
