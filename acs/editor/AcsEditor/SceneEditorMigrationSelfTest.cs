// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace AcsEditor;

/// <summary>
/// Source-level regression audit for the single-scene editor migration. This intentionally runs
/// only through an explicit CLI flag because it validates the checked-out managed UI sources.
/// </summary>
internal static class SceneEditorMigrationSelfTest
{
    private static readonly Regex[] StaleUserWording =
    {
        new(@"\b(?:2D|3D)\s+Scene\b",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant),
        new(@"\bScene\s+(?:2D|3D)\b",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant),
        new(@"(?:2D|3D)\s*シーン",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant),
        new(@"\bactive\s+(?:2D/3D|3D/2D|document)\s+mode\b",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant),
        new(@"\b(?:2D/3D|3D/2D)\s+(?:scene\s+)?mode\b",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant),
        new(@"\bSave\s+All\s+Scenes\b",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant),
        new(@"\bScene\s+mode\b",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant),
    };

    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label, string? detail = null)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS  " + label);
                return;
            }

            failed++;
            output.WriteLine("FAIL  " + label);
            if (!string.IsNullOrWhiteSpace(detail))
                output.WriteLine("      " + detail);
        }

        int contractFailures =
            SceneContractFixtureSelfTest.Run(output);

        string? sourceRoot = FindManagedSourceRoot();
        Check(sourceRoot != null, "managed editor source root was located");
        if (sourceRoot == null)
        {
            output.WriteLine(
                $"Scene editor migration self-test: passed={passed} " +
                $"failed={failed + contractFailures}");
            return failed + contractFailures;
        }

        string[] sourceFiles = Directory
            .EnumerateFiles(sourceRoot, "*", SearchOption.AllDirectories)
            .Where(IsAuditedSourceFile)
            .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        var staleFindings = new List<string>();
        foreach (string path in sourceFiles)
        {
            string text = File.ReadAllText(path);
            foreach (Regex pattern in StaleUserWording)
            {
                foreach (Match match in pattern.Matches(text))
                {
                    int line = 1;
                    for (int index = 0; index < match.Index; index++)
                    {
                        if (text[index] == '\n')
                            line++;
                    }
                    staleFindings.Add(
                        $"{Path.GetRelativePath(sourceRoot, path)}:{line}");
                }
            }
        }
        Check(
            staleFindings.Count == 0,
            "managed UI contains no separate-scene migration wording",
            string.Join(", ", staleFindings.Distinct(StringComparer.OrdinalIgnoreCase)));

        string sceneModePath = Path.Combine(sourceRoot, "MainWindow.SceneMode.cs");
        string shellPath = Path.Combine(sourceRoot, "MainWindow.xaml.cs");
        string sceneModeSource = File.ReadAllText(sceneModePath);
        string shellSource = File.ReadAllText(shellPath);
        string documentsSource = File.ReadAllText(
            Path.Combine(sourceRoot, "MainWindow.Documents.cs"));
        string autosaveSource = File.ReadAllText(
            Path.Combine(sourceRoot, "MainWindow.Autosave.cs"));
        string view3DSource = File.ReadAllText(
            Path.Combine(sourceRoot, "MainWindow.View3D.cs"));
        string cameraViewsSource = File.ReadAllText(
            Path.Combine(sourceRoot, "MainWindow.CameraViews.cs"));
        string switchBody = ExtractMethodBody(sceneModeSource, "SwitchSceneViewMode(");
        string initializeBody =
            ExtractMethodBody(sceneModeSource, "InitializeProjectSceneDocument(");
        string beginLoadBody =
            ExtractMethodBody(sceneModeSource, "private ActiveSceneLoad BeginSceneLoad(");
        string establishEmptyBody =
            ExtractMethodBody(sceneModeSource, "EstablishEmptySceneDocument(");
        string configureAdapterBody =
            ExtractMethodBody(sceneModeSource, "ConfigureSceneDocumentAdapter(");
        string legacySourceLoadBody =
            ExtractMethodBody(sceneModeSource, "LoadLegacySceneSourceAsDocument(");
        string restoreOpenBody =
            ExtractMethodBody(sceneModeSource, "RestoreSceneOpenRollbackSnapshot(");
        string completeLoadBody =
            ExtractMethodBody(sceneModeSource, "private void CompleteSceneLoad(");
        string openCommandBody =
            ExtractMethodBody(
                shellSource,
                "private async void OnOpenScene(");
        string openBody =
            ExtractMethodBody(
                shellSource,
                "private async System.Threading.Tasks.Task OpenScenePathAsync(");
        string assetActivatedBody =
            ExtractMethodBody(
                shellSource,
                "private async void OnAssetActivated(");
        string onLoadedBody =
            ExtractMethodBody(shellSource, "private void OnLoaded(");
        string onEngineAttachedBody =
            ExtractMethodBody(shellSource, "private void OnEngineAttached(");
        string beginRendererWarmupBody =
            ExtractMethodBody(shellSource, "private void BeginRendererWarmup(");
        string completeEngineStartupBody =
            ExtractMethodBody(shellSource, "private void CompleteEngineStartup(");
        string runEngineStartupCompletionBody =
            ExtractMethodBody(
                shellSource,
                "private void RunEngineStartupCompletionStage(");
        string failEngineStartupBody =
            ExtractMethodBody(shellSource, "private void FailEngineStartup(");
        string retryEngineAttachmentBody =
            ExtractMethodBody(shellSource, "internal bool RetryEngineAttachment(");
        string setGameViewBody =
            ExtractMethodBody(shellSource, "private void SetGameView(");
        string newSceneBody =
            ExtractMethodBody(shellSource, "private async void OnNewScene(");
        string refreshPrefabInstance3DBody =
            ExtractMethodBody(
                shellSource,
                "private int RefreshPrefabInstance3D(");
        string selectivePrefabRootRevertBody =
            ExtractMethodBody(
                shellSource,
                "private void RevertPrefabRootOverride3D(");
        string selectivePrefabRootApplyBody =
            ExtractMethodBody(
                shellSource,
                "private void ApplyPrefabRootOverride3D(");
        string placeBlueprintBody =
            ExtractMethodBody(shellSource, "private void PlaceBlueprint(");
        string instantiatePrefabBody =
            ExtractMethodBody(shellSource, "private void InstantiatePrefab(");
        string restoreDocumentBody =
            ExtractMethodBody(
                documentsSource,
                "RestoreCanonicalSceneDocumentState(");
        string applyRecoveryBody =
            ExtractMethodBody(
                autosaveSource,
                "ApplyRecoveryCandidateAsync(");
        string composeRecoveryBody =
            ExtractMethodBody(
                autosaveSource,
                "ComposeSceneRecoveryState(");
        string openCameraViewBody =
            ExtractMethodBody(
                cameraViewsSource,
                "private void OpenCameraView(");
        string createInitialCameraViewLeaseBody =
            ExtractMethodBody(
                cameraViewsSource,
                "private bool TryCreateInitialCameraViewLease(");
        string cameraLiveSurfaceAttachedBody =
            ExtractMethodBody(
                cameraViewsSource,
                "private void OnCameraLiveSurfaceAttached(");

        int publishedCompletionCalls = 0;
        int unavailableCompletionCalls = 0;
        SceneLoadPublicationBranch.Run(
            publishScene: true,
            () => publishedCompletionCalls++,
            () => unavailableCompletionCalls++);
        bool publishedCompletionWasExclusive =
            publishedCompletionCalls == 1 &&
            unavailableCompletionCalls == 0;
        SceneLoadPublicationBranch.Run(
            publishScene: false,
            () => publishedCompletionCalls++,
            () => unavailableCompletionCalls++);
        Check(
            publishedCompletionWasExclusive &&
            publishedCompletionCalls == 1 &&
            unavailableCompletionCalls == 1,
            "scene load completion executes exactly one published or unavailable presentation branch");

        const string viewAssignment = @"_view3d\s*=(?!=)";
        const string sourceAssignment =
            @"_legacySceneSourceMode\s*=(?!=)";
        const string nativeAdapterSelection =
            @"EngineInterop\.acs_editor_set_view3d\s*\(";
        const string payloadLoad =
            @"acs_editor_scene(?:3d)?_load_text\s*\(";

        string auditedManagedCs = string.Join(
            "\n",
            sourceFiles
                .Where(path =>
                    string.Equals(
                        Path.GetExtension(path),
                        ".cs",
                        StringComparison.OrdinalIgnoreCase) &&
                    !string.Equals(
                        Path.GetFileName(path),
                        nameof(SceneEditorMigrationSelfTest) + ".cs",
                        StringComparison.OrdinalIgnoreCase))
                .Select(File.ReadAllText));

        string allowedAdapterBodies =
            initializeBody + "\n" + establishEmptyBody + "\n" +
            configureAdapterBody + "\n" + restoreOpenBody + "\n" + openBody;
        bool adapterWritesAreConfined =
            CountMatches(auditedManagedCs, viewAssignment) ==
                // One additional write is the unpublished 3D-first bootstrap initializer.
                CountMatches(allowedAdapterBodies, viewAssignment) + 1 &&
            // The one additional source-mode assignment is its default field initializer.
            CountMatches(auditedManagedCs, sourceAssignment) ==
                CountMatches(allowedAdapterBodies, sourceAssignment) + 1 &&
            CountMatches(auditedManagedCs, nativeAdapterSelection) ==
                CountMatches(allowedAdapterBodies, nativeAdapterSelection) &&
            CountMatches(initializeBody, viewAssignment) > 0 &&
            CountMatches(openBody, viewAssignment) > 0 &&
            CountMatches(configureAdapterBody, nativeAdapterSelection) == 1;
        Check(
            adapterWritesAreConfined,
            "source adapter selection is confined to project initialization and explicit Open");
        Check(
            sceneModeSource.Contains(
                "_sceneViewMode = EditorSceneViewMode.Perspective;",
                StringComparison.Ordinal) &&
            sceneModeSource.Contains(
                "_legacySceneSourceMode = SceneDocumentMode.ThreeD;",
                StringComparison.Ordinal) &&
            view3DSource.Contains(
                "private bool _view3d = true;",
                StringComparison.Ordinal),
            "unpublished editor bootstrap state is one 3D world in Perspective view");
        Check(
            initializeBody.Contains(
                "EditorSceneStartupPolicy.Resolve(",
                StringComparison.Ordinal) &&
            initializeBody.Contains(
                "bool initialIs3D = startupPlan.Uses3D;",
                StringComparison.Ordinal) &&
            !initializeBody.Contains(
                "scenePath != null && Is3DScenePath(scenePath)",
                StringComparison.Ordinal),
            "missing startup source follows the explicit blank ACS3D plan instead of the legacy 2D adapter");

        int parseResult = openBody.IndexOf(
            "if (ok != 0)",
            StringComparison.Ordinal);
        string beforeSuccessfulParse =
            parseResult > 0 ? openBody[..parseResult] : openBody;
        int completeLoad = openBody.IndexOf(
            "CompleteSceneLoad(",
            StringComparison.Ordinal);
        int deferredRecovery = openBody.LastIndexOf(
            "await ApplyRecoveryCandidateAsync(recoveryToApply)",
            StringComparison.Ordinal);
        bool manualOpenIsTransactional =
            parseResult > 0 &&
            openBody.Contains(
                "SceneSourceFile.ReadBoundedTextAsync(",
                StringComparison.Ordinal) &&
            openBody.Contains(
                "CaptureSceneOpenRollbackSnapshot()",
                StringComparison.Ordinal) &&
            openBody.Contains(
                "RestoreSceneOpenRollbackSnapshot(rollback)",
                StringComparison.Ordinal) &&
            sceneModeSource.Contains(
                "EngineInterop.SceneText(Engine)",
                StringComparison.Ordinal) &&
            sceneModeSource.Contains(
                "EngineInterop.Scene3DText(Engine)",
                StringComparison.Ordinal) &&
            sceneModeSource.Contains(
                "document?.CaptureCheckpoint()",
                StringComparison.Ordinal) &&
            openBody.Contains(
                "LoadLegacySceneSourceAsDocument(",
                StringComparison.Ordinal) &&
            legacySourceLoadBody.Contains(
                "EngineInterop.acs_editor_scene_document_load_text(",
                StringComparison.Ordinal) &&
            restoreOpenBody.Contains(
                "EngineInterop.acs_editor_scene_document_load_text(",
                StringComparison.Ordinal) &&
            establishEmptyBody.Contains(
                "EngineInterop.acs_editor_scene_document_new(",
                StringComparison.Ordinal) &&
            CountMatches(beforeSuccessfulParse, viewAssignment) == 0 &&
            CountMatches(beforeSuccessfulParse, sourceAssignment) == 0 &&
            !beforeSuccessfulParse.Contains(
                "SetCurrentScenePath(",
                StringComparison.Ordinal) &&
            CountMatches(openBody, @"SetCurrentScenePath\s*\(\s*selectedPath\s*\)") == 1 &&
            CountMatches(openBody, @"MarkSceneClean\s*\(") == 1 &&
            completeLoad > 0 &&
            deferredRecovery > completeLoad &&
            openBody.Contains(
                "The current scene was restored unchanged.",
                StringComparison.Ordinal);
        Check(
            manualOpenIsTransactional,
            "manual Open rolls back both native graphs, managed metadata and history atomically");
        Check(
            openBody.Contains(
                "CameraViewScenePublicationPolicy.ShouldRefresh(",
                StringComparison.Ordinal) &&
            CountMatches(
                openBody,
                @"NotifyCameraViewSceneChanged\s*\(") == 1 &&
            CountMatches(
                newSceneBody,
                @"NotifyCameraViewSceneChanged\s*\(") == 1 &&
            CountMatches(
                restoreDocumentBody,
                @"NotifyCameraViewSceneChanged\s*\(") == 2 &&
            restoreDocumentBody.Contains(
                "if (rollbackLoaded != 0)",
                StringComparison.Ordinal),
            "Camera View re-resolves exactly once after each published full-document replacement or successful rollback");
        bool fullDocumentRetirementIsCombined =
            CountMatches(
                auditedManagedCs,
                @"EngineInterop\.acs_editor_scene(?:3d)?_(?:new|load_text)\s*\(") == 0 &&
            newSceneBody.Contains(
                "EngineInterop.acs_editor_scene_document_new(",
                StringComparison.Ordinal) &&
            establishEmptyBody.Contains(
                "EngineInterop.acs_editor_scene_document_new(",
                StringComparison.Ordinal) &&
            restoreDocumentBody.Contains(
                "EngineInterop.acs_editor_scene_document_load_text(",
                StringComparison.Ordinal) &&
            restoreOpenBody.Contains(
                "EngineInterop.acs_editor_scene_document_load_text(",
                StringComparison.Ordinal) &&
            legacySourceLoadBody.Contains(
                "EngineInterop.acs_editor_scene_document_load_text(",
                StringComparison.Ordinal) &&
            applyRecoveryBody.Contains(
                "recoveryDocument.ApplyRecoveredState(recoveredState)",
                StringComparison.Ordinal) &&
            applyRecoveryBody.Contains(
                "ComposeSceneRecoveryState(",
                StringComparison.Ordinal) &&
            composeRecoveryBody.Contains(
                "SceneWorldDocumentEnvelope.Pack(",
                StringComparison.Ordinal) &&
            !applyRecoveryBody.Contains(
                "LoadLegacySceneSourceAsDocument(",
                StringComparison.Ordinal);
        Check(
            fullDocumentRetirementIsCombined,
            "new, open, rollback, recovery and document history use atomic full-document retirement");
        Check(
            refreshPrefabInstance3DBody.Contains(
                "return EngineInterop.acs_editor_prefab_instance3d_refresh(",
                StringComparison.Ordinal) &&
            refreshPrefabInstance3DBody.Contains(
                "acs_editor_prefab_instance3d_refresh_with_root_overrides(",
                StringComparison.Ordinal) &&
            refreshPrefabInstance3DBody.Contains(
                "acs_editor_prefab_instance3d_root_override_mask(",
                StringComparison.Ordinal) &&
            !refreshPrefabInstance3DBody.Contains(
                "acs_editor_delete_node3d(",
                StringComparison.Ordinal) &&
            !refreshPrefabInstance3DBody.Contains(
                "acs_editor_paste_subtree3d(",
                StringComparison.Ordinal) &&
            !shellSource.Contains(
                "ReinstantiateInstance3D(",
                StringComparison.Ordinal),
            "3D Prefab refresh routes through one native rollback and Undo transaction");
        Check(
            selectivePrefabRootRevertBody.Contains(
                "acs_editor_prefab_instance3d_revert_root_overrides(",
                StringComparison.Ordinal) &&
            selectivePrefabRootRevertBody.Contains(
                "RefreshAfterSceneChange()",
                StringComparison.Ordinal) &&
            view3DSource.Contains(
                "CreatePrefabRootOverrideRevertButton3D(",
                StringComparison.Ordinal),
            "3D Prefab root overrides expose selective Revert through one native transaction");
        Check(
            selectivePrefabRootApplyBody.Contains(
                "PrefabRootPropertyApply3D.TryBuildSource(",
                StringComparison.Ordinal) &&
            selectivePrefabRootApplyBody.Contains(
                "SceneSourceFile.WriteAtomicText(",
                StringComparison.Ordinal) &&
            selectivePrefabRootApplyBody.Contains(
                "acs_editor_prefab_instance3d_clear_root_overrides(",
                StringComparison.Ordinal) &&
            selectivePrefabRootApplyBody.Contains(
                "preserveRootOverrides: true",
                StringComparison.Ordinal) &&
            selectivePrefabRootApplyBody.Contains(
                "acs_editor_select3d(Engine, id)",
                StringComparison.Ordinal) &&
            view3DSource.Contains(
                "CreatePrefabRootOverrideApplyButton3D(",
                StringComparison.Ordinal),
            "3D Prefab root overrides expose selective Apply through pure calculation and explicit adapters");
        Check(
            placeBlueprintBody.Contains(
                "EngineInterop.acs_editor_prefab_instance3d_instantiate(",
                StringComparison.Ordinal) &&
            instantiatePrefabBody.Contains(
                "EngineInterop.acs_editor_prefab_instance3d_instantiate(",
                StringComparison.Ordinal) &&
            placeBlueprintBody.Contains(
                "NewPrefabInstanceId3D()",
                StringComparison.Ordinal) &&
            instantiatePrefabBody.Contains(
                "NewPrefabInstanceId3D()",
                StringComparison.Ordinal) &&
            shellSource.Contains(
                "PFAB(?:3D)?|PINS3D|POVR3D",
                StringComparison.Ordinal),
            "3D Prefab placement supplies explicit identity through one native transaction and strips template self-links and overrides");
        Check(
            CountMatches(
                applyRecoveryBody,
                @"SetSceneDirty\s*\(\s*true\s*\)") == 1 &&
            !applyRecoveryBody.Contains(
                "MarkSceneDirty()",
                StringComparison.Ordinal),
            "successful recovery remains source-dirty without queuing a phantom hosted-document edit");
        Check(
            openCommandBody.Contains(
                "await OpenScenePathAsync(dlg.FileName)",
                StringComparison.Ordinal) &&
            assetActivatedBody.Contains(
                "await OpenScenePathAsync(e.FullPath)",
                StringComparison.Ordinal) &&
            openCommandBody.Contains(
                "catch (Exception error)",
                StringComparison.Ordinal) &&
            assetActivatedBody.Contains(
                "catch (Exception error)",
                StringComparison.Ordinal),
            "File/Open and Asset View scene activation share one transactional loader and contain async-void faults");

        bool startupLoadIsGated =
            initializeBody.Contains("BeginSceneLoad(", StringComparison.Ordinal) &&
            beginLoadBody.Contains(
                "SceneModeText.Text = \"VIEW: LOADING\"",
                StringComparison.Ordinal) &&
            initializeBody.Contains(
                "SceneSourceFile.ReadBoundedTextAsync(",
                StringComparison.Ordinal) &&
            initializeBody.Contains(
                "EstablishEmptySceneDocument(",
                StringComparison.Ordinal) &&
            initializeBody.Contains(
                "the startup viewport remains blank",
                StringComparison.Ordinal);
        Check(
            startupLoadIsGated,
            "startup scene load is bounded, generation-gated, and fails blank");
        bool existingStartupSourceUsesOneRetirement =
            Regex.IsMatch(
                initializeBody,
                @"if\s*\(exists\)\s*\{(?:(?!EstablishEmptySceneDocument).)*" +
                @"ConfigureSceneDocumentAdapter\s*\(",
                RegexOptions.Singleline | RegexOptions.CultureInvariant) &&
            initializeBody.Contains(
                "loaded = LoadLegacySceneSourceAsDocument(",
                StringComparison.Ordinal) &&
            Regex.IsMatch(
                initializeBody,
                @"else\s*\{\s*EstablishEmptySceneDocument\s*\(",
                RegexOptions.Singleline | RegexOptions.CultureInvariant);
        Check(
            existingStartupSourceUsesOneRetirement,
            "existing startup source loads directly once; only missing or rejected sources create a blank transaction");

        string appSource = File.ReadAllText(Path.Combine(sourceRoot, "App.xaml.cs"));
        bool directProjectOpenIsAsync =
            appSource.Contains(
                "protected override async void OnStartup",
                StringComparison.Ordinal) &&
            Regex.IsMatch(
                appSource,
                @"Task\.Run\s*\(\s*\(\)\s*=>\s*ProjectManager\.Open\(cliProject\)\s*\)");
        Check(
            directProjectOpenIsAsync,
            "direct .acsproject startup reconciles project I/O off the WPF dispatcher");

        string assetBrowserSource = File.ReadAllText(
            Path.Combine(sourceRoot, "AssetBrowserPanel.xaml.cs"));
        string setProjectBody =
            ExtractMethodBody(assetBrowserSource, "public void SetProject(");
        string initializeAssetsBody =
            ExtractMethodBody(
                assetBrowserSource,
                "private async Task InitializeProjectAsync(");
        string assetDropBody =
            ExtractMethodBody(
                assetBrowserSource,
                "private bool TryGetAssetBrowserDrop(");
        Check(
            !setProjectBody.Contains(
                "Directory.CreateDirectory(",
                StringComparison.Ordinal) &&
            !setProjectBody.Contains(
                "new AssetBrowserSourcesStore(",
                StringComparison.Ordinal) &&
            initializeAssetsBody.Contains(
                "await RunAssetOperationAsync(",
                StringComparison.Ordinal) &&
            initializeAssetsBody.Contains(
                "Directory.CreateDirectory(project.AssetsDir)",
                StringComparison.Ordinal) &&
            initializeAssetsBody.Contains(
                "new AssetBrowserSourcesStore(",
                StringComparison.Ordinal) &&
            !assetDropBody.Contains(
                "Directory.Exists(",
                StringComparison.Ordinal),
            "Asset View startup and drag-over keep filesystem probes off the WPF dispatcher");

        string viewportSource = File.ReadAllText(
            Path.Combine(sourceRoot, "EngineViewport.cs"));
        string buildWindowCoreBody =
            ExtractMethodBody(
                viewportSource,
                "protected override HandleRef BuildWindowCore(");
        string renderOneFrameBody =
            ExtractMethodBody(
                viewportSource,
                "private bool RenderOneFrame(");
        string viewportRetryAttachBody =
            ExtractMethodBody(
                viewportSource,
                "internal bool RetryAttach(");
        string nativeBootstrapSource = File.ReadAllText(
            Path.Combine(sourceRoot, "EditorNativeBootstrap.cs"));
        string mainWindowXaml = File.ReadAllText(
            Path.Combine(sourceRoot, "MainWindow.xaml"));
        int createCall = nativeBootstrapSource.IndexOf(
            "EngineInterop.acs_editor_create,",
            StringComparison.Ordinal);
        Match suppressMatch = Regex.Match(
            nativeBootstrapSource,
            @"acs_editor_set_scene_presentation_suppressed\s*\(\s*" +
            @"engine\s*,\s*1\s*\)",
            RegexOptions.CultureInvariant);
        int suppressCall = suppressMatch.Success ? suppressMatch.Index : -1;
        int bootstrapStart = viewportSource.IndexOf(
            "EditorNativeBootstrap.StartAsync(",
            StringComparison.Ordinal);
        int nativeHostPublish = viewportSource.IndexOf(
            "_engine = result.Engine",
            StringComparison.Ordinal);
        int pumpAfterPublish = viewportSource.IndexOf(
            "QueueRenderPump();",
            nativeHostPublish,
            StringComparison.Ordinal);
        int attachCall = viewportSource.IndexOf(
            "EngineInterop.acs_editor_attach(",
            StringComparison.Ordinal);
        int renderCall = viewportSource.IndexOf(
            "EngineInterop.TryRenderEditorFrame(",
            StringComparison.Ordinal);
        Check(
            createCall >= 0 &&
            suppressCall > createCall &&
            bootstrapStart >= 0 &&
            nativeHostPublish > bootstrapStart &&
            pumpAfterPublish > nativeHostPublish &&
            attachCall > nativeHostPublish &&
            renderCall > nativeHostPublish &&
            nativeBootstrapSource.Contains(
                "return Task.Run(",
                StringComparison.Ordinal),
            "viewport creates and suppresses an unpublished native host off-dispatcher before its render pump starts");
        int initialHide = onLoadedBody.IndexOf(
            "ViewportHost.Visibility = Visibility.Hidden",
            StringComparison.Ordinal);
        int initialHiddenRenderingAllowance = onLoadedBody.IndexOf(
            "SetHiddenStartupRenderingAllowed(true)",
            StringComparison.Ordinal);
        int childPublish = onLoadedBody.IndexOf(
            "ViewportHost.Child = _viewport",
            StringComparison.Ordinal);
        int nativePublish = completeLoadBody.IndexOf(
            "acs_editor_set_scene_presentation_suppressed(engine, 0)",
            StringComparison.Ordinal);
        int hwndPublish = completeLoadBody.IndexOf(
            "ViewportHost.Visibility = Visibility.Visible",
            StringComparison.Ordinal);
        int viewDescriptorPublish = completeLoadBody.IndexOf(
            "ApplySceneViewModePresentation()",
            StringComparison.Ordinal);
        int attachedCallback = renderOneFrameBody.IndexOf(
            "Attached?.Invoke()",
            StringComparison.Ordinal);
        int postAttachContinuationGate = renderOneFrameBody.IndexOf(
            "ShouldContinueRenderingAfterAttachCallback(",
            attachedCallback >= 0 ? attachedCallback : 0,
            StringComparison.Ordinal);
        int postAttachStop = renderOneFrameBody.IndexOf(
            "return false;",
            postAttachContinuationGate >= 0
                ? postAttachContinuationGate
                : 0,
            StringComparison.Ordinal);
        int postAttachNativeFrame = renderOneFrameBody.IndexOf(
            "EngineInterop.TryRenderEditorFrame(",
            postAttachStop >= 0 ? postAttachStop : 0,
            StringComparison.Ordinal);
        int generationBootstrapPolicy = buildWindowCoreBody.IndexOf(
            "ShouldBeginNativeBootstrapForHostGeneration(",
            StringComparison.Ordinal);
        int guardedNativeBootstrap = buildWindowCoreBody.IndexOf(
            "BeginNativeBootstrap(",
            generationBootstrapPolicy >= 0
                ? generationBootstrapPolicy
                : 0,
            StringComparison.Ordinal);
        int explicitRetryPolicy = viewportRetryAttachBody.IndexOf(
            "CanExplicitlyRetryAttach(",
            StringComparison.Ordinal);
        int explicitRetryFailureClear = viewportRetryAttachBody.IndexOf(
            "AttachFailed = false",
            StringComparison.Ordinal);
        int explicitRetrySuspensionClear = viewportRetryAttachBody.IndexOf(
            "_renderPumpSuspended = false",
            StringComparison.Ordinal);
        Check(
            Regex.IsMatch(
                mainWindowXaml,
                @"x:Name=""ViewportHost""[^>]*Visibility=""Hidden""",
                RegexOptions.CultureInvariant |
                RegexOptions.Singleline) &&
            Regex.IsMatch(
                mainWindowXaml,
                @"x:Name=""SceneModeText""[^>]*Text=""VIEW: LOADING""",
                RegexOptions.CultureInvariant |
                RegexOptions.Singleline) &&
            completeLoadBody.Contains(
                "SceneModeText.Text = \"VIEW: UNAVAILABLE\"",
                StringComparison.Ordinal) &&
            completeLoadBody.Contains(
                "SceneLoadPublicationBranch.Run(",
                StringComparison.Ordinal) &&
            initialHiddenRenderingAllowance >= 0 &&
            initialHiddenRenderingAllowance < initialHide &&
            initialHiddenRenderingAllowance < childPublish &&
            initialHide >= 0 &&
            childPublish > initialHide &&
            viewDescriptorPublish >= 0 &&
            nativePublish >= 0 &&
            viewDescriptorPublish < nativePublish &&
            hwndPublish > nativePublish &&
            !setGameViewBody.Contains(
                "ViewportHost.Visibility",
                StringComparison.Ordinal) &&
            CountMatches(
                auditedManagedCs,
                @"ViewportHost\.Visibility\s*=\s*Visibility\.Visible") == 1,
            "published scenes restore their view descriptor before the gated HwndHost is revealed");
        Check(
            onEngineAttachedBody.Contains(
                "SetHiddenStartupRenderingAllowed(false)",
                StringComparison.Ordinal) &&
            onEngineAttachedBody.Contains(
                "ContinueEngineStartupAfterProjectSettingsLoadAsync",
                StringComparison.Ordinal) &&
            onEngineAttachedBody.Contains(
                "BeginRendererWarmup(generation)",
                StringComparison.Ordinal) &&
            beginRendererWarmupBody.Contains(
                "SetHiddenStartupRenderingAllowed(true)",
                StringComparison.Ordinal) &&
            !completeEngineStartupBody.Contains(
                "SetHiddenStartupRenderingAllowed(false)",
                StringComparison.Ordinal) &&
            runEngineStartupCompletionBody.Contains(
                "_engineStartupState = EditorEngineStartupState.Ready",
                StringComparison.Ordinal) &&
            runEngineStartupCompletionBody.IndexOf(
                "_engineStartupState = EditorEngineStartupState.Ready",
                StringComparison.Ordinal) <
            runEngineStartupCompletionBody.IndexOf(
                "SetHiddenStartupRenderingAllowed(false)",
                StringComparison.Ordinal) &&
            failEngineStartupBody.Contains(
                "SetHiddenStartupRenderingAllowed(false)",
                StringComparison.Ordinal) &&
            retryEngineAttachmentBody.IndexOf(
                "SetHiddenStartupRenderingAllowed(true)",
                StringComparison.Ordinal) >= 0 &&
            retryEngineAttachmentBody.IndexOf(
                "SetHiddenStartupRenderingAllowed(true)",
                StringComparison.Ordinal) <
            retryEngineAttachmentBody.IndexOf(
                "RetryAttach()",
                StringComparison.Ordinal) &&
            viewportSource.Contains(
                "_hiddenStartupRenderingAllowed",
                StringComparison.Ordinal) &&
            !buildWindowCoreBody.Contains(
                "_hiddenStartupRenderingAllowed = false",
                StringComparison.Ordinal),
            "hidden rendering attaches safely, pauses for worker Settings load, then warm-up is bounded by ready/failure");
        Check(
            attachedCallback >= 0 &&
            postAttachContinuationGate > attachedCallback &&
            postAttachStop > postAttachContinuationGate &&
            postAttachNativeFrame > postAttachStop,
            "an Attached callback that pauses hidden rendering exits its current frame before native startup advances");
        Check(
            generationBootstrapPolicy >= 0 &&
            guardedNativeBootstrap > generationBootstrapPolicy &&
            CountMatches(
                buildWindowCoreBody,
                @"if\s*\(\s*beginNativeBootstrap\s*\)") == 2 &&
            !buildWindowCoreBody.Contains(
                "AttachFailed = false",
                StringComparison.Ordinal) &&
            !buildWindowCoreBody.Contains(
                "_renderPumpSuspended = false",
                StringComparison.Ordinal) &&
            explicitRetryPolicy >= 0 &&
            explicitRetryFailureClear > explicitRetryPolicy &&
            explicitRetrySuspensionClear > explicitRetryPolicy,
            "failed HwndHost generations preserve their latch across rebuild and only explicit retry clears it");

        string engineInteropSource = File.ReadAllText(
            Path.Combine(sourceRoot, "EngineInterop.cs"));
        string cooperativeRenderBody = ExtractMethodBody(
            engineInteropSource,
            "internal static int TryRenderEditorFrame(");
        Check(
            cooperativeRenderBody.Contains(
                "catch (EntryPointNotFoundException)",
                StringComparison.Ordinal) &&
            cooperativeRenderBody.Contains(
                "return EditorRenderFatalResult",
                StringComparison.Ordinal) &&
            !cooperativeRenderBody.Contains(
                "acs_editor_render(handle",
                StringComparison.Ordinal) &&
            viewportSource.Contains(
                "EngineInterop.TryRenderEditorFrame(",
                StringComparison.Ordinal) &&
            viewportSource.Contains(
                "RenderingFailed?.Invoke(",
                StringComparison.Ordinal) &&
            shellSource.Contains(
                "_viewport.RenderingFailed += OnEngineRenderingFailed",
                StringComparison.Ordinal),
            "missing cooperative ABI entry points fail closed, stop the viewport pump, and never fall back to blocking render");

        string nativeEditorPath = Path.GetFullPath(
            Path.Combine(
                sourceRoot,
                "..",
                "..",
                "src",
                "editor_abi",
                "EditorAbi.cpp"));
        string nativeEditorSource = File.ReadAllText(nativeEditorPath);
        string nativeCreateBody =
            ExtractMethodBody(
                nativeEditorSource,
                "ACS_EDITOR_API void* acs_editor_create(");
        string nativeRenderBody =
            ExtractMethodBody(
                nativeEditorSource,
                "static int RenderEditorFrame(");
        Check(
            nativeCreateBody.Contains("ClearScene(*host)", StringComparison.Ordinal) &&
            !nativeCreateBody.Contains("InitDemoScene", StringComparison.Ordinal),
            "production native editor hosts start with an explicit blank scene");
        Check(
            nativeRenderBody.Contains(
                "scene_presentation_suppressed",
                StringComparison.Ordinal) &&
            nativeRenderBody.Contains(
                "PresentNeutralEditorFrame(",
                StringComparison.Ordinal) &&
            nativeRenderBody.Contains(
                "const int present_result",
                StringComparison.Ordinal) &&
            nativeRenderBody.Contains(
                "if (present_result <= 0) return present_result;",
                StringComparison.Ordinal),
            "native loading gate presents a neutral frame without drawing scene content and preserves busy/fatal results");

        bool viewSwitchIsProjectionOnly =
            switchBody.Length > 0 &&
            CountMatches(switchBody, viewAssignment) == 0 &&
            CountMatches(switchBody, sourceAssignment) == 0 &&
            CountMatches(switchBody, nativeAdapterSelection) == 0 &&
            CountMatches(switchBody, payloadLoad) == 0 &&
            !switchBody.Contains(
                "SetCurrentScenePath(",
                StringComparison.Ordinal);
        Check(
            viewSwitchIsProjectionOnly,
            "view preset switching cannot select, load, or rename a source adapter");

        string viewSwitchPlanBody =
            ExtractMethodBody(
                shellSource,
                "internal static EditorViewSwitchPlan Plan(");
        bool sceneGameViewIsCameraAndPlayIndependent =
            viewSwitchPlanBody.Contains(
                "StartPlay: false",
                StringComparison.Ordinal) &&
            viewSwitchPlanBody.Contains(
                "StopPlay: false",
                StringComparison.Ordinal) &&
            viewSwitchPlanBody.Contains(
                "MutateEditorNavigationCamera: false",
                StringComparison.Ordinal) &&
            setGameViewBody.Contains(
                "EditorViewSwitchPolicy.Plan(",
                StringComparison.Ordinal) &&
            setGameViewBody.Contains(
                "EngineInterop.acs_editor_set_game_view(",
                StringComparison.Ordinal) &&
            !setGameViewBody.Contains(
                "StartPlayMode(",
                StringComparison.Ordinal) &&
            !setGameViewBody.Contains(
                "StopPlayMode(",
                StringComparison.Ordinal) &&
            !setGameViewBody.Contains(
                "acs_editor_camera3d_set(",
                StringComparison.Ordinal);
        Check(
            sceneGameViewIsCameraAndPlayIndependent,
            "Scene/Game presentation preserves editor camera state and never starts or stops Play");
        int createUnboundCameraLease = openCameraViewBody.IndexOf(
            "TryCreateInitialCameraViewLease(",
            StringComparison.Ordinal);
        int showCameraWindow = openCameraViewBody.IndexOf(
            "window.Show()",
            StringComparison.Ordinal);
        int verifyCameraSurfaceOwner =
            cameraLiveSurfaceAttachedBody.IndexOf(
                "CameraViewPresenterPublicationPolicy.CanBindPresenter(",
                StringComparison.Ordinal);
        int bindCameraPresenter = cameraLiveSurfaceAttachedBody.IndexOf(
            "TryActivateCameraViewSlot(",
            StringComparison.Ordinal);
        int enterCameraGameView = cameraLiveSurfaceAttachedBody.IndexOf(
            "SetGameView(true)",
            StringComparison.Ordinal);
        Check(
            createInitialCameraViewLeaseBody.Contains(
                "activateImmediately: false",
                StringComparison.Ordinal) &&
            createUnboundCameraLease >= 0 &&
            showCameraWindow > createUnboundCameraLease &&
            verifyCameraSurfaceOwner >= 0 &&
            bindCameraPresenter > verifyCameraSurfaceOwner &&
            enterCameraGameView > bindCameraPresenter,
            "Camera View binds its preview only after the floating window owns the shared surface");

        string removedHydrationHelper =
            "InitializeLegacy" + "SourceAdapterIfNeeded";
        Check(
            !auditedManagedCs.Contains(
                removedHydrationHelper,
                StringComparison.Ordinal),
            "dormant compatibility-payload hydration path is absent");

        int totalFailures = failed + contractFailures;
        output.WriteLine(
            $"Scene editor migration self-test: passed={passed} failed={totalFailures}");
        return totalFailures;
    }

    private static int CountMatches(string source, string pattern) =>
        Regex.Matches(
            source,
            pattern,
            RegexOptions.CultureInvariant).Count;

    private static string ExtractMethodBody(string source, string methodMarker)
    {
        int marker = source.IndexOf(methodMarker, StringComparison.Ordinal);
        if (marker < 0)
            return "";
        int open = source.IndexOf('{', marker);
        if (open < 0)
            return "";

        int depth = 0;
        for (int index = open; index < source.Length; index++)
        {
            if (source[index] == '{')
            {
                depth++;
            }
            else if (source[index] == '}' && --depth == 0)
            {
                return source.Substring(open, index - open + 1);
            }
        }
        return "";
    }

    private static bool IsAuditedSourceFile(string path)
    {
        string extension = Path.GetExtension(path);
        if (!string.Equals(extension, ".cs", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(extension, ".xaml", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        string[] segments = path.Split(
            new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
            StringSplitOptions.RemoveEmptyEntries);
        return !segments.Any(segment =>
            string.Equals(segment, "bin", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(segment, "obj", StringComparison.OrdinalIgnoreCase));
    }

    private static string? FindManagedSourceRoot()
    {
        foreach (string start in new[]
                 {
                     Directory.GetCurrentDirectory(),
                     AppContext.BaseDirectory,
                 })
        {
            var directory = new DirectoryInfo(Path.GetFullPath(start));
            while (directory != null)
            {
                string direct = Path.Combine(directory.FullName, "MainWindow.xaml");
                if (File.Exists(direct) &&
                    File.Exists(Path.Combine(
                        directory.FullName,
                        "MainWindow.SceneMode.cs")))
                {
                    return directory.FullName;
                }

                string nested = Path.Combine(
                    directory.FullName,
                    "editor",
                    "AcsEditor");
                if (File.Exists(Path.Combine(nested, "MainWindow.xaml")) &&
                    File.Exists(Path.Combine(
                        nested,
                        "MainWindow.SceneMode.cs")))
                {
                    return nested;
                }
                directory = directory.Parent;
            }
        }
        return null;
    }
}
