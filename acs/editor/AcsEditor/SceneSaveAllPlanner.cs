// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;

namespace AcsEditor;

/// <summary>
/// Produces a deterministic dirty-document save plan. The active document is first so a path
/// prompt or write failure is reported before Save All mutates the background document.
/// </summary>
internal static class SceneSaveAllPlanner
{
    internal static IReadOnlyList<SceneDocumentMode> BuildOrder(
        bool activeDocumentIs3D,
        bool twoDInitialized,
        bool twoDDirty,
        bool threeDInitialized,
        bool threeDDirty)
    {
        var order = new List<SceneDocumentMode>(2);
        SceneDocumentMode active = activeDocumentIs3D
            ? SceneDocumentMode.ThreeD
            : SceneDocumentMode.TwoD;
        SceneDocumentMode inactive = activeDocumentIs3D
            ? SceneDocumentMode.TwoD
            : SceneDocumentMode.ThreeD;

        AddIfRequired(active);
        AddIfRequired(inactive);
        return order;

        void AddIfRequired(SceneDocumentMode mode)
        {
            bool required = mode == SceneDocumentMode.ThreeD
                ? threeDInitialized && threeDDirty
                : twoDInitialized && twoDDirty;
            if (required)
                order.Add(mode);
        }
    }
}
