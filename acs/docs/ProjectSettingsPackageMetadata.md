# Package metadata in Project Settings

`Project Settings > Distribution` is the authoring surface for the optional
`Config/PackageMetadata.json` document. It edits the publisher, description,
copyright, and HTTPS support URL that Package embeds into
`package-manifest.json`.

## One validation contract

The UI, package preflight, manifest creation, CLI verification, and archive
inspection all use `PackageProductMetadataContract`. The editor displays the
contract's field issues inline; it does not mirror URL, length, whitespace, or
control-character rules in a second UI validator. Consequently, a value shown
as valid in Project Settings is evaluated by the same code when the package is
built.

Loading is strict and fail-closed. An oversized file, malformed JSON, duplicate
or unknown property, unsupported schema, invalid UTF-8, reparse point, or
invalid field is shown as a load error. Fields and Apply stay disabled, and the
editor never repairs or overwrites the rejected source. `Retry load` is
available after the file is repaired externally.

## Staging, Apply, and Revert

Field edits remain in a window-local draft:

- the status card changes to `Unsaved package metadata`;
- `Apply` is enabled only when the draft is dirty and the shared contract has
  no validation issues;
- `Revert` restores the last successfully loaded or applied values without
  touching disk; and
- closing with a dirty draft offers Apply, discard, or cancel. Closing while
  an Apply is publishing waits for its result.

Apply runs filesystem work off the UI thread. Non-empty metadata is serialized
in fixed property order as BOM-less UTF-8, flushed to a private same-directory
temporary file, and atomically published. A failed write or replacement keeps
the previous `PackageMetadata.json`; private staging is cleaned where safe.
Before either replacement or deletion, the current source is strictly loaded
again. A malformed source or reparse-point target is therefore preserved and
reported instead of being overwritten by a stale editor draft. The editor also
keeps a raw SHA-256 fingerprint (including the distinct missing-file state)
from Load. Apply compares it at admission and immediately before publication,
then verifies the published bytes afterward.

These fingerprint checks are optimistic concurrency detection, not a
filesystem-wide compare-and-swap and not a latest-writer-wins guarantee.
Replacement uses a same-directory `File.Replace` backup and validates the
bytes actually displaced by that operation. Deletion first moves the actual
leaf to a same-directory quarantine and validates those moved bytes before
removal. If a non-cooperating process wins an overlapping write, observed
competing bytes are restored when the destination is still safe to restore;
otherwise they remain in a
`PackageMetadata.json.tmp-recovery-<operation>-<guid>` file in `Config`.
The editor never silently deletes those observed competing bytes. A recovery
file is deliberately excluded from packaging and requires explicit user
inspection. The failure card offers Reload; accepting it confirms that the
local draft will be discarded.

All four empty fields mean **not configured**. Applying that draft removes the
optional file. Opening an already-empty file does not mutate it merely by
viewing Project Settings; it remains present until the user changes and
applies the draft. A missing file is also presented explicitly as not
configured and preserves backward-compatible package behavior.

Package metadata has its own Apply/Revert transaction because it is a separate
optional JSON document, not a native `ProjectSettings.ini` key. Its close guard
prevents it from being silently discarded and does not overload the native
settings document with distribution-only state.

## Verification

Run the focused contract:

```powershell
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe `
  --package-metadata-editor-selftest
```

The self-test covers deterministic serialization, shared inline validation,
staged dirty/revert behavior, atomic Apply, failed-publication preservation,
empty-file removal, raw-fingerprint external-edit conflicts,
malformed-source and reparse preservation,
missing-file state, strict-load rejection, and injected overlapping
replace/delete races that either restore or retain the actual displaced bytes.

The WPF fixture renders both configured and invalid/dirty states without
opening or modifying a real project:

```powershell
.\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe `
  --package-metadata-editor-visual-fixture `
  "$env:TEMP\acs-package-metadata-project-settings.png"
```

It verifies the Distribution surface has usable geometry and that Apply/Revert
remain visible and non-overlapping when inline URL validation is present. It
writes the configured image and a sibling `-invalid.png` image.
