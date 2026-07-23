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

        var scenePaths = new SceneAssetPathState(
            oldAsset,
            oldAsset,
            Path.Combine(root, "World.acs3d"));
        var deleteSceneFolder = new AssetPathMutationStartingEventArgs(
            Guid.NewGuid(),
            AssetPathMutationKind.Delete,
            new[] { oldFolder });
        var rewriteSceneFolder = new AssetPathMutationStartingEventArgs(
            Guid.NewGuid(),
            AssetPathMutationKind.ContentRewrite,
            new[] { oldFolder });
        var moveSceneFolder = new AssetPathMutationStartingEventArgs(
            Guid.NewGuid(),
            AssetPathMutationKind.Move,
            new[] { oldFolder });
        Check(scenePaths.ShouldVeto(deleteSceneFolder) &&
              scenePaths.ShouldVeto(rewriteSceneFolder) &&
              !scenePaths.ShouldVeto(moveSceneFolder),
            "open scene paths veto destructive mutations but allow path-preserving moves");

        SceneAssetPathState movedScenePaths = scenePaths.Apply(
            moved,
            out bool sceneRemapped,
            out bool sceneDetached);
        Check(sceneRemapped &&
              !sceneDetached &&
              AssetPathBoundary.Equals(movedScenePaths.CurrentPath!, expectedAsset) &&
              AssetPathBoundary.Equals(movedScenePaths.TwoDPath!, expectedAsset) &&
              AssetPathBoundary.Equals(
                  movedScenePaths.ThreeDPath!,
                  Path.Combine(root, "World.acs3d")),
            "committed mappings atomically retarget every matching scene path alias");

        SceneAssetPathState deletedScenePaths = scenePaths.Apply(
            deleted,
            out bool deletionRemapped,
            out bool deletionDetached);
        Check(!deletionRemapped &&
              deletionDetached &&
              deletedScenePaths.CurrentPath == null &&
              deletedScenePaths.TwoDPath == null &&
              AssetPathBoundary.Equals(
                  deletedScenePaths.ThreeDPath!,
                  Path.Combine(root, "World.acs3d")),
            "unexpected deletion notifications detach scene aliases instead of retaining stale paths");

        var successfulGuard = new SceneAssetPathMutationGuard(
            scenePaths,
            moveSceneFolder);
        Check(successfulGuard.IsActive && !successfulGuard.PathChangePublished,
            "scene path guard blocks save and autosave from mutation preflight");
        SceneAssetPathState guardedMovedPaths = successfulGuard.Publish(
            moved,
            out bool guardedRemapped,
            out bool guardedDetached);
        Check(guardedRemapped &&
              !guardedDetached &&
              successfulGuard.IsActive &&
              successfulGuard.PathChangePublished &&
              AssetPathBoundary.Equals(guardedMovedPaths.CurrentPath!, expectedAsset) &&
              AssetPathBoundary.Equals(guardedMovedPaths.TwoDPath!, expectedAsset),
            "scene path guard publishes every matching alias before resume");
        SceneAssetPathState completedMovedPaths = successfulGuard.Complete(
            succeeded: true,
            out bool successfulFallbackDetach);
        Check(!successfulGuard.IsActive &&
              !successfulFallbackDetach &&
              AssetPathBoundary.Equals(completedMovedPaths.CurrentPath!, expectedAsset),
            "successful published mutation resumes with the committed scene identity");

        var failedGuard = new SceneAssetPathMutationGuard(
            scenePaths,
            moveSceneFolder);
        SceneAssetPathState failedPaths = failedGuard.Complete(
            succeeded: false,
            out bool failedFallbackDetach);
        Check(!failedGuard.IsActive &&
              !failedFallbackDetach &&
              AssetPathBoundary.Equals(failedPaths.CurrentPath!, oldAsset) &&
              AssetPathBoundary.Equals(failedPaths.TwoDPath!, oldAsset),
            "failed mutation resumes with every original scene alias intact");

        var missingPublicationGuard = new SceneAssetPathMutationGuard(
            scenePaths,
            moveSceneFolder);
        SceneAssetPathState failClosedPaths = missingPublicationGuard.Complete(
            succeeded: true,
            out bool missingPublicationDetached);
        Check(!missingPublicationGuard.IsActive &&
              missingPublicationDetached &&
              failClosedPaths.CurrentPath == null &&
              failClosedPaths.TwoDPath == null &&
              AssetPathBoundary.Equals(
                  failClosedPaths.ThreeDPath!,
                  Path.Combine(root, "World.acs3d")),
            "success without a destination publication detaches stale aliases before resume");

        var inputBlockTransitions = new System.Collections.Generic.List<bool>();
        var inputBlock = new SceneEditingBlockState(inputBlockTransitions.Add);
        IDisposable outerInputBlock = inputBlock.Enter();
        IDisposable nestedInputBlock = inputBlock.Enter();
        Check(inputBlock.IsBlocked &&
              inputBlock.Depth == 2 &&
              inputBlockTransitions.Count == 1 &&
              inputBlockTransitions[0],
            "scene mutation input block remains active across nested slow-drain owners");
        outerInputBlock.Dispose();
        Check(inputBlock.IsBlocked &&
              inputBlock.Depth == 1 &&
              inputBlockTransitions.Count == 1,
            "one completion cannot unblock another active scene mutation lease");
        nestedInputBlock.Dispose();
        Check(!inputBlock.IsBlocked &&
              inputBlock.Depth == 0 &&
              inputBlockTransitions.Count == 2 &&
              !inputBlockTransitions[1],
            "final scene mutation completion restores input exactly once");

        bool exceptionalExitObserved = false;
        try
        {
            using IDisposable exceptionalInputBlock = inputBlock.Enter();
            throw new IOException("simulated autosave drain failure");
        }
        catch (IOException)
        {
            exceptionalExitObserved = true;
        }
        Check(exceptionalExitObserved &&
              !inputBlock.IsBlocked &&
              inputBlock.Depth == 0 &&
              inputBlockTransitions.Count == 4 &&
              inputBlockTransitions[2] &&
              !inputBlockTransitions[3],
            "scene mutation input block releases after exceptional completion cleanup");

        output.WriteLine($"Asset path change self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
