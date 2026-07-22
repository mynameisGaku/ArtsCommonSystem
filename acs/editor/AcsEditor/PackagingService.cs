// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using AcsEditor.Packaging;

namespace AcsEditor;

/// <summary>
/// Editor-facing orchestration: optional standalone Release build, exact PE
/// runtime dependency resolution, then the shared deterministic staging core.
/// </summary>
public static class PackagingService
{
    public static PackageProjectInfo ProjectInfo(Project project) => new(
        Name: project.Name,
        ProjectSchemaVersion: project.Version,
        EngineVersion: project.EngineVersion,
        ProjectFilePath: project.ProjectFilePath,
        InitialScene: project.InitialScene,
        CanonicalSceneAssetId: project.CanonicalSceneAssetId);

    public static IReadOnlyList<PackageIssue> Validate(
        Project project,
        PackageOptions options,
        IReadOnlyList<string>? runtimeDependencies = null)
    {
        string? engineRoot = BuildService.FindEngineRoot();
        string? assetPackTool = engineRoot == null
            ? null
            : FindAssetPackTool(engineRoot);
        return PackageCore.Validate(
            ProjectInfo(project),
            options with { AssetPackToolPath = assetPackTool },
            BuildService.ExePath(project),
            runtimeDependencies);
    }

    public static async Task<PackageResult> PackageAsync(
        Project project,
        PackageOptions options,
        bool buildRelease,
        bool forceConfigure,
        Action<string> log,
        IProgress<PackageProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        string executable = BuildService.ExePath(project);
        if (buildRelease)
        {
            log("==== Package: Release build ====");
            string? built = await BuildService.BuildAsync(
                project,
                log,
                forceConfigure,
                standalone: true,
                cancellationToken: cancellationToken);
            if (built == null)
                throw new InvalidOperationException("Release build に失敗したためパッケージを中止しました。");
            executable = built;
        }

        cancellationToken.ThrowIfCancellationRequested();
        string? engineRoot = BuildService.FindEngineRoot();
        if (engineRoot == null)
            throw new DirectoryNotFoundException("ACS engine root を検出できません。");

        string assetPackTool = await EnsureAssetPackToolAsync(
            engineRoot,
            log,
            cancellationToken);
        options = options with { AssetPackToolPath = assetPackTool };

        log("Release実行ファイルのランタイム依存DLLを解決しています…");
        RuntimeDependencyResolution dependencies =
            await RuntimeDependencyResolver.ResolveAsync(
                executable,
                [
                    Path.GetDirectoryName(executable)!,
                    Path.Combine(engineRoot, "Binaries", "Release"),
                    Path.Combine(engineRoot, "Binaries"),
                ],
                log,
                cancellationToken);

        if (dependencies.Unresolved.Count > 0)
        {
            throw new PackageValidationException(
                dependencies.Unresolved.Select(name => new PackageIssue(
                    PackageIssueSeverity.Error,
                    "RUNTIME_UNRESOLVED",
                    $"必要なランタイムDLLを解決できません: {name}"))
                .ToArray());
        }

        foreach (string dependency in dependencies.Dependencies)
            log("Runtime DLL: " + dependency);
        if (dependencies.Dependencies.Count == 0)
            log("追加の配布ランタイムDLLはありません (OS DLLは除外)。");

        return await PackageCore.CreatePackageAsync(
            ProjectInfo(project),
            options,
            executable,
            dependencies.Dependencies,
            progress,
            cancellationToken);
    }

    private static string? FindAssetPackTool(string engineRoot)
    {
        string[] candidates =
        [
            Path.Combine(
                engineRoot,
                "Binaries",
                "Release",
                "acs_assetpack.exe"),
            Path.Combine(
                engineRoot,
                "Intermediate",
                "assetpack_tool",
                "tools",
                "acs_assetpack",
                "Release",
                "acs_assetpack.exe"),
            Path.Combine(
                engineRoot,
                "Intermediate",
                "assetpack_tool",
                "tools",
                "acs_assetpack",
                "acs_assetpack.exe"),
        ];
        return candidates.FirstOrDefault(File.Exists);
    }

    private static async Task<string> EnsureAssetPackToolAsync(
        string engineRoot,
        Action<string> log,
        CancellationToken cancellationToken)
    {
        string? existing = FindAssetPackTool(engineRoot);
        if (existing != null)
            return existing;

        string build = Path.Combine(
            engineRoot,
            "Intermediate",
            "assetpack_tool");
        log("Configuring acs_assetpack Cook tool…");
        int configure = await RunProcessAsync(
            "cmake",
            [
                "-S", Path.Combine(engineRoot, "engine"),
                "-B", build,
                "-DACS_BUILD_TOOLS=ON",
                "-DACS_BUILD_SAMPLES=OFF",
                "-DACS_BUILD_TESTS=OFF",
                "-DACS_RENDER_DX12_RAW=ON",
                "-DACS_RENDER_DILIGENT=OFF",
            ],
            engineRoot,
            log,
            cancellationToken);
        if (configure != 0)
        {
            throw new InvalidOperationException(
                $"acs_assetpack CMake configureに失敗しました (exit {configure})。");
        }

        log("Building acs_assetpack (Release)…");
        int buildResult = await RunProcessAsync(
            "cmake",
            [
                "--build", build,
                "--target", "acs_assetpack_cli",
                "--config", "Release",
            ],
            engineRoot,
            log,
            cancellationToken);
        if (buildResult != 0)
        {
            throw new InvalidOperationException(
                $"acs_assetpack buildに失敗しました (exit {buildResult})。");
        }

        return FindAssetPackTool(engineRoot)
            ?? throw new FileNotFoundException(
                "build完了後にacs_assetpack.exeが見つかりません。");
    }

    private static async Task<int> RunProcessAsync(
        string fileName,
        IEnumerable<string> arguments,
        string workingDirectory,
        Action<string> log,
        CancellationToken cancellationToken)
    {
        var start = new ProcessStartInfo
        {
            FileName = fileName,
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

        using var process = new Process { StartInfo = start };
        process.OutputDataReceived += (_, eventArgs) =>
        {
            if (eventArgs.Data != null) log(eventArgs.Data);
        };
        process.ErrorDataReceived += (_, eventArgs) =>
        {
            if (eventArgs.Data != null) log(eventArgs.Data);
        };
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        try
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            try
            {
                if (!process.HasExited)
                    process.Kill(entireProcessTree: true);
            }
            catch { }
            throw;
        }
        return process.ExitCode;
    }
}
