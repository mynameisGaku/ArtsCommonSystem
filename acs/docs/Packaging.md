# Packaging and distribution

The Editor's **Build → Package Project…** command is the first shipping-oriented
vertical slice. It performs:

1. project, canonical Scene Asset ID, and reachable asset-reference validation;
2. a standalone Windows x64 Release build;
3. bounded, side-effect-free PE32+ AMD64 launchability preflight, followed by
   PE runtime dependency resolution;
4. deterministic dependency-closure Cook rooted at the canonical Scene Asset ID
   into the existing `.acpak` v1 format, with staged-only conversion of
   scene/material references to portable virtual paths;
5. deterministic `acs_assetpack pack` output followed by native
   `acs_assetpack verify` of every entry and CRC;
6. path-safe staging of a private game-executable copy, required non-system
   DLLs, Microsoft VC runtime DLLs, `game.acpak`, and `Config/`;
7. deterministic publication and immediate read-back verification of project
   identity and distribution fields in the staged EXE's PE `VERSIONINFO`,
   plus preservation of a compatible embedded application manifest or
   generation of a missing `asInvoker` manifest;
8. a manifest containing the Cooked pack path, SHA-256, format version,
   compression policy, source entry count, content-derived build ID, canonical
   scene Asset ID/importer/graph hash, and a reversible scene bootstrap
   envelope, plus validated distribution metadata; and
9. a deterministic ZIP with stable entry order and timestamps, followed by a
   complete manifest/path/size/SHA-256, executable-header, application-manifest,
   and manifest-to-`VERSIONINFO` verification before atomic publication; and
10. a private-copy, bounded, hidden packaged-runtime launch through its first
    successful frame, followed by an atomic structured package report.

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

The executable preflight reads at most the bounded DOS/PE header region and
never starts project-controlled code. It requires an executable, non-DLL
PE32+ AMD64 image with a non-zero entry point, a bounded section table, and a
Windows GUI or console subsystem. Every section range must be structurally
consistent, and the entry point must resolve inside an executable code
section's raw or virtual range. A text file, malformed section layout, invalid
entry point, or foreign-architecture binary renamed to `.exe` fails as
`EXECUTABLE_INVALID` both before staging and when a completed archive is
independently verified.

PE resource inspection is also bounded and side-effect-free: the resource
directory is limited to 64 MiB and each relevant `VERSIONINFO` or application
manifest payload to 4 MiB. The parser honors PE32+
`NumberOfRvaAndSizes`, rejects a directory count that does not fit the
optional header, and limits one resource tree to 4,096 unique directory
visits and 16,384 aggregate entries; repeated or cyclic directory offsets
fail closed. An embedded process manifest is parsed with DTDs disabled. It
must be a Windows `assembly` identity compatible with AMD64 (or
architecture-neutral) and must use the effective
`asInvoker` / `uiAccess=false` policy. Multiple language variants, malformed
XML, `highestAvailable`, or `requireAdministrator` fail preflight as
`EXECUTABLE_MANIFEST_INVALID`. Package never replaces an existing compatible
manifest. If the executable has no process manifest, the Windows packaging
host adds a deterministic AMD64 `asInvoker` identity to the private staged
copy. No external `rc.exe` or `mt.exe` lookup is required.
After the Windows resource update, packaging normalizes the COFF timestamp,
every reachable resource-directory timestamp, every bounded and section-mapped
`IMAGE_DEBUG_DIRECTORY` timestamp, and the PE checksum to zero before hashing,
so volatile PE metadata cannot perturb identical packages.

On Windows, runtime dependency resolution pins CMake's script-mode scanner to
the `windows+pe`/`dumpbin` backend. `dumpbin.exe` is resolved from `PATH`, the
active `VCToolsInstallDir`, or an installed Visual Studio x64 toolchain. This
avoids host introspection selecting an unavailable `objdump`; absence of a
supported PE scanner remains a fail-closed packaging error.

## Product metadata

Optional distribution metadata lives at `Config/PackageMetadata.json` and is
captured by the same immutable Config snapshot used for Package. A Shipping
preflight without the file emits `PACKAGE_METADATA_MISSING` as a warning for
backward compatibility. If the file exists, malformed or ambiguous input
fails as `PACKAGE_METADATA_INVALID`; it is never silently repaired.

The object contains the integer `schemaVersion` field and the string fields
`publisher`, `description`, `copyright`, and `supportUrl`. `schemaVersion` must
identify schema 1, and `supportUrl` must be a canonical absolute HTTPS URL.

The parser accepts only those five fields, rejects duplicate or unknown JSON
properties, bounds the file and each string, rejects hidden control
characters, and requires a canonical absolute HTTPS support URL. The validated
object is embedded as `productMetadata` in `package-manifest.json`; CLI
`verify`, `inspect`, and `diff` preserve and compare it. Older schema-v3
archives without `productMetadata` retain structural and compatible-manifest
verification; exact manifest-to-PE field binding begins when that object is
present.

The same captured object is applied to the private staged executable before
payload hashing:

- project name -> `ProductName`;
- Package dialog version -> exact `ProductVersion`;
- publisher -> `CompanyName`;
- description -> `FileDescription`;
- copyright -> `LegalCopyright`; and
- support URL -> the custom `SupportUrl` string entry.

`FileVersion` and the fixed Win32 file/product versions use the SemVer numeric
core as `major.minor.patch.0`; prerelease/build text remains exact in
`ProductVersion`. Each numeric component must fit `0..65535`, otherwise
preflight reports `EXECUTABLE_METADATA_INVALID` instead of truncating it.
`OriginalFilename` is the packaged EXE leaf name. The resource uses one
canonical ID/language/string table, and Package reads it back byte-for-byte.
Archive verification reconstructs the expected fields from
`productName`, `productVersion`, `executable`, and `productMetadata` in
`package-manifest.json`; any missing, duplicate, non-canonical, or mismatched
resource fails closed. The original Release build output is never modified.

The Editor exposes this document under `Project Settings > Distribution`.
Edits are staged with inline validation from the same package contract, then
published with explicit Apply or discarded with Revert. Strict load failures
are shown without overwriting the source. Applying four empty fields returns
the project to the unconfigured state by removing the optional file. See
[Package metadata in Project Settings](ProjectSettingsPackageMetadata.md) for
the complete durability and UI contract.

Before opening the Package dialog, Asset View exposes
**Asset Actions > Package Readiness…**. It runs this same canonical Cook
closure asynchronously, shows missing/unsupported/stale/reparse diagnostics
with locate and Reference Viewer repair routes, and can atomically export a
schema-versioned JSON report. See
[Asset Package Readiness](AssetPackageReadiness.md).

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

dotnet run --project tools/acspackage -- smoke `
  C:\Games\MyGame\Build\Packages\MyGame-1.0.0-win64.zip `
  --timeout-seconds 45
```

`--include-symbols` adds only the game PDB in Development/Test.
`--skip-build` packages an existing Release executable. `--self-test`
exercises byte-identical Cook/ZIP output, the native pack verifier, manifest
pack hashes, all profiles, canonical identity/bootstrap metadata, 2D and
supported 3D reference rewriting, product metadata, PE32+ AMD64 preflight,
byte-identical independent `VERSIONINFO` updates, Windows version-API
visibility, exact manifest-to-PE round trips, compatible/generated
application manifests, duplicate resource/language rejection, metadata
reparse protection, unsupported/external inputs, traversal protection, the
3D fail-closed boundary, and adversarial launch readiness/timeout/cancellation
contracts.

## Package launch report and startup smoke

Editor Package and the CLI `package` command now finish with the same
`PackageLaunchSmoke` contract. The already-published ZIP is never executed or
modified in place. The runner opens the ordinary source archive without write
sharing, copies and hashes it into a unique private directory below operating
system TEMP, runs the complete existing archive verifier on that private
copy, and only then extracts it. Extraction uses `CreateNew`, rejects reparse
points and path collisions, and is bounded to 16 GiB by default (hard maximum
128 GiB). A non-write-sharing handle pins the private ZIP across hash,
verification, and extraction. The extracted executable is independently
hashed against its verified manifest record, PE-inspected through another
non-write-sharing handle, and that handle remains open through process startup.
The verified manifest remains authoritative for the package root and
executable name.

The child is started without a shell, console window, arguments, or user
interaction. It receives one random 256-bit lowercase nonce only through
`ACS_PACKAGE_SMOKE_TOKEN`. A valid token makes `FApplication` create its real
HWND and swapchain without calling `ShowWindow`; it therefore cannot activate
or cover the Editor. Windows critical-error and unhandled-fault UI is disabled
for that machine-driven child. The normal input path is never focused or
captured. The child does not inherit the Editor/terminal environment:
CI variables, cloud tokens, signing credentials, tool authentication, and the
user's `PATH` are absent. Windows directories and runtime identity are derived
from runtime/OS APIs rather than caller variables; the child receives a
System32-only `PATH`, while TEMP, profile, AppData, and managed-tool state are
redirected into private staging.

Readiness is stricter than "the process stayed alive": after renderer
initialization, canonical Scene `OnStart`, and the first completed standard or
contract-compliant custom submit/present, the runtime writes exactly one
authenticated `ACS_PACKAGE_SMOKE_V1 READY <nonce>` line and requests a clean
shutdown. A streaming finite-state recognizer counts markers independently of
the bounded diagnostic capture, so a duplicate after more than 8 MiB of output
still fails. A missing or duplicate marker, Scene/bootstrap failure,
render/present failure, non-zero exit, or private-staging cleanup failure fails
the smoke. The default deadline is 45 seconds; CLI may choose 1..300 seconds.
Timeout and cancellation terminate the complete process tree, wait for exit,
bound stdout/stderr capture to 8 MiB per stream, drain the pipes, and then
remove private staging with bounded retries. No modal prompt or unattended
process is intentionally left behind.
On Windows the launched root is created with `CREATE_SUSPENDED`, assigned to a
kill-on-close Job Object, and only then resumed. Package code therefore cannot
create a helper in a create/assign race outside containment. After the root
exits, the runner terminates any inherited descendants before draining output,
so detached helpers cannot retain pipes or private staging.

Each run atomically writes `<package-name>.package-report.json` beside the ZIP,
or the explicit `smoke --report` path. It records:

- archive SHA-256 plus manifest package/build/profile/executable identity;
- payload and Cook/pack counts and hashes;
- configured timeout, extraction, and output-capture limits;
- archive-copy, verify, extract, hidden-launch, readiness, exit, and cleanup
  checks; and
- stable diagnostic codes with bounded, TEMP/nonce-redacted excerpts on
  failure.

A successful report intentionally excludes timestamps, random tokens, private
paths, and observed timings. The same package and limits therefore produce
byte-identical successful report JSON. The Editor still shows measured package
and smoke durations in Build Results, where observational timing belongs.
An existing ordinary report is atomically replaced; the ZIP is retained and
reported as created-but-smoke-failed if runtime startup does not pass.
Standalone reruns use:

```powershell
dotnet run --project tools/acspackage -- smoke <package.zip> `
  --report <report.json> `
  --timeout-seconds 45 `
  --max-extract-mib 16384
```

## Distribution E2E audit

Run the retained, end-to-end distribution audit with:

```powershell
dotnet run --project tools/acspackage -- distribution-e2e
```

The command creates a new real 3D project only under the operating-system TEMP
directory. It invokes the production `package` CLI twice, requires
byte-identical Shipping ZIPs, then exercises standalone `verify`,
`inspect --json`, and identical `diff --json`. The inspection result must
preserve the fixture's four product-metadata fields exactly, and the executable
inside the ZIP is independently checked against the PE32+ AMD64 contract.

The audit then mutates `Config/ProjectSettings.ini` in a copied ZIP without
updating the manifest. `verify` and `inspect` must return invalid-archive
failures and write `verified: false` JSON; `diff` must refuse the comparison
with exit code `3` and `compared: false`. A schema-versioned summary records
all command exit codes, archive SHA-256 values, metadata, and PE fields.
Both valid archives, the tampered archive, and every JSON report remain under
`%TEMP%\acs-distribution-e2e-*`.

`--artifacts <new-temp-directory>` chooses the retained directory, but it must
be a new ordinary path strictly below TEMP. Existing paths, paths outside TEMP,
and reparse-point ancestors are rejected. `verify_editor.ps1 -Mode full`
executes the same gate inside its isolated verification session. The script
normally deletes that session after completion; use `-KeepArtifacts` when its
E2E files must be retained for inspection.

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
`EMPTY3D`, `CMP3D`, `CPROP3D`, `PLY3D`, `SPR3D`, `PFAB3D`, `PINS3D`,
`PSID3D`, `POVR3D`, `PNOVR3D`, `PCOVR3D`, and `SEL3D`, including sparse IDs and
multiple top-level nodes. Polygon payloads are validated and preserved without
inventing an asset dependency; runtime deterministically builds their XY mesh.
`SPR3D` texture paths, non-executing `PFAB3D` instance-source links, `PINS3D`
stable instance identities, `PSID3D` source-node identities, `POVR3D` root override masks, `PNOVR3D` child value/Transform override masks, and `PCOVR3D` root-component
property override records are validated, included in the Cook closure, and
rewritten in the isolated bootstrap copy. Unknown directives, invalid component/property records, or
dependencies that cannot be decoded fail validation with an explicit
scene-adapter diagnostic. Standalone `.gltf` assets with non-data external
buffer/image URIs also fail closed; Cook never ships a scene with silently
missing runtime behavior.

## Distribution roadmap

Portable ZIP generation does not perform executable signing or publish to an
external service. The Config snapshot and package manifest now carry
publisher, copyright, description, and HTTPS support URL, and the staged EXE
now carries the corresponding deterministic `VERSIONINFO`. Remaining
distribution work includes:

- a durable project-level semantic version/release-channel policy (the current
  version is entered in the Package dialog);
- application icon selection and icon-resource publication;
- package identifier and additional platform distribution profiles;
- signing certificate selection backed by an OS credential store;
- installer/MSIX generation and prerequisite declarations;
- patch/chunk manifests, delta updates, and crash-symbol upload;
- Steam/Epic/Microsoft Store upload adapters with explicit authentication and
  confirmation.

Signing keys, store credentials, and store submission require user-owned
external authority and are intentionally outside the local package command.
Icon publication also remains separate because the current package metadata
schema has no icon source. Signing must occur after all resource updates;
Package intentionally does not claim that its staged resource rewrite
preserves a signature from the unsigned build-input phase.
