<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR 0001: Single scene document with Orthographic 2D authoring

- Status: Accepted
- Scope: Editor, Runtime, Asset, and Packaging

## Context

ACS supports the legacy `.acscene` source format and the spatial `.acs3d` source format.
Treating a viewport projection change as a document change would give Save, Play,
Standalone, and Package more than one possible source identity. It would also allow
the two payloads to diverge while the Editor presents them as one scene.

The Editor already has a spatial node graph, authored cameras, a navigation camera,
one dirty/history boundary, and one package bootstrap authority. The document model
must preserve those ACS contracts while keeping legacy projects readable.

## Decision

1. The Editor owns one logical scene document and one stable document identity.
   Perspective and Orthographic are view presets. Camera, projection, selection,
   gizmo, and workspace layout do not change scene-content identity.
2. New `3d`, `blank`, and `2d` projects persist `Assets/main.acs3d` using
   `ACS3D v2`. The `2d` template selects the XY-front Orthographic preset and
   authors ordinary spatial nodes.
3. Existing `.acscene` projects remain supported through the explicit
   `legacy-acscene-v1` adapter. Opening a project or changing a view never
   converts or swaps the source adapter.
4. `.acsproject.initialScene` is the portable presentation path.
   `.acsproject.canonicalSceneAssetId` is the durable package-root identity.
   Both must resolve to the same authoritative Asset Database record.
5. During compatibility, `Game.DefaultScene` mirrors the manifest path as the
   runtime-settings projection. Project open fails closed after recovery if the
   two durable paths disagree.
6. `SceneWorldDocumentEnvelope` groups the legacy 2D and 3D native snapshots into
   one atomic Editor transaction. The envelope is an internal history/recovery
   representation, not a shipping scene format.
7. Package emits the virtual bootstrap path `main.acscene` for runtime
   compatibility. `sceneBootstrap.contract` and `sceneBootstrap.sourceFormat`
   identify the source adapter; the virtual path does not identify the format.
8. Unsupported versions or directives, missing canonical identity, mismatched
   path/identity, and partial dual-payload restoration fail closed. The Editor
   retains the previous published document or shows an unavailable viewport.
9. Before a source is published, the native surface and its WPF host remain
   hidden. Startup without a configured source creates a blank `ACS3D v2`
   document in Perspective and never reveals a previous frame.
10. Scene View owns the editor navigation camera. Game View resolves an authored
    game camera or a deterministic scene-bounds fallback. Switching Scene/Game
    view does not start or stop Play and does not mutate the navigation pose.
11. Authored cameras resolve by active preference, priority, stable camera ID,
    and node ID. A Camera View request is a transient owner of the shared V1
    render surface and releases its override before returning that surface to
    the main Game View.

## Compatibility matrix

| Input | Editor document | Initial view | Package source format |
|---|---|---|---|
| New `3d` / `blank` | One `ACS3D v2` source | Perspective | `legacy-acs3d-v2` |
| New `2d` | One `ACS3D v2` source | Orthographic | `legacy-acs3d-v2` |
| Existing `.acs3d` | One `ACS3D v2` source | Project/view policy | `legacy-acs3d-v2` |
| Existing `.acscene` | One legacy source adapter | Orthographic | `legacy-acscene-v1` |

## Current boundaries

- The Editor has one managed document identity, one dirty/history transaction,
  and one project/package bootstrap authority.
- Runtime `AScene` and `CSceneNodeGraph` use one scene owner, while legacy source
  adapters, serializers, physics domains, and render paths remain explicit.
- `.acscene` cannot switch to Perspective or mix with ACS3D content. A viewport
  projection change never migrates source content.
- Automatic `.acscene` conversion is not provided.
- Camera View exposes up to eight logical request leases but only one physical
  shared-swapchain presenter. Dedicated offscreen targets, simultaneous live
  outputs, and asynchronous readback are not part of this contract.
- Current fixture checks prove state, ordering, ownership, and rollback. They do
  not prove first-present pixels.
- Legacy source adapters and the legacy startup path remain in service until a
  captured first-frame/CRC check and semantic round-trip, recovery, Play,
  Standalone, and package parity gates all pass for the replacement path.

## Consequences

- 2D and 3D tools share Save All, dirty state, Undo/Redo, autosave, recovery,
  Play, and package identity.
- Legacy projects are not implicitly rewritten.
- The fixed package bootstrap filename remains a compatibility detail.
- Standalone runtime support remains explicit per source directive. Unsupported
  content is rejected rather than silently omitted.

## Failure contract

The Editor must not publish a new document, viewport source, or package root when:

- the manifest path and canonical Asset ID disagree;
- a source or envelope version is unsupported;
- either payload of an atomic restoration is missing or invalid;
- a source contains a directive unsupported by the selected runtime adapter;
- an authored-camera request refers to a stale scene or node generation.

Failure preserves the last complete document and does not partially rewrite the
source or its durable identity.

## Verification

`SceneContractFixtureSelfTest`, invoked by
`--scene-editor-migration-selftest`, checks:

- the shared `Assets/main.acs3d` project-template contract;
- Orthographic as a view preset;
- bootstrap path, contract, and source-format discrimination;
- exact dual-payload transaction round trips;
- blank startup selecting `ACS3D`/Perspective;
- editor/game camera and Play-lifetime separation;
- deterministic multi-camera ordering;
- fail-closed handling for unknown formats and malformed envelopes.

```powershell
dotnet run --project acs/editor/AcsEditor/AcsEditor.csproj -c Release -- --scene-editor-migration-selftest
powershell -NoProfile -ExecutionPolicy Bypass -File .\acs\scripts\verify_editor.ps1 -Mode full
```

The broader Editor ownership and package contracts are described in
[EditorArchitecture.md](../EditorArchitecture.md).

Retiring a legacy source adapter or startup path additionally requires the
captured first-frame/CRC and parity gates listed in Current boundaries; the
fixture self-test alone is not sufficient release evidence.
