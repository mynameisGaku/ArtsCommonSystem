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
- Reflected component cards are shown only when the exact component type exists
  once on every selected node. Their properties are shown only when name, kind,
  flags, category, and schema default match exactly on every target.
- Reflected Bool fields use the same three-state presentation. Numeric and
  vector fields expose mixed values per component and preserve every untouched
  authored float bit. Known enum fields and ObjectRef fields use typed choices;
  ObjectRef choices exclude the selected nodes to prevent self-reference.
- `Reset to Default` applies the reflected schema default to every target. It is
  unavailable when the provider has no default or the property is hidden,
  read-only, or String.
- Component add/remove/reorder, Material reassignment, reflected String edits,
  and CallInEditor methods remain single-selection operations until their
  serializers have a complete rollback contract.

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

Reflected component batches apply the same contract at the component-property
level. Capture resolves the per-target component slot and property index, then
revalidates the exact type and schema immediately before reading any value.
Duplicate component types, schema drift, stale selection, non-finite values,
and integer values outside the exact float-integer range fail with zero writes.
After each mutation and rollback, the complete Float4 payload is read back and
compared bit-for-bit. The failing target is included in reverse rollback, and
the whole batch is one Scene Undo/Redo transaction.

The editor ABI exposes each reflected property's finite four-component schema
default through `acs_editor_component_prop_default_at`. Invalid types, indices,
or non-finite defaults fail without modifying caller output. Older providers
that do not expose this optional entry point still permit compatible edits, but
Details omits Reset rather than guessing a default.

## Verification

Run:

```powershell
acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --inspector-multi-edit-selftest
acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe --inspector-reflected-multi-edit-selftest
```

The base headless test covers common/mixed presentation, stable selection
identity, stale-selection comparison, capture-before-write, successful batch
apply, reverse rollback including the failing target, visible rollback failure,
and bit-exact preservation of unedited legacy scale axes. It also proves that a
non-restorable selected scale aborts during capture with zero writes. The
native lifecycle suite fixes mask validation, finite selected values, per-axis
scale canonicalization, and readback after rejected writes.

The reflected suite additionally covers exact component/property intersection,
per-target slot and property-index resolution, duplicate-type and schema-drift
rejection, explicit vector and Bool mixed values, sparse bit preservation,
integer canonicalization, schema-default Reset, stale selection, preflight
before writes, failing-target rollback, document-host integration, and one-unit
Scene Undo/Redo history. Both switches are part of the fast and full
`scripts/verify_editor.ps1` suites.

## Deliberate next steps

- atomic component add/remove/reorder;
- typed Material assignment with full before-state restoration;
- reflected String editing and CallInEditor method batching;
- property/component copy and paste with versioned payload validation;
- prefab/default diff markers and reset-to-source;
- parity for the legacy 2D adapter while it remains supported.
