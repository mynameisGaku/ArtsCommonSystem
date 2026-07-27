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
   record.
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

## Verification

`SceneContractFixtureSelfTest` is executed by
`--scene-editor-migration-selftest` and the full Editor verifier. It pins:

- the shared `Assets/main.acs3d` project template contract;
- Orthographic as a view-only 2D preset;
- canonical bootstrap path, contract, and source-format discrimination;
- exact dual-payload transaction round trips;
- fail-closed handling for unknown formats and malformed envelopes.

The broader migration and persistence gates remain:

```powershell
dotnet run --project acs/editor/AcsEditor/AcsEditor.csproj -c Release -- --scene-editor-migration-selftest
powershell -NoProfile -ExecutionPolicy Bypass -File .\acs\scripts\verify_editor.ps1 -Mode full
```
