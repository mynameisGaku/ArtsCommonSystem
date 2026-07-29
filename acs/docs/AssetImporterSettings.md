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

These settings describe importer intent. Importers that do not yet transform
their source bytes still preserve the recipe in metadata, so adding a native or
worker-backed processor later does not require another asset identity migration.

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

## Transaction behavior

The profile is normalized before a worker starts. The exact immutable settings
snapshot is then passed through both file-only and recursive import paths.
Each staged payload and its metadata are still published through the existing
import journal:

1. copy and hash the source into private staging;
2. create canonical recipe metadata;
3. durably publish the journal;
4. publish payload and verify its size/hash;
5. publish metadata and verify its hash;
6. refresh the Asset Database;
7. roll back the complete batch on cancellation or index failure.

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
- recursive propagation and recipe preservation through Reimport;
- accessible texture, mesh, and audio controls appearing before import
  publication;
- rendered mixed-selection geometry, unclipped actions, inline validation, and
  disabling **Import** while settings are invalid.

## Remaining work

The contract is ready for importer-specific derived payload producers. Follow-up
work should add:

- native/worker texture transcoding and mip generation;
- mesh optimization, tangent/collision generation, and skeletal import;
- audio resampling/compression;
- progress and cancellation for those processors;
- content-addressed storage of the processed payload, not only Cook output;
- per-asset **Reimport Settings** diff/apply and bulk preset assets.
