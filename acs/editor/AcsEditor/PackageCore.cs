// SPDX-License-Identifier: Apache-2.0
// Deterministic, path-safe staging used by both ACS Editor and acspackage CLI.

using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor.Packaging;

public enum PackageIssueSeverity
{
    Info,
    Warning,
    Error,
}

public sealed record PackageIssue(
    PackageIssueSeverity Severity,
    string Code,
    string Message,
    string? Path = null);

public sealed record PackageProjectInfo(
    string Name,
    int ProjectSchemaVersion,
    string EngineVersion,
    string ProjectFilePath,
    string InitialScene,
    string CanonicalSceneAssetId = "")
{
    public string RootDirectory =>
        Path.GetDirectoryName(Path.GetFullPath(ProjectFilePath)) ?? "";

    public string AssetsDirectory => Path.Combine(RootDirectory, "Assets");
    public string ConfigDirectory => Path.Combine(RootDirectory, "Config");
    public string TempDirectory => Path.Combine(RootDirectory, "Temp");
}

public enum PackageProfile
{
    Development,
    Test,
    Shipping,
}

public sealed record PackageOptions(
    string OutputDirectory,
    string ProductVersion = "0.1.0",
    bool IncludeDebugSymbols = false,
    PackageProfile Profile = PackageProfile.Shipping,
    string? AssetPackToolPath = null);

public sealed record PackageProgress(
    string Phase,
    string Message,
    int Completed,
    int Total);

public sealed record PackageResult(
    string ZipPath,
    string PackageId,
    string BuildId,
    int FileCount,
    long UncompressedBytes,
    string AssetPackSha256 = "",
    int CookedAssetCount = 0,
    PackageProfile Profile = PackageProfile.Shipping,
    bool ArchiveVerified = false);

public sealed record PackageVerificationResult(
    string ZipPath,
    string PackageId,
    string BuildId,
    int FileCount,
    long UncompressedBytes,
    string AssetPackSha256,
    int CookedAssetCount,
    PackageProfile Profile);

public sealed class PackageValidationException : InvalidOperationException
{
    public IReadOnlyList<PackageIssue> Issues { get; }

    public PackageValidationException(IReadOnlyList<PackageIssue> issues)
        : base(string.Join(
            Environment.NewLine,
            issues.Where(issue => issue.Severity == PackageIssueSeverity.Error)
                  .Select(issue => $"[{issue.Code}] {issue.Message}")))
    {
        Issues = issues;
    }
}

public static class PackageCore
{
    private const string AssetCookerVersion = "acs-package-cook-v2";
    private const string ReferenceRewriteVersion = "4";
    private const int ArchiveEntryLimit = 100_000;
    private const int ArchiveManifestLimitBytes = 4 * 1024 * 1024;
    private const long ArchiveUncompressedLimitBytes = 1L << 40;
    private static readonly UTF8Encoding Utf8NoBom = new(false);
    private static readonly UTF8Encoding Utf8Strict = new(false, true);
    private static readonly DateTimeOffset ZipEpoch =
        new(1980, 1, 1, 0, 0, 0, TimeSpan.Zero);

    private static readonly Regex SceneReference = new(
        @"^(?<prefix>(?:SPRT|MAT|PFAB)\s+-?\d+\s+)(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    private static readonly Regex Scene3DReference = new(
        @"^(?<prefix>(?<verb>MSH3D|SPR3D|MAT3D|PFAB3D)\s+-?\d+\s+)(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    private static readonly Regex MaterialReference = new(
        @"^(?<prefix>(?:albedo|normal|substrateExprTexture\d+)\s+)(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex ProductVersionPattern = new(
        @"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    private static readonly HashSet<string> CookableExtensions = new(
        [
            ".acscene", ".acsprefab", ".acsmat", ".acsbp", ".acs3d",
            ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".ktx",
            ".ktx2", ".hdr", ".exr", ".webp", ".gif",
            ".wav", ".ogg", ".mp3", ".flac",
            ".fbx", ".gltf", ".glb", ".obj", ".mdl", ".mtl",
            ".ttf", ".otf",
            ".txt", ".json", ".csv", ".xml", ".yaml", ".yml", ".toml",
            ".ini", ".md", ".log",
            ".lua", ".hlsl", ".glsl", ".vert", ".frag",
            ".cso", ".dxil", ".spv", ".bin", ".dat",
        ],
        StringComparer.OrdinalIgnoreCase);

    private sealed record ManifestFile(string path, long size, string sha256);

    private sealed record ManifestAssetPack(
        string path,
        long size,
        string sha256,
        int formatVersion,
        bool compressed,
        int sourceFileCount);

    private sealed record CookResult(
        string PackPath,
        string Sha256,
        int SourceFileCount,
        bool Compressed,
        string CanonicalSceneAssetId,
        string CanonicalSceneKind,
        string CanonicalSceneImporter,
        int CanonicalSceneImporterVersion,
        string AssetGraphHash,
        CanonicalSceneBootstrapEnvelope SceneBootstrap);

    internal sealed record PackageInputFileSnapshot(
        string RelativePath,
        long Size,
        string Sha256);

    internal sealed record PackageDirectorySnapshot(
        bool Existed,
        IReadOnlyList<PackageInputFileSnapshot> Files);

    private sealed record PackageManifest(
        int schemaVersion,
        string productName,
        string productVersion,
        int projectSchemaVersion,
        string engineVersion,
        string platform,
        string configuration,
        string profile,
        string executable,
        string buildId,
        string canonicalSceneAssetId,
        string canonicalSceneKind,
        string canonicalSceneImporter,
        int canonicalSceneImporterVersion,
        string assetGraphHash,
        CanonicalSceneBootstrapEnvelope sceneBootstrap,
        ManifestAssetPack assetPack,
        IReadOnlyList<ManifestFile> files);

    public static string SanitizeIdentifier(string name)
    {
        var builder = new StringBuilder();
        foreach (char value in name)
            builder.Append(char.IsLetterOrDigit(value) ? value : '_');
        if (builder.Length == 0 || char.IsDigit(builder[0]))
            builder.Insert(0, '_');
        return builder.ToString();
    }

    public static string PackageId(PackageProjectInfo project, PackageOptions options) =>
        SanitizeFileName(project.Name) + "-" +
        SanitizeFileName(options.ProductVersion) +
        (options.Profile == PackageProfile.Shipping
            ? ""
            : "-" + options.Profile.ToString().ToLowerInvariant()) +
        "-win64";

    public static IReadOnlyList<PackageIssue> Validate(
        PackageProjectInfo project,
        PackageOptions options,
        string executablePath,
        IReadOnlyList<string>? runtimeDependencies = null,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var issues = new List<PackageIssue>();
        string root;
        string assets;
        string config;
        string output;
        string initialScene;
        string executable;
        string staging;
        string derivedDataCache;
        var assetFiles = new List<string>();

        try
        {
            root = Path.GetFullPath(project.RootDirectory);
            assets = Path.GetFullPath(project.AssetsDirectory);
            config = Path.GetFullPath(project.ConfigDirectory);
            output = Path.GetFullPath(options.OutputDirectory);
            initialScene = ResolveUnderRoot(root, project.InitialScene);
            executable = Path.GetFullPath(executablePath);
            staging = Path.GetFullPath(
                Path.Combine(project.TempDirectory, "PackageStaging"));
            derivedDataCache = Path.GetFullPath(
                Path.Combine(project.TempDirectory, "DerivedDataCache", "Cook"));
        }
        catch (Exception error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INVALID_PATH",
                $"パスを正規化できません: {error.Message}"));
            return issues;
        }

        if (ProjectManager.HasPendingInitialScenePathFollow(assets))
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INITIAL_SCENE_MOVE_PENDING",
                "An interrupted initial-scene move still requires recovery. Packaging remains " +
                "fail-closed until the durable move intent is reconciled."));
        }
        cancellationToken.ThrowIfCancellationRequested();

        try
        {
            RejectExistingReparsePointsInPath(staging, "Package staging");
        }
        catch (Exception error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "STAGING_REPARSE",
                error.Message,
                staging));
        }

        try
        {
            RejectExistingReparsePointsInPath(output, "Package output");
        }
        catch (Exception error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "OUTPUT_REPARSE",
                error.Message,
                output));
        }

        try
        {
            RejectExistingReparsePointsInPath(
                derivedDataCache,
                "Derived Data Cache");
        }
        catch (Exception error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "DDC_REPARSE",
                error.Message,
                derivedDataCache));
        }

        if (string.IsNullOrWhiteSpace(project.Name))
            issues.Add(new(PackageIssueSeverity.Error, "PROJECT_NAME_EMPTY", "プロジェクト名が空です。"));

        if (!ProductVersionPattern.IsMatch(options.ProductVersion))
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "PRODUCT_VERSION_INVALID",
                "パッケージ版は SemVer 形式 (例: 1.0.0 / 1.0.0-beta.1) で指定してください。"));
        }

        issues.Add(new(
            PackageIssueSeverity.Info,
            "PACKAGE_PROFILE",
            $"Package profile: {options.Profile}"));
        if (options.Profile == PackageProfile.Shipping &&
            options.IncludeDebugSymbols)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "SHIPPING_SYMBOLS_FORBIDDEN",
                "Shipping ZIPへPDBを同梱できません。Test/Development profileを使うか、シンボルを別保管してください。"));
        }

        if (!File.Exists(project.ProjectFilePath))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "PROJECT_FILE_MISSING",
                "プロジェクトファイルが見つかりません。",
                project.ProjectFilePath));

        if (!Directory.Exists(root))
            issues.Add(new(PackageIssueSeverity.Error, "PROJECT_ROOT_MISSING", "プロジェクトフォルダが見つかりません。", root));

        if (!Directory.Exists(assets))
            issues.Add(new(PackageIssueSeverity.Error, "ASSETS_MISSING", "Assets フォルダが見つかりません。", assets));
        else
        {
            try
            {
                RejectExistingReparsePointsInPath(assets, "Assets");
                assetFiles = EnumerateFilesSafe(
                    assets,
                    issues,
                    excludeAssetMetadata: true,
                    cancellationToken: cancellationToken).ToList();
            }
            catch (Exception error)
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "INPUT_TREE_UNSAFE",
                    $"Assets の走査に失敗しました: {error.Message}",
                    assets));
            }

            foreach (string asset in assetFiles)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string extension = Path.GetExtension(asset);
                if (string.IsNullOrEmpty(extension) ||
                    !CookableExtensions.Contains(extension))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "ASSET_TYPE_UNSUPPORTED",
                        "Cook対象として未対応のアセット形式です。対応形式へimport/変換するかAssets外へ移動してください。",
                        asset));
                }
            }

        }

        if (Directory.Exists(config))
            ValidateTree(config, "Config", issues, cancellationToken);
        else
            issues.Add(new(PackageIssueSeverity.Info, "CONFIG_EMPTY", "Config フォルダはありません。既定設定でパッケージします。"));

        string settingsIni = Path.Combine(config, "ProjectSettings.ini");
        if (File.Exists(settingsIni) &&
            TryReadIniValue(
                settingsIni,
                "Game",
                "DefaultScene",
                out string configuredScene,
                cancellationToken) &&
            !string.IsNullOrWhiteSpace(configuredScene))
        {
            try
            {
                string configuredReference = configuredScene.Trim();
                string configuredValue = configuredReference.Replace(
                    '/',
                    Path.DirectorySeparatorChar);
                if (Path.IsPathRooted(configuredValue))
                    throw new InvalidDataException(
                        "DefaultScene must be a relative path under Assets.");
                string configuredPath = ResolveUnderRoot(root, configuredReference);
                if (!IsWithin(assets, configuredPath))
                    throw new InvalidDataException(
                        "DefaultScene must resolve inside Assets.");
                if (!HasSupportedSceneExtension(configuredPath))
                    throw new InvalidDataException(
                        "DefaultScene must use the .acscene or .acs3d extension.");
                if (!string.Equals(
                        configuredPath,
                        initialScene,
                        StringComparison.OrdinalIgnoreCase))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "DEFAULT_SCENE_MISMATCH",
                        "Config [Game] DefaultScene と .acsproject initialScene が一致しません。Editor表示と配布内容が分岐しないよう同じsceneへ揃えてください。",
                        $"{configuredScene} != {project.InitialScene}"));
                }
            }
            catch (Exception error)
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "DEFAULT_SCENE_INVALID",
                    $"Config [Game] DefaultScene が不正です: {error.Message}",
                    configuredScene));
            }
        }

        string initialSceneValue = project.InitialScene.Replace(
            '/',
            Path.DirectorySeparatorChar);
        if (Path.IsPathRooted(initialSceneValue))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INITIAL_SCENE_ABSOLUTE",
                "初期シーンは Assets 配下への相対パスで指定してください。",
                project.InitialScene));

        if (!IsWithin(assets, initialScene))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INITIAL_SCENE_ESCAPE",
                "初期シーンは Assets フォルダ内でなければなりません。",
                initialScene));
        else if (!HasSupportedSceneExtension(initialScene))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INITIAL_SCENE_EXTENSION",
                "初期シーンは .acscene または .acs3d でなければなりません。",
                initialScene));
        else if (!File.Exists(initialScene))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INITIAL_SCENE_MISSING",
                "初期シーンが見つかりません。",
                initialScene));
        else if (HasReparsePointBetween(root, initialScene))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INITIAL_SCENE_REPARSE",
                "初期シーンが reparse point を経由しています。",
                initialScene));

        if (Directory.Exists(assets) &&
            File.Exists(initialScene) &&
            IsWithin(assets, initialScene) &&
            !HasReparsePointBetween(assets, initialScene))
        {
            CanonicalSceneAdapterInspection sceneInspection =
                CanonicalSceneAdapter.InspectFile(initialScene);
            cancellationToken.ThrowIfCancellationRequested();
            issues.AddRange(
                sceneInspection.Diagnostics.Select(MapSceneAdapterDiagnostic));
            try
            {
                AssetCookPlan cookPlan = BuildCookPlan(
                    project,
                    root,
                    assets,
                    initialScene,
                    cancellationToken);
                issues.AddRange(cookPlan.Diagnostics.Select(MapCookDiagnostic));
                if (cookPlan.Root != null &&
                    !string.Equals(
                        Path.GetFullPath(cookPlan.Root.FullPath),
                        Path.GetFullPath(initialScene),
                        StringComparison.OrdinalIgnoreCase))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "CANONICAL_SCENE_PATH_MISMATCH",
                        "canonicalSceneAssetId and legacy initialScene resolve to different assets.",
                        $"{cookPlan.Root.RelativePath} != {project.InitialScene}"));
                }
                if (!cookPlan.HasErrors)
                {
                    foreach (AssetRecord nestedScene in cookPlan.Assets.Where(item =>
                                 !string.Equals(
                                     item.AssetId,
                                     cookPlan.Root?.AssetId,
                                     StringComparison.OrdinalIgnoreCase) &&
                                 HasSupportedSceneExtension(item.FullPath)))
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        issues.AddRange(
                            CanonicalSceneAdapter.InspectFile(nestedScene.FullPath)
                                .Diagnostics
                                .Select(MapSceneAdapterDiagnostic));
                    }
                    foreach (AssetRecord gltf in cookPlan.Assets.Where(static item =>
                                 string.Equals(
                                     Path.GetExtension(item.RelativePath),
                                     ".gltf",
                                     StringComparison.OrdinalIgnoreCase)))
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        issues.AddRange(
                            CanonicalSceneAdapter.ValidateStandaloneGltf(gltf.FullPath)
                                .Select(MapSceneAdapterDiagnostic));
                    }
                    issues.Add(new(
                        PackageIssueSeverity.Info,
                        "ASSET_COOK_CLOSURE",
                        $"Cook closure: {cookPlan.Assets.Count} assets, graph {cookPlan.GraphHash}.",
                        initialScene));
                }
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or InvalidDataException or
                    JsonException)
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "ASSET_COOK_PLAN_FAILED",
                    $"Asset dependency closure could not be built: {error.Message}",
                    initialScene));
            }
        }

        if (!File.Exists(executable))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "EXECUTABLE_MISSING",
                "Release 実行ファイルが見つかりません。先にビルドしてください。",
                executable));
        else if (IsReparsePoint(executable))
            issues.Add(new(
                PackageIssueSeverity.Error,
                "EXECUTABLE_REPARSE",
                "Release実行ファイルが reparse point です。",
                executable));
        else if (!string.Equals(
                     Path.GetFileName(executable),
                     SanitizeIdentifier(project.Name) + ".exe",
                     StringComparison.OrdinalIgnoreCase))
            issues.Add(new(
                PackageIssueSeverity.Warning,
                "EXECUTABLE_NAME",
                "実行ファイル名がプロジェクト名から期待される名前と異なります。",
                executable));

        if (string.IsNullOrWhiteSpace(options.AssetPackToolPath))
        {
            issues.Add(new(
                PackageIssueSeverity.Info,
                "ASSETPACK_TOOL_PENDING",
                "acs_assetpack はPackage開始時に解決またはビルドされます。"));
        }
        else
        {
            try
            {
                string tool = Path.GetFullPath(options.AssetPackToolPath);
                if (!File.Exists(tool))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "ASSETPACK_TOOL_MISSING",
                        "acs_assetpack実行ファイルが見つかりません。",
                        tool));
                }
                else if (IsReparsePoint(tool))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "ASSETPACK_TOOL_REPARSE",
                        "acs_assetpack実行ファイルがreparse pointです。",
                        tool));
                }
            }
            catch (Exception error)
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "ASSETPACK_TOOL_INVALID",
                    $"acs_assetpackのパスが不正です: {error.Message}",
                    options.AssetPackToolPath));
            }
        }

        if (options.IncludeDebugSymbols && File.Exists(executable))
        {
            string pdb = Path.ChangeExtension(executable, ".pdb");
            if (!File.Exists(pdb))
            {
                issues.Add(new(
                    PackageIssueSeverity.Warning,
                    "DEBUG_SYMBOLS_MISSING",
                    "Include game PDB が有効ですが、Release実行ファイルの隣にPDBがありません。ZIPはPDBなしで生成されます。",
                    pdb));
            }
            else if (IsReparsePoint(pdb))
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "DEBUG_SYMBOLS_REPARSE",
                    "game PDB が reparse point です。",
                    pdb));
            }
        }

        if (IsWithin(assets, output) || IsWithin(config, output) ||
            IsWithin(Path.Combine(root, "Source"), output))
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "OUTPUT_INSIDE_INPUT",
                "出力先を Assets、Config、Source の中には置けません。",
                output));
        }

        if (ContainsCMakeUnsafeCharacter(root) ||
            ContainsCMakeUnsafeCharacter(executable))
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "CMAKE_UNSAFE_PATH",
                "パスに CMake の依存解決で安全に扱えない改行またはセミコロンが含まれています。",
                root));
        }

        if (File.Exists(initialScene) && !HasReparsePointBetween(root, initialScene))
            ValidateReferenceFile(
                initialScene,
                assets,
                root,
                SceneReference,
                issues,
                cancellationToken);

        if (Directory.Exists(assets))
        {
            foreach (string material in assetFiles
                         .Where(path => string.Equals(
                             Path.GetExtension(path),
                             ".acsmat",
                             StringComparison.OrdinalIgnoreCase)))
            {
                cancellationToken.ThrowIfCancellationRequested();
                ValidateReferenceFile(
                    material,
                    assets,
                    root,
                    MaterialReference,
                    issues,
                    cancellationToken);
            }

            foreach (string gltf in assetFiles
                         .Where(path => string.Equals(
                             Path.GetExtension(path),
                             ".gltf",
                             StringComparison.OrdinalIgnoreCase)))
            {
                cancellationToken.ThrowIfCancellationRequested();
                ValidateGltfReferences(
                    gltf,
                    assets,
                    issues,
                    cancellationToken);
            }
        }

        var dependencyNames = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (string dependency in runtimeDependencies ?? Array.Empty<string>())
        {
            cancellationToken.ThrowIfCancellationRequested();
            string full;
            try { full = Path.GetFullPath(dependency); }
            catch
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "RUNTIME_PATH_INVALID",
                    "ランタイム依存DLLのパスが不正です。",
                    dependency));
                continue;
            }

            if (!File.Exists(full))
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "RUNTIME_MISSING",
                    "必要なランタイム依存DLLが見つかりません。",
                    full));
                continue;
            }

            if (IsReparsePoint(full))
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "RUNTIME_REPARSE",
                    "ランタイム依存DLLが reparse point です。",
                    full));
                continue;
            }

            string fileName = Path.GetFileName(full);
            if (fileName.EndsWith("_reflect.dll", StringComparison.OrdinalIgnoreCase))
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "EDITOR_DLL_DEPENDENCY",
                    "Editor用 reflection DLL は配布ランタイムへ含められません。",
                    full));
                continue;
            }

            if (dependencyNames.TryGetValue(fileName, out string? existing) &&
                !string.Equals(existing, full, StringComparison.OrdinalIgnoreCase))
            {
                issues.Add(new(
                    PackageIssueSeverity.Error,
                    "RUNTIME_NAME_COLLISION",
                    $"同名の依存DLLが複数の場所で解決されました: {fileName}",
                    full));
            }
            else
            {
                dependencyNames[fileName] = full;
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        return issues;
    }

    private static PackageIssue MapCookDiagnostic(AssetCookDiagnostic diagnostic) =>
        new(
            diagnostic.Severity switch
            {
                AssetCookDiagnosticSeverity.Info => PackageIssueSeverity.Info,
                AssetCookDiagnosticSeverity.Warning => PackageIssueSeverity.Warning,
                _ => PackageIssueSeverity.Error,
            },
            diagnostic.Code,
            diagnostic.Message,
            string.IsNullOrWhiteSpace(diagnostic.AssetPath)
                ? (string.IsNullOrWhiteSpace(diagnostic.AssetId)
                    ? null
                    : diagnostic.AssetId)
                : diagnostic.AssetPath);

    private static AssetCookPlan BuildCookPlan(
        PackageProjectInfo project,
        string projectRoot,
        string assetsRoot,
        string legacyInitialScene,
        CancellationToken cancellationToken = default)
    {
        var planner = new AssetCookPlanner(projectRoot, assetsRoot);
        return string.IsNullOrWhiteSpace(project.CanonicalSceneAssetId)
            ? planner.Build(legacyInitialScene, cancellationToken)
            : planner.BuildByAssetId(
                project.CanonicalSceneAssetId,
                cancellationToken);
    }

    private static PackageIssue MapSceneAdapterDiagnostic(
        CanonicalSceneAdapterDiagnostic diagnostic) =>
        new(
            diagnostic.Severity == CanonicalSceneAdapterSeverity.Error
                ? PackageIssueSeverity.Error
                : PackageIssueSeverity.Warning,
            diagnostic.Code,
            diagnostic.Line > 0
                ? $"line {diagnostic.Line}: {diagnostic.Message}"
                : diagnostic.Message,
            string.IsNullOrWhiteSpace(diagnostic.Path)
                ? null
                : diagnostic.Path);

    public static async Task<PackageResult> CreatePackageAsync(
        PackageProjectInfo project,
        PackageOptions options,
        string executablePath,
        IReadOnlyList<string> runtimeDependencies,
        IProgress<PackageProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        var issues =
            Validate(
                    project,
                    options,
                    executablePath,
                    runtimeDependencies,
                    cancellationToken)
                .ToList();
        if (string.IsNullOrWhiteSpace(options.AssetPackToolPath))
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "ASSETPACK_TOOL_REQUIRED",
                "Cookにはacs_assetpack実行ファイルが必要です。"));
        }
        if (issues.Any(issue => issue.Severity == PackageIssueSeverity.Error))
            throw new PackageValidationException(issues);

        string assetPackTool = Path.GetFullPath(options.AssetPackToolPath!);
        string packageId = PackageId(project, options);
        string stagingParent = Path.GetFullPath(Path.Combine(project.TempDirectory, "PackageStaging"));
        string stagingRoot = Path.Combine(stagingParent, Guid.NewGuid().ToString("N"));
        string packageRoot = Path.Combine(stagingRoot, packageId);
        string outputDirectory = Path.GetFullPath(options.OutputDirectory);
        string finalZip = Path.Combine(outputDirectory, packageId + ".zip");
        string temporaryZip = finalZip + ".tmp-" + Guid.NewGuid().ToString("N");

        // Validate before the first write so a project-controlled Temp junction
        // cannot make CreateDirectory mutate a location outside the project.
        RejectExistingReparsePointsInPath(stagingParent, "Package staging");
        Directory.CreateDirectory(stagingParent);
        // Re-check the created path as a narrow defense against replacement
        // between validation and creation.
        RejectExistingReparsePointsInPath(stagingParent, "Package staging");
        Directory.CreateDirectory(packageRoot);

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            progress?.Report(new("Stage", "Release 実行ファイルをステージングしています。", 0, 1));

            string executableDestination =
                Path.Combine(packageRoot, Path.GetFileName(executablePath));
            CopyFile(executablePath, executableDestination);

            var runtimeByName = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (string dependency in runtimeDependencies)
            {
                string full = Path.GetFullPath(dependency);
                string name = Path.GetFileName(full);
                if (name.EndsWith("_reflect.dll", StringComparison.OrdinalIgnoreCase))
                    continue;
                runtimeByName.TryAdd(name, full);
            }

            int dependencyIndex = 0;
            foreach ((string name, string source) in runtimeByName.OrderBy(
                         item => item.Key,
                         StringComparer.Ordinal))
            {
                cancellationToken.ThrowIfCancellationRequested();
                progress?.Report(new(
                    "Dependencies",
                    $"ランタイム依存DLL: {name}",
                    ++dependencyIndex,
                    runtimeByName.Count));
                CopyFile(source, Path.Combine(packageRoot, name));
            }

            if (options.IncludeDebugSymbols)
            {
                string pdb = Path.ChangeExtension(executablePath, ".pdb");
                if (File.Exists(pdb))
                    CopyFile(pdb, Path.Combine(packageRoot, Path.GetFileName(pdb)));
            }

            progress?.Report(new(
                "Config",
                "Snapshotting project configuration...",
                0,
                0));
            PackageDirectorySnapshot configSnapshot =
                await StageDirectorySnapshotAsync(
                    project.ConfigDirectory,
                    Path.Combine(packageRoot, "Config"),
                    cancellationToken);
            ValidateStagedConfiguration(
                Path.Combine(packageRoot, "Config"),
                project,
                cancellationToken);

            CookResult cooked = await CookAssetPackAsync(
                project,
                options,
                assetPackTool,
                stagingRoot,
                packageRoot,
                progress,
                cancellationToken);

            var files = new List<ManifestFile>();
            string manifestPath = Path.Combine(packageRoot, "package-manifest.json");
            string[] payloadFiles = Directory.EnumerateFiles(
                    packageRoot,
                    "*",
                    SearchOption.AllDirectories)
                .Where(path => !string.Equals(path, manifestPath, StringComparison.OrdinalIgnoreCase))
                .OrderBy(path => RelativeZipPath(packageRoot, path), StringComparer.Ordinal)
                .ToArray();

            for (int index = 0; index < payloadFiles.Length; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string file = payloadFiles[index];
                string relative = RelativeZipPath(packageRoot, file);
                progress?.Report(new(
                    "Hash",
                    $"SHA-256: {relative}",
                    index + 1,
                    payloadFiles.Length));
                files.Add(new(relative, new FileInfo(file).Length, await Sha256Async(file, cancellationToken)));
            }

            string buildId = ComputeBuildId(files);
            var manifest = new PackageManifest(
                schemaVersion: 3,
                productName: project.Name,
                productVersion: options.ProductVersion,
                projectSchemaVersion: project.ProjectSchemaVersion,
                engineVersion: project.EngineVersion,
                platform: "win-x64",
                configuration: "Release",
                profile: options.Profile.ToString(),
                executable: Path.GetFileName(executablePath),
                buildId: buildId,
                canonicalSceneAssetId: cooked.CanonicalSceneAssetId,
                canonicalSceneKind: cooked.CanonicalSceneKind,
                canonicalSceneImporter: cooked.CanonicalSceneImporter,
                canonicalSceneImporterVersion: cooked.CanonicalSceneImporterVersion,
                assetGraphHash: cooked.AssetGraphHash,
                sceneBootstrap: cooked.SceneBootstrap,
                assetPack: new(
                    path: "game.acpak",
                    size: new FileInfo(cooked.PackPath).Length,
                    sha256: cooked.Sha256,
                    formatVersion: 1,
                    compressed: cooked.Compressed,
                    sourceFileCount: cooked.SourceFileCount),
                files: files);
            await File.WriteAllTextAsync(
                manifestPath,
                JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true }),
                Utf8NoBom,
                cancellationToken);

            // Check the complete existing ancestor chain before creating a
            // directory or temporary ZIP. Checking only the leaf misses
            // "junction\new-child" redirection.
            RejectExistingReparsePointsInPath(outputDirectory, "Package output");
            RejectExistingReparsePointsInPath(finalZip, "Package ZIP");
            Directory.CreateDirectory(outputDirectory);
            RejectExistingReparsePointsInPath(outputDirectory, "Package output");
            RejectExistingReparsePointsInPath(finalZip, "Package ZIP");

            progress?.Report(new("Archive", "再現可能なZIPを生成しています。", 0, files.Count + 1));
            await CreateDeterministicZipAsync(
                stagingRoot,
                temporaryZip,
                progress,
                cancellationToken);

            progress?.Report(new(
                "Verify",
                "Verifying archive manifest, paths, sizes, and SHA-256 hashes...",
                0,
                files.Count));
            PackageVerificationResult verification =
                await VerifyPackageArchiveAsync(
                    temporaryZip,
                    progress,
                    cancellationToken);
            long expectedBytes = files.Sum(file => file.size);
            if (!string.Equals(
                    verification.PackageId,
                    packageId,
                    StringComparison.Ordinal) ||
                !string.Equals(
                    verification.BuildId,
                    buildId,
                    StringComparison.Ordinal) ||
                verification.FileCount != files.Count ||
                verification.UncompressedBytes != expectedBytes ||
                verification.Profile != options.Profile ||
                verification.CookedAssetCount != cooked.SourceFileCount ||
                !string.Equals(
                    verification.AssetPackSha256,
                    cooked.Sha256,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "The completed archive does not match the package that was staged.");
            }

            // 初回検証の後には、長時間の cook/archive が続く可能性がある。プロセス間の
            // 変更リースを再取得して ZIP 公開まで保持し、この最終 journal/identity 検証後に
            // Initial Scene の移動が開始されないようにする。
            AssetMutationLock publishLease;
            try
            {
                publishLease = AssetMutationLock.AcquireFailFast(
                    project.AssetsDirectory,
                    "Publish package");
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    InvalidDataException or ArgumentException)
            {
                throw ProjectChangedDuringPackage(
                    "The asset mutation lease could not be acquired immediately before " +
                    "publication: " + error.Message);
            }
            using (publishLease)
            {
                progress?.Report(new(
                    "Publish",
                    "Revalidating project and configuration snapshot...",
                    0,
                    1));
                ValidateProjectSceneStateForPublish(
                    project,
                    cooked.CanonicalSceneAssetId,
                    configSnapshot,
                    cancellationToken);
                cancellationToken.ThrowIfCancellationRequested();
                File.Move(temporaryZip, finalZip, overwrite: true);
            }
            long totalBytes = files.Sum(file => file.size);
            progress?.Report(new("Complete", $"パッケージを生成しました: {finalZip}", files.Count, files.Count));
            return new(
                finalZip,
                packageId,
                buildId,
                files.Count,
                totalBytes,
                cooked.Sha256,
                cooked.SourceFileCount,
                options.Profile,
                ArchiveVerified: true);
        }
        finally
        {
            bool zipCleaned = await TryDeleteFileWithRetryAsync(temporaryZip);
            bool stagingCleaned =
                await TryDeleteDirectoryWithRetryAsync(stagingRoot, stagingParent);
            if (!zipCleaned || !stagingCleaned)
            {
                progress?.Report(new(
                    "Cleanup",
                    "Package cleanup could not remove one or more private temporary " +
                    "paths after bounded retries.",
                    0,
                    0));
            }
        }
    }

    /// <summary>
    /// Verifies a completed package without extracting it. Every payload must be
    /// declared by the manifest and match its size and SHA-256 digest before the
    /// archive is eligible for publication.
    /// </summary>
    public static async Task<PackageVerificationResult> VerifyPackageArchiveAsync(
        string zipPath,
        IProgress<PackageProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(zipPath);
        cancellationToken.ThrowIfCancellationRequested();

        string fullZipPath = Path.GetFullPath(zipPath);
        RejectExistingReparsePointsInPath(fullZipPath, "Package ZIP");
        if (!File.Exists(fullZipPath))
            throw new FileNotFoundException("Package archive was not found.", fullZipPath);

        await using FileStream input = new(
            fullZipPath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 128 * 1024,
            useAsync: true);
        using var archive = new ZipArchive(
            input,
            ZipArchiveMode.Read,
            leaveOpen: false,
            entryNameEncoding: Utf8Strict);

        if (archive.Entries.Count is < 2 or > ArchiveEntryLimit)
        {
            throw new InvalidDataException(
                $"Package archive entry count must be between 2 and {ArchiveEntryLimit}.");
        }

        var entriesByPath = new Dictionary<string, ZipArchiveEntry>(
            StringComparer.OrdinalIgnoreCase);
        var manifestEntries = new List<ZipArchiveEntry>();
        string? archiveRoot = null;
        long declaredArchiveBytes = 0;

        foreach (ZipArchiveEntry entry in archive.Entries)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ValidateArchiveEntryPath(entry.FullName);
            if (entry.Name.Length == 0 || entry.ExternalAttributes != 0)
            {
                throw new InvalidDataException(
                    "Package entries must be ordinary files without external attributes.");
            }
            if (!entriesByPath.TryAdd(entry.FullName, entry))
            {
                throw new InvalidDataException(
                    $"Package archive contains a duplicate Windows path: {entry.FullName}");
            }

            string[] segments = entry.FullName.Split('/');
            if (segments.Length < 2)
            {
                throw new InvalidDataException(
                    $"Every package entry must be below one package root: {entry.FullName}");
            }
            archiveRoot ??= segments[0];
            if (!string.Equals(segments[0], archiveRoot, StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "Package archive contains entries below more than one root directory.");
            }

            if (entry.Length < 0 ||
                entry.Length > ArchiveUncompressedLimitBytes - declaredArchiveBytes)
            {
                throw new InvalidDataException(
                    "Package archive exceeds the supported uncompressed size limit.");
            }
            declaredArchiveBytes += entry.Length;

            if (segments.Length == 2 &&
                string.Equals(
                    segments[1],
                    "package-manifest.json",
                    StringComparison.Ordinal))
            {
                manifestEntries.Add(entry);
            }
        }

        if (manifestEntries.Count != 1)
        {
            throw new InvalidDataException(
                "Package archive must contain exactly one root package-manifest.json.");
        }
        string packageId = archiveRoot ??
            throw new InvalidDataException("Package archive root is missing.");

        ZipArchiveEntry manifestEntry = manifestEntries[0];
        if (manifestEntry.Length is < 2 or > ArchiveManifestLimitBytes)
        {
            throw new InvalidDataException(
                $"Package manifest must be between 2 and {ArchiveManifestLimitBytes} bytes.");
        }

        PackageManifest? manifest;
        await using (Stream manifestStream = manifestEntry.Open())
        {
            manifest = await JsonSerializer.DeserializeAsync<PackageManifest>(
                manifestStream,
                new JsonSerializerOptions
                {
                    PropertyNameCaseInsensitive = false,
                    UnmappedMemberHandling =
                        System.Text.Json.Serialization.JsonUnmappedMemberHandling.Disallow,
                    MaxDepth = 64,
                },
                cancellationToken);
        }
        if (manifest is null)
            throw new InvalidDataException("Package manifest is empty.");

        if (manifest.schemaVersion != 3 ||
            string.IsNullOrWhiteSpace(manifest.productName) ||
            string.IsNullOrWhiteSpace(manifest.productVersion) ||
            manifest.projectSchemaVersion < 1 ||
            string.IsNullOrWhiteSpace(manifest.engineVersion) ||
            !ProductVersionPattern.IsMatch(manifest.productVersion) ||
            !string.Equals(manifest.platform, "win-x64", StringComparison.Ordinal) ||
            !string.Equals(manifest.configuration, "Release", StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Package manifest identity or schema metadata is invalid.");
        }
        if (!Enum.TryParse(
                manifest.profile,
                ignoreCase: false,
                out PackageProfile profile) ||
            !Enum.IsDefined(profile) ||
            !string.Equals(
                manifest.profile,
                profile.ToString(),
                StringComparison.Ordinal))
        {
            throw new InvalidDataException("Package manifest profile is invalid.");
        }

        string expectedPackageId =
            SanitizeFileName(manifest.productName) + "-" +
            SanitizeFileName(manifest.productVersion) +
            (profile == PackageProfile.Shipping
                ? ""
                : "-" + profile.ToString().ToLowerInvariant()) +
            "-win64";
        if (!string.Equals(packageId, expectedPackageId, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Package archive root does not match its manifest identity.");
        }
        if (!IsLowerHexSha256(manifest.buildId) ||
            !IsLowerHexSha256(manifest.assetGraphHash) ||
            !Guid.TryParseExact(
                manifest.canonicalSceneAssetId,
                "N",
                out Guid canonicalSceneAssetId) ||
            canonicalSceneAssetId == Guid.Empty ||
            !string.Equals(
                manifest.canonicalSceneAssetId,
                canonicalSceneAssetId.ToString("N"),
                StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(manifest.canonicalSceneKind) ||
            string.IsNullOrWhiteSpace(manifest.canonicalSceneImporter) ||
            manifest.canonicalSceneImporterVersion < 1)
        {
            throw new InvalidDataException(
                "Package manifest build or canonical-scene metadata is invalid.");
        }
        if (manifest.sceneBootstrap is null ||
            !string.Equals(
                manifest.sceneBootstrap.path,
                CanonicalSceneAdapter.BootstrapPath,
                StringComparison.Ordinal) ||
            !string.Equals(
                manifest.sceneBootstrap.contract,
                CanonicalSceneAdapter.BootstrapContract,
                StringComparison.Ordinal) ||
            (manifest.sceneBootstrap.sourceFormat !=
                 CanonicalSceneAdapter.LegacyScene2DFormat &&
             manifest.sceneBootstrap.sourceFormat !=
                 CanonicalSceneAdapter.LegacyScene3DFormat) ||
            (manifest.sceneBootstrap.sourceFormat ==
                 CanonicalSceneAdapter.LegacyScene2DFormat &&
             manifest.sceneBootstrap.adapterProjectionHint != "orthographic") ||
            (manifest.sceneBootstrap.sourceFormat ==
                 CanonicalSceneAdapter.LegacyScene3DFormat &&
             manifest.sceneBootstrap.adapterProjectionHint != "perspective"))
        {
            throw new InvalidDataException(
                "Package manifest scene bootstrap metadata is invalid.");
        }
        if (manifest.files is null ||
            manifest.files.Count != archive.Entries.Count - 1)
        {
            throw new InvalidDataException(
                "Package manifest payload count does not match the archive.");
        }

        var manifestPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long verifiedBytes = 0;
        string? priorPath = null;
        for (int index = 0; index < manifest.files.Count; index++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ManifestFile file = manifest.files[index] ??
                throw new InvalidDataException("Package manifest contains a null file record.");
            ValidateArchiveEntryPath(file.path);
            if (string.Equals(
                    file.path,
                    "package-manifest.json",
                    StringComparison.OrdinalIgnoreCase) ||
                !manifestPaths.Add(file.path))
            {
                throw new InvalidDataException(
                    $"Package manifest contains a duplicate or reserved path: {file.path}");
            }
            if (priorPath is not null &&
                StringComparer.Ordinal.Compare(priorPath, file.path) >= 0)
            {
                throw new InvalidDataException(
                    "Package manifest payload paths are not strictly ordinal-sorted.");
            }
            priorPath = file.path;

            if (file.size < 0 ||
                file.size > ArchiveUncompressedLimitBytes - verifiedBytes ||
                !IsLowerHexSha256(file.sha256))
            {
                throw new InvalidDataException(
                    $"Package manifest file metadata is invalid: {file.path}");
            }

            string archivePath = packageId + "/" + file.path;
            if (!entriesByPath.TryGetValue(archivePath, out ZipArchiveEntry? entry) ||
                entry.Length != file.size)
            {
                throw new InvalidDataException(
                    $"Package payload is missing or has the wrong size: {file.path}");
            }

            progress?.Report(new(
                "Verify",
                $"SHA-256: {file.path}",
                index + 1,
                manifest.files.Count));
            (string hash, long bytesRead) =
                await HashArchiveEntryAsync(entry, cancellationToken);
            if (bytesRead != file.size ||
                !string.Equals(hash, file.sha256, StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    $"Package payload hash verification failed: {file.path}");
            }
            verifiedBytes += bytesRead;
        }

        string recomputedBuildId = ComputeBuildId(manifest.files);
        if (!string.Equals(
                recomputedBuildId,
                manifest.buildId,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Package manifest build ID does not match its payload records.");
        }
        ValidateArchiveEntryPath(manifest.executable);
        if (manifest.executable.Contains('/') ||
            !manifest.executable.EndsWith(
                ".exe",
                StringComparison.OrdinalIgnoreCase) ||
            !manifestPaths.Contains(manifest.executable))
        {
            throw new InvalidDataException(
                "Package manifest executable is missing from the payload.");
        }

        ManifestAssetPack assetPack = manifest.assetPack ??
            throw new InvalidDataException("Package manifest asset pack metadata is missing.");
        if (!string.Equals(assetPack.path, "game.acpak", StringComparison.Ordinal) ||
            assetPack.formatVersion != 1 ||
            assetPack.sourceFileCount < 0 ||
            !manifestPaths.Contains(assetPack.path))
        {
            throw new InvalidDataException(
                "Package manifest asset pack metadata is invalid.");
        }
        ManifestFile assetPackFile = manifest.files.First(
            file => string.Equals(
                file.path,
                assetPack.path,
                StringComparison.OrdinalIgnoreCase));
        if (assetPack.size != assetPackFile.size ||
            !string.Equals(
                assetPack.sha256,
                assetPackFile.sha256,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Package asset pack metadata does not match its payload record.");
        }

        return new(
            fullZipPath,
            packageId,
            manifest.buildId,
            manifest.files.Count,
            verifiedBytes,
            assetPack.sha256,
            assetPack.sourceFileCount,
            profile);
    }

    /// <summary>
    /// パッケージ公開直前の最終ゲート。呼び出し元は、後続する ZIP の原子的な置換が
    /// 完了するまで、プロジェクトのアセット変更リースを保持する。
    /// </summary>
    internal static void ValidateProjectSceneStateForPublish(
        PackageProjectInfo snapshot,
        string cookedCanonicalSceneAssetId,
        PackageDirectorySnapshot? configSnapshot = null,
        CancellationToken cancellationToken = default)
    {
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (ProjectManager.HasPendingInitialScenePathFollow(snapshot.AssetsDirectory))
            {
                throw new InvalidDataException(
                    "An initial-scene move journal appeared while packaging.");
            }

            Project persisted = ProjectManager.ReadManifest(snapshot.ProjectFilePath);
            if (!string.Equals(persisted.Name, snapshot.Name, StringComparison.Ordinal) ||
                persisted.Version != snapshot.ProjectSchemaVersion ||
                !string.Equals(
                    persisted.EngineVersion,
                    snapshot.EngineVersion,
                    StringComparison.Ordinal) ||
                !string.Equals(
                    persisted.InitialScene,
                    snapshot.InitialScene,
                    StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(
                    persisted.CanonicalSceneAssetId,
                    snapshot.CanonicalSceneAssetId,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "The project manifest changed while packaging.");
            }

            string authoritativeAssetId =
                ProjectManager.ValidateInitialSceneAssetIdentity(persisted);
            if (!string.Equals(
                    authoritativeAssetId,
                    cookedCanonicalSceneAssetId,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "The initial scene identity no longer matches the cooked package.");
            }
            if (configSnapshot != null)
            {
                ValidateDirectorySnapshot(
                    snapshot.ConfigDirectory,
                    configSnapshot,
                    cancellationToken);
            }
        }
        catch (PackageValidationException)
        {
            throw;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException or JsonException or
                KeyNotFoundException or NotSupportedException)
        {
            throw ProjectChangedDuringPackage(error.Message);
        }
    }

    private static PackageValidationException ProjectChangedDuringPackage(string message) =>
        new(
        [
            new PackageIssue(
                PackageIssueSeverity.Error,
                "PROJECT_CHANGED_DURING_PACKAGE",
                "Package publication was refused because the startup-scene state could no " +
                "longer be proven identical to the cooked input. " + message),
        ]);

    private static void ValidateTree(
        string sourceRoot,
        string label,
        List<PackageIssue> issues,
        CancellationToken cancellationToken)
    {
        try
        {
            RejectReparsePoint(sourceRoot, label);
            _ = EnumerateFilesSafe(
                    sourceRoot,
                    issues,
                    cancellationToken: cancellationToken)
                .Count();
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "INPUT_TREE_UNSAFE",
                $"{label} の走査に失敗しました: {error.Message}",
                sourceRoot));
        }
    }

    private static IEnumerable<string> EnumerateFilesSafe(
        string sourceRoot,
        List<PackageIssue> issues,
        bool excludeAssetMetadata = false,
        CancellationToken cancellationToken = default)
    {
        var pending = new Stack<string>();
        pending.Push(Path.GetFullPath(sourceRoot));
        while (pending.Count > 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string directory = pending.Pop();
            RejectReparsePoint(directory, "Input directory");
            foreach (string entry in Directory.EnumerateFileSystemEntries(directory)
                         .OrderBy(path => path, StringComparer.Ordinal))
            {
                cancellationToken.ThrowIfCancellationRequested();
                FileAttributes attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "REPARSE_POINT",
                        "Assets/Config 内の symlink、junction、reparse point は配布対象にできません。",
                        entry));
                    continue;
                }

                if (excludeAssetMetadata &&
                    ShouldExcludeCookMetadata(sourceRoot, entry))
                {
                    continue;
                }

                if ((attributes & FileAttributes.Directory) != 0)
                    pending.Push(entry);
                else
                    yield return entry;
            }
        }
    }

    private static bool ShouldExcludeCookMetadata(
        string assetsRoot,
        string path)
    {
        string relative = Path.GetRelativePath(assetsRoot, path);
        string[] segments = relative.Split(
            [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
            StringSplitOptions.RemoveEmptyEntries);
        if (segments.Any(segment =>
                string.Equals(segment, ".acsdb", StringComparison.OrdinalIgnoreCase)))
        {
            return true;
        }

        string name = Path.GetFileName(path);
        return name.EndsWith(".acsmeta", StringComparison.OrdinalIgnoreCase) ||
               name.Contains(".tmp-", StringComparison.OrdinalIgnoreCase);
    }

    private static void ValidateReferenceFile(
        string file,
        string assetsRoot,
        string projectRoot,
        Regex expression,
        List<PackageIssue> issues,
        CancellationToken cancellationToken)
    {
        try
        {
            foreach (string line in File.ReadLines(file, Encoding.UTF8))
            {
                cancellationToken.ThrowIfCancellationRequested();
                Match match = expression.Match(line);
                if (!match.Success)
                    continue;

                string reference = match.Groups["path"].Value.Trim();
                if (!TryResolveAssetReference(
                        reference,
                        assetsRoot,
                        projectRoot,
                        out string resolved,
                        out string relative,
                        out string error))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "ASSET_REFERENCE_INVALID",
                        error,
                        $"{file}: {reference}"));
                    continue;
                }

                if (relative.Any(value => value > 127))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "ASSET_PATH_NON_ASCII",
                        "現在のstandaloneローダーは非ASCIIアセットパスを安全に開けません。ASCII名へ変更してください。",
                        resolved));
                }
            }
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "REFERENCE_SCAN_FAILED",
                $"参照アセットの検証に失敗しました: {error.Message}",
                file));
        }
    }

    private static void ValidateGltfReferences(
        string file,
        string assetsRoot,
        List<PackageIssue> issues,
        CancellationToken cancellationToken)
    {
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            using JsonDocument document = JsonDocument.Parse(
                File.ReadAllBytes(file));
            cancellationToken.ThrowIfCancellationRequested();
            ValidateGltfUriArray(document.RootElement, "buffers");
            ValidateGltfUriArray(document.RootElement, "images");
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (JsonException error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "GLTF_INVALID",
                $"glTF JSONを解析できません: {error.Message}",
                file));
        }
        catch (Exception error)
        {
            issues.Add(new(
                PackageIssueSeverity.Error,
                "GLTF_REFERENCE_SCAN_FAILED",
                $"glTF外部参照の検証に失敗しました: {error.Message}",
                file));
        }

        void ValidateGltfUriArray(JsonElement root, string property)
        {
            if (!root.TryGetProperty(property, out JsonElement values) ||
                values.ValueKind != JsonValueKind.Array)
            {
                return;
            }

            foreach (JsonElement value in values.EnumerateArray())
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!value.TryGetProperty("uri", out JsonElement uriElement) ||
                    uriElement.ValueKind != JsonValueKind.String)
                {
                    continue;
                }

                string uri = uriElement.GetString() ?? "";
                if (uri.StartsWith("data:", StringComparison.OrdinalIgnoreCase))
                    continue;
                if (string.IsNullOrWhiteSpace(uri) ||
                    uri.Contains('#') || uri.Contains('?') ||
                    Uri.TryCreate(uri, UriKind.Absolute, out _))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "GLTF_EXTERNAL_URI_UNSUPPORTED",
                        "glTFの外部URIはAssets内の相対ファイルだけを指定できます。",
                        $"{file}: {uri}"));
                    continue;
                }

                string decoded = Uri.UnescapeDataString(uri)
                    .Replace('/', Path.DirectorySeparatorChar);
                string resolved = Path.GetFullPath(Path.Combine(
                    Path.GetDirectoryName(file)!, decoded));
                if (!IsWithin(assetsRoot, resolved) ||
                    !File.Exists(resolved) ||
                    HasReparsePointBetween(assetsRoot, resolved))
                {
                    issues.Add(new(
                        PackageIssueSeverity.Error,
                        "GLTF_EXTERNAL_URI_INVALID",
                        "glTFの外部参照は存在するAssets内の通常ファイルでなければなりません。",
                        $"{file}: {uri}"));
                }
            }
        }
    }

    private static async Task<CookResult> CookAssetPackAsync(
        PackageProjectInfo project,
        PackageOptions options,
        string assetPackTool,
        string stagingRoot,
        string packageRoot,
        IProgress<PackageProgress>? progress,
        CancellationToken cancellationToken)
    {
        string projectRoot = Path.GetFullPath(project.RootDirectory);
        string assetsRoot = Path.GetFullPath(project.AssetsDirectory);
        string initialScene = ResolveUnderRoot(projectRoot, project.InitialScene);
        string cookRoot = Path.Combine(stagingRoot, "_CookInput");
        string cookedAssets = Path.Combine(cookRoot, "Assets");
        string packPath = Path.Combine(packageRoot, "game.acpak");

        RejectExistingReparsePointsInPath(cookRoot, "Cook staging");
        Directory.CreateDirectory(cookedAssets);
        RejectExistingReparsePointsInPath(cookRoot, "Cook staging");

        try
        {
            AssetCookPlan cookPlan = BuildCookPlan(
                project,
                projectRoot,
                assetsRoot,
                initialScene,
                cancellationToken);
            if (cookPlan.HasErrors)
            {
                throw new PackageValidationException(
                    cookPlan.Diagnostics.Select(MapCookDiagnostic).ToArray());
            }
            AssetRecord rootAsset = cookPlan.Root
                ?? throw new InvalidDataException(
                    "Cook planner did not return a canonical scene asset.");
            if (!string.Equals(
                    Path.GetFullPath(rootAsset.FullPath),
                    Path.GetFullPath(initialScene),
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new PackageValidationException(
                [
                    new(
                        PackageIssueSeverity.Error,
                        "CANONICAL_SCENE_PATH_MISMATCH",
                        "canonicalSceneAssetId and legacy initialScene resolve to different assets.",
                        $"{rootAsset.RelativePath} != {project.InitialScene}"),
                ]);
            }
            CanonicalSceneAdapterInspection sceneInspection =
                CanonicalSceneAdapter.InspectFile(rootAsset.FullPath);
            if (sceneInspection.HasErrors)
            {
                throw new PackageValidationException(
                    sceneInspection.Diagnostics
                        .Select(MapSceneAdapterDiagnostic)
                        .ToArray());
            }
            var cache = new DerivedDataCache(
                projectRoot,
                Path.Combine(project.TempDirectory, "DerivedDataCache", "Cook"));
            KeyValuePair<string, string>[] cookerSettings =
                CreateAssetCookerSettings(cookPlan.GraphHash);

            string cookedScene = Path.Combine(cookRoot, "main.acscene");
            for (int index = 0; index < cookPlan.Assets.Count; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                AssetRecord asset = cookPlan.Assets[index];
                bool isRoot = string.Equals(
                    asset.AssetId,
                    rootAsset.AssetId,
                    StringComparison.OrdinalIgnoreCase);
                string destination;
                if (isRoot)
                {
                    destination = cookedScene;
                }
                else
                {
                    destination = Path.GetFullPath(
                        Path.Combine(cookedAssets, asset.RelativePath));
                    if (!IsWithin(cookedAssets, destination))
                        throw new IOException(
                            $"Cook output escapes Assets staging: {asset.RelativePath}");
                }

                byte[] verifiedSource = ReadVerifiedAssetSnapshot(
                    asset,
                    assetsRoot,
                    cancellationToken);
                DerivedDataCacheResult derived = cache.GetOrCreate(
                    asset,
                    AssetCookerVersion,
                    cookerSettings,
                    () => CookAssetPayload(
                        asset,
                        verifiedSource,
                        assetsRoot,
                        projectRoot));
                WriteCookedPayload(destination, derived.Payload);
                progress?.Report(new(
                    "Cook",
                    $"Cook ({derived.Status}): " +
                    (isRoot
                        ? "main.acscene"
                        : "Assets/" + asset.RelativePath.Replace('\\', '/')),
                    index + 1,
                    cookPlan.Assets.Count));
            }

            var cookedIssues = new List<PackageIssue>();
            string[] cookedFiles = EnumerateFilesSafe(
                    cookRoot,
                    cookedIssues,
                    cancellationToken: cancellationToken)
                .OrderBy(
                    path => Path.GetRelativePath(cookRoot, path),
                    StringComparer.Ordinal)
                .ToArray();
            if (cookedIssues.Any(issue =>
                    issue.Severity == PackageIssueSeverity.Error))
            {
                throw new PackageValidationException(cookedIssues);
            }
            if (cookedFiles.Length == 0)
                throw new InvalidDataException("Cook対象アセットがありません。");

            bool compressed = options.Profile != PackageProfile.Development;
            progress?.Report(new(
                "Cook",
                compressed
                    ? "決定的なLZ4圧縮game.acpakを生成しています。"
                    : "決定的な非圧縮game.acpakを生成しています。",
                cookedFiles.Length,
                cookedFiles.Length));
            var packArguments = new List<string>
            {
                "pack",
                cookRoot,
                packPath,
            };
            if (compressed)
                packArguments.Add("--compress");
            await RunAssetPackToolAsync(
                assetPackTool,
                packArguments,
                projectRoot,
                cancellationToken);

            progress?.Report(new(
                "Verify",
                "game.acpakの全entryとCRCを検証しています。",
                0,
                cookedFiles.Length));
            await RunAssetPackToolAsync(
                assetPackTool,
                ["verify", packPath],
                projectRoot,
                cancellationToken);
            if (!File.Exists(packPath) || IsReparsePoint(packPath))
                throw new IOException(
                    "acs_assetpack完了後のgame.acpakが見つからないか安全でありません。");

            string packHash = await Sha256Async(packPath, cancellationToken);

            if (options.Profile == PackageProfile.Development)
            {
                CopyDirectorySafe(
                    cookedAssets,
                    Path.Combine(packageRoot, "Assets"),
                    progress,
                    cancellationToken);
                CopyFile(cookedScene, Path.Combine(packageRoot, "main.acscene"));
            }

            return new(
                packPath,
                packHash,
                cookedFiles.Length,
                compressed,
                rootAsset.AssetId,
                rootAsset.Kind,
                rootAsset.Metadata.Importer,
                rootAsset.Metadata.ImporterVersion,
                cookPlan.GraphHash,
                sceneInspection.Envelope);
        }
        finally
        {
            await TryDeleteDirectoryWithRetryAsync(cookRoot, stagingRoot);
        }
    }

    private static async Task RunAssetPackToolAsync(
        string toolPath,
        IEnumerable<string> arguments,
        string workingDirectory,
        CancellationToken cancellationToken)
    {
        var start = new ProcessStartInfo
        {
            FileName = toolPath,
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (string argument in arguments)
            start.ArgumentList.Add(argument);

        PackageProcessResult process;
        try
        {
            process = await PackageProcessRunner.RunAsync(
                start,
                log: null,
                cancellationToken);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            throw new InvalidOperationException(
                $"acs_assetpackを起動できません: {error.Message}",
                error);
        }
        if (process.ExitCode != 0)
        {
            string detail = string.Join(
                Environment.NewLine,
                new[]
                {
                    process.StandardOutput.Trim(),
                    process.StandardError.Trim(),
                }
                    .Where(value => !string.IsNullOrWhiteSpace(value)));
            throw new InvalidDataException(
                $"acs_assetpackが失敗しました (exit {process.ExitCode})." +
                (string.IsNullOrEmpty(detail)
                    ? ""
                    : Environment.NewLine + detail));
        }
    }

    private static void CopyDirectorySafe(
        string sourceRoot,
        string destinationRoot,
        IProgress<PackageProgress>? progress,
        CancellationToken cancellationToken)
    {
        var issues = new List<PackageIssue>();
        string[] files = EnumerateFilesSafe(
                sourceRoot,
                issues,
                cancellationToken: cancellationToken)
            .OrderBy(path => Path.GetRelativePath(sourceRoot, path), StringComparer.Ordinal)
            .ToArray();
        if (issues.Any(issue => issue.Severity == PackageIssueSeverity.Error))
            throw new PackageValidationException(issues);

        for (int index = 0; index < files.Length; index++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string source = files[index];
            string relative = Path.GetRelativePath(sourceRoot, source);
            string destination = Path.GetFullPath(Path.Combine(destinationRoot, relative));
            if (!IsWithin(destinationRoot, destination))
                throw new IOException($"コピー先がステージング領域から外れます: {relative}");
            progress?.Report(new(
                "Assets",
                $"コピー: {relative.Replace('\\', '/')}",
                index + 1,
                files.Length));
            CopyFile(source, destination);
        }
    }

    internal static async Task<PackageDirectorySnapshot>
        StageDirectorySnapshotAsync(
            string sourceRoot,
            string destinationRoot,
            CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string source = Path.GetFullPath(sourceRoot);
        string destination = Path.GetFullPath(destinationRoot);
        if (!Directory.Exists(source))
            return new(false, Array.Empty<PackageInputFileSnapshot>());

        var issues = new List<PackageIssue>();
        RejectExistingReparsePointsInPath(source, "Config");
        string[] files = EnumerateFilesSafe(
                source,
                issues,
                cancellationToken: cancellationToken)
            .OrderBy(path => Path.GetRelativePath(source, path), StringComparer.Ordinal)
            .ToArray();
        if (issues.Any(issue => issue.Severity == PackageIssueSeverity.Error))
            throw new PackageValidationException(issues);

        RejectExistingReparsePointsInPath(destination, "Staged Config");
        Directory.CreateDirectory(destination);
        RejectExistingReparsePointsInPath(destination, "Staged Config");
        var snapshots = new List<PackageInputFileSnapshot>(files.Length);
        foreach (string file in files)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string relative = Path.GetRelativePath(source, file);
            string portable = relative.Replace('\\', '/');
            string staged = Path.GetFullPath(Path.Combine(destination, relative));
            if (!IsWithin(destination, staged))
                throw new IOException(
                    $"Config snapshot destination escapes staging: {portable}");

            string parent = Path.GetDirectoryName(staged)
                ?? throw new InvalidDataException(
                    "Config snapshot destination has no parent directory.");
            RejectExistingReparsePointsInPath(parent, "Staged Config");
            Directory.CreateDirectory(parent);
            RejectExistingReparsePointsInPath(staged, "Staged Config");

            long size = 0;
            using IncrementalHash hash = IncrementalHash.CreateHash(
                HashAlgorithmName.SHA256);
            await using (FileStream input = new(
                             file,
                             FileMode.Open,
                             FileAccess.Read,
                             FileShare.Read,
                             bufferSize: 128 * 1024,
                             FileOptions.Asynchronous |
                             FileOptions.SequentialScan))
            await using (FileStream output = new(
                             staged,
                             FileMode.CreateNew,
                             FileAccess.Write,
                             FileShare.None,
                             bufferSize: 128 * 1024,
                             FileOptions.Asynchronous |
                             FileOptions.SequentialScan))
            {
                byte[] buffer = new byte[128 * 1024];
                while (true)
                {
                    int read = await input.ReadAsync(
                        buffer.AsMemory(),
                        cancellationToken);
                    if (read == 0)
                        break;
                    hash.AppendData(buffer, 0, read);
                    await output.WriteAsync(
                        buffer.AsMemory(0, read),
                        cancellationToken);
                    size += read;
                }
                await output.FlushAsync(cancellationToken);
            }
            snapshots.Add(new(
                portable,
                size,
                Convert.ToHexString(hash.GetHashAndReset())
                    .ToLowerInvariant()));
        }

        var snapshot = new PackageDirectorySnapshot(
            true,
            snapshots.AsReadOnly());
        // Detect additions, removals, or replacement that raced the staging
        // pass itself. The same proof is repeated immediately before publish.
        ValidateDirectorySnapshot(source, snapshot, cancellationToken);
        return snapshot;
    }

    internal static void ValidateDirectorySnapshot(
        string sourceRoot,
        PackageDirectorySnapshot snapshot,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        cancellationToken.ThrowIfCancellationRequested();
        string source = Path.GetFullPath(sourceRoot);
        bool exists = Directory.Exists(source);
        if (exists != snapshot.Existed)
        {
            throw new InvalidDataException(
                "Config directory existence changed while packaging.");
        }
        if (!exists)
            return;

        var issues = new List<PackageIssue>();
        string[] files = EnumerateFilesSafe(
                source,
                issues,
                cancellationToken: cancellationToken)
            .OrderBy(path => Path.GetRelativePath(source, path), StringComparer.Ordinal)
            .ToArray();
        if (issues.Any(issue => issue.Severity == PackageIssueSeverity.Error))
            throw new PackageValidationException(issues);

        string[] currentRelative = files
            .Select(path => Path.GetRelativePath(source, path).Replace('\\', '/'))
            .ToArray();
        string[] expectedRelative = snapshot.Files
            .Select(static item => item.RelativePath)
            .ToArray();
        if (!currentRelative.SequenceEqual(expectedRelative, StringComparer.Ordinal))
        {
            throw new InvalidDataException(
                "Config file set changed while packaging.");
        }

        for (int index = 0; index < files.Length; index++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            PackageInputFileSnapshot expected = snapshot.Files[index];
            string file = files[index];
            var info = new FileInfo(file);
            if (info.Length != expected.Size)
            {
                throw new InvalidDataException(
                    $"Config file changed while packaging: {expected.RelativePath}");
            }

            string hash = Sha256File(file, cancellationToken);
            if (!string.Equals(
                    hash,
                    expected.Sha256,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    $"Config file changed while packaging: {expected.RelativePath}");
            }
        }
    }

    internal static void ValidateStagedConfiguration(
        string stagedConfigRoot,
        PackageProjectInfo project,
        CancellationToken cancellationToken = default)
    {
        string settings = Path.Combine(
            Path.GetFullPath(stagedConfigRoot),
            "ProjectSettings.ini");
        if (!File.Exists(settings) ||
            !TryReadIniValue(
                settings,
                "Game",
                "DefaultScene",
                out string configuredScene,
                cancellationToken) ||
            string.IsNullOrWhiteSpace(configuredScene))
        {
            return;
        }

        string configuredValue = configuredScene.Trim();
        string normalized = configuredValue.Replace(
            '/',
            Path.DirectorySeparatorChar);
        if (Path.IsPathRooted(normalized))
            throw StagedConfigMismatch(
                "DefaultScene became absolute while packaging.",
                configuredScene);
        string configuredPath = ResolveUnderRoot(
            project.RootDirectory,
            configuredValue);
        string initialScene = ResolveUnderRoot(
            project.RootDirectory,
            project.InitialScene);
        if (!IsWithin(project.AssetsDirectory, configuredPath) ||
            !HasSupportedSceneExtension(configuredPath) ||
            !string.Equals(
                configuredPath,
                initialScene,
                StringComparison.OrdinalIgnoreCase))
        {
            throw StagedConfigMismatch(
                "Staged Config DefaultScene no longer matches the cooked initial scene.",
                configuredScene);
        }
    }

    private static PackageValidationException StagedConfigMismatch(
        string message,
        string path) =>
        new(
        [
            new PackageIssue(
                PackageIssueSeverity.Error,
                "CONFIG_CHANGED_DURING_PACKAGE",
                message,
                path),
        ]);

    private static string Sha256File(
        string file,
        CancellationToken cancellationToken)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using FileStream stream = new(
            file,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        byte[] buffer = new byte[128 * 1024];
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read == 0)
                break;
            hash.AppendData(buffer, 0, read);
        }
        return Convert.ToHexString(hash.GetHashAndReset())
            .ToLowerInvariant();
    }

    private static byte[] ReadVerifiedAssetSnapshot(
        AssetRecord asset,
        string sourceAssetsRoot,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!File.Exists(asset.FullPath) ||
            HasReparsePointBetween(sourceAssetsRoot, asset.FullPath))
        {
            throw new InvalidDataException(
                $"Cook source is missing or unsafe: {asset.RelativePath}");
        }
        var info = new FileInfo(asset.FullPath);
        if (info.Length != asset.SizeBytes)
        {
            throw new InvalidDataException(
                $"Asset changed after the cook graph snapshot was built: {asset.RelativePath}");
        }
        if (info.Length > 1024L * 1024 * 1024)
            throw new InvalidDataException(
                $"Cook source exceeds the 1 GiB derived-data limit: {asset.RelativePath}");
        byte[] source = File.ReadAllBytes(asset.FullPath);
        cancellationToken.ThrowIfCancellationRequested();
        string hash = Convert.ToHexString(SHA256.HashData(source))
            .ToLowerInvariant();
        if (source.LongLength != asset.SizeBytes ||
            !string.Equals(hash, asset.ContentHash, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Asset changed after the cook graph snapshot was built: {asset.RelativePath}");
        }
        return source;
    }

    private static byte[] CookAssetPayload(
        AssetRecord asset,
        byte[] source,
        string sourceAssetsRoot,
        string projectRoot)
    {
        string extension = Path.GetExtension(asset.RelativePath).ToLowerInvariant();
        if (extension is ".acscene" or ".acs3d")
        {
            return RewriteCanonicalScenePayload(
                source,
                extension,
                sourceAssetsRoot,
                projectRoot);
        }
        if (extension == ".acsprefab")
        {
            return RewritePrefabPayload(
                source,
                sourceAssetsRoot,
                projectRoot);
        }
        if (extension == ".acsbp")
        {
            return RewriteBlueprintPayload(
                source,
                sourceAssetsRoot,
                projectRoot);
        }
        Regex? expression = extension switch
        {
            ".acsmat" => MaterialReference,
            _ => null,
        };
        return expression == null
            ? source
            : RewriteReferencePayload(
                source,
                sourceAssetsRoot,
                projectRoot,
                expression);
    }

    internal static byte[] RewritePrefabPayloadForSelfTest(
        byte[] source,
        string sourceAssetsRoot,
        string projectRoot) =>
        RewritePrefabPayload(source, sourceAssetsRoot, projectRoot);

    internal static byte[] RewriteBlueprintPayloadForSelfTest(
        byte[] source,
        string sourceAssetsRoot,
        string projectRoot) =>
        RewriteBlueprintPayload(source, sourceAssetsRoot, projectRoot);

    internal static string AssetCookerVersionForSelfTest =>
        AssetCookerVersion;

    internal static KeyValuePair<string, string>[]
        CreateAssetCookerSettingsForSelfTest(string assetGraphHash) =>
        CreateAssetCookerSettings(assetGraphHash);

    private static KeyValuePair<string, string>[] CreateAssetCookerSettings(
        string assetGraphHash) =>
    [
        new("assetGraphHash", assetGraphHash),
        new("platform", "win-x64"),
        new("referenceRewriteVersion", ReferenceRewriteVersion),
    ];

    private static byte[] RewritePrefabPayload(
        byte[] source,
        string sourceAssetsRoot,
        string projectRoot) =>
        RewriteReferencePayload(
            source,
            sourceAssetsRoot,
            projectRoot,
            SelectPrefabReferenceExpression(source));

    private static Regex SelectPrefabReferenceExpression(byte[] source)
    {
        try
        {
            using var memory = new MemoryStream(source, writable: false);
            using var reader = new StreamReader(
                memory,
                Utf8Strict,
                detectEncodingFromByteOrderMarks: true);
            return reader.ReadLine() switch
            {
                "ACS3D v2" => Scene3DReference,
                "ACSCENE v1" => SceneReference,
                _ => throw new InvalidDataException(
                    "Prefab payload must begin with exactly 'ACS3D v2' or 'ACSCENE v1'."),
            };
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "Prefab Cook source is not valid UTF-8.",
                error);
        }
    }

    private static byte[] RewriteBlueprintPayload(
        byte[] source,
        string sourceAssetsRoot,
        string projectRoot)
    {
        string text;
        try
        {
            text = Utf8Strict.GetString(source);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "Blueprint Cook source is not valid UTF-8.",
                error);
        }
        AcsbpCookDocument document = AcsbpFormat.ParseForCook(text);
        string[] lines = document.Lines.ToArray();

        bool changed = false;
        int parentCount = 0;
        for (int index = 1; index < lines.Length; ++index)
        {
            if (document.IsComponentLine(index))
                continue;
            if (!AcsbpFormat.TryParseCanonicalParentDirective(
                    lines[index],
                    out string reference))
                continue;
            if (++parentCount > 1)
            {
                throw new InvalidDataException(
                    "Blueprint payload may contain at most one PARENT directive.");
            }

            if (!Path.GetExtension(reference).Equals(
                    ".acsbp",
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "Blueprint PARENT must reference an .acsbp asset.");
            }
            if (!TryResolveAssetReference(
                    reference,
                    sourceAssetsRoot,
                    projectRoot,
                    out _,
                    out string relative,
                    out string error))
            {
                throw new InvalidDataException(error);
            }

            string rewritten =
                "PARENT " +
                "Assets/" +
                relative.Replace('\\', '/');
            if (!string.Equals(lines[index], rewritten, StringComparison.Ordinal))
            {
                lines[index] = rewritten;
                changed = true;
            }
        }

        if (document.HasComponents)
        {
            Regex componentExpression = document.ComponentExtension == ".acs3d"
                ? Scene3DReference
                : SceneReference;
            int componentEnd = document.ComponentStart + document.ComponentCount;
            for (int index = document.ComponentStart + 1;
                 index < componentEnd;
                 ++index)
            {
                string directive = FirstToken(lines[index]);
                bool referenceDirective = document.ComponentExtension == ".acs3d"
                    ? directive is "MSH3D" or "SPR3D" or "MAT3D" or "PFAB3D"
                    : directive is "SPRT" or "MAT" or "PFAB";
                if (!referenceDirective)
                    continue;

                Match match = componentExpression.Match(lines[index]);
                if (!match.Success)
                {
                    throw new InvalidDataException(
                        $"Blueprint CMP reference directive '{directive}' is malformed.");
                }
                string reference = match.Groups["path"].Value.Trim();
                if (directive == "MAT3D" && IsLegacyNumeric3DMaterial(reference))
                    continue;
                if (!TryResolveAssetReference(
                        reference,
                        sourceAssetsRoot,
                        projectRoot,
                        out _,
                        out string relative,
                        out string error))
                {
                    throw new InvalidDataException(error);
                }
                string rewritten =
                    match.Groups["prefix"].Value +
                    "Assets/" +
                    relative.Replace('\\', '/');
                if (!string.Equals(lines[index], rewritten, StringComparison.Ordinal))
                {
                    lines[index] = rewritten;
                    changed = true;
                }
            }
        }

        if (!changed)
            return source;
        while (lines.Length > 0 && lines[^1].Length == 0)
            Array.Resize(ref lines, lines.Length - 1);
        return Utf8NoBom.GetBytes(string.Join('\n', lines) + "\n");
    }

    private static byte[] RewriteCanonicalScenePayload(
        byte[] source,
        string sourceExtension,
        string sourceAssetsRoot,
        string projectRoot)
    {
        string text;
        try
        {
            text = Utf8Strict.GetString(source);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "Canonical scene source is not valid UTF-8.",
                error);
        }
        string rewritten = CanonicalSceneAdapter.RewriteReferences(
            text,
            sourceExtension,
            reference =>
            {
                if (!TryResolveAssetReference(
                        reference.Path,
                        sourceAssetsRoot,
                        projectRoot,
                        out _,
                        out string relative,
                        out string error))
                {
                    throw new InvalidDataException(error);
                }
                return "Assets/" + relative.Replace('\\', '/');
            });
        return Utf8NoBom.GetBytes(rewritten);
    }

    private static byte[] RewriteReferencePayload(
        byte[] source,
        string sourceAssetsRoot,
        string projectRoot,
        Regex expression)
    {
        var lines = new List<string>();
        try
        {
            using var memory = new MemoryStream(source, writable: false);
            using var reader = new StreamReader(
                memory,
                Utf8Strict,
                detectEncodingFromByteOrderMarks: true);
            while (reader.ReadLine() is { } line)
                lines.Add(line);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "Reference-bearing Cook source is not valid UTF-8.",
                error);
        }

        bool changed = false;
        for (int index = 0; index < lines.Count; index++)
        {
            Match match = expression.Match(lines[index]);
            if (!match.Success)
                continue;

            string reference = match.Groups["path"].Value.Trim();
            if (string.Equals(
                    match.Groups["verb"].Value,
                    "MAT3D",
                    StringComparison.Ordinal) &&
                IsLegacyNumeric3DMaterial(reference))
            {
                continue;
            }
            if (!TryResolveAssetReference(
                    reference,
                    sourceAssetsRoot,
                    projectRoot,
                    out _,
                    out string relative,
                    out string error))
            {
                throw new InvalidDataException(error);
            }

            string packaged = "Assets/" + relative.Replace('\\', '/');
            string rewritten = match.Groups["prefix"].Value + packaged;
            if (!string.Equals(lines[index], rewritten, StringComparison.Ordinal))
            {
                lines[index] = rewritten;
                changed = true;
            }
        }

        return changed
            ? Utf8NoBom.GetBytes(string.Join('\n', lines) + "\n")
            : source;
    }

    private static bool IsLegacyNumeric3DMaterial(string value)
    {
        string[] fields = value.Split(
            [' ', '\t'],
            StringSplitOptions.RemoveEmptyEntries);
        return fields.Length == 2 &&
               float.TryParse(
                   fields[0],
                   System.Globalization.NumberStyles.Float,
                   System.Globalization.CultureInfo.InvariantCulture,
                   out _) &&
               float.TryParse(
                   fields[1],
                   System.Globalization.NumberStyles.Float,
                   System.Globalization.CultureInfo.InvariantCulture,
                   out _);
    }

    private static string FirstToken(string line)
    {
        int separator = line.IndexOfAny([' ', '\t']);
        return separator < 0 ? line : line[..separator];
    }

    private static void WriteCookedPayload(string destination, byte[] payload)
    {
        string full = Path.GetFullPath(destination);
        string parent = Path.GetDirectoryName(full)
            ?? throw new InvalidDataException("Cook output has no parent directory.");
        RejectExistingReparsePointsInPath(parent, "Cook output");
        Directory.CreateDirectory(parent);
        RejectExistingReparsePointsInPath(full, "Cook output");
        string temporary = full + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       128 * 1024,
                       FileOptions.WriteThrough))
            {
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }
            RejectExistingReparsePointsInPath(full, "Cook output");
            File.Move(temporary, full, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temporary) && !IsReparsePoint(temporary))
                    File.Delete(temporary);
            }
            catch { }
        }
    }

    private static bool TryResolveAssetReference(
        string reference,
        string assetsRoot,
        string projectRoot,
        out string resolved,
        out string relative,
        out string error)
    {
        resolved = "";
        relative = "";
        error = "";
        try
        {
            string normalized = reference.Replace('/', Path.DirectorySeparatorChar);
            if (Path.IsPathRooted(normalized))
            {
                resolved = Path.GetFullPath(normalized);
            }
            else
            {
                string fromRoot = Path.GetFullPath(Path.Combine(projectRoot, normalized));
                string fromAssets = Path.GetFullPath(Path.Combine(assetsRoot, normalized));
                resolved = File.Exists(fromRoot) ? fromRoot : fromAssets;
            }

            if (!IsWithin(assetsRoot, resolved))
            {
                error = "参照アセットは Assets フォルダ内へimportしてください。外部絶対パスは配布できません。";
                return false;
            }

            if (!File.Exists(resolved))
            {
                error = "参照アセットが見つかりません。";
                return false;
            }

            if (HasReparsePointBetween(assetsRoot, resolved))
            {
                error = "参照アセットが reparse point を経由しています。";
                return false;
            }

            relative = Path.GetRelativePath(assetsRoot, resolved);
            if (relative == ".." ||
                relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal))
            {
                error = "参照アセットが Assets フォルダ外へ移動します。";
                return false;
            }
            return true;
        }
        catch (Exception exception)
        {
            error = $"参照パスが不正です: {exception.Message}";
            return false;
        }
    }

    private static async Task CreateDeterministicZipAsync(
        string stagingRoot,
        string zipPath,
        IProgress<PackageProgress>? progress,
        CancellationToken cancellationToken)
    {
        string[] files = Directory.EnumerateFiles(stagingRoot, "*", SearchOption.AllDirectories)
            .OrderBy(path => RelativeZipPath(stagingRoot, path), StringComparer.Ordinal)
            .ToArray();

        await using FileStream output = new(
            zipPath,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 128 * 1024,
            useAsync: true);
        using var archive = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true, Utf8NoBom);
        for (int index = 0; index < files.Length; index++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string source = files[index];
            string relative = RelativeZipPath(stagingRoot, source);
            progress?.Report(new("Archive", $"圧縮: {relative}", index + 1, files.Length));
            ZipArchiveEntry entry = archive.CreateEntry(relative, CompressionLevel.Optimal);
            entry.LastWriteTime = ZipEpoch;
            entry.ExternalAttributes = 0;
            await using Stream destination = entry.Open();
            await using FileStream input = new(
                source,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                bufferSize: 128 * 1024,
                useAsync: true);
            await input.CopyToAsync(destination, 128 * 1024, cancellationToken);
        }
    }

    private static async Task<string> Sha256Async(
        string file,
        CancellationToken cancellationToken)
    {
        await using FileStream stream = new(
            file,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 128 * 1024,
            useAsync: true);
        byte[] hash = await SHA256.HashDataAsync(stream, cancellationToken);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    private static async Task<(string Hash, long BytesRead)> HashArchiveEntryAsync(
        ZipArchiveEntry entry,
        CancellationToken cancellationToken)
    {
        await using Stream stream = entry.Open();
        using IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buffer = ArrayPool<byte>.Shared.Rent(128 * 1024);
        try
        {
            long total = 0;
            while (true)
            {
                int read = await stream.ReadAsync(
                    buffer.AsMemory(0, 128 * 1024),
                    cancellationToken);
                if (read == 0)
                    break;
                if (read > ArchiveUncompressedLimitBytes - total)
                {
                    throw new InvalidDataException(
                        "Package payload exceeds the supported uncompressed size limit.");
                }
                total += read;
                hash.AppendData(buffer, 0, read);
            }
            return (
                Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant(),
                total);
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    private static void ValidateArchiveEntryPath(string path)
    {
        if (string.IsNullOrWhiteSpace(path) ||
            path.Length > 1024 ||
            path[0] == '/' ||
            path[^1] == '/' ||
            path.Contains('\\') ||
            path.Contains('\0') ||
            path.Contains(':'))
        {
            throw new InvalidDataException($"Package archive path is invalid: {path}");
        }

        foreach (string segment in path.Split('/'))
        {
            if (segment.Length == 0 ||
                segment is "." or ".." ||
                segment[^1] is '.' or ' ' ||
                segment.Any(character =>
                    character < ' ' ||
                    character is '<' or '>' or '"' or '|' or '?' or '*') ||
                IsWindowsDeviceName(segment))
            {
                throw new InvalidDataException($"Package archive path is invalid: {path}");
            }
        }
    }

    private static bool IsWindowsDeviceName(string segment)
    {
        string stem = segment.Split('.', 2)[0];
        return stem.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
               stem.Equals("PRN", StringComparison.OrdinalIgnoreCase) ||
               stem.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
               stem.Equals("NUL", StringComparison.OrdinalIgnoreCase) ||
               (stem.Length == 4 &&
                (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) ||
                 stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)) &&
                stem[3] is >= '1' and <= '9');
    }

    private static bool IsLowerHexSha256(string value) =>
        value is { Length: 64 } &&
        value.All(character =>
            character is (>= '0' and <= '9') or (>= 'a' and <= 'f'));

    private static string ComputeBuildId(IReadOnlyList<ManifestFile> files)
    {
        var canonical = new StringBuilder();
        foreach (ManifestFile file in files)
            canonical.Append(file.path).Append('\0')
                     .Append(file.size).Append('\0')
                     .Append(file.sha256).Append('\n');
        byte[] hash = SHA256.HashData(Utf8NoBom.GetBytes(canonical.ToString()));
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    private static string ResolveUnderRoot(string root, string relativeOrAbsolute)
    {
        string value = relativeOrAbsolute.Replace('/', Path.DirectorySeparatorChar);
        return Path.GetFullPath(Path.IsPathRooted(value) ? value : Path.Combine(root, value));
    }

    private static bool HasSupportedSceneExtension(string path)
    {
        string extension = Path.GetExtension(path);
        return string.Equals(extension, ".acscene", StringComparison.OrdinalIgnoreCase) ||
               string.Equals(extension, ".acs3d", StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryReadIniValue(
        string file,
        string wantedSection,
        string wantedKey,
        out string value,
        CancellationToken cancellationToken = default)
    {
        value = "";
        string section = "";
        foreach (string rawLine in File.ReadLines(file, Encoding.UTF8))
        {
            cancellationToken.ThrowIfCancellationRequested();
            string line = rawLine.Trim();
            if (line.Length == 0 ||
                line.StartsWith(';') ||
                line.StartsWith('#'))
            {
                continue;
            }
            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                section = line[1..^1].Trim();
                continue;
            }
            if (!string.Equals(
                    section,
                    wantedSection,
                    StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            int equals = line.IndexOf('=');
            if (equals <= 0)
                continue;
            string key = line[..equals].Trim();
            if (!string.Equals(
                    key,
                    wantedKey,
                    StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            value = line[(equals + 1)..].Trim();
            return true;
        }
        return false;
    }

    private static bool ContainsCMakeUnsafeCharacter(string path) =>
        path.IndexOfAny([';', '\r', '\n']) >= 0;

    private static bool IsWithin(string root, string candidate)
    {
        string normalizedRoot = Path.GetFullPath(root)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) +
            Path.DirectorySeparatorChar;
        string normalizedCandidate = Path.GetFullPath(candidate);
        return normalizedCandidate.StartsWith(normalizedRoot, StringComparison.OrdinalIgnoreCase) ||
               string.Equals(
                   normalizedCandidate.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                   normalizedRoot.TrimEnd(Path.DirectorySeparatorChar),
                   StringComparison.OrdinalIgnoreCase);
    }

    private static bool HasReparsePointBetween(string root, string file)
    {
        string current = Path.GetFullPath(file);
        string normalizedRoot = Path.GetFullPath(root)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        while (IsWithin(normalizedRoot, current))
        {
            if (IsReparsePoint(current))
                return true;
            if (string.Equals(
                    current.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                    normalizedRoot,
                    StringComparison.OrdinalIgnoreCase))
                break;
            string? parent = Path.GetDirectoryName(current);
            if (string.IsNullOrEmpty(parent) || string.Equals(parent, current, StringComparison.OrdinalIgnoreCase))
                break;
            current = parent;
        }
        return false;
    }

    private static bool IsReparsePoint(string path) =>
        (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;

    /// <summary>
    /// Rejects an existing symlink/junction anywhere between a path and its
    /// filesystem root. Missing descendants are safe to create only after all
    /// of their existing ancestors have passed this check.
    /// </summary>
    private static void RejectExistingReparsePointsInPath(
        string path,
        string label)
    {
        string current = Path.GetFullPath(path);
        while (!string.IsNullOrEmpty(current))
        {
            if ((Directory.Exists(current) || File.Exists(current)) &&
                IsReparsePoint(current))
            {
                throw new IOException(
                    $"{label} が reparse point を経由しています: {current}");
            }

            string? parent = Path.GetDirectoryName(current);
            if (string.IsNullOrEmpty(parent) ||
                string.Equals(parent, current, StringComparison.OrdinalIgnoreCase))
            {
                break;
            }
            current = parent;
        }
    }

    private static void RejectReparsePoint(string path, string label)
    {
        if (IsReparsePoint(path))
            throw new IOException($"{label} が reparse point です: {path}");
    }

    private static string RelativeZipPath(string root, string file) =>
        Path.GetRelativePath(root, file).Replace('\\', '/');

    private static void CopyFile(string source, string destination)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        File.Copy(source, destination, overwrite: true);
    }

    private static string SanitizeFileName(string name)
    {
        var invalid = Path.GetInvalidFileNameChars().ToHashSet();
        var result = new StringBuilder();
        foreach (char value in name.Trim())
            result.Append(invalid.Contains(value) || char.IsControl(value) ? '_' : value);
        string sanitized = result.ToString().Trim().TrimEnd('.');
        return string.IsNullOrEmpty(sanitized) ? "Game" : sanitized;
    }

    internal static async Task<bool> TryDeleteFileWithRetryAsync(string file)
    {
        string full;
        try
        {
            full = Path.GetFullPath(file);
        }
        catch
        {
            return false;
        }

        int[] delays = [0, 40, 120, 300];
        foreach (int delay in delays)
        {
            if (delay != 0)
                await Task.Delay(delay).ConfigureAwait(false);
            try
            {
                if (!File.Exists(full))
                    return true;
                if (IsReparsePoint(full))
                    return false;
                File.Delete(full);
                if (!File.Exists(full))
                    return true;
            }
            catch
            {
            }
        }
        return !File.Exists(full);
    }

    internal static async Task<bool> TryDeleteDirectoryWithRetryAsync(
        string target,
        string allowedParent)
    {
        string fullTarget;
        string fullParent;
        try
        {
            fullTarget = Path.GetFullPath(target);
            fullParent = Path.GetFullPath(allowedParent);
        }
        catch
        {
            return false;
        }
        if (!IsWithin(fullParent, fullTarget) ||
            string.Equals(
                fullTarget,
                fullParent,
                StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        int[] delays = [0, 40, 120, 300];
        foreach (int delay in delays)
        {
            if (delay != 0)
                await Task.Delay(delay).ConfigureAwait(false);
            try
            {
                if (!Directory.Exists(fullTarget))
                    return true;
                if (IsReparsePoint(fullTarget))
                    return false;
                Directory.Delete(fullTarget, recursive: true);
                if (!Directory.Exists(fullTarget))
                    return true;
            }
            catch
            {
            }
        }
        return !Directory.Exists(fullTarget);
    }
}
