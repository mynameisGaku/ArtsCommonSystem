# Details multi-selection editing

The 3D Details panel treats the native selection set as one edit target when
two or more nodes are selected. It does not copy the primary node over the
other nodes.

## User-facing behavior

- The header shows the number of selected objects.
- `Enabled` is a three-state checkbox. An indeterminate state means the
  selection contains both enabled and disabled nodes.
- Location, Rotation, Scale, and Mesh Renderer Color resolve each numeric
  component independently. `—` means that component has multiple values.
- Entering one component applies only that component. Every unedited component
  keeps its per-node value.
- `Reset selected transforms` resets Location and Rotation to zero and Scale to
  one for the whole selection.
- Mesh Renderer remains a native card in the ordinary Components stack after
  Transform. Type and Material show a common value or `—`; Color supports
  multi-editing.
- Component add/remove and Material reassignment remain single-selection
  operations until their serializers have a complete rollback contract.

## Consistency and history contract

`InspectorMultiEditContract` normalizes the selection, resolves mixed values,
and coordinates mutation callbacks. A batch:

1. rejects an empty, duplicate, invalid, or stale selection;
2. captures every selected node before the first write;
3. applies writes in selection order;
4. on failure, restores the failing target and every earlier write in reverse
   order, continuing even if one restore reports failure;
5. reports rollback failure instead of presenting a false success.

The WPF integration wraps each batch in exactly one
`BeginSceneDocumentTransaction` scope. Therefore a successful multi-edit
produces one canonical Scene Undo/Redo entry, while a fully rolled-back batch
has the same final canonical snapshot as its initial state. Selection identity
is included in the merge key so two different selections cannot accidentally
share edit history.

Native transform, color, and enabled values are read back after writes.
Transform writes use the ABI-negotiated `sparse-transform-mutation-v1`
contract. Its nine-bit mask writes only edited components; unmasked legacy
scale values are not even reassigned. The native minimum-invertible-scale
clamp applies only to a scale axis present in that mask, and managed expected
readback mirrors that same per-axis rule. Unknown or empty masks, non-finite
selected values, and any buffer count other than the complete nine-float
transform fail before mutation.

A legacy zero or near-zero scale is representable in older Scene payloads but
cannot be restored through the safety-clamping setter if a later target fails.
Therefore a batch that edits such an axis fails during capture, before any
write. Details reports the exact node and asks the user to select that object
alone, enter a non-zero scale, then retry the batch. Location and Rotation
batch edits remain valid and preserve those legacy scale bits unchanged.

## Verification

Run:

```powershell
acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --inspector-multi-edit-selftest
```

The headless test covers common/mixed presentation, stable selection identity,
stale-selection comparison, capture-before-write, successful batch apply,
reverse rollback including the failing target, visible rollback failure, and
bit-exact preservation of unedited legacy scale axes. It also proves that a
non-restorable selected scale aborts during capture with zero writes. The native lifecycle
suite also fixes mask validation, finite selected values, per-axis scale
canonicalization, and readback after rejected writes. The switch is part of
both fast and full `scripts/verify_editor.ps1` suites.

## Deliberate next steps

- reflected component-property multi-edit based on a common schema;
- atomic component add/remove/reorder;
- typed Material assignment with full before-state restoration;
- property/component copy and paste with versioned payload validation;
- prefab/default diff markers and reset-to-source;
- parity for the legacy 2D adapter while it remains supported.
