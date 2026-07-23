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

        string? sourceRoot = FindManagedSourceRoot();
        Check(sourceRoot != null, "managed editor source root was located");
        if (sourceRoot == null)
        {
            output.WriteLine(
                $"Scene editor migration self-test: passed={passed} failed={failed}");
            return failed;
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
        string switchBody = ExtractMethodBody(sceneModeSource, "SwitchSceneViewMode(");
        string initializeBody =
            ExtractMethodBody(sceneModeSource, "InitializeProjectSceneDocument(");
        string establishEmptyBody =
            ExtractMethodBody(sceneModeSource, "EstablishEmptySceneDocument(");
        string restoreOpenBody =
            ExtractMethodBody(sceneModeSource, "RestoreSceneOpenRollbackSnapshot(");
        string openBody = ExtractMethodBody(shellSource, "OnOpenScene(");

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
            restoreOpenBody + "\n" + openBody;
        bool adapterWritesAreConfined =
            CountMatches(auditedManagedCs, viewAssignment) ==
                CountMatches(allowedAdapterBodies, viewAssignment) &&
            // The one additional source-mode assignment is its default field initializer.
            CountMatches(auditedManagedCs, sourceAssignment) ==
                CountMatches(allowedAdapterBodies, sourceAssignment) + 1 &&
            CountMatches(auditedManagedCs, nativeAdapterSelection) ==
                CountMatches(allowedAdapterBodies, nativeAdapterSelection) &&
            CountMatches(initializeBody, viewAssignment) > 0 &&
            CountMatches(openBody, viewAssignment) > 0 &&
            CountMatches(establishEmptyBody, nativeAdapterSelection) == 1;
        Check(
            adapterWritesAreConfined,
            "source adapter selection is confined to project initialization and explicit Open");

        int parseResult = openBody.IndexOf(
            "if (ok != 0)",
            StringComparison.Ordinal);
        string beforeSuccessfulParse =
            parseResult > 0 ? openBody[..parseResult] : openBody;
        int completeLoad = openBody.IndexOf(
            "CompleteSceneLoad(sceneLoad, publishScene)",
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
                "EngineInterop.acs_editor_scene3d_load_text(",
                StringComparison.Ordinal) &&
            openBody.Contains(
                "EngineInterop.acs_editor_scene_load_text(",
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

        bool startupLoadIsGated =
            initializeBody.Contains("BeginSceneLoad(", StringComparison.Ordinal) &&
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

        string viewportSource = File.ReadAllText(
            Path.Combine(sourceRoot, "EngineViewport.cs"));
        int createCall = viewportSource.IndexOf(
            "EngineInterop.acs_editor_create()",
            StringComparison.Ordinal);
        int suppressCall = viewportSource.IndexOf(
            "acs_editor_set_scene_presentation_suppressed(_engine, 1)",
            StringComparison.Ordinal);
        Check(
            createCall >= 0 && suppressCall > createCall,
            "viewport suppresses native presentation before its render pump starts");

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
                "ACS_EDITOR_API void acs_editor_render(");
        Check(
            nativeCreateBody.Contains("ClearScene(*host)", StringComparison.Ordinal) &&
            !nativeCreateBody.Contains("InitDemoScene", StringComparison.Ordinal),
            "production native editor hosts start with an explicit blank scene");
        Check(
            nativeRenderBody.Contains(
                "scene_presentation_suppressed",
                StringComparison.Ordinal) &&
            nativeRenderBody.Contains(
                "loadingClear",
                StringComparison.Ordinal),
            "native loading gate presents a neutral frame without drawing scene content");

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

        string removedHydrationHelper =
            "InitializeLegacy" + "SourceAdapterIfNeeded";
        Check(
            !auditedManagedCs.Contains(
                removedHydrationHelper,
                StringComparison.Ordinal),
            "dormant compatibility-payload hydration path is absent");

        output.WriteLine(
            $"Scene editor migration self-test: passed={passed} failed={failed}");
        return failed;
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
