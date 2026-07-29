# Asset importer settings and cache identity

## Scope

The Asset View import command now opens one bounded settings surface before it
publishes source files. The first vertical slice covers the built-in texture,
mesh, and audio importers:

- texture color-space intent, compression preset, mip generation, and normal-map detection;
- mesh uniform scale, tangent import/generation, and simple-collision generation;
- audio streaming, peak normalization, and target sample rate.

Folder drag-and-drop uses the last accepted project-local profile without
opening one modal per file. Manual **Import** can review or change the profile
before the batch starts.

These settings describe importer intent. A source-preserving worker now emits
one validated, content-addressed v1 artifact for every import before source
publication. Texture, mesh, and audio native transcoding can replace that
artifact payload later without changing the transaction or asset identity
boundary.

## Persistence boundary

The last accepted profile is stored at:

```text
<Project>/Saved/Editor/AssetImporterSettings.v1.json
```

It is editor state and is not scanned as an Asset. The store:

- accepts only schema version 1 and a fixed property set;
- rejects unknown or duplicate properties, missing required properties,
  non-finite mesh scale, unsupported enum values, and unsupported sample rates;
- is limited to 64 KiB;
- rejects file/directory reparse points;
- writes a same-directory unique temporary file with write-through semantics
  and atomically replaces the committed profile;
- falls back to safe defaults with a visible log warning when an existing
  profile is malformed.

Project switching resets the in-memory profile before asynchronous project
initialization. A late result from an old project therefore cannot change the
new project’s importer defaults.

## Metadata and derived-data identity

Every imported asset receives a canonical recipe:

```text
importer
importerVersion
recipeSchema
recipeHash
<type-specific normalized settings>
sourceContentHash
sourceLastWriteUtcTicks
sourceSizeBytes
processedArtifactKey
processedArtifactSchema
processedProcessor
processedProcessorVersion
processedFormat
```

`recipeHash` is SHA-256 over length-prefixed UTF-8 fields sorted with ordinal
ordering. It includes importer identity, importer version, and normalized
settings, but not the destination path. Rename and move therefore preserve the
same recipe identity.

The adjacent `.acsmeta` remains authoritative. `DerivedDataCache.ComputeKey`
already includes the complete normalized `ImportSettings` dictionary and
importer version. Changing color space, mip policy, scale, collision intent,
streaming, normalization, or sample rate therefore invalidates Cook/derived
data deterministically even before all native processors are connected.

## Processed import DDC

The worker validates the complete staged source against the captured byte
length and SHA-256, identifies a bounded container/format probe, and emits a
canonical descriptor under:

```text
<Project>/Temp/DerivedDataCache/AssetImports/v1/<prefix>/<key>.acsimpddc
```

The key contains the processor contract, normalized asset kind, source
extension, importer/version, verified recipe hash, source SHA-256, and source
length. It deliberately excludes both source and destination paths, so an
identical source/options pair shares one artifact after rename, move, or
collision-safe import. The descriptor stores the verified recipe hash rather
than arbitrary raw setting values, so path-bearing or private settings cannot
copy project-local strings into the cache payload.

Each cache entry has a fixed magic/schema prefix, its 32-byte key, payload
SHA-256, bounded payload length, and the canonical artifact. Reads verify every
field and byte. Missing entries are created with a same-directory write-through
temporary file and atomic replacement; a corrupt entry is deterministically
rebuilt. Concurrent publishers must converge on the same canonical bytes and
leave no shared temporary name. Project/cache containment, every directory
segment, the entry, and the staged source reject reparse points. Source, entry,
and temporary files are also revalidated from the opened Windows handle, closing
the path-check/open substitution window.

Recipe ingestion is bounded to 128 entries and 192 KiB of normalized UTF-8
before artifact construction. Source fingerprints and every case variant of
the reserved `processed*` namespace are dynamic metadata: they never contribute
to recipe identity, and Reimport drops the old set before adding the newly
validated authoritative values. This prevents stale or spoofed diagnostic
fields from surviving a source replacement.

The v1 artifact is intentionally source-preserving: it records exact processing
intent and safe format facts without pretending to be GPU-ready texture, mesh,
or audio data. This establishes worker execution, cancellation, DDC identity,
corruption recovery, and Reimport parity before native codecs are introduced.

## Transaction behavior

The profile is normalized before a worker starts. The exact immutable settings
snapshot is then passed through both file-only and recursive import paths.
Each staged payload and its metadata are still published through the existing
import journal:

1. copy and hash the source into private staging;
2. verify the complete staged copy and build/reuse its processed DDC artifact;
3. create canonical recipe and processed-artifact metadata;
4. durably publish the journal;
5. publish payload and verify its size/hash;
6. publish metadata and verify its hash;
7. refresh the Asset Database;
8. roll back the complete batch on cancellation or index failure.

An invalid recipe fails before publication. A settings-profile write failure
also aborts the import rather than silently using values that will be lost on
the next editor launch.

## Verification

Run:

```powershell
dotnet run --project acs/editor/AcsEditor/AcsEditor.csproj `
  --configuration Release -- --asset-import-selftest
```

Rendered UI geometry and the invalid-input state can be checked without taking
focus from the active desktop:

```powershell
acs/editor/AcsEditor/bin/Release/net10.0-windows/win-x64/AcsEditor.exe `
  --asset-import-settings-visual-fixture `
  "$env:TEMP/acs-import-settings.png"
```

The aggregate verifies:

- canonical normalization and invalid-value rejection;
- deterministic recipe hashes and setting-sensitive cache identity;
- strict profile round-trip, BOM absence, unknown/duplicate-field rejection,
  and atomic temporary cleanup;
- transactional `.acsmeta` publication with importer version 2 recipes;
- destination-independent processed artifact reuse and corrupt-entry rebuild;
- complete source length/hash checks, bounded recipe ingestion, opened-handle
  path identity, and concurrent atomic publication;
- recursive propagation plus recipe/artifact advancement through Reimport;
- rejection of recipe metadata tampering and replacement of spoofed/stale
  dynamic metadata before cache publication;
- accessible texture, mesh, and audio controls appearing before import
  publication;
- rendered mixed-selection geometry, unclipped actions, inline validation, and
  disabling **Import** while settings are invalid.

## Remaining work

The worker/DDC contract is ready for importer-specific native payload
producers. Follow-up work should add:

- native/worker texture transcoding and mip generation;
- mesh optimization, tangent/collision generation, and skeletal import;
- audio resampling/compression;
- per-processor progress reporting beyond the current cooperative cancellation;
- streaming native payload publication inside the established DDC envelope;
- per-asset **Reimport Settings** diff/apply and bulk preset assets.
