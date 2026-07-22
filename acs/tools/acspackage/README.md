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

dotnet run --project tools/acspackage -- --self-test
```

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
