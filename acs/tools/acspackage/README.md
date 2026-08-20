# ACS Package CLI

`acspackage` is the non-UI backend for the Editor's **Package Project** flow.
It validates the canonical Scene identity and portable reachable references,
builds a Windows x64 Release target, verifies its bounded PE32+ AMD64 loader
contract, resolves non-system PE runtime DLLs, runs
the path-safe deterministic dependency-closure Cook, creates and natively
verifies the existing `.acpak` v1 format, stages the game, writes a pack/file
SHA-256 manifest with optional distribution metadata, publishes matching
canonical PE `VERSIONINFO` plus a compatible/generated application manifest
to the private staged EXE, creates a deterministic ZIP, then verifies a hidden
first-frame runtime launch and writes a structured package report.

```powershell
dotnet run --project tools/acspackage -- package `
  path/to/Game.acsproject --version 1.0.0 --profile Shipping

dotnet run --project tools/acspackage -- validate `
  path/to/Game.acsproject --version 1.0.0

dotnet run --project tools/acspackage -- verify `
  Build/Packages/Game-1.0.0-win64.zip `
  --report Build/Reports/Game-1.0.0-verification.json

dotnet run --project tools/acspackage -- inspect `
  Build/Packages/Game-1.0.0-win64.zip `
  --json Build/Reports/Game-1.0.0-inspection.json

dotnet run --project tools/acspackage -- diff `
  Build/Packages/Game-1.0.0-win64.zip `
  Build/Packages/Game-1.0.1-win64.zip `
  --json Build/Reports/Game-1.0.0-to-1.0.1.json

dotnet run --project tools/acspackage -- smoke `
  Build/Packages/Game-1.0.0-win64.zip `
  --report Build/Reports/Game-1.0.0-package.json `
  --timeout-seconds 45

dotnet run --project tools/acspackage -- distribution-e2e

dotnet run --project tools/acspackage -- --self-test
```

`verify` validates the archive root, manifest identity, declared file set,
uncompressed sizes, every payload SHA-256, and the packaged executable's
PE32+ AMD64 headers without extracting or executing the ZIP. For newly
produced archives it also reconstructs the expected EXE product fields from
the package manifest and requires one byte-canonical `VERSIONINFO` resource
plus a bounded, compatible `asInvoker` / `uiAccess=false` process manifest.
Malformed/elevated manifests, duplicate VERSIONINFO IDs or languages, and any
manifest-to-PE field mismatch fail closed. Legacy schema-v3 archives without
`productMetadata` retain structural PE verification.
The manifest is read into a bounded buffer of at most 4 MiB and must end at
its declared decompressed length before JSON deserialization. Payload hashing
reads exactly the declared length and probes at most one additional byte, so
early EOF and overruns fail without consuming an attacker-controlled tail.
The shared verification gate also rejects duplicate JSON properties at every
manifest nesting level so downstream readers cannot disagree about provenance.
It returns `0` for a valid package, `1` for a verification or I/O failure,
and `2` for invalid command usage. `--quiet` suppresses progress and the PASS
summary for CI while errors remain visible.

`--report` atomically creates a schema-versioned JSON result containing the
package/build IDs, profile, file and Cooked-asset counts, byte total, and
asset-pack digest, plus validated product metadata when present. A failed
archive verification produces `verified: false`
with an error code when the new report can be safely created. Reports never
overwrite an existing path, never replace the package ZIP, reject reparse
ancestors, and publish from a private sibling temporary file only after a
durable flush.

`inspect` runs that same complete verification gate, then emits provenance
and integrity information without extracting the archive: the whole-ZIP
SHA-256, package/build identity, product and engine versions, profile,
canonical-scene/bootstrap and asset-graph metadata, asset-pack provenance,
and the ordinal payload ledger with every size and SHA-256. It returns `0`
for a verified package, `1` for an invalid archive or output failure, and `2`
for invalid command usage.

`diff` verifies both archives before comparing them. Its result separates
byte-for-byte ZIP determinism, manifest provenance, and payload content. JSON
contains stable metadata changes plus added, removed, modified, and unchanged
file counts. Exit codes are suitable for CI: `0` means fully identical, `1`
means both packages are valid but differ, `2` means invalid command usage,
and `3` means an archive could not be verified or the JSON result could not
be safely published.

`inspect --json` and `diff --json` use schema version `1`. Their output paths
must be new files and must not alias an input ZIP. Like verification reports,
they reject existing reparse points, write through a private sibling file,
durably flush, atomically publish without overwrite, and bound archive entry,
manifest, central-directory, compressed-file, and verified uncompressed sizes.
Inspection rejects non-canonical ZIP envelopes, archives above 129 GiB,
verified payload totals above 128 GiB, and entries of at least 16 MiB whose
declared compression ratio exceeds 200:1 before payload hashing. The same
200:1 limit applies to aggregate declared sizes once total uncompressed data
reaches 16 MiB, so splitting content into smaller entries does not bypass it.
Manifest JSON passes the same duplicate-property gate as `verify`. A `diff`
whose two
normalized input paths are identical inspects that archive once, closing the
between-read exchange window. Human-readable inspect/diff progress, summaries,
and errors escape terminal control/format characters and bound each emitted
line to 512 characters. JSON preserves the raw semantic strings; the serializer
performs its normal JSON escaping. `--quiet` suppresses progress and
human-readable summaries while preserving exit codes and JSON output. Neither
command extracts or executes package content.

`package` and Editor Package automatically run the `smoke` contract after the
ZIP has passed complete verification. `smoke` first copies the ordinary ZIP to
a unique private TEMP directory, hashes and verifies that fixed copy, and
extracts it with path/reparse/collision checks and a 16 GiB default bound. It
starts only the verified manifest executable with shell execution disabled,
redirected bounded output, `CreateNoWindow`, and a hidden real ACS window.
The original ZIP and Release executable are never mutated.

The runtime accepts only a random 64-character lowercase-hex nonce from
`ACS_PACKAGE_SMOKE_TOKEN`. After Scene startup and the first successful
submit/present it emits exactly one authenticated readiness line and exits
cleanly. Missing/duplicate readiness, non-zero exit, or a 45-second default
deadline fails. Timeout, Ctrl+C cancellation, and editor shutdown terminate
the complete process tree and drain output before private staging is removed.
The timeout is configurable from 1 to 300 seconds; extraction has a 128 GiB
hard ceiling. Hidden startup skips `ShowWindow`, does not activate the game,
and suppresses machine-driven crash/error prompts.
The launch root is created suspended, contained in a Windows kill-on-close Job
Object before its first instruction, and then resumed. Remaining descendants
are terminated after the root exits and before pipe draining, so a helper
cannot escape the create/assign interval, outlive a successful smoke, or retain
private staging.
Before launch, non-write-sharing handles pin the private ZIP through
verification/extraction and the manifest-hashed EXE through PE inspection and
`CreateProcess`, closing the verify-to-launch replacement window. The child
environment starts empty: user/CI/cloud/signing/tool credential variables and
the inherited `PATH` are not forwarded. Windows paths and runtime identity are
derived from runtime/OS APIs rather than caller variables; System32 `PATH`,
TEMP, AppData, profile, and managed CLI state are rebased into private staging.

`smoke` returns `0` only when archive verification, extraction, readiness,
clean exit, and cleanup pass; `1` reports a validation/startup failure, `2`
invalid usage, and `130` cancellation. It atomically writes
`<package>.package-report.json` by default. Successful JSON contains stable
archive/build/profile/executable identity, hashes, bounds, and per-gate checks,
but no timestamp, nonce, TEMP path, captured logs, or observed duration, so
identical inputs and limits produce byte-identical reports. Failure reports
add stable codes and bounded nonce/TEMP-redacted output excerpts. Re-running
replaces only an ordinary report file; it cannot alias the ZIP.
Readiness is counted by a streaming finite-state recognizer independent of the
8 MiB diagnostic capture, so late duplicate markers fail closed. A cleanup
failure also makes the report and command fail. Ctrl+C during both standalone
`smoke` and the automatic post-`package` smoke cancels the bounded runner,
terminates its child tree, and writes the cancellation report.

Profiles are `Development`, `Test`, and `Shipping`. Test/Shipping use a
verified compressed `game.acpak` without redundant loose source assets;
Development also retains loose Cooked assets for inspection. PDBs and
Editor-only `*_reflect.dll` files are excluded by default, and Shipping
rejects PDB inclusion. `Assets/` and `Config/` reject reparse points, and
referenced files must live under `Assets/`. Absolute Editor asset paths are
rewritten only in the isolated Cook copy. Unknown extensions and external
glTF URIs fail closed when reachable; valid indexed assets outside the
canonical Scene dependency closure are omitted without affecting the graph
hash. Scene configuration mismatches always fail closed.

`Config/PackageMetadata.json` optionally supplies schema-v1 `publisher`,
`description`, `copyright`, and a canonical HTTPS `supportUrl`. The strict
bounded parser rejects duplicate/unknown properties and invalid strings. The
validated object joins the immutable Config snapshot, is embedded as
`productMetadata`, and is preserved by `verify`/`inspect`/`diff`. Shipping
without it remains compatible but reports `PACKAGE_METADATA_MISSING`. The
private staged EXE receives `CompanyName`, `FileDescription`,
`LegalCopyright`, and `SupportUrl`; project name and the exact Package version
provide `ProductName` and `ProductVersion`. Numeric SemVer components must fit
Windows' UInt16 version fields. Existing compatible application manifests are
inspected and preserved; a missing one is generated deterministically.

`distribution-e2e` is the retained distribution audit. It creates a fresh
3D fixture project strictly under the operating-system TEMP directory, invokes
the real `package` command twice, and proves byte-identical Shipping archives.
It then runs `verify`, `inspect --json`, and identical `diff --json`, checks the
exact embedded product metadata, and independently inspects the packaged
`Game.exe` as PE32+ AMD64. Finally it changes a Config payload without changing
the manifest and requires `verify`/`inspect` to fail and `diff` to return its
invalid-archive exit code. The summary, both valid ZIPs, the tampered ZIP, and
all success/failure JSON documents remain in
`%TEMP%\acs-distribution-e2e-*`. An explicit
`--artifacts <new-temp-directory>` is accepted only for a new,
non-reparse-point directory strictly inside TEMP; existing and non-TEMP paths
are rejected.

`verify_editor.ps1 -Mode full` runs the same audit in its isolated TEMP
session. That session is normally removed after the gate; pass
`-KeepArtifacts` to retain its distribution E2E output.

The canonical root scene is selected only by its non-empty persistent Asset
ID and Cooked to the single bootstrap path `main.acscene`. The CLI never falls
back to `initialScene` when that ID is missing or malformed. The legacy path
must resolve to the same record, otherwise validation fails. Each reachable
GUID is closed in ordinal order, and missing IDs, dependency cycles, path
escape/reparse paths, source/metadata dependency drift, and required
unsupported formats produce stable error codes. The `.acsdb` index is treated
as a disposable cache; authoritative `*.acsmeta` sidecars and verified source
content drive the plan. Blueprint parent/component edges discovered in source
are retained as compatibility additions because legacy metadata did not mirror
every `PARENT` edge. The publication gate rebuilds this closure while
holding the project mutation lease and rejects a changed required graph as
`PROJECT_CHANGED_DURING_PACKAGE`; content/import-metadata changes to a valid
indexed asset that remains unreachable leave the graph hash unchanged, while
complete-tree metadata-authority, path, and reparse-point safety checks still
apply.

The mutation lease coordinates ACS processes; it is not a filesystem-wide
compare-and-swap or input-tree freeze. A non-cooperating same-user process can
still replace a Windows path after its final validation read. See
[`Packaging.md`](../../docs/Packaging.md#cook-closure-snapshot-and-publication-boundary)
for the precise publication boundary and threat model.

The manifest's
`sceneBootstrap.sourceFormat` records `legacy-acscene-v1` or
`legacy-acs3d-v2`; `adapterProjectionHint` is an import hint only, because
projection is selected per camera. The runtime header-dispatches to the 2D
adapter or the supported `ACS3D v2` adapter.

The reversible 3D subset supports `N3D`, `MSH3D`, `MAT3D`, `FLG3D`,
`EMPTY3D`, `CMP3D`, `CPROP3D`, `PLY3D`, `SPR3D`, `PFAB3D`, and `SEL3D`. `PLY3D`
remains inline deterministic geometry, while `SPR3D` adds a Cooked texture dependency
and `PFAB3D` adds a non-executing Prefab/Blueprint source link. Unknown directives, invalid reflected components, and standalone
glTF files with external non-data URIs fail closed with explicit diagnostics.
Executable signing, application icon resources, installer generation, and
store upload are not part of this local packaging step. The current metadata
schema has no icon source, and future signing must run after the deterministic
resource update.
