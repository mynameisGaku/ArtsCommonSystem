# ADR 0001: Single 3D scene document with Orthographic 2D authoring

- Status: Accepted
- Date: 2026-07-27
- Owners: Editor, Runtime, Asset, and Packaging

## Context

ACS historically exposed separate 2D and 3D editing surfaces backed by
`.acscene` and `.acs3d` serializers. That made a view switch look like a
document switch, allowed the two payloads to diverge, and made Save, Play,
Standalone, and Package choose different sources.

Established engines treat 2D authoring as a view and tooling specialization
over a world rather than as an unrelated project scene. ACS needs the same
authoring model without silently corrupting existing `.acscene` projects or
pretending that the current standalone adapters already implement every
editor-only directive.

The established-engine comparison is deliberately nuanced:

- Unity keeps one Scene document while its 2D Scene-view mode selects an
  XY-facing Orthographic editor camera and pan-oriented navigation.
- Unreal keeps one Level/World while editor viewports and authored Camera
  actors independently select Perspective or Orthographic projection.
- Godot keeps one scene-tree document model, but `Node2D` and `Node3D` have
  distinct transform/rendering domains and dedicated 2D/3D workspaces. It is
  therefore evidence for one document identity, not evidence that every
  engine implements 2D as a literal 3D orthographic camera.

ACS follows the Unity/Unreal authoring shape for new content because its
canonical editor graph is already spatial and camera-driven. It retains
`.acscene`, `FScene2D`, 2D physics, and the dedicated 2D renderer as explicit
compatibility/runtime domains instead of claiming that those APIs are merely
aliases for 3D.

## Decision

1. The Editor owns one logical scene document and one stable document identity.
   Perspective and Orthographic are view presets. Camera, projection,
   selection, gizmo, and workspace layout are not scene-content identity.
2. New `3d`, `blank`, and `2d` projects persist `Assets/main.acs3d` using
   `ACS3D v2`. The `2d` template selects the XY-front Orthographic preset and
   authors ordinary 3D nodes.
3. Existing `.acscene` projects remain supported through an explicit
   `legacy-acscene-v1` adapter. Opening or changing a view never converts,
   hydrates, or swaps the source adapter.
4. `.acsproject.initialScene` is the portable presentation path.
   `.acsproject.canonicalSceneAssetId` is the durable package root identity.
   Path and identity must resolve to the same authoritative Asset Database
   record. During the compatibility period, `Game.DefaultScene` mirrors the
   manifest path as the runtime-settings projection; project open fails closed
   after recovery if those two durable paths disagree.
5. During the compatibility period, `SceneWorldDocumentEnvelope` groups the
   legacy 2D and 3D native snapshots into one atomic Editor transaction. This
   envelope is an internal managed history/recovery format, not a shipping
   scene format.
6. Package always emits the virtual bootstrap path `main.acscene` for runtime
   compatibility. `sceneBootstrap.contract` and
   `sceneBootstrap.sourceFormat` discriminate the actual source adapter;
   callers must not infer source format from the virtual path.
7. Unsupported versions, directives, missing canonical identity, mismatched
   path/identity, and partial dual-payload restoration fail closed. The Editor
   keeps the previous published document or a neutral unavailable viewport.
8. Before a source is published, the native surface remains suppressed and the
   WPF host remains hidden. A session with no configured source starts as a
   blank `ACS3D v2` document in Perspective; it must never fall through to the
   legacy 2D adapter or reveal a previous/default 2D frame.
9. Scene View owns only the editor navigation camera. When the main viewport
   owns the render surface, Game View resolves an authored game camera (or a
   deterministic scene-bounds fallback), and Scene/Game tab changes neither
   start nor stop Play nor mutate the editor navigation pose. Multiple
   authored cameras resolve by active preference, priority, stable camera id,
   then node id. A Camera View request is a transient, non-persistent preview
   owner of the shared V1 surface and must release that override before the
   surface is returned to the main Game View.

## Compatibility matrix

| Input | Editor document | Initial view | Package source format |
|---|---|---|---|
| New `3d` / `blank` | One `ACS3D v2` source | Perspective | `legacy-acs3d-v2` |
| New `2d` | One `ACS3D v2` source | Orthographic | `legacy-acs3d-v2` |
| Existing `.acs3d` | One `ACS3D v2` source | Project/view policy | `legacy-acs3d-v2` |
| Existing `.acscene` | One legacy source adapter | Orthographic | `legacy-acscene-v1` |

## Consequences

- 2D and 3D tools can share Save All, dirty state, Undo/Redo, autosave,
  recovery, Play, and package identity.
- Legacy projects remain reversible and are never implicitly rewritten.
- The fixed package bootstrap filename remains a compatibility detail and
  cannot be used as a format discriminator.
- A future canonical native world serializer can replace both legacy
  serializers behind the same Editor document identity and transaction
  contract.
- Standalone runtime parity is still gated per directive. Editor-only ACS3D
  directives must not be advertised as shippable until the runtime adapter and
  package smoke tests support them.

## Transitional implementation status

The accepted target and the present implementation are intentionally
distinguished:

- The Editor currently has one managed document identity, one dirty/history
  transaction, and one project/package bootstrap authority.
- Native `FScene2D`/`FScene3D` graphs, serializers, physics domains, and
  renderers remain separate compatibility subsystems. The managed
  `SceneWorldDocumentEnvelope` can restore both atomically, but it is not a
  canonical mixed-world scene schema.
- `.acscene` cannot be switched to Perspective or mixed with ACS3D content.
  Changing the viewport projection never migrates it.
- No automatic `.acscene` conversion is implemented. A future explicit
  converter must preflight unsupported content, preserve stable asset
  identities, write a new destination without overwriting the source, report
  every lossy mapping, and either publish the complete converted document or
  leave the previous document untouched.
- Retirement of the legacy adapters requires a canonical world schema that can
  host the engine's dedicated 2D and 3D transform/rendering domains under one
  document root, plus golden semantic round-trip, recovery, Play, standalone,
  and package parity gates.
- Camera View V1 exposes up to eight logical preview leases but exactly one
  physical shared-swapchain presenter. It does not provide simultaneous live
  camera outputs, dedicated offscreen targets, or asynchronous readback.
  Those require a later render-target architecture.

Current source and fixture self-tests verify state, ordering, ownership, and
rollback contracts. They do not prove first-present pixels. A captured
first-frame/CRC test remains a required release gate before the legacy startup
path can be retired.

## Verification

`SceneContractFixtureSelfTest` is executed by
`--scene-editor-migration-selftest` and the full Editor verifier. It pins:

- the shared `Assets/main.acs3d` project template contract;
- Orthographic as a view-only 2D preset;
- canonical bootstrap path, contract, and source-format discrimination;
- exact dual-payload transaction round trips;
- blank startup selecting `ACS3D`/Perspective rather than legacy 2D;
- editor/game camera and Play-lifetime separation;
- deterministic multi-camera ordering;
- fail-closed handling for unknown formats and malformed envelopes.

## External reference calibration

- [Unity Scene view navigation](https://docs.unity3d.com/Manual/SceneViewNavigation.html)
  documents Perspective/Orthographic Scene-camera projection and the
  XY-perpendicular, pan-oriented 2D mode.
- [Unreal Engine viewport toolbar](https://dev.epicgames.com/documentation/en-us/unreal-engine/viewport-toolbar)
  separates editor Perspective/Orthographic views from Game View, and
  [Unreal cameras](https://dev.epicgames.com/documentation/unreal-engine/cameras-in-unreal-engine)
  places projection, frustum, and player-view behavior on authored cameras.
- [Godot nodes and scenes](https://docs.godotengine.org/en/stable/getting_started/step_by_step/nodes_and_scenes.html)
  defines one scene-tree resource model, while
  [Godot's editor overview](https://docs.godotengine.org/en/stable/getting_started/introduction/first_look_at_the_editor.html)
  documents distinct 2D, 3D, and Game workspaces.

The broader migration and persistence gates remain:

```powershell
dotnet run --project acs/editor/AcsEditor/AcsEditor.csproj -c Release -- --scene-editor-migration-selftest
powershell -NoProfile -ExecutionPolicy Bypass -File .\acs\scripts\verify_editor.ps1 -Mode full
```
