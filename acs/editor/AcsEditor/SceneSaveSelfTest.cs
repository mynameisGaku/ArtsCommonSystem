// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using AcsEditor.Packaging;

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

            var sceneLoads = new SceneLoadGenerationState();
            SceneLoadTicket firstSceneLoad = sceneLoads.Begin();
            SceneLoadTicket replacementSceneLoad = sceneLoads.Begin();
            Check(
                !sceneLoads.TryComplete(firstSceneLoad) &&
                sceneLoads.IsCurrent(replacementSceneLoad),
                "superseded scene load cannot publish or restore input");
            Check(
                sceneLoads.TryComplete(replacementSceneLoad) &&
                !sceneLoads.IsLoading,
                "current scene load completion closes the presentation gate");
            SceneLoadTicket closingSceneLoad = sceneLoads.Begin();
            sceneLoads.Invalidate();
            Check(
                !sceneLoads.IsCurrent(closingSceneLoad) &&
                !sceneLoads.TryComplete(closingSceneLoad),
                "editor close invalidates late scene load completion");

            var inputTransitions = new List<bool>();
            var sceneInput = new SceneEditingBlockState(
                blocked => inputTransitions.Add(blocked));
            IDisposable sceneLoadLease = sceneInput.Enter();
            int inputRestoreCalls = 0;
            bool presentationFaultObserved = false;
            try
            {
                SceneLoadCompletionGuard.Run(
                    () => throw new InvalidOperationException(
                        "injected presentation failure"),
                    sceneLoadLease,
                    () => inputRestoreCalls++);
            }
            catch (InvalidOperationException error)
            {
                presentationFaultObserved =
                    error.Message == "injected presentation failure";
            }
            Check(
                presentationFaultObserved &&
                !sceneInput.IsBlocked &&
                sceneInput.Depth == 0 &&
                inputTransitions.SequenceEqual(new[] { true, false }) &&
                inputRestoreCalls == 1,
                "scene presentation faults cannot strand editor input");

            IDisposable viewportLoadLease = sceneInput.Enter();
            bool viewportInputDuringLoad =
                SceneLoadCompletionGuard.ShouldEnableViewportInput(
                    sceneInputEnabled: !sceneInput.IsBlocked,
                    viewportPublished: true);
            viewportLoadLease.Dispose();
            bool viewportInputAfterLoad =
                SceneLoadCompletionGuard.ShouldEnableViewportInput(
                    sceneInputEnabled: !sceneInput.IsBlocked,
                    viewportPublished: true);
            Check(
                !viewportInputDuringLoad &&
                viewportInputAfterLoad &&
                !SceneLoadCompletionGuard.ShouldEnableViewportInput(
                    sceneInputEnabled: true,
                    viewportPublished: false),
                "viewport input returns only after scene publication and the final load lease");

            int cleanupFaultRestoreCalls = 0;
            bool cleanupFaultObserved = false;
            try
            {
                SceneLoadCompletionGuard.Run(
                    static () => { },
                    new ThrowingDisposable("injected cleanup failure"),
                    () => cleanupFaultRestoreCalls++);
            }
            catch (InvalidOperationException error)
            {
                cleanupFaultObserved =
                    error.Message == "injected cleanup failure";
            }
            Check(
                cleanupFaultObserved && cleanupFaultRestoreCalls == 1,
                "scene load cleanup faults still run final input recovery");

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
                        "Assets/main.acscene",
                        new RuntimeBuildCapabilities(
                            SupportsLegacyScene3D: false,
                            Evidence: "not required for 2D")).IsSupported),
                ".acscene build compatibility is independent from editor view preset");
            BuildSceneCompatibility.Result legacy3DBuild =
                BuildSceneCompatibility.Evaluate(
                    runtimeSourceIs3D: true,
                    "Assets/world.acs3d",
                    new RuntimeBuildCapabilities(
                        SupportsLegacyScene3D: false,
                        Evidence: "test runtime has no 3D route"));
            Check(
                !legacy3DBuild.IsSupported &&
                legacy3DBuild.Code == BuildSceneCompatibility.Unsupported3DCode,
                ".acs3d build remains fail-closed without a verified runtime route");
            Check(
                BuildSceneCompatibility.Evaluate(
                    runtimeSourceIs3D: true,
                    "Assets/world.acs3d",
                    new RuntimeBuildCapabilities(
                        SupportsLegacyScene3D: true,
                        Evidence: "verified test runtime")).IsSupported,
                ".acs3d build is enabled by verified project runtime capability");
            ProjectManager.NewProjectScenePlan new3DProject =
                ProjectManager.PlanNewProjectScene("3d");
            ProjectManager.NewProjectScenePlan new2DProject =
                ProjectManager.PlanNewProjectScene("2d");
            ProjectManager.NewProjectScenePlan legacyBlankAlias =
                ProjectManager.PlanNewProjectScene("blank");
            Check(
                new3DProject.InitialScene == "Assets/main.acs3d" &&
                new2DProject.InitialScene == "Assets/main.acs3d" &&
                new3DProject.SourceMode == SceneDocumentMode.ThreeD &&
                new2DProject.SourceMode == SceneDocumentMode.ThreeD &&
                new3DProject.SceneText.StartsWith(
                    "ACS3D v2\n",
                    StringComparison.Ordinal) &&
                new2DProject.SceneText.StartsWith(
                    "ACS3D v2\n",
                    StringComparison.Ordinal) &&
                !CanonicalSceneAdapter.InspectText(
                    new3DProject.SceneText,
                    ".acs3d").HasErrors &&
                !CanonicalSceneAdapter.InspectText(
                    new2DProject.SceneText,
                    ".acs3d").HasErrors &&
                !new3DProject.StartsOrthographic &&
                new2DProject.StartsOrthographic &&
                legacyBlankAlias.Template == "3d",
                "new 3D and 2D templates share main.acs3d while 2D is only an Orthographic preset");

            string runtimeRoot = Path.Combine(root, "RuntimeCapabilityProject");
            string runtimeSourceDir = Path.Combine(runtimeRoot, "Source");
            Directory.CreateDirectory(runtimeSourceDir);
            var runtimeProject = new Project
            {
                Name = "RuntimeCapabilityProject",
                ProjectFilePath = Path.Combine(
                    runtimeRoot,
                    "RuntimeCapabilityProject.acsproject"),
            };
            string gameSourcePath = Path.Combine(runtimeSourceDir, "Game.cpp");
            const string capableRuntime =
                "class FMainScene3D final : public FLegacyScene3DAdapter {};\n" +
                "auto Probe(){ return EBootstrapSceneKind::Legacy3D; }\n" +
                "auto Route(){ return FMainScene3D{}; }\n" +
                "void Load(){ LoadFile(\"main.acscene\"); }\n";
            File.WriteAllText(gameSourcePath, capableRuntime);
            Check(
                ProjectManager.DetectRuntimeBuildCapabilities(runtimeProject)
                    .SupportsLegacyScene3D,
                "runtime capability scan recognizes the concrete legacy-3D bootstrap");
            const string oldRuntimeSentinel =
                "// customized legacy runtime: preserve exactly\n" +
                "void Boot2DOnly() {}\n";
            File.WriteAllText(gameSourcePath, oldRuntimeSentinel);
            Check(
                !ProjectManager.DetectRuntimeBuildCapabilities(runtimeProject)
                    .SupportsLegacyScene3D &&
                File.ReadAllText(gameSourcePath) == oldRuntimeSentinel,
                "capability scan rejects and preserves an old customized Game.cpp");

            string scenePath = Path.Combine(assets, "main.acscene");
            SceneSourceFile.WriteProjectSceneAtomicText(
                scenePath, "first", projectRoot, assets, SceneDocumentMode.TwoD);
            SceneSourceFile.WriteProjectSceneAtomicText(
                scenePath, "second", projectRoot, assets, SceneDocumentMode.TwoD);
            Check(File.ReadAllText(scenePath) == "second",
                "atomic source write creates and replaces scene content");
            Check(!Directory.EnumerateFiles(assets, "*.tmp", SearchOption.TopDirectoryOnly).Any(),
                "atomic source write leaves no temporary file");

            SceneSourceFile.ReadResult boundedRead =
                SceneSourceFile.ReadBoundedTextAsync(scenePath)
                    .GetAwaiter()
                    .GetResult();
            Check(
                boundedRead.Exists && boundedRead.Text == "second",
                "bounded scene read returns strict UTF-8 source content");

            string invalidUtf8Path = Path.Combine(assets, "invalid-utf8.acscene");
            File.WriteAllBytes(invalidUtf8Path, [0x41, 0xC3, 0x28]);
            bool invalidUtf8Rejected = false;
            try
            {
                _ = SceneSourceFile.ReadBoundedTextAsync(invalidUtf8Path)
                    .GetAwaiter()
                    .GetResult();
            }
            catch (InvalidDataException)
            {
                invalidUtf8Rejected = true;
            }
            Check(
                invalidUtf8Rejected,
                "bounded scene read rejects malformed UTF-8");

            string oversizedScenePath = Path.Combine(assets, "oversized.acscene");
            using (var oversized = new FileStream(
                       oversizedScenePath,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None))
            {
                oversized.SetLength(SceneSourceFile.MaxSceneSourceBytes + 1L);
            }
            bool oversizedSceneRejected = false;
            try
            {
                _ = SceneSourceFile.ReadBoundedTextAsync(oversizedScenePath)
                    .GetAwaiter()
                    .GetResult();
            }
            catch (InvalidDataException)
            {
                oversizedSceneRejected = true;
            }
            Check(
                oversizedSceneRejected,
                "bounded scene read rejects oversized input before allocation");

            SceneSourceFile.ReadResult missingRead =
                SceneSourceFile.ReadBoundedTextAsync(
                        Path.Combine(assets, "missing.acscene"))
                    .GetAwaiter()
                    .GetResult();
            Check(
                !missingRead.Exists && missingRead.Text == null,
                "bounded scene read represents a missing initial source explicitly");

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
            bool ManifestBytesRejected(byte[] bytes)
            {
                File.WriteAllBytes(manifestPath, bytes);
                try
                {
                    _ = ProjectManager.ReadManifest(manifestPath);
                    return false;
                }
                catch (InvalidDataException)
                {
                    return true;
                }
                catch (JsonException)
                {
                    return true;
                }
            }
            bool ManifestBytesRejectedAsInvalidData(byte[] bytes)
            {
                File.WriteAllBytes(manifestPath, bytes);
                try
                {
                    _ = ProjectManager.ReadManifest(manifestPath);
                    return false;
                }
                catch (InvalidDataException)
                {
                    return true;
                }
                catch
                {
                    return false;
                }
            }

            Check(
                ManifestBytesRejected(new byte[(1024 * 1024) + 1]),
                "project manifest read rejects payloads above the 1 MiB bound");
            Check(
                ManifestBytesRejected(new byte[] { 0x7B, 0x22, 0xC3, 0x28, 0x7D }),
                "project manifest read rejects malformed UTF-8");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes("{\"version\":1,\0\"name\":\"bad\"}")),
                "project manifest read rejects embedded NUL bytes");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"VERSION\":1,\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read rejects case-insensitive duplicate properties");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"extension\":{\"mode\":1,\"MODE\":2}," +
                        "\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read rejects duplicate properties inside extension objects");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":2,\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read fails closed on unsupported schema versions");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"VERSION\":2,\"initialScene\":\"Assets/main.acscene\"}")),
                "case-variant schema fields cannot bypass version validation");
            Check(
                ManifestBytesRejectedAsInvalidData(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":\"one\",\"initialScene\":\"Assets/main.acscene\"}")) &&
                ManifestBytesRejectedAsInvalidData(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"name\":{},\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest field type failures use the stable invalid-data boundary");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"canonicalSceneAssetId\":\"not-an-asset-id\"," +
                        "\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read rejects malformed canonical Asset IDs");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"canonicalSceneAssetId\":" +
                        "\"00000000000000000000000000000000\"," +
                        "\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read rejects the reserved zero Asset GUID");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"name\":\"spoof\\u202Ename\"," +
                        "\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read rejects escaped bidirectional formatting characters");
            Check(
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"name\":\"line\\u2028break\"," +
                        "\"initialScene\":\"Assets/main.acscene\"}")) &&
                ManifestBytesRejected(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"name\":\"zero\\u200Bwidth\"," +
                        "\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read rejects line separators and invisible format characters");
            Check(
                ManifestBytesRejectedAsInvalidData(
                    Encoding.UTF8.GetBytes(
                        "{\"version\":1,\"name\":\"broken\\uD800\"," +
                        "\"initialScene\":\"Assets/main.acscene\"}")),
                "project manifest read rejects malformed surrogate text through the stable boundary");

            File.WriteAllText(
                manifestPath,
                "{\"name\":\"Legacy\",\"initialScene\":\"Assets/main.acscene\"}");
            Project legacyVersionProject = ProjectManager.ReadManifest(manifestPath);
            Check(
                legacyVersionProject.Version == 1,
                "legacy manifest without an explicit version migrates to schema version 1");
            string unsafeFallbackManifest = Path.Combine(
                projectRoot,
                "fallback\u202Ename.acsproject");
            File.WriteAllText(
                unsafeFallbackManifest,
                "{\"version\":1,\"name\":\"\"," +
                "\"initialScene\":\"Assets/main.acscene\"}");
            bool unsafeFallbackNameRejected = false;
            try
            {
                _ = ProjectManager.ReadManifest(unsafeFallbackManifest);
            }
            catch (InvalidDataException)
            {
                unsafeFallbackNameRejected = true;
            }
            Check(
                unsafeFallbackNameRejected,
                "unsafe manifest filenames cannot bypass project-name text validation");

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

            CheckCanonicalSceneIdentityMigration(root, Check);
            CheckInitialSceneReferenceFollow(root, Check);
            CheckInterruptedInitialSceneRecovery(root, Check);
            CheckCombinedAssetAndSceneRecovery(root, Check);
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

    private sealed class ThrowingDisposable(string message) : IDisposable
    {
        public void Dispose() => throw new InvalidOperationException(message);
    }

    private sealed record CanonicalMigrationFixture(
        Project Project,
        AssetDatabase Database,
        string ScenePath,
        string MetadataPath,
        string ManifestPath);

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

    private static void CheckCanonicalSceneIdentityMigration(
        string root,
        Action<bool, string> check)
    {
        string projectRoot = Path.Combine(root, "CanonicalSceneIdentityProject");
        string assets = Path.Combine(projectRoot, "Assets");
        Directory.CreateDirectory(assets);
        string scenePath = Path.Combine(assets, "main.acs3d");
        File.WriteAllText(scenePath, "legacy-scene");

        string manifestPath = Path.Combine(
            projectRoot,
            "CanonicalSceneIdentity.acsproject");
        string legacyManifest =
            """
            {
              "version": 1,
              "name": "CanonicalSceneIdentity",
              "engineVersion": "self-test",
              "template": "3d",
              "initialScene": "Assets/main.acs3d",
              "CanonicalSceneAssetId": "",
              "futureProperty": { "preserve": true }
            }
            """;
        File.WriteAllBytes(
            manifestPath,
            Encoding.UTF8.GetPreamble()
                .Concat(Encoding.UTF8.GetBytes(legacyManifest))
                .ToArray());

        Project project = ProjectManager.ReadManifest(manifestPath);
        AssetDatabase database = AssetDatabase.ForProject(project);
        bool migrated;
        using (AssetMutationLock.AcquireForRecovery(
                   assets,
                   "Self-test canonical scene identity migration"))
        {
            migrated = ProjectManager.BackfillCanonicalSceneAssetId(
                project,
                database);
        }

        byte[] migratedBytes = File.ReadAllBytes(manifestPath);
        Project persisted = ProjectManager.ReadManifest(manifestPath);
        JsonObject migratedJson =
            JsonNode.Parse(migratedBytes)!.AsObject();
        string canonicalProperty = migratedJson
            .Select(static property => property.Key)
            .Single(name => string.Equals(
                name,
                "canonicalSceneAssetId",
                StringComparison.OrdinalIgnoreCase));
        check(
            migrated &&
            project.CanonicalSceneAssetId.Length == 32 &&
            persisted.CanonicalSceneAssetId == project.CanonicalSceneAssetId &&
            migratedJson[canonicalProperty]?.GetValue<string>() ==
                project.CanonicalSceneAssetId &&
            File.Exists(scenePath + AssetDatabase.MetadataSuffix),
            "legacy startup scene receives one durable Asset ID before manifest publication");
        check(
            migratedJson["futureProperty"]?["preserve"]?.GetValue<bool>() == true &&
            !migratedBytes.AsSpan().StartsWith(
                new byte[] { 0xEF, 0xBB, 0xBF }),
            "canonical scene migration preserves unknown manifest data and removes legacy BOM");

        byte[] beforeRetry = File.ReadAllBytes(manifestPath);
        bool retried;
        using (AssetMutationLock.AcquireForRecovery(
                   assets,
                   "Self-test canonical scene identity retry"))
        {
            retried = ProjectManager.BackfillCanonicalSceneAssetId(
                project,
                database);
        }
        check(
            !retried &&
            File.ReadAllBytes(manifestPath).SequenceEqual(beforeRetry) &&
            !Directory.EnumerateFiles(projectRoot)
                .Any(static path => path.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)),
            "canonical scene migration is idempotent and leaves no publication temporary");

        // Model a stale Project object racing a different manifest identity. The authoritative
        // sidecar must win by rejection, never by silently rewriting either identity.
        JsonObject mismatchedJson = JsonNode.Parse(beforeRetry)!.AsObject();
        mismatchedJson[canonicalProperty] = "22222222222222222222222222222222";
        File.WriteAllText(
            manifestPath,
            mismatchedJson.ToJsonString(new JsonSerializerOptions
            {
                WriteIndented = true,
            }));
        project.CanonicalSceneAssetId = "";
        byte[] mismatchBefore = File.ReadAllBytes(manifestPath);
        bool mismatchRejected = false;
        using (AssetMutationLock.AcquireForRecovery(
                   assets,
                   "Self-test canonical scene identity mismatch"))
        {
            try
            {
                _ = ProjectManager.BackfillCanonicalSceneAssetId(
                    project,
                    database);
            }
            catch (InvalidDataException)
            {
                mismatchRejected = true;
            }
        }
        check(
            mismatchRejected &&
            File.ReadAllBytes(manifestPath).SequenceEqual(mismatchBefore),
            "canonical scene identity mismatch fails closed without mutating the manifest");

        string missingRoot = Path.Combine(root, "MissingCanonicalSceneProject");
        string missingAssets = Path.Combine(missingRoot, "Assets");
        Directory.CreateDirectory(missingAssets);
        string missingManifestPath = Path.Combine(missingRoot, "Missing.acsproject");
        File.WriteAllText(
            missingManifestPath,
            """
            {
              "version": 1,
              "name": "Missing",
              "engineVersion": "self-test",
              "template": "3d",
              "initialScene": "Assets/missing.acs3d",
              "canonicalSceneAssetId": ""
            }
            """);
        Project missingProject = ProjectManager.ReadManifest(missingManifestPath);
        AssetDatabase missingDatabase = AssetDatabase.ForProject(missingProject);
        byte[] missingBefore = File.ReadAllBytes(missingManifestPath);
        bool missingRejected = false;
        using (AssetMutationLock.AcquireForRecovery(
                   missingAssets,
                   "Self-test missing canonical scene"))
        {
            try
            {
                _ = ProjectManager.BackfillCanonicalSceneAssetId(
                    missingProject,
                    missingDatabase);
            }
            catch (InvalidDataException)
            {
                missingRejected = true;
            }
        }
        check(
            missingRejected &&
            File.ReadAllBytes(missingManifestPath).SequenceEqual(missingBefore),
            "missing startup scene cannot be assigned a guessed canonical identity");

        CheckCanonicalMigrationInputDrift(
            root,
            "SceneDrift",
            CanonicalSceneIdentityMigrationPoint.BeforeManifestPublish,
            static (scene, _) =>
                File.WriteAllText(scene, "externally-mutated-scene"),
            "canonical migration rejects initial Scene drift before manifest publication",
            check);
        CheckCanonicalMigrationInputDrift(
            root,
            "SidecarDrift",
            CanonicalSceneIdentityMigrationPoint.BeforeManifestPublish,
            static (_, metadata) =>
                File.AppendAllText(metadata, Environment.NewLine),
            "canonical migration rejects sidecar drift before manifest publication",
            check);
        CheckCanonicalMigrationInputDrift(
            root,
            "CapturedSidecarGuid",
            CanonicalSceneIdentityMigrationPoint.AfterAuthoritativeRefresh,
            static (_, metadata) =>
            {
                JsonObject sidecar =
                    JsonNode.Parse(File.ReadAllBytes(metadata))!.AsObject();
                sidecar["id"] = "33333333333333333333333333333333";
                File.WriteAllText(
                    metadata,
                    sidecar.ToJsonString(new JsonSerializerOptions
                    {
                        WriteIndented = true,
                    }));
            },
            "canonical migration derives GUID from the captured sidecar and " +
            "rejects refreshed-record drift",
            check);
        CheckCanonicalMigrationPublishRetry(root, check);
    }

    private static void CheckCanonicalMigrationInputDrift(
        string root,
        string name,
        CanonicalSceneIdentityMigrationPoint mutationPoint,
        Action<string, string> mutate,
        string label,
        Action<bool, string> check)
    {
        CanonicalMigrationFixture fixture =
            CreateCanonicalMigrationFixture(root, name);
        byte[] manifestBefore = File.ReadAllBytes(fixture.ManifestPath);
        bool rejected = false;
        using (AssetMutationLock.AcquireForRecovery(
                   fixture.Project.AssetsDir,
                   "Self-test canonical migration input drift"))
        {
            try
            {
                _ = ProjectManager.BackfillCanonicalSceneAssetId(
                    fixture.Project,
                    fixture.Database,
                    point =>
                    {
                        if (point == mutationPoint)
                        {
                            mutate(
                                fixture.ScenePath,
                                fixture.MetadataPath);
                        }
                    });
            }
            catch (Exception error) when (
                error is IOException or InvalidDataException)
            {
                rejected = true;
            }
        }

        check(
            rejected &&
            fixture.Project.CanonicalSceneAssetId.Length == 0 &&
            File.ReadAllBytes(fixture.ManifestPath)
                .SequenceEqual(manifestBefore) &&
            !HasCanonicalMigrationTemporary(fixture.Project.RootDir),
            label);
    }

    private static void CheckCanonicalMigrationPublishRetry(
        string root,
        Action<bool, string> check)
    {
        CanonicalMigrationFixture fixture =
            CreateCanonicalMigrationFixture(root, "PublishRetry");
        bool interrupted = false;
        using (AssetMutationLock.AcquireForRecovery(
                   fixture.Project.AssetsDir,
                   "Self-test canonical migration publish interruption"))
        {
            try
            {
                _ = ProjectManager.BackfillCanonicalSceneAssetId(
                    fixture.Project,
                    fixture.Database,
                    point =>
                    {
                        if (point ==
                            CanonicalSceneIdentityMigrationPoint
                                .AfterManifestPublish)
                        {
                            throw new IOException(
                                "injected post-publication interruption");
                        }
                    });
            }
            catch (IOException error)
            {
                interrupted = error.Message ==
                    "injected post-publication interruption";
            }
        }

        Project published =
            ProjectManager.ReadManifest(fixture.ManifestPath);
        byte[] publishedBytes =
            File.ReadAllBytes(fixture.ManifestPath);
        bool retried;
        using (AssetMutationLock.AcquireForRecovery(
                   fixture.Project.AssetsDir,
                   "Self-test canonical migration retry"))
        {
            retried = ProjectManager.BackfillCanonicalSceneAssetId(
                fixture.Project,
                fixture.Database);
        }

        check(
            interrupted &&
            published.CanonicalSceneAssetId.Length == 32 &&
            !retried &&
            fixture.Project.CanonicalSceneAssetId ==
                published.CanonicalSceneAssetId &&
            File.ReadAllBytes(fixture.ManifestPath)
                .SequenceEqual(publishedBytes) &&
            !HasCanonicalMigrationTemporary(fixture.Project.RootDir),
            "canonical migration retry after manifest publication is idempotent");
    }

    private static CanonicalMigrationFixture CreateCanonicalMigrationFixture(
        string root,
        string name)
    {
        string projectRoot = Path.Combine(
            root,
            "CanonicalMigration" + name);
        string assets = Path.Combine(projectRoot, "Assets");
        Directory.CreateDirectory(assets);
        string scene = Path.Combine(assets, "main.acs3d");
        File.WriteAllText(scene, "snapshot-scene-" + name);
        string manifest = Path.Combine(
            projectRoot,
            name + ".acsproject");
        File.WriteAllText(
            manifest,
            $$"""
            {
              "version": 1,
              "name": "{{name}}",
              "engineVersion": "self-test",
              "template": "3d",
              "initialScene": "Assets/main.acs3d",
              "canonicalSceneAssetId": ""
            }
            """);
        Project project = ProjectManager.ReadManifest(manifest);
        return new(
            project,
            AssetDatabase.ForProject(project),
            scene,
            scene + AssetDatabase.MetadataSuffix,
            manifest);
    }

    private static bool HasCanonicalMigrationTemporary(string projectRoot) =>
        Directory.EnumerateFiles(
                projectRoot,
                "*.scene-ref-*.tmp",
                SearchOption.TopDirectoryOnly)
            .Any();

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
        string manifestText =
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
            """;
        File.WriteAllBytes(
            manifestPath,
            Encoding.UTF8.GetPreamble()
                .Concat(Encoding.UTF8.GetBytes(manifestText))
                .ToArray());
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
            "initial-scene path follow preflight accepts a coherent BOM legacy manifest");

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
            ReadIniValue(settingsPath, "Game", "DefaultScene") == first.CurrentReference &&
            !File.ReadAllBytes(manifestPath).AsSpan().StartsWith(
                new byte[] { 0xEF, 0xBB, 0xBF }),
            "committed scene move updates BOM legacy manifest, Project state, and Game.DefaultScene together");
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

    private static void CheckInterruptedInitialSceneRecovery(
        string root,
        Action<bool, string> check)
    {
        string projectRoot = Path.Combine(root, "InterruptedReferenceFollowProject");
        string assets = Path.Combine(projectRoot, "Assets");
        string scenes = Path.Combine(assets, "Scenes");
        string config = Path.Combine(projectRoot, "Config");
        Directory.CreateDirectory(scenes);
        Directory.CreateDirectory(config);

        string manifestPath = Path.Combine(projectRoot, "Interrupted.acsproject");
        File.WriteAllText(
            manifestPath,
            """
            {
              "version": 1,
              "name": "Interrupted",
              "engineVersion": "self-test",
              "template": "blank",
              "initialScene": "Assets/main.acscene",
              "canonicalSceneAssetId": "22222222222222222222222222222222",
              "futureProperty": { "preserve": "journal-recovery" }
            }
            """);
        string settingsPath = Path.Combine(config, "ProjectSettings.ini");
        File.WriteAllText(
            settingsPath,
            """
            [Rendering]
            Exposure=1.75

            [Game]
            DefaultScene=Assets/main.acscene
            WindowWidth=1440
            """);
        string source = Path.Combine(assets, "main.acscene");
        string sourceMetadata = source + AssetDatabase.MetadataSuffix;
        File.WriteAllText(source, "journal-scene");
        File.WriteAllText(
            sourceMetadata,
            """
            {
              "schemaVersion": 1,
              "id": "22222222222222222222222222222222",
              "kind": "scene",
              "source": "Assets/main.acscene",
              "importer": "legacy-acscene",
              "importerVersion": 1,
              "dependencies": [],
              "importSettings": {}
            }
            """);

        Project project = ProjectManager.ReadManifest(manifestPath);
        string firstDestination = Path.Combine(scenes, "Recovered.acscene");
        bool prepareContentionRejected = false;
        using (AssetMutationLockProcessHolder held =
               AssetMutationLockProcessHolder.Start(assets))
        {
            try
            {
                _ = ProjectManager.PrepareInitialScenePathFollow(
                    project,
                    Guid.NewGuid(),
                    firstDestination);
            }
            catch (IOException)
            {
                prepareContentionRejected = true;
            }
        }
        check(
            prepareContentionRejected &&
            !ProjectManager.HasPendingInitialScenePathFollow(project),
            "scene move journal prepare fails before writing under cross-process contention");

        ProjectSceneReferenceMoveIntent firstIntent =
            ProjectManager.PrepareInitialScenePathFollow(
                project,
                Guid.NewGuid(),
                firstDestination);
        check(
            ProjectManager.HasPendingInitialScenePathFollow(project),
            "scene move preflight durably prepares a recovery journal before physical move");

        ProjectSceneReferenceRecoveryResult liveRecovery =
            ProjectManager.ReconcileInitialScenePathFollow(project);
        check(
            liveRecovery.Status == ProjectSceneReferenceRecoveryStatus.LiveOperation &&
            project.InitialScene == "Assets/main.acscene",
            "startup reconciliation never races a live scene move lease");

        // 生存確認ハンドルだけを解放し、永続 prepare 後かつ物理移動前のプロセス終了を再現する。
        // 移動元の identity が一致し、移動先が存在しないことを根拠として中止を確定できる。
        firstIntent.Dispose();
        ProjectSceneReferenceRecoveryResult aborted =
            ProjectManager.ReconcileInitialScenePathFollow(project);
        check(
            aborted.Status == ProjectSceneReferenceRecoveryStatus.Aborted &&
            !ProjectManager.HasPendingInitialScenePathFollow(project) &&
            File.Exists(source) &&
            File.Exists(sourceMetadata),
            "crash before physical move aborts only after source identity is proven intact");

        ProjectSceneReferenceMoveIntent secondIntent =
            ProjectManager.PrepareInitialScenePathFollow(
                project,
                Guid.NewGuid(),
                firstDestination);
        secondIntent.Dispose();
        File.Move(source, firstDestination);
        File.Move(
            sourceMetadata,
            firstDestination + AssetDatabase.MetadataSuffix);

        // 二つの参照公開の途中でプロセスが終了し、settings だけが新しく manifest は古い状態を再現する。
        // 復旧では、journal に記録されたこの二つの値だけを受理しなければならない。
        File.WriteAllText(
            settingsPath,
            File.ReadAllText(settingsPath).Replace(
                "Assets/main.acscene",
                "Assets/Scenes/Recovered.acscene",
                StringComparison.Ordinal));
        ProjectSceneReferenceRecoveryResult rolledForward =
            ProjectManager.ReconcileInitialScenePathFollow(project);
        Project recovered = ProjectManager.Open(manifestPath);
        JsonObject recoveredManifest =
            JsonNode.Parse(File.ReadAllText(manifestPath))!.AsObject();
        check(
            rolledForward.Status == ProjectSceneReferenceRecoveryStatus.RolledForward &&
            recovered.InitialScene == "Assets/Scenes/Recovered.acscene" &&
            ReadIniValue(settingsPath, "Game", "DefaultScene") ==
                recovered.InitialScene &&
            !ProjectManager.HasPendingInitialScenePathFollow(recovered),
            "crash after physical move rolls manifest and settings forward to the pinned identity");
        check(
            recoveredManifest["futureProperty"]?["preserve"]?.GetValue<string>() ==
                "journal-recovery" &&
            ReadIniValue(settingsPath, "Rendering", "Exposure") == "1.75" &&
            ReadIniValue(settingsPath, "Game", "WindowWidth") == "1440",
            "journal recovery preserves unknown manifest and unrelated INI data");

        string secondDestination = Path.Combine(scenes, "RecoveredAgain.acscene");
        ProjectSceneReferenceMoveIntent ambiguousIntent =
            ProjectManager.PrepareInitialScenePathFollow(
                recovered,
                Guid.NewGuid(),
                secondDestination);
        ambiguousIntent.Dispose();
        File.Move(firstDestination, secondDestination);
        File.Move(
            firstDestination + AssetDatabase.MetadataSuffix,
            secondDestination + AssetDatabase.MetadataSuffix);
        string duplicate = Path.Combine(scenes, "Duplicate.acscene");
        File.Copy(secondDestination, duplicate);
        File.Copy(
            secondDestination + AssetDatabase.MetadataSuffix,
            duplicate + AssetDatabase.MetadataSuffix);

        ProjectSceneReferenceRecoveryResult ambiguous =
            ProjectManager.ReconcileInitialScenePathFollow(recovered);
        bool openFailedClosed = false;
        try
        {
            _ = ProjectManager.Open(manifestPath);
        }
        catch (InvalidDataException error)
        {
            openFailedClosed = error.Message.Contains(
                "fail-closed",
                StringComparison.OrdinalIgnoreCase);
        }
        var blockedBuildLog = new List<string>();
        string? blockedBuild = BuildService.BuildAsync(
                recovered,
                blockedBuildLog.Add)
            .GetAwaiter()
            .GetResult();
        check(
            ambiguous.Status == ProjectSceneReferenceRecoveryStatus.Deferred &&
            ProjectManager.HasPendingInitialScenePathFollow(recovered) &&
            openFailedClosed &&
            blockedBuild == null &&
            blockedBuildLog.Any(line => line.Contains(
                "INITIAL_SCENE_MOVE_PENDING",
                StringComparison.Ordinal)),
            "duplicate identity is never guessed and keeps project open/build fail-closed");

        File.Delete(duplicate);
        File.Delete(duplicate + AssetDatabase.MetadataSuffix);
        ProjectSceneReferenceRecoveryResult retry =
            ProjectManager.ReconcileInitialScenePathFollow(recovered);
        Project finalProject = ProjectManager.Open(manifestPath);
        check(
            retry.Status == ProjectSceneReferenceRecoveryStatus.RolledForward &&
            finalProject.InitialScene == "Assets/Scenes/RecoveredAgain.acscene" &&
            !ProjectManager.HasPendingInitialScenePathFollow(finalProject),
            "deferred recovery remains retryable after identity ambiguity is removed");

        BuildInitialSceneSnapshot buildSnapshot =
            BuildService.CaptureInitialSceneSnapshot(finalProject);
        PackageProjectInfo packageSnapshot = PackagingService.ProjectInfo(finalProject);
        bool finalPublishStateAccepted = true;
        try
        {
            using AssetMutationLock publishLease = AssetMutationLock.AcquireFailFast(
                finalProject.AssetsDir,
                "Scene save self-test package publish");
            PackageCore.ValidateProjectSceneStateForPublish(
                packageSnapshot,
                finalProject.CanonicalSceneAssetId);
        }
        catch
        {
            finalPublishStateAccepted = false;
        }

        string publishRaceDestination =
            Path.Combine(scenes, "PublishRace.acscene");
        ProjectSceneReferenceMoveIntent publishRaceIntent =
            ProjectManager.PrepareInitialScenePathFollow(
                finalProject,
                Guid.NewGuid(),
                publishRaceDestination);
        bool finalPublishBlocked = false;
        try
        {
            PackageCore.ValidateProjectSceneStateForPublish(
                packageSnapshot,
                finalProject.CanonicalSceneAssetId);
        }
        catch (PackageValidationException error)
        {
            finalPublishBlocked = error.Issues.Any(issue =>
                issue.Code == "PROJECT_CHANGED_DURING_PACKAGE");
        }
        ProjectSceneReferenceRecoveryResult publishRaceSettlement =
            ProjectManager.SettleInitialScenePathFollow(
                finalProject,
                publishRaceIntent,
                operationSucceeded: false,
                referencesCommitted: false);
        check(
            finalPublishStateAccepted &&
            finalPublishBlocked &&
            publishRaceSettlement.Status ==
                ProjectSceneReferenceRecoveryStatus.Aborted &&
            !ProjectManager.HasPendingInitialScenePathFollow(finalProject),
            "package final publish revalidates identity and refuses a newly prepared scene move");

        string previousBuildScenePath = finalProject.InitialScenePath;
        ProjectSceneReferenceMoveIntent committedRaceIntent =
            ProjectManager.PrepareInitialScenePathFollow(
                finalProject,
                Guid.NewGuid(),
                publishRaceDestination);
        File.Move(previousBuildScenePath, publishRaceDestination);
        File.Move(
            previousBuildScenePath + AssetDatabase.MetadataSuffix,
            publishRaceDestination + AssetDatabase.MetadataSuffix);
        _ = ProjectManager.FollowInitialScenePath(
            finalProject,
            publishRaceDestination);
        ProjectSceneReferenceRecoveryResult committedRaceSettlement =
            ProjectManager.SettleInitialScenePathFollow(
                finalProject,
                committedRaceIntent,
                operationSucceeded: true,
                referencesCommitted: true);

        File.WriteAllText(previousBuildScenePath, "replacement-at-stale-build-path");
        JsonObject replacementMetadata =
            JsonNode.Parse(File.ReadAllText(
                publishRaceDestination + AssetDatabase.MetadataSuffix))!.AsObject();
        replacementMetadata["id"] = Guid.NewGuid().ToString("N");
        replacementMetadata["source"] = buildSnapshot.InitialScene;
        File.WriteAllText(
            previousBuildScenePath + AssetDatabase.MetadataSuffix,
            replacementMetadata.ToJsonString());
        string staleBuildCopy = Path.Combine(
            projectRoot,
            "BuildRaceOutput",
            "main.acscene");
        bool staleBuildCopyRejected = false;
        try
        {
            BuildService.CopyInitialSceneForBuild(
                finalProject,
                buildSnapshot,
                staleBuildCopy);
        }
        catch (InvalidDataException)
        {
            staleBuildCopyRejected = true;
        }
        check(
            committedRaceSettlement.Status ==
                ProjectSceneReferenceRecoveryStatus.AlreadyComplete &&
            staleBuildCopyRejected &&
            !File.Exists(staleBuildCopy),
            "build final publish rejects a completed scene move even when the stale path is recreated");
        File.Delete(previousBuildScenePath);
        File.Delete(previousBuildScenePath + AssetDatabase.MetadataSuffix);

        string malformedJournal = Path.Combine(
            assets,
            AssetDatabase.InternalDirectoryName,
            "scene-reference-follow.v1.json");
        File.WriteAllText(malformedJournal, "{}");
        bool malformedFailedClosed = false;
        try
        {
            _ = ProjectManager.Open(manifestPath);
        }
        catch (InvalidDataException)
        {
            malformedFailedClosed = true;
        }
        check(
            malformedFailedClosed &&
            ProjectManager.HasPendingInitialScenePathFollow(finalProject),
            "malformed recovery journals are retained and keep project open fail-closed");
        File.Delete(malformedJournal);
    }

    private static void CheckCombinedAssetAndSceneRecovery(
        string root,
        Action<bool, string> check)
    {
        string projectRoot = Path.Combine(root, "CombinedStartupRecoveryProject");
        string assets = Path.Combine(projectRoot, "Assets");
        string scenes = Path.Combine(assets, "Scenes");
        string imported = Path.Combine(assets, "Imported");
        string config = Path.Combine(projectRoot, "Config");
        string sources = Path.Combine(projectRoot, "ExternalSources");
        Directory.CreateDirectory(scenes);
        Directory.CreateDirectory(imported);
        Directory.CreateDirectory(config);
        Directory.CreateDirectory(sources);

        const string sceneId = "33333333333333333333333333333333";
        string manifestPath = Path.Combine(projectRoot, "Combined.acsproject");
        File.WriteAllText(
            manifestPath,
            $$"""
            {
              "version": 1,
              "name": "Combined",
              "engineVersion": "self-test",
              "template": "blank",
              "initialScene": "Assets/main.acscene",
              "canonicalSceneAssetId": "{{sceneId}}"
            }
            """);
        string settingsPath = Path.Combine(config, "ProjectSettings.ini");
        File.WriteAllText(
            settingsPath,
            """
            [Game]
            DefaultScene=Assets/main.acscene
            """);
        string sourceScene = Path.Combine(assets, "main.acscene");
        File.WriteAllText(sourceScene, "combined-recovery-scene");
        File.WriteAllText(
            sourceScene + AssetDatabase.MetadataSuffix,
            $$"""
            {
              "schemaVersion": 1,
              "id": "{{sceneId}}",
              "kind": "scene",
              "source": "Assets/main.acscene",
              "importer": "legacy-acscene",
              "importerVersion": 1,
              "dependencies": [],
              "importSettings": {}
            }
            """);

        Project project = ProjectManager.ReadManifest(manifestPath);
        var database = AssetDatabase.ForProject(project);
        string external = Path.Combine(sources, "interrupted.bin");
        File.WriteAllText(external, "interrupted-import");
        bool importCrashed = false;
        try
        {
            _ = AssetImportWorkflow.ImportFiles(
                database,
                imported,
                new[] { external },
                testHooks: new AssetImportTestHooks(
                    SimulateCrashAfter: AssetImportCheckpoint.AssetPublished));
        }
        catch (AssetImportSimulatedCrashException)
        {
            importCrashed = true;
        }

        string importedAsset = Path.Combine(imported, "interrupted.bin");
        string destinationScene = Path.Combine(scenes, "Recovered.acscene");
        string journalPath = Path.Combine(
            assets,
            AssetDatabase.InternalDirectoryName,
            "scene-reference-follow.v1.json");
        File.WriteAllText(
            journalPath,
            JsonSerializer.Serialize(new
            {
                schemaVersion = 1,
                operationId = Guid.NewGuid().ToString("D"),
                projectFileName = Path.GetFileName(manifestPath),
                sourceReference = "Assets/main.acscene",
                destinationReference = "Assets/Scenes/Recovered.acscene",
                assetId = sceneId,
                createdUtcTicks = DateTime.UtcNow.Ticks,
            }));
        File.Move(sourceScene, destinationScene);
        File.Move(
            sourceScene + AssetDatabase.MetadataSuffix,
            destinationScene + AssetDatabase.MetadataSuffix);

        Project recovered = ProjectManager.Open(manifestPath);
        var recoveredDatabase = AssetDatabase.ForProject(recovered);
        _ = recoveredDatabase.Refresh(verifyContent: true);
        bool importRecovered =
            recoveredDatabase.TryGetByPath(importedAsset, out AssetRecord? importedRecord) &&
            importedRecord != null &&
            File.Exists(importedAsset + AssetDatabase.MetadataSuffix);
        bool stagingEmpty = !Directory.EnumerateFileSystemEntries(
                Path.Combine(
                    assets,
                    AssetDatabase.InternalDirectoryName,
                    AssetImportWorkflow.StagingDirectoryName))
            .Any();
        check(
            importCrashed &&
            importRecovered &&
            stagingEmpty &&
            recovered.InitialScene == "Assets/Scenes/Recovered.acscene" &&
            ReadIniValue(settingsPath, "Game", "DefaultScene") ==
                recovered.InitialScene &&
            !ProjectManager.HasPendingInitialScenePathFollow(recovered),
            "project open recovers Import before an overlapping initial-scene journal");
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
