// SPDX-License-Identifier: Apache-2.0

using System.Windows.Input;

namespace AcsEditor;

internal enum BuildShortcutAction
{
    None,
    Build,
    BuildAndRun,
    StandaloneRun,
}

internal static class EditorShortcutRouting
{
    private const ModifierKeys TrackedModifiers =
        ModifierKeys.Control | ModifierKeys.Shift | ModifierKeys.Alt | ModifierKeys.Windows;

    internal static BuildShortcutAction ResolveBuildShortcut(
        Key key,
        ModifierKeys modifiers)
    {
        ModifierKeys exact = modifiers & TrackedModifiers;
        if (key == Key.F5 && exact == ModifierKeys.Control)
            return BuildShortcutAction.StandaloneRun;
        if (key == Key.F5 && exact == ModifierKeys.None)
            return BuildShortcutAction.BuildAndRun;
        if (key == Key.F7 && exact == ModifierKeys.None)
            return BuildShortcutAction.Build;
        return BuildShortcutAction.None;
    }
}
