// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal static class EditorWorkspaceSelfTest
{
    internal static int Run(TextWriter output)
    {
        int failures = 0;
        int passes = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-editor-workspace-selftest",
            Guid.NewGuid().ToString("N"));
        string path = Path.Combine(root, "workspaces.json");

        void Check(bool condition, string name)
        {
            if (condition)
            {
                passes++;
                output.WriteLine($"PASS: {name}");
            }
            else
            {
                failures++;
                output.WriteLine($"FAIL: {name}");
            }
        }

        bool Throws<T>(Action action) where T : Exception
        {
            try
            {
                action();
                return false;
            }
            catch (T)
            {
                return true;
            }
        }

        try
        {
            var store = new EditorWorkspaceStore(path);
            Check(store.GetProfiles().Count == 3, "fresh store exposes built-in workspaces");
            Check(store.GetProfiles().All(profile => profile.IsBuiltIn),
                "fresh store has no user workspaces");

            var authored = new EditorWorkspaceLayout
            {
                HierarchyWidth = 315,
                InspectorWidth = 440,
                BottomDockHeight = 275,
                HierarchyVisible = true,
                InspectorVisible = false,
                BottomDockVisible = true,
                BottomTab = "assets",
            };
            EditorWorkspaceProfile saved =
                store.SaveUserProfile("Animation", authored, overwrite: false);
            Check(!saved.IsBuiltIn && saved.Name == "Animation",
                "save creates a user workspace");
            Check(File.Exists(path), "save creates the catalogue");
            Check(!Directory.EnumerateFiles(root, "*.tmp").Any(),
                "atomic save leaves no temp file");

            byte[] firstSave = File.ReadAllBytes(path);
            store.MarkActive("Animation");
            byte[] secondSave = File.ReadAllBytes(path);
            Check(firstSave.SequenceEqual(secondSave),
                "same state serializes deterministically");

            var reloaded = new EditorWorkspaceStore(path);
            Check(reloaded.TryGetProfile("animation", out EditorWorkspaceProfile loaded) &&
                  loaded.Layout.InspectorWidth == 440 &&
                  loaded.Layout.BottomTab == "assets",
                "profile round-trips case-insensitive lookup and layout");
            Check(reloaded.LastActiveName == "Animation",
                "last active workspace round-trips");

            reloaded.DuplicateProfile("Debugging", "My Debug");
            Check(reloaded.TryGetProfile("My Debug", out EditorWorkspaceProfile duplicate) &&
                  duplicate.Layout.BottomTab == "build" &&
                  duplicate.Layout.BottomDockHeight == 360,
                "built-in workspace can be duplicated");

            reloaded.RenameUserProfile("My Debug", "Diagnostics");
            Check(!reloaded.TryGetProfile("My Debug", out _) &&
                  reloaded.TryGetProfile("Diagnostics", out _),
                "user workspace can be renamed");
            reloaded.DeleteUserProfile("Diagnostics");
            Check(!reloaded.TryGetProfile("Diagnostics", out _),
                "user workspace can be deleted");

            Check(Throws<InvalidOperationException>(() =>
                    reloaded.SaveUserProfile("Level Editing", authored, overwrite: true)),
                "built-in overwrite is rejected");
            Check(Throws<InvalidOperationException>(() =>
                    reloaded.DeleteUserProfile("Debugging")),
                "built-in delete is rejected");
            Check(Throws<ArgumentException>(() =>
                    reloaded.SaveUserProfile("bad\nname", authored, overwrite: false)),
                "control characters in names are rejected");

            EditorWorkspaceProfile normalized = reloaded.SaveUserProfile(
                "Normalized",
                new EditorWorkspaceLayout
                {
                    HierarchyWidth = double.NaN,
                    InspectorWidth = 99999,
                    BottomDockHeight = -1,
                    BottomTab = "unknown",
                },
                overwrite: false);
            Check(normalized.Layout.HierarchyWidth == 260 &&
                  normalized.Layout.InspectorWidth == 1200 &&
                  normalized.Layout.BottomDockHeight == 140 &&
                  normalized.Layout.BottomTab == "console",
                "invalid layout dimensions and tabs are normalized");
            Check(EditorWorkspaceStore.NormalizeBottomTab("profiler") == "profiler",
                "profiler is a persistable bottom-dock tab");

            File.WriteAllText(path, "{not-json");
            var corrupt = new EditorWorkspaceStore(path);
            Check(corrupt.LoadWarning != null &&
                  corrupt.GetProfiles().Count == 3 &&
                  corrupt.GetProfiles().All(profile => profile.IsBuiltIn),
                "corrupt catalogue falls back to built-ins");

            File.WriteAllBytes(path, new byte[300 * 1024]);
            var oversized = new EditorWorkspaceStore(path);
            Check(oversized.LoadWarning != null &&
                  oversized.GetProfiles().Count == 3,
                "oversized catalogue falls back safely");

            var limitStore = new EditorWorkspaceStore(Path.Combine(root, "limit.json"));
            for (int index = 0; index < EditorWorkspaceStore.MaxUserProfiles; index++)
                limitStore.SaveUserProfile($"User {index:D2}", authored, overwrite: false);
            Check(Throws<InvalidOperationException>(() =>
                    limitStore.SaveUserProfile("One Too Many", authored, overwrite: false)),
                "user workspace count is bounded");
        }
        catch (Exception ex)
        {
            failures++;
            output.WriteLine("FAIL: unhandled exception: " + ex);
        }
        finally
        {
            try
            {
                if (Directory.Exists(root))
                    Directory.Delete(root, recursive: true);
            }
            catch
            {
                // A failed cleanup is not a product failure; the directory is uniquely named.
            }
        }

        output.WriteLine($"Editor workspace self-test: {passes} PASS / {failures} failures");
        return failures;
    }
}
