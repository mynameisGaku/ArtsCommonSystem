// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

internal static class AssetPathChangeSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS: " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL: " + label);
            }
        }

        string root = Path.Combine(Path.GetTempPath(), "acs-path-change-selftest");
        string oldFolder = Path.Combine(root, "Cloud");
        string newFolder = Path.Combine(root, "Weather", "Cloud");
        string oldAsset = Path.Combine(oldFolder, "Materials", "Sky.acsmat");
        string expectedAsset = Path.Combine(newFolder, "Materials", "Sky.acsmat");
        var moved = new AssetPathsChangedEventArgs(mappings: new[]
        {
            new AssetPathMapping(oldFolder.ToUpperInvariant(), newFolder),
        });
        Check(moved.TryRemapPath(oldAsset, out string remapped) &&
              AssetPathBoundary.Equals(remapped, expectedAsset),
            "folder mappings rebind descendants case-insensitively");
        Check(!moved.TryRemapPath(
                Path.Combine(root, "CloudBank", "Sky.acsmat"),
                out _),
            "folder mappings reject textual-prefix siblings");
        Check(moved.AffectsPath(oldAsset) &&
              !moved.AffectsPath(expectedAsset) &&
              !moved.AffectsPath(Path.Combine(root, "CloudBank", "Sky.acsmat")),
            "path-change targeting uses only boundary-safe source subtrees");

        var deleted = new AssetPathsChangedEventArgs(deletedRoots: new[] { oldFolder });
        Check(deleted.IsDeletedPath(oldAsset.ToUpperInvariant()) &&
              !deleted.IsDeletedPath(Path.Combine(root, "CloudBank", "Sky.acsmat")),
            "delete roots use case-insensitive component boundaries");

        string[] collapsed = AssetPathBoundary.CollapseRoots(new[]
        {
            oldAsset,
            oldFolder,
            oldFolder.ToUpperInvariant(),
        });
        Check(collapsed.Length == 1 && AssetPathBoundary.Equals(collapsed[0], oldFolder),
            "nested selections collapse to a single top-level root");

        var starting = new AssetPathMutationStartingEventArgs(
            Guid.NewGuid(),
            AssetPathMutationKind.Move,
            new[] { oldFolder });
        Check(starting.AffectsPath(oldAsset) &&
              !starting.AffectsPath(Path.Combine(root, "CloudBank", "Sky.acsmat")),
            "mutation preflight targets only the selected root subtree");

        var unchanged = new AssetPathsChangedEventArgs(mappings: new[]
        {
            new AssetPathMapping(oldAsset, oldAsset.ToUpperInvariant()),
        });
        Check(unchanged.Mappings.Count == 0,
            "case-insensitive no-op mappings do not retarget documents");

        output.WriteLine($"Asset path change self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
