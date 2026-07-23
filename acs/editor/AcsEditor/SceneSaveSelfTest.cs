// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;

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

            string lockedScenePath = Path.Combine(assets, "locked.acscene");
            bool externalSceneLockRejected = false;
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                try
                {
                    SceneSourceFile.WriteProjectSceneAtomicText(
                        lockedScenePath,
                        "must-not-write",
                        projectRoot,
                        assets,
                        SceneDocumentMode.TwoD);
                }
                catch (IOException)
                {
                    externalSceneLockRejected = true;
                }
            }
            Check(
                externalSceneLockRejected &&
                !File.Exists(lockedScenePath) &&
                !Directory.EnumerateFiles(
                    assets,
                    "." + Path.GetFileName(lockedScenePath) + ".*.tmp",
                    SearchOption.TopDirectoryOnly).Any(),
                "external project lock rejects a scene save before source or temp creation");

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

            CheckInitialSceneReferenceFollow(root, Check);
            CheckProjectSettingsSerialization(root, Check);
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

    private static void CheckInitialSceneReferenceFollow(
        string root,
        Action<bool, string> check)
    {
        string projectRoot = Path.Combine(root, "ReferenceFollowProject");
        string assets = Path.Combine(projectRoot, "Assets");
        string scenes = Path.Combine(assets, "Scenes");
        string config = Path.Combine(projectRoot, "Config");
        Directory.CreateDirectory(scenes);
        Directory.CreateDirectory(config);

        string manifestPath = Path.Combine(projectRoot, "ReferenceFollow.acsproject");
        File.WriteAllText(
            manifestPath,
            """
            {
              "version": 1,
              "name": "ReferenceFollow",
              "engineVersion": "self-test",
              "template": "blank",
              "initialScene": "Assets/main.acscene",
              "canonicalSceneAssetId": "11111111111111111111111111111111",
              "futureProperty": { "preserve": true }
            }
            """);
        string settingsPath = Path.Combine(config, "ProjectSettings.ini");
        File.WriteAllText(
            settingsPath,
            """
            [Rendering]
            Exposure=1.25

            [Game]
            DefaultScene=Assets/main.acscene
            WindowWidth=1600
            """);
        string initial = Path.Combine(assets, "main.acscene");
        File.WriteAllText(initial, "initial-scene");

        Project project = ProjectManager.ReadManifest(manifestPath);
        bool preflightAccepted = true;
        try
        {
            ProjectManager.ValidateInitialSceneReferenceFollow(project);
        }
        catch
        {
            preflightAccepted = false;
        }
        check(preflightAccepted,
            "initial-scene path follow preflight accepts coherent manifest and settings");

        string firstDestination = Path.Combine(scenes, "Opening.acscene");
        File.Move(initial, firstDestination);
        byte[] manifestBeforeLockContention = File.ReadAllBytes(manifestPath);
        byte[] settingsBeforeLockContention = File.ReadAllBytes(settingsPath);
        bool lockContentionRejected = false;
        using (AssetMutationLockProcessHolder held =
               AssetMutationLockProcessHolder.Start(assets))
        {
            try
            {
                _ = ProjectManager.FollowInitialScenePath(project, firstDestination);
            }
            catch (IOException error)
            {
                lockContentionRejected =
                    error.Message.Contains(
                        "another editor",
                        StringComparison.OrdinalIgnoreCase) &&
                    error.Message.Contains(
                        "mutation lock",
                        StringComparison.OrdinalIgnoreCase);
            }
        }
        check(
            lockContentionRejected &&
            File.ReadAllBytes(manifestPath).SequenceEqual(
                manifestBeforeLockContention) &&
            File.ReadAllBytes(settingsPath).SequenceEqual(
                settingsBeforeLockContention) &&
            project.InitialScene == "Assets/main.acscene",
            "initial-scene follow rejects a competing editor lease before either file changes");

        ProjectSceneReferenceUpdate first =
            ProjectManager.FollowInitialScenePath(project, firstDestination);
        Project persistedFirst = ProjectManager.ReadManifest(manifestPath);
        JsonObject firstManifest = JsonNode.Parse(File.ReadAllText(manifestPath))!.AsObject();
        check(
            first.PreviousReference == "Assets/main.acscene" &&
            first.CurrentReference == "Assets/Scenes/Opening.acscene" &&
            project.InitialScene == first.CurrentReference &&
            persistedFirst.InitialScene == first.CurrentReference &&
            ReadIniValue(settingsPath, "Game", "DefaultScene") == first.CurrentReference,
            "committed scene move updates manifest, Project state, and Game.DefaultScene together");
        check(
            firstManifest["futureProperty"]?["preserve"]?.GetValue<bool>() == true &&
            ReadIniValue(settingsPath, "Rendering", "Exposure") == "1.25" &&
            ReadIniValue(settingsPath, "Game", "WindowWidth") == "1600",
            "scene reference update preserves future manifest data and unrelated settings");

        // Model a second editor that remains open across the next initial-scene move.
        Project staleSettingsWriter = ProjectManager.ReadManifest(manifestPath);
        string secondDestination = Path.Combine(scenes, "OpeningRenamed.acscene");
        File.Move(firstDestination, secondDestination);
        bool injectedFailureObserved = false;
        bool competingSettingsSaveRejected = false;
        try
        {
            ProjectManager.FollowInitialScenePath(
                project,
                secondDestination,
                point =>
                {
                    if (point == ProjectSceneReferenceCommitPoint.AfterSettingsPublish)
                    {
                        competingSettingsSaveRejected = Task.Run(() =>
                        {
                            try
                            {
                                ProjectManager.SaveProjectSettings(
                                    staleSettingsWriter,
                                    """
                                    [Rendering]
                                    Exposure=2.0

                                    [Game]
                                    DefaultScene=Assets/Scenes/Opening.acscene
                                    """);
                                return false;
                            }
                            catch (IOException error)
                            {
                                return error.Message.Contains(
                                        "another editor",
                                        StringComparison.OrdinalIgnoreCase) &&
                                    error.Message.Contains(
                                        "mutation lock",
                                        StringComparison.OrdinalIgnoreCase);
                            }
                        }).GetAwaiter().GetResult();
                        throw new IOException("injected manifest publication failure");
                    }
                });
        }
        catch (IOException)
        {
            injectedFailureObserved = true;
        }
        Project persistedAfterRollback = ProjectManager.ReadManifest(manifestPath);
        check(
            injectedFailureObserved &&
            competingSettingsSaveRejected &&
            project.InitialScene == first.CurrentReference &&
            persistedAfterRollback.InitialScene == first.CurrentReference &&
            ReadIniValue(settingsPath, "Game", "DefaultScene") == first.CurrentReference,
            "startup-scene transaction rejects a competing settings writer and rolls back coherently");

        ProjectSceneReferenceUpdate second =
            ProjectManager.FollowInitialScenePath(project, secondDestination);
        check(
            second.CurrentReference == "Assets/Scenes/OpeningRenamed.acscene" &&
            ProjectManager.ReadManifest(manifestPath).InitialScene == second.CurrentReference &&
            ReadIniValue(settingsPath, "Game", "DefaultScene") == second.CurrentReference,
            "retry after reference rollback commits the new scene path");

        ProjectManager.SaveProjectSettings(
            staleSettingsWriter,
            """
            [Rendering]
            Exposure=2.5

            [Game]
            DefaultScene=Assets/Scenes/Opening.acscene
            WindowWidth=1920
            """);
        check(
            ReadIniValue(settingsPath, "Game", "DefaultScene") == second.CurrentReference &&
            ReadIniValue(settingsPath, "Rendering", "Exposure") == "2.5" &&
            ReadIniValue(settingsPath, "Game", "WindowWidth") == "1920",
            "stale editor settings save preserves the latest manifest DefaultScene");

        string coherentSettings = File.ReadAllText(settingsPath);
        File.WriteAllText(
            settingsPath,
            coherentSettings.Replace(
                second.CurrentReference,
                "Assets/other.acscene",
                StringComparison.Ordinal));
        bool mismatchVetoed = false;
        try
        {
            ProjectManager.ValidateInitialSceneReferenceFollow(project);
        }
        catch (InvalidDataException)
        {
            mismatchVetoed = true;
        }
        check(mismatchVetoed,
            "preflight vetoes an existing manifest and Game.DefaultScene mismatch");
        File.WriteAllText(settingsPath, coherentSettings);

        File.Delete(settingsPath);
        string thirdDestination = Path.Combine(scenes, "Startup.acscene");
        File.Move(secondDestination, thirdDestination);
        ProjectSceneReferenceUpdate third =
            ProjectManager.FollowInitialScenePath(project, thirdDestination);
        check(
            third.SettingsFileCreated &&
            ReadIniValue(settingsPath, "Game", "DefaultScene") == third.CurrentReference &&
            ProjectManager.ReadManifest(manifestPath).InitialScene == third.CurrentReference,
            "missing ProjectSettings.ini is created with a coherent Game.DefaultScene");
        check(
            !Directory.EnumerateFiles(
                    projectRoot,
                    "*.scene-ref-*.tmp",
                    SearchOption.AllDirectories)
                .Any(),
            "scene reference transactions leave no staging files");
    }

    private static void CheckProjectSettingsSerialization(
        string root,
        Action<bool, string> check)
    {
        string projectRoot = Path.Combine(root, "SettingsSerializationProject");
        string assets = Path.Combine(projectRoot, "Assets");
        string config = Path.Combine(projectRoot, "Config");
        Directory.CreateDirectory(assets);
        Directory.CreateDirectory(config);

        string scenePath = Path.Combine(assets, "main.acscene");
        File.WriteAllText(scenePath, "serialization-scene");
        string manifestPath = Path.Combine(
            projectRoot,
            "SettingsSerialization.acsproject");
        File.WriteAllText(
            manifestPath,
            """
            {
              "version": 1,
              "name": "SettingsSerialization",
              "engineVersion": "self-test",
              "template": "blank",
              "initialScene": "Assets/main.acscene"
            }
            """);
        string settingsPath = Path.Combine(config, "ProjectSettings.ini");
        File.WriteAllText(
            settingsPath,
            """
            [Game]
            DefaultScene=Assets/main.acscene
            """);
        Project project = ProjectManager.ReadManifest(manifestPath);

        var largeBuilder = new StringBuilder(96 * 1024);
        largeBuilder.Append(
            """
            ; serializer regression payload
            [Rendering]
            Exposure=1.25

            [Game]
            DefaultScene=Assets/main.acscene

            [Bulk]
            """);
        largeBuilder.Append('\n');
        for (int index = 0; index < 450; index++)
        {
            largeBuilder.Append("Key")
                .Append(index.ToString("D4"))
                .Append('=')
                .Append('x', 180)
                .Append('\n');
        }
        largeBuilder.Append("Tail=preserved-over-64KiB-\u96f2\n");

        string largeSettings = largeBuilder.ToString();
        byte[] largeBytes = new UTF8Encoding(false, true).GetBytes(largeSettings);
        string captured = ProjectSettingsSerialization.Capture(buffer =>
        {
            largeBytes.CopyTo(buffer, 0);
            buffer[largeBytes.Length] = 0;
            return largeBytes.Length;
        });
        ProjectManager.SaveProjectSettings(project, captured);
        check(
            largeBytes.Length > 64 * 1024 &&
            captured == largeSettings &&
            File.ReadAllBytes(settingsPath).SequenceEqual(largeBytes),
            "project settings larger than 64 KiB are captured and published completely");

        bool exactMaximumAccepted = false;
        try
        {
            string maximum = ProjectSettingsSerialization.Capture(buffer =>
            {
                buffer.AsSpan(
                    0,
                    ProjectSettingsSerialization.MaximumUtf8Bytes).Fill((byte)'x');
                buffer[ProjectSettingsSerialization.MaximumUtf8Bytes] = 0;
                return ProjectSettingsSerialization.MaximumUtf8Bytes;
            });
            exactMaximumAccepted =
                maximum.Length == ProjectSettingsSerialization.MaximumUtf8Bytes &&
                maximum[0] == 'x' &&
                maximum[^1] == 'x';
        }
        catch
        {
            exactMaximumAccepted = false;
        }
        check(
            exactMaximumAccepted,
            "serializer capture accepts an exactly 1 MiB terminated UTF-8 payload");

        byte[] persisted = File.ReadAllBytes(settingsPath);
        bool RejectedWithoutWrite(Func<byte[], int> serializer)
        {
            try
            {
                string text = ProjectSettingsSerialization.Capture(serializer);
                ProjectManager.SaveProjectSettings(project, text);
                return false;
            }
            catch (InvalidDataException)
            {
                return File.ReadAllBytes(settingsPath).SequenceEqual(persisted);
            }
        }

        check(
            RejectedWithoutWrite(buffer => buffer.Length),
            "out-of-buffer serializer byte counts are rejected without publishing");
        check(
            RejectedWithoutWrite(buffer =>
            {
                // Native SerializeText returns cap - 1 after truncation. The probe byte
                // makes that value one byte larger than the legal persistence payload.
                buffer[^1] = 0;
                return buffer.Length - 1;
            }),
            "truncated serializer output is rejected without publishing");
        check(
            RejectedWithoutWrite(buffer =>
            {
                byte[] payload = Encoding.UTF8.GetBytes(
                    "[Game]\nDefaultScene=Assets/main.acscene\n");
                payload.CopyTo(buffer, 0);
                buffer[payload.Length] = (byte)'!';
                return payload.Length;
            }),
            "unterminated serializer output is rejected without publishing");
        check(
            RejectedWithoutWrite(buffer =>
            {
                buffer[0] = 0xc3;
                buffer[1] = 0;
                return 1;
            }),
            "invalid UTF-8 serializer output is rejected without publishing");
        check(
            RejectedWithoutWrite(buffer =>
            {
                buffer[0] = (byte)'x';
                buffer[1] = 0;
                buffer[2] = 0;
                return 2;
            }),
            "embedded-NUL serializer output is rejected without publishing");
    }

    private static string ReadIniValue(
        string path,
        string wantedSection,
        string wantedKey)
    {
        string section = "";
        foreach (string raw in File.ReadLines(path))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith(';') || line.StartsWith('#'))
                continue;
            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                section = line[1..^1].Trim();
                continue;
            }
            if (!string.Equals(section, wantedSection, StringComparison.OrdinalIgnoreCase))
                continue;
            int equals = line.IndexOf('=');
            if (equals <= 0 ||
                !string.Equals(
                    line[..equals].Trim(),
                    wantedKey,
                    StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            return line[(equals + 1)..].Trim();
        }
        return "";
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
