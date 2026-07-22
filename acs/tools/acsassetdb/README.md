# acsassetdb

`acsassetdb` validates and inspects the editor asset database without starting
the WPF editor.

```powershell
dotnet run --project tools/acsassetdb/AcsAssetDb.csproj -- --self-test
dotnet run --project tools/acsassetdb/AcsAssetDb.csproj -- --index C:\Game\Assets
```

Each source asset has an adjacent deterministic `<asset>.acsmeta` sidecar.
`Assets/.acsdb/index.v1.json` is a deterministic cache used for incremental
hashing and recovery when an asset is renamed without its sidecar. Sidecars and
`.acsdb` are editor inputs and must be excluded from cooked/package payloads.

Indexing never follows a symlink, junction, or other reparse point. Invalid or
duplicate metadata is reported and left untouched instead of silently assigning
a new identity.

The database also exposes deterministic direct/transitive dependency and
referencer queries. Missing IDs remain in the result, and reachable cycles are
reported as canonical paths. In the editor, right-click an indexed asset and
choose **Reference Viewer…** to inspect this graph without modifying files.
