# Packaging and distribution

The Editor's **Build → Package Project…** command is the first shipping-oriented
vertical slice. It performs:

1. project, canonical Scene Asset ID, and reachable asset-reference validation;
2. a standalone Windows x64 Release build;
3. PE runtime dependency resolution;
4. deterministic dependency-closure Cook rooted at the canonical Scene Asset ID
   into the existing `.acpak` v1 format, with staged-only conversion of
   scene/material references to portable virtual paths;
5. deterministic `acs_assetpack pack` output followed by native
   `acs_assetpack verify` of every entry and CRC;
6. path-safe staging of the exact game executable, required non-system DLLs,
   Microsoft VC runtime DLLs, `game.acpak`, and `Config/`;
7. a manifest containing the Cooked pack path, SHA-256, format version,
   compression policy, source entry count, content-derived build ID, canonical
   scene Asset ID/importer/graph hash, and a reversible scene bootstrap
   envelope; and
8. a deterministic ZIP with stable entry order and timestamps.

PDBs are opt-in. Editor-only `*_reflect.dll` files are always excluded.
`Assets/` and `Config/` reject symlinks, junctions, and other reparse points.
References outside `Assets/`, missing runtime dependencies, traversal paths,
unsupported **reachable** asset extensions, reachable external glTF URIs,
conflicting `Game.DefaultScene` / project `initialScene`, and output folders
inside an input tree fail validation. Asset database implementation details
(`*.acsmeta`, `.acsdb/**`) and `*.tmp-*` artifacts are not Cook inputs.
The pass-through Cook allowlist includes ACS scenes/materials/prefabs/
Blueprints, common image/audio/mesh/font formats, text/config formats, and
compiled shader blobs (`.cso`, `.dxil`, `.spv`). An unknown extension is an
error when it is in the required closure; a valid indexed asset that is not
reachable from the initial Scene is intentionally omitted.

## Cook closure, snapshot, and publication boundary

Cook never determines reachability by filename guessing. It refreshes a
content-verified, read-only Asset DB snapshot, resolves the canonical Scene
GUID, and walks normalized dependency GUIDs in ordinal order. Source formats
whose references can be inspected (Scene, Material, Prefab, Blueprint, glTF,
OBJ, and MTL) are scanned again and compared with their authoritative
`*.acsmeta` dependency list. Blueprint is the compatibility exception:
source-authored parent/component edges are added to the closure while existing
importer metadata is retained as a safe superset, because older sidecars did
not mirror every `PARENT` edge. Missing GUIDs, cycles, path escape/reparse
paths, and source/metadata divergence emit stable diagnostic codes and stop
the Cook.
The `.acsdb/index.v1.json` file is only an acceleration cache: a stale or
invalid cache is discarded with `ASSET_INDEX_CACHE_IGNORED`; authoritative
sidecars are re-read instead of consuming stale cached dependencies.
Immediately before atomic ZIP publication, Package reacquires the project
asset-mutation lease, rebuilds the required closure, and compares its logical
graph hash with the Cooked manifest. A required asset changed during the
long-running Cook/archive phase therefore fails as
`PROJECT_CHANGED_DURING_PACKAGE`. Content or import-metadata changes to a valid
indexed asset that remains proven-unreachable do not perturb the graph hash;
project-wide metadata-authority, path, and reparse-point safety checks still
apply to the complete input tree.
Each required Cook payload is read once into a content-hash-verified byte
snapshot. Canonical Scene bootstrap inspection and reference rewriting consume
that same captured root snapshot; they never reopen the authored Scene and
cannot mix two transient versions into one package. The final graph rebuild
detects ordinary external edits that complete before the corresponding
revalidation read. The mutation lease is cooperative between ACS processes,
not a filesystem compare-and-swap primitive: a hostile same-user process can
still replace a Windows path after its last validation read. Publication does
not claim to atomically freeze an externally modified authoring tree.
Dependency scanners likewise parse an in-memory source snapshot only after its
size and SHA-256 match the `AssetRecord` used by the graph. The later Cook copy
independently enforces that same hash, so scanner and payload cannot silently
select different source revisions.

## Package profiles

- **Development** emits `game.acpak` plus loose *Cooked* assets and
  `main.acscene` for inspection. The pack is uncompressed for faster
  iteration. A normal Editor Build & Run with no adjacent `game.acpak`
  continues to use the existing loose path.
- **Test** emits a verified LZ4-compressed `game.acpak` as the authoritative
  asset source, with no redundant loose `Assets/`. The game PDB can be
  included explicitly.
- **Shipping** has the same authoritative compressed pack boundary, strips
  loose assets, and rejects PDB inclusion in the distributable ZIP.

The generated runtime checks for `game.acpak` and reads one canonical bootstrap
entry, `main.acscene`. Its exact content header dispatches to either the
`ACSCENE v1` 2D adapter or the supported `ACS3D v2` 3D adapter. The 2D path
retains SpriteBatch, 2D physics, material textures, and normal maps; it is not
converted into meshes. The 3D path restores the `ANode` graph and PBR mesh
state. Mount/read/CRC/parse/dependency failure is fatal for that load and never
silently falls back to loose assets. If no pack is present, the same adapters
remain available through the loose development path.

The same backend is available without WPF:

```powershell
dotnet run --project tools/acspackage -- package `
  C:\Games\MyGame\MyGame.acsproject `
  --version 1.0.0 `
  --profile Shipping

dotnet run --project tools/acspackage -- validate `
  C:\Games\MyGame\MyGame.acsproject `
  --version 1.0.0
```

`--include-symbols` adds only the game PDB in Development/Test.
`--skip-build` packages an existing Release executable. `--self-test`
exercises byte-identical Cook/ZIP output, the native pack verifier, manifest
pack hashes, all profiles, canonical identity/bootstrap metadata, 2D and
supported 3D reference rewriting, metadata exclusions, unsupported/external
inputs, traversal protection, and the 3D fail-closed boundary.

## Canonical scene bootstrap and fail-closed boundary

The project manifest's non-empty `canonicalSceneAssetId` identifies the root
scene. Package and CLI validation do not silently fall back to the legacy path:
missing and malformed IDs fail as `CANONICAL_SCENE_ASSET_ID_REQUIRED` and
`CANONICAL_SCENE_ASSET_ID_INVALID`. Opening a legacy project in the Editor
migrates the ID from the unique authoritative metadata record before Package is
allowed. Legacy `initialScene` remains a compatibility locator and must resolve
to that same asset. During Cook the root is always written as `main.acscene`,
while the package manifest records:

- `sceneBootstrap.contract = acs.scene.bootstrap.v1`;
- `sceneBootstrap.sourceFormat = legacy-acscene-v1` or `legacy-acs3d-v2`; and
- a legacy-only `adapterProjectionHint`.

The projection hint is not authoritative scene state. Perspective or
Orthographic is selected per camera at runtime.

The reversible `ACS3D v2` subset supports `N3D`, `MSH3D`, `MAT3D`, `FLG3D`,
`EMPTY3D`, `CMP3D`, `CPROP3D`, and `SEL3D`, including sparse IDs and multiple
top-level nodes. `SPR3D`, `PLY3D`, `PFAB3D`, unknown directives, invalid
component/property records, or dependencies that cannot be decoded fail
validation with an explicit scene-adapter diagnostic. Standalone `.gltf`
assets with non-data external buffer/image URIs also fail closed; Cook never
ships a scene with silently missing runtime behavior.

## Distribution roadmap

Portable ZIP generation does not perform executable signing or publish to an
external service. The project model also still needs durable product metadata:

- semantic product version (currently entered in the Package dialog);
- publisher, copyright, description, and support URL;
- application icon and Windows version resources;
- package identifier and additional platform distribution profiles;
- signing certificate selection backed by an OS credential store;
- installer/MSIX generation and prerequisite declarations;
- patch/chunk manifests, delta updates, and crash-symbol upload;
- Steam/Epic/Microsoft Store upload adapters with explicit authentication and
  confirmation.

Signing keys, store credentials, and store submission require user-owned
external authority and are intentionally outside the local package command.
