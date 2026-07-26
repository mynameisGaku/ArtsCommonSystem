# ACS Package CLI

`acspackage` is the non-UI backend for the Editor's **Package Project** flow.
It validates portable asset references, builds a Windows x64 Release target,
resolves non-system PE runtime DLLs, runs the path-safe deterministic Cook,
creates and natively verifies the existing `.acpak` v1 format, stages the
game, writes a pack/file SHA-256 manifest, and creates a deterministic ZIP.

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

dotnet run --project tools/acspackage -- --self-test
```

`verify` validates the archive root, manifest identity, declared file set,
uncompressed sizes, and every payload SHA-256 without extracting the ZIP.
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
asset-pack digest. A failed archive verification produces `verified: false`
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

Profiles are `Development`, `Test`, and `Shipping`. Test/Shipping use a
verified compressed `game.acpak` without redundant loose source assets;
Development also retains loose Cooked assets for inspection. PDBs and
Editor-only `*_reflect.dll` files are excluded by default, and Shipping
rejects PDB inclusion. `Assets/` and `Config/` reject reparse points, and
referenced files must live under `Assets/`. Absolute Editor asset paths are
rewritten only in the isolated Cook copy. Unknown extensions, external glTF
URIs, and scene configuration mismatches fail closed.

The canonical root scene is selected by its persistent Asset ID and Cooked to
the single bootstrap path `main.acscene`. The manifest's
`sceneBootstrap.sourceFormat` records `legacy-acscene-v1` or
`legacy-acs3d-v2`; `adapterProjectionHint` is an import hint only, because
projection is selected per camera. The runtime header-dispatches to the 2D
adapter or the supported `ACS3D v2` adapter.

The reversible 3D subset supports `N3D`, `MSH3D`, `MAT3D`, `FLG3D`,
`EMPTY3D`, `CMP3D`, `CPROP3D`, and `SEL3D`. Editor-only `SPR3D`, `PLY3D`,
`PFAB3D`, unknown directives, invalid reflected components, and standalone
glTF files with external non-data URIs fail closed with explicit diagnostics.
Executable signing, publisher/icon metadata, installer generation, and store
upload are not part of this local packaging step.
