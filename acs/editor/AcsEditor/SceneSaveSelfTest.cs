// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace AcsEditor;

/// <summary>Headless checks for Save All planning and the atomic scene-source durability boundary.</summary>
internal static class SceneSaveSelfTest
{
    internal static int Run(TextWriter log)
    {
        int failures = 0;
        string root = Path.Combine(
            Path.GetTempPath(), "acs-scene-save-selftest-" + Guid.NewGuid().ToString("N"));
        string assets = Path.Combine(root, "Project", "Assets");

        void Check(bool condition, string name)
        {
            if (condition)
                log.WriteLine("PASS  " + name);
            else
            {
                ++failures;
                log.WriteLine("FAIL  " + name);
            }
        }

        try
        {
            Directory.CreateDirectory(assets);
            string projectRoot = Path.GetDirectoryName(assets)!;

            IReadOnlyList<SceneDocumentMode> none = SceneSaveAllPlanner.BuildOrder(
                activeDocumentIs3D: false,
                twoDInitialized: true,
                twoDDirty: false,
                threeDInitialized: true,
                threeDDirty: false);
            Check(none.Count == 0, "clean documents produce an empty Save All plan");

            IReadOnlyList<SceneDocumentMode> activeFirst = SceneSaveAllPlanner.BuildOrder(
                activeDocumentIs3D: true,
                twoDInitialized: true,
                twoDDirty: true,
                threeDInitialized: true,
                threeDDirty: true);
            Check(activeFirst.SequenceEqual(
                    new[] { SceneDocumentMode.ThreeD, SceneDocumentMode.TwoD }),
                "active dirty document is saved first");

            IReadOnlyList<SceneDocumentMode> initializedOnly = SceneSaveAllPlanner.BuildOrder(
                activeDocumentIs3D: false,
                twoDInitialized: false,
                twoDDirty: true,
                threeDInitialized: true,
                threeDDirty: true);
            Check(initializedOnly.SequenceEqual(new[] { SceneDocumentMode.ThreeD }),
                "uninitialized documents are never included");

            Check(
                EditorShortcutRouting.ResolveBuildShortcut(
                    System.Windows.Input.Key.F5,
                    System.Windows.Input.ModifierKeys.Control) ==
                BuildShortcutAction.StandaloneRun,
                "Ctrl+F5 routes to Standalone Run");
            Check(
                EditorShortcutRouting.ResolveBuildShortcut(
                    System.Windows.Input.Key.F5,
                    System.Windows.Input.ModifierKeys.None) ==
                BuildShortcutAction.BuildAndRun,
                "plain F5 routes to Build and Run");
            Check(
                EditorShortcutRouting.ResolveBuildShortcut(
                    System.Windows.Input.Key.F5,
                    System.Windows.Input.ModifierKeys.Control |
                    System.Windows.Input.ModifierKeys.Shift) ==
                BuildShortcutAction.None,
                "Ctrl+Shift+F5 does not alias Ctrl+F5");
            Check(
                EditorShortcutRouting.ResolveBuildShortcut(
                    System.Windows.Input.Key.F7,
                    System.Windows.Input.ModifierKeys.None) ==
                BuildShortcutAction.Build,
                "plain F7 routes to Build");

            Check(
                Enum.GetValues<EditorSceneViewMode>().All(_ =>
                    BuildSceneCompatibility.Evaluate(
                        runtimeSourceIs3D: false,
                        "Assets/main.acscene").IsSupported),
                ".acscene build compatibility is independent from editor view preset");
            BuildSceneCompatibility.Result legacy3DBuild =
                BuildSceneCompatibility.Evaluate(
                    runtimeSourceIs3D: true,
                    "Assets/world.acs3d");
            Check(
                !legacy3DBuild.IsSupported &&
                legacy3DBuild.Code == BuildSceneCompatibility.Unsupported3DCode,
                ".acs3d build limitation follows source format rather than editor view");

            string scenePath = Path.Combine(assets, "main.acscene");
            SceneSourceFile.WriteProjectSceneAtomicText(
                scenePath, "first", projectRoot, assets, SceneDocumentMode.TwoD);
            SceneSourceFile.WriteProjectSceneAtomicText(
                scenePath, "second", projectRoot, assets, SceneDocumentMode.TwoD);
            Check(File.ReadAllText(scenePath) == "second",
                "atomic source write creates and replaces scene content");
            Check(!Directory.EnumerateFiles(assets, "*.tmp", SearchOption.TopDirectoryOnly).Any(),
                "atomic source write leaves no temporary file");

            string outside = Path.Combine(root, "outside.acscene");
            bool traversalRefused = false;
            try
            {
                SceneSourceFile.WriteAtomicText(outside, "escape", assets);
            }
            catch (InvalidDataException)
            {
                traversalRefused = true;
            }
            Check(traversalRefused && !File.Exists(outside),
                "required Assets root rejects an escaping destination");

            string manifestPath = Path.Combine(projectRoot, "Safety.acsproject");
            void WriteManifest(string reference) => File.WriteAllText(
                manifestPath,
                JsonSerializer.Serialize(new
                {
                    version = 1,
                    name = "Safety",
                    engineVersion = "self-test",
                    template = "blank",
                    initialScene = reference,
                }));
            bool ManifestReferenceRejected(string reference)
            {
                WriteManifest(reference);
                try
                {
                    _ = ProjectManager.ReadManifest(manifestPath);
                    return false;
                }
                catch (InvalidDataException)
                {
                    return true;
                }
                catch (ArgumentException)
                {
                    return true;
                }
                catch (NotSupportedException)
                {
                    return true;
                }
            }

            WriteManifest("Assets/Scenes/../main.acscene");
            Project normalizedProject = ProjectManager.ReadManifest(manifestPath);
            Check(normalizedProject.InitialScene == "Assets/main.acscene",
                "project InitialScene is normalized to a portable Assets-relative path");
            Check(SceneSourceFile.PathsEqual(normalizedProject.InitialScenePath, scenePath),
                "validated project InitialScene resolves to the intended source file");

            Check(ManifestReferenceRejected("../outside.acscene"),
                "project manifest rejects traversal outside the project");
            Check(ManifestReferenceRejected("Assets/../outside.acscene"),
                "project manifest rejects traversal outside Assets");
            Check(ManifestReferenceRejected("Config/main.acscene"),
                "project manifest rejects a project-relative scene outside Assets");
            Check(ManifestReferenceRejected(scenePath),
                "project manifest rejects absolute scene references, including inside Assets");
            Check(ManifestReferenceRejected("Assets/main.txt"),
                "project manifest rejects unsupported scene extensions");

            string scene3DPath = Path.Combine(assets, "world.acs3d");
            SceneSourceFile.WriteProjectSceneAtomicText(
                scene3DPath,
                "scene3d",
                projectRoot,
                assets,
                SceneDocumentMode.ThreeD);
            Check(
                SceneSourceFile.PathsEqual(
                    SceneSourceFile.ResolveProjectSceneReference(
                        projectRoot,
                        assets,
                        "Assets/world.acs3d",
                        SceneDocumentMode.ThreeD),
                    scene3DPath),
                "3D project scene contract accepts .acs3d");
            Check(RejectsMode(scene3DPath, assets, SceneDocumentMode.TwoD),
                "2D project scene contract rejects .acs3d");
            Check(RejectsMode(scenePath, assets, SceneDocumentMode.ThreeD),
                "3D project scene contract rejects .acscene");

            CheckReparseDefense(root, assets, Check, log);
        }
        catch (Exception ex)
        {
            ++failures;
            log.WriteLine("FAIL  unexpected exception");
            log.WriteLine(ex);
        }
        finally
        {
            try
            {
                string fullRoot = Path.GetFullPath(root);
                string fullTemp = Path.GetFullPath(Path.GetTempPath());
                if (fullRoot.StartsWith(fullTemp, StringComparison.OrdinalIgnoreCase) &&
                    Path.GetFileName(fullRoot).StartsWith(
                        "acs-scene-save-selftest-",
                        StringComparison.Ordinal))
                    Directory.Delete(fullRoot, recursive: true);
            }
            catch (Exception ex)
            {
                log.WriteLine("WARN  self-test cleanup: " + ex.Message);
            }
        }

        log.WriteLine($"Scene save self-test: {failures} failure(s)");
        return failures;
    }

    private static bool RejectsMode(
        string path,
        string assets,
        SceneDocumentMode mode)
    {
        try
        {
            _ = SceneSourceFile.ValidateProjectScenePath(path, assets, mode);
            return false;
        }
        catch (InvalidDataException)
        {
            return true;
        }
    }

    private static void CheckReparseDefense(
        string root,
        string assets,
        Action<bool, string> check,
        TextWriter log)
    {
        string outsideDirectory = Path.Combine(root, "link-target");
        string linkedDirectory = Path.Combine(assets, "Linked");
        Directory.CreateDirectory(outsideDirectory);
        try
        {
            Directory.CreateSymbolicLink(linkedDirectory, outsideDirectory);
            bool refused = false;
            try
            {
                SceneSourceFile.WriteProjectSceneAtomicText(
                    Path.Combine(linkedDirectory, "linked.acscene"),
                    "must-not-follow-link",
                    Path.GetDirectoryName(assets)!,
                    assets,
                    SceneDocumentMode.TwoD);
            }
            catch (InvalidDataException)
            {
                refused = true;
            }
            check(refused, "reparse directory inside Assets is refused");
            check(!Directory.EnumerateFiles(outsideDirectory).Any(),
                "reparse target receives no scene source");

            bool referenceRefused = false;
            try
            {
                _ = SceneSourceFile.ResolveProjectSceneReference(
                    Path.GetDirectoryName(assets)!,
                    assets,
                    "Assets/Linked/linked.acscene",
                    SceneDocumentMode.TwoD);
            }
            catch (InvalidDataException)
            {
                referenceRefused = true;
            }
            check(referenceRefused,
                "project scene resolver refuses a reparse directory inside Assets");

            string projectRoot = Path.GetDirectoryName(assets)!;
            string ordinaryManifest = Path.Combine(projectRoot, "Ordinary.acsproject");
            File.WriteAllText(
                ordinaryManifest,
                """
                {
                  "version": 1,
                  "name": "Ordinary",
                  "engineVersion": "self-test",
                  "template": "blank",
                  "initialScene": "Assets/main.acscene"
                }
                """);
            string linkedManifest = Path.Combine(projectRoot, "Linked.acsproject");
            File.CreateSymbolicLink(linkedManifest, ordinaryManifest);
            bool manifestLinkRefused = false;
            try
            {
                _ = ProjectManager.ReadManifest(linkedManifest);
            }
            catch (InvalidDataException)
            {
                manifestLinkRefused = true;
            }
            check(manifestLinkRefused, ".acsproject reparse files are refused");

            string linkedProjectTarget = Path.Combine(root, "project-link-target");
            string linkedProjectAssets = Path.Combine(linkedProjectTarget, "Assets");
            Directory.CreateDirectory(linkedProjectAssets);
            File.WriteAllText(
                Path.Combine(linkedProjectAssets, "main.acscene"),
                "linked-root-scene");
            File.WriteAllText(
                Path.Combine(linkedProjectTarget, "LinkedRoot.acsproject"),
                """
                {
                  "version": 1,
                  "name": "LinkedRoot",
                  "engineVersion": "self-test",
                  "template": "blank",
                  "initialScene": "Assets/main.acscene"
                }
                """);
            string linkedProjectRoot = Path.Combine(root, "LinkedProject");
            Directory.CreateSymbolicLink(linkedProjectRoot, linkedProjectTarget);
            bool projectRootContractRefused = false;
            try
            {
                SceneSourceFile.ValidateProjectRootDirectory(linkedProjectRoot);
            }
            catch (InvalidDataException)
            {
                projectRootContractRefused = true;
            }
            check(projectRootContractRefused,
                "project creation/open root contract refuses reparse directories");

            string escapedWriteTarget = Path.Combine(
                linkedProjectTarget,
                "Assets",
                "must-not-write.acscene");
            bool projectRootWriteRefused = false;
            try
            {
                SceneSourceFile.WriteProjectSceneAtomicText(
                    Path.Combine(
                        linkedProjectRoot,
                        "Assets",
                        "must-not-write.acscene"),
                    "must-not-follow-project-root",
                    linkedProjectRoot,
                    Path.Combine(linkedProjectRoot, "Assets"),
                    SceneDocumentMode.TwoD);
            }
            catch (InvalidDataException)
            {
                projectRootWriteRefused = true;
            }
            check(
                projectRootWriteRefused && !File.Exists(escapedWriteTarget),
                "project scene writer refuses a reparse root before creating its temp file");

            bool projectRootLinkRefused = false;
            try
            {
                _ = ProjectManager.ReadManifest(
                    Path.Combine(linkedProjectRoot, "LinkedRoot.acsproject"));
            }
            catch (InvalidDataException)
            {
                projectRootLinkRefused = true;
            }
            check(projectRootLinkRefused, "project root reparse directories are refused");
        }
        catch (UnauthorizedAccessException)
        {
            log.WriteLine("SKIP  scene source reparse test (symbolic-link privilege unavailable)");
        }
        catch (PlatformNotSupportedException)
        {
            log.WriteLine("SKIP  scene source reparse test (symbolic links unsupported)");
        }
        catch (IOException ex)
        {
            log.WriteLine("SKIP  scene source reparse test: " + ex.Message);
        }
    }
}
