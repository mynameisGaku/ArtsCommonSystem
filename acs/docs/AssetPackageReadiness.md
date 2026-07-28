# Asset Package Readiness

Asset View exposes **Asset Actions > Package Readiness…** as the preflight
surface for the content that will actually enter a package. It does not scan
every filename indiscriminately. The audit starts from the project's
`canonicalSceneAssetId` and runs the same metadata-authoritative
`AssetCookPlanner` closure used by Package.

## What the audit reports

The summary distinguishes:

- whether the canonical closure is ready to Cook;
- the canonical root Scene and stable graph SHA-256;
- required asset, error, and warning counts;
- every required asset with its persistent ID, kind, source size, and content
  SHA-256; and
- structured diagnostics for missing or invalid metadata, missing dependency
  IDs, unsupported reachable formats, stale source/sidecar dependency edges,
  dependency cycles, unsafe/reparse paths, invalid external references,
  non-ASCII runtime paths, and invalid canonical Scene identity.

Each diagnostic includes a concrete repair route. Double-clicking a diagnostic
or choosing **Locate in Asset View** navigates to an existing safe asset.
**Reference Viewer** opens the existing dependency explorer for diagnostics
with a resolvable Asset ID. Missing assets remain fail-closed and are repaired
from the owning asset's Reference Viewer rather than by guessing a filename.

The **Cook Closure** tab is the exact required set. Unreachable assets are not
listed because they do not enter the build graph.

## Responsiveness and cancellation

Strict metadata refresh, content verification, dependency scanning, and graph
construction run on a worker task. The window stays interactive and exposes a
Cancel action; Escape cancels an active audit and F5 runs it again. Cancellation
is propagated into `AssetDatabase.RefreshForCook` and every traversal/scanner
checkpoint. A cancelled or failed audit does not mutate the project, synthesize
metadata, or publish a partial success result.

Opening the window snapshots the project root, Assets root, project name, and
canonical Scene ID. If a different project replaces the Asset View, navigation
callbacks from the old window are ignored.

## JSON report

**Save JSON…** publishes schema version `1` with camel-case field names and
string severities. JSON contains the same summary, diagnostics, resolution
text, and required-asset ledger shown in the window.

Reports are deterministic for one verified project snapshot. Publication:

- never overwrites an existing file;
- rejects existing reparse-point ancestors;
- writes a private sibling temporary file;
- flushes it durably;
- rechecks the destination before an atomic no-overwrite move; and
- removes only its own ordinary temporary file on failure.

This report is a local Cook-readiness result, not proof that a later executable
build, runtime dependency scan, signing step, or store upload will succeed.
Package repeats the authoritative closure validation before ZIP publication.

## Verification

Run the focused backend contract:

```powershell
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe `
  --asset-package-readiness-selftest
```

It covers ready and blocked closures, unsupported reachable dependencies,
missing authoritative sidecars, cancellation, deterministic JSON, atomic
no-overwrite publication, and repair-route coverage.

Render the blocked diagnostics and closure tabs:

```powershell
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe `
  --asset-package-readiness-visual-fixture `
  "$env:TEMP\acs-asset-package-readiness.png"
```

The fixture writes the requested diagnostics image plus a sibling
`-closure.png` image and validates that the summary, grids, and report/navigation
actions have usable, non-overlapping geometry.
