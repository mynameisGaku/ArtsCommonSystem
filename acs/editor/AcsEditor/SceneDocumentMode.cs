// SPDX-License-Identifier: Apache-2.0

namespace AcsEditor;

/// <summary>
/// Persistence format of a scene document. This small shared contract is kept separate from the
/// autosave implementation so headless packaging tools can validate scene references without
/// linking editor-only recovery services.
/// </summary>
internal enum SceneDocumentMode
{
    TwoD,
    ThreeD,
}
