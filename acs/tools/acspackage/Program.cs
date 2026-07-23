// SPDX-License-Identifier: Apache-2.0

using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using AcsEditor;
using AcsEditor.Packaging;

namespace AcsPackage;

internal static class Program
{
    private sealed class CaptureProgress<T> : IProgress<T>
    {
        public List<T> Items { get; } = [];

        public void Report(T value) => Items.Add(value);
    }

    private sealed class ProjectManifest
    {
        public int version { get; set; } = 1;
        public string name { get; set; } = "";
        public string engineVersion { get; set; } = "";
        public string initialScene { get; set; } = "Assets/main.acscene";
        public string canonicalSceneAssetId { get; set; } = "";
    }

    private sealed record CommandOptions(
        string ProjectFile,
        string OutputDirectory,
        string ProductVersion,
        string? EngineRoot,
        bool SkipBuild,
        bool IncludeSymbols,
        PackageProfile Profile);

    public static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        if (args.Length == 1 && args[0] == "--self-test")
            return await RunSelfTestAsync();
        if (args.Length >= 2 && args[0] == "deps")
        {
            string executable = Path.GetFullPath(args[1]);
            RuntimeDependencyResolution resolution =
                await RuntimeDependencyResolver.ResolveAsync(
                    executable,
                    new[] { Path.GetDirectoryName(executable)! }
                        .Concat(args.Skip(2).Select(Path.GetFullPath)),
                    line => Console.WriteLine("[deps] " + line));
            foreach (string dependency in resolution.Dependencies)
                Console.WriteLine("RUNTIME " + dependency);
            foreach (string unresolved in resolution.Unresolved)
                Console.WriteLine("UNRESOLVED " + unresolved);
            return resolution.Unresolved.Count == 0 ? 0 : 1;
        }
        if (args.Length < 2 || args[0] is not ("validate" or "package"))
        {
            PrintUsage();
            return 2;
        }

        try
        {
            CommandOptions options = ParseOptions(args);
            PackageProjectInfo project = LoadProject(options.ProjectFile);
            var packageOptions = new PackageOptions(
                options.OutputDirectory,
                options.ProductVersion,
                options.IncludeSymbols,
                options.Profile);
            string executable = Path.Combine(
                project.RootDirectory,
                "Binaries",
                "Release",
                PackageCore.SanitizeIdentifier(project.Name) + ".exe");

            if (args[0] == "package" && !options.SkipBuild)
            {
                string engineRoot = FindEngineRoot(options.EngineRoot)
                    ?? throw new DirectoryNotFoundException(
                        "ACS root を検出できません。--engine-root <acs-dir> を指定してください。");
                await BuildReleaseAsync(project, engineRoot);
            }

            string? resolvedEngineRoot = FindEngineRoot(options.EngineRoot);
            string? assetPackTool = resolvedEngineRoot == null
                ? null
                : FindAssetPackTool(resolvedEngineRoot);
            if (args[0] == "package" && assetPackTool == null)
            {
                if (resolvedEngineRoot == null)
                {
                    throw new DirectoryNotFoundException(
                        "Cook tool用のACS rootを検出できません。--engine-rootを指定してください。");
                }
                assetPackTool = await EnsureAssetPackToolAsync(
                    resolvedEngineRoot);
            }
            packageOptions = packageOptions with
            {
                AssetPackToolPath = assetPackTool
            };

            IReadOnlyList<string> dependencies = Array.Empty<string>();
            var dependencyIssues = new List<PackageIssue>();
            if (File.Exists(executable))
            {
                string? engineRoot = FindEngineRoot(options.EngineRoot);
                var search = new List<string> { Path.GetDirectoryName(executable)! };
                if (engineRoot != null)
                {
                    search.Add(Path.Combine(engineRoot, "Binaries", "Release"));
                    search.Add(Path.Combine(engineRoot, "Binaries"));
                }

                RuntimeDependencyResolution resolution =
                    await RuntimeDependencyResolver.ResolveAsync(
                        executable,
                        search,
                        line => Console.WriteLine("[deps] " + line));
                dependencies = resolution.Dependencies;
                dependencyIssues.AddRange(resolution.Unresolved.Select(name => new PackageIssue(
                    PackageIssueSeverity.Error,
                    "RUNTIME_UNRESOLVED",
                    $"必要なランタイムDLLを解決できません: {name}")));
            }

            IReadOnlyList<PackageIssue> validation =
                PackageCore.Validate(project, packageOptions, executable, dependencies)
                    .Concat(dependencyIssues)
                    .ToArray();
            PrintIssues(validation);
            if (validation.Any(issue => issue.Severity == PackageIssueSeverity.Error))
                return 1;
            if (args[0] == "validate")
            {
                Console.WriteLine("Validation succeeded.");
                return 0;
            }

            var progress = new Progress<PackageProgress>(item =>
                Console.WriteLine($"[{item.Phase}] {item.Message}"));
            PackageResult result = await PackageCore.CreatePackageAsync(
                project,
                packageOptions,
                executable,
                dependencies,
                progress);
            Console.WriteLine($"Package: {result.ZipPath}");
            Console.WriteLine($"Build ID: {result.BuildId}");
            Console.WriteLine($"Files: {result.FileCount}, bytes: {result.UncompressedBytes}");
            return 0;
        }
        catch (PackageValidationException error)
        {
            PrintIssues(error.Issues);
            return 1;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("ERROR: " + error.Message);
            return 1;
        }
    }

    private static CommandOptions ParseOptions(string[] args)
    {
        string project = Path.GetFullPath(args[1]);
        string root = Path.GetDirectoryName(project) ?? Environment.CurrentDirectory;
        string output = Path.Combine(root, "Build", "Packages");
        string version = "0.1.0";
        string? engineRoot = null;
        bool skipBuild = false;
        bool includeSymbols = false;
        PackageProfile profile = PackageProfile.Shipping;

        for (int index = 2; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--output":
                    output = Path.GetFullPath(NextValue(args, ref index, "--output"));
                    break;
                case "--version":
                    version = NextValue(args, ref index, "--version");
                    break;
                case "--engine-root":
                    engineRoot = Path.GetFullPath(NextValue(args, ref index, "--engine-root"));
                    break;
                case "--skip-build":
                    skipBuild = true;
                    break;
                case "--include-symbols":
                    includeSymbols = true;
                    break;
                case "--profile":
                    if (!Enum.TryParse(
                            NextValue(args, ref index, "--profile"),
                            ignoreCase: true,
                            out profile))
                    {
                        throw new ArgumentException(
                            "--profileはDevelopment、Test、Shippingのいずれかです。");
                    }
                    break;
                default:
                    throw new ArgumentException($"不明な引数です: {args[index]}");
            }
        }
        return new(
            project,
            output,
            version,
            engineRoot,
            skipBuild,
            includeSymbols,
            profile);
    }

    private static string NextValue(string[] args, ref int index, string option)
    {
        if (++index >= args.Length)
            throw new ArgumentException($"{option} には値が必要です。");
        return args[index];
    }

    private static PackageProjectInfo LoadProject(string projectFile)
    {
        projectFile = Path.GetFullPath(projectFile);
        if (!File.Exists(projectFile))
            throw new FileNotFoundException(".acsproject が見つかりません。", projectFile);
        string? cursor = projectFile;
        while (!string.IsNullOrEmpty(cursor))
        {
            if ((File.Exists(cursor) || Directory.Exists(cursor)) &&
                (File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException(
                    $".acsproject が reparse point を経由しています: {cursor}");
            }
            string? parent = Path.GetDirectoryName(cursor);
            if (string.IsNullOrEmpty(parent) ||
                string.Equals(parent, cursor, StringComparison.OrdinalIgnoreCase))
                break;
            cursor = parent;
        }
        ProjectManifest dto = JsonSerializer.Deserialize<ProjectManifest>(
                                  File.ReadAllText(projectFile, Encoding.UTF8))
                              ?? throw new InvalidDataException(".acsproject のJSONが不正です。");
        return new(
            string.IsNullOrWhiteSpace(dto.name)
                ? Path.GetFileNameWithoutExtension(projectFile)
                : dto.name,
            dto.version,
            dto.engineVersion,
            Path.GetFullPath(projectFile),
            string.IsNullOrWhiteSpace(dto.initialScene)
                ? "Assets/main.acscene"
                : dto.initialScene,
            dto.canonicalSceneAssetId ?? "");
    }

    private static async Task BuildReleaseAsync(
        PackageProjectInfo project,
        string engineRoot)
    {
        string source = Path.Combine(project.RootDirectory, "Source");
        string cmakeLists = Path.Combine(source, "CMakeLists.txt");
        if (!File.Exists(cmakeLists))
            throw new FileNotFoundException(
                "Source/CMakeLists.txt がありません。Editorでプロジェクトを一度Buildしてください。",
                cmakeLists);

        string build = Path.Combine(engineRoot, "Intermediate", "acspackage");
        Console.WriteLine("Configuring Release package build…");
        int configure = await RunProcessAsync(
            "cmake",
            [
                "-S", Path.Combine(engineRoot, "engine"),
                "-B", build,
                "-DACS_EXTERNAL_PROJECT_DIR=" + source.Replace('\\', '/'),
                "-DACS_BUILD_SAMPLES=OFF",
                "-DACS_BUILD_TESTS=OFF",
                "-DACS_BUILD_TOOLS=ON",
                "-DACS_RENDER_DX12_RAW=ON",
                "-DACS_RENDER_DILIGENT=OFF",
            ],
            engineRoot);
        if (configure != 0)
            throw new InvalidOperationException($"CMake configure が失敗しました (exit {configure})。");

        string target = PackageCore.SanitizeIdentifier(project.Name);
        Console.WriteLine($"Building {target} (Release)…");
        int buildResult = await RunProcessAsync(
            "cmake",
            [
                "--build", build,
                "--target", target,
                "--target", "acs_assetpack_cli",
                "--config", "Release",
            ],
            engineRoot);
        if (buildResult != 0)
            throw new InvalidOperationException($"Release build が失敗しました (exit {buildResult})。");
    }

    private static async Task<int> RunProcessAsync(
        string fileName,
        IEnumerable<string> arguments,
        string workingDirectory)
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
            if (eventArgs.Data != null)
                Console.WriteLine(eventArgs.Data);
        };
        process.ErrorDataReceived += (_, eventArgs) =>
        {
            if (eventArgs.Data != null)
                Console.Error.WriteLine(eventArgs.Data);
        };
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await process.WaitForExitAsync();
        return process.ExitCode;
    }

    private static string? FindEngineRoot(string? explicitRoot)
    {
        if (!string.IsNullOrEmpty(explicitRoot) &&
            File.Exists(Path.Combine(explicitRoot, "engine", "CMakeLists.txt")))
            return Path.GetFullPath(explicitRoot);

        foreach (string start in new[] { Environment.CurrentDirectory, AppContext.BaseDirectory })
        {
            var directory = new DirectoryInfo(start);
            while (directory != null)
            {
                if (File.Exists(Path.Combine(directory.FullName, "engine", "CMakeLists.txt")))
                    return directory.FullName;
                directory = directory.Parent;
            }
        }
        return null;
    }

    private static string? FindAssetPackTool(string engineRoot)
    {
        string installed = Path.Combine(
            engineRoot,
            "Binaries",
            "Release",
            "acs_assetpack.exe");
        if (File.Exists(installed))
            return installed;

        string[] builds =
        [
            Path.Combine(engineRoot, "Intermediate", "acspackage"),
            Path.Combine(engineRoot, "Intermediate", "assetpack_tool"),
        ];
        foreach (string build in builds)
        {
            string[] candidates =
            [
                Path.Combine(
                    build,
                    "tools",
                    "acs_assetpack",
                    "Release",
                    "acs_assetpack.exe"),
                Path.Combine(
                    build,
                    "tools",
                    "acs_assetpack",
                    "acs_assetpack.exe"),
            ];
            string? found = candidates.FirstOrDefault(File.Exists);
            if (found != null)
                return found;
        }
        return null;
    }

    private static async Task<string> EnsureAssetPackToolAsync(
        string engineRoot)
    {
        string? existing = FindAssetPackTool(engineRoot);
        if (existing != null)
            return existing;

        string build = Path.Combine(
            engineRoot,
            "Intermediate",
            "assetpack_tool");
        Console.WriteLine("Configuring acs_assetpack Cook tool…");
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
            engineRoot);
        if (configure != 0)
        {
            throw new InvalidOperationException(
                $"acs_assetpack CMake configureに失敗しました (exit {configure})。");
        }

        Console.WriteLine("Building acs_assetpack (Release)…");
        int buildResult = await RunProcessAsync(
            "cmake",
            [
                "--build", build,
                "--target", "acs_assetpack_cli",
                "--config", "Release",
            ],
            engineRoot);
        if (buildResult != 0)
        {
            throw new InvalidOperationException(
                $"acs_assetpack buildに失敗しました (exit {buildResult})。");
        }

        return FindAssetPackTool(engineRoot)
            ?? throw new FileNotFoundException(
                "build完了後にacs_assetpack.exeが見つかりません。");
    }

    private static void PrintIssues(IEnumerable<PackageIssue> issues)
    {
        foreach (PackageIssue issue in issues)
        {
            string path = string.IsNullOrEmpty(issue.Path) ? "" : $" ({issue.Path})";
            Console.WriteLine($"{issue.Severity.ToString().ToUpperInvariant()} [{issue.Code}] {issue.Message}{path}");
        }
    }

    private static void PrintUsage()
    {
        Console.WriteLine(
            """
            ACS game packaging

              acspackage validate <project.acsproject> [options]
              acspackage package  <project.acsproject> [options]
              acspackage deps <game.exe> [additional-search-dir ...]
              acspackage --self-test

            Options:
              --output <dir>          ZIP output directory (default: Build/Packages)
              --version <semver>      Product/package version (default: 0.1.0)
              --profile <name>        Development, Test, or Shipping (default)
              --engine-root <dir>     ACS directory containing engine/CMakeLists.txt
              --skip-build            Package an existing Release executable
              --include-symbols        Include game PDB (Development/Test only)
            """);
    }

    private static async Task<int> RunSelfTestAsync()
    {
        string engineRoot = FindEngineRoot(null)
            ?? throw new DirectoryNotFoundException(
                "SELF-TESTにはACS engine rootが必要です。repo内から実行してください。");
        string assetPackTool = await EnsureAssetPackToolAsync(engineRoot);
        string testRoot = Path.Combine(
            Path.GetTempPath(),
            "acs-package-selftest-" + Guid.NewGuid().ToString("N"));
        try
        {
            string projectRoot = Path.Combine(testRoot, "Project");
            string assets = Path.Combine(projectRoot, "Assets");
            string textures = Path.Combine(assets, "Textures");
            string config = Path.Combine(projectRoot, "Config");
            string binaries = Path.Combine(projectRoot, "Binaries", "Release");
            Directory.CreateDirectory(textures);
            Directory.CreateDirectory(config);
            Directory.CreateDirectory(binaries);

            string albedo = Path.Combine(textures, "albedo.png");
            string normal = Path.Combine(textures, "normal detail.png");
            string material = Path.Combine(assets, "Water.acsmat");
            string scene = Path.Combine(assets, "main.acscene");
            File.WriteAllBytes(albedo, [1, 2, 3, 4, 5]);
            File.WriteAllBytes(normal, [9, 8, 7, 6]);
            File.WriteAllText(
                Path.Combine(textures, "albedo.png.tmp-editor"),
                "temporary");
            var passThroughAssets = new Dictionary<string, byte[]>
            {
                ["Blueprints/Player.acsbp"] =
                    Encoding.UTF8.GetBytes("ACSBP 1\n"),
                ["Fonts/Ui.ttf"] = [0, 1, 2, 3],
                ["Textures/modern.webp"] = [4, 5, 6],
                ["Textures/modern.ktx2"] = [7, 8, 9],
                ["Textures/linear.exr"] = [10, 11, 12],
                ["Data/game.toml"] =
                    Encoding.UTF8.GetBytes("name = \"Game\"\n"),
                ["Shaders/game.cso"] = [13, 14],
                ["Shaders/game.dxil"] = [15, 16],
                ["Shaders/game.spv"] = [17, 18],
            };
            foreach ((string relative, byte[] content) in passThroughAssets)
            {
                string path = Path.Combine(
                    assets,
                    relative.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllBytes(path, content);
            }
            File.WriteAllText(
                material,
                $"ACSMAT 1\nnormal {normal}\nsubstrateExprTexture0 {albedo}\n",
                new UTF8Encoding(false));
            File.WriteAllText(
                scene,
                $"ACSCENE v1\n1\nSPRT 1 {albedo}\nMAT 1 {material}\n",
                new UTF8Encoding(false));

            var assetDatabase = new AssetDatabase(projectRoot, assets);
            assetDatabase.Refresh(verifyContent: true);
            AssetRecord sceneAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "main.acscene");
            AssetRecord materialAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "Water.acsmat");
            AssetRecord albedoAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "Textures/albedo.png");
            AssetRecord normalAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "Textures/normal detail.png");
            assetDatabase.UpdateImportMetadata(
                sceneAsset.AssetId,
                sceneAsset.Metadata.Source,
                "legacy-acscene",
                1,
                [albedoAsset.AssetId, materialAsset.AssetId],
                new Dictionary<string, string>
                {
                    ["scene.subsystems"] = "renderer2d",
                });
            assetDatabase.UpdateImportMetadata(
                materialAsset.AssetId,
                materialAsset.Metadata.Source,
                materialAsset.Metadata.Importer,
                materialAsset.Metadata.ImporterVersion,
                [albedoAsset.AssetId, normalAsset.AssetId],
                materialAsset.Metadata.ImportSettings);
            File.WriteAllText(Path.Combine(config, "ProjectSettings.ini"), "[Game]\nQuality=High\n");

            string projectFile = Path.Combine(projectRoot, "Game.acsproject");
            File.WriteAllText(
                projectFile,
                $$"""
                {
                  "version": 1,
                  "name": "Game",
                  "engineVersion": "self-test",
                  "initialScene": "Assets/main.acscene",
                  "canonicalSceneAssetId": "{{sceneAsset.AssetId}}"
                }
                """,
                new UTF8Encoding(false));

            string executable = Path.Combine(binaries, "Game.exe");
            string runtime = Path.Combine(binaries, "Runtime.dll");
            File.WriteAllBytes(executable, Encoding.ASCII.GetBytes("dummy executable"));
            File.WriteAllBytes(runtime, Encoding.ASCII.GetBytes("dummy runtime"));
            File.WriteAllText(Path.Combine(binaries, "Game.pdb"), "debug only");
            File.WriteAllText(Path.Combine(binaries, "Game_reflect.dll"), "editor only");

            PackageProjectInfo project = LoadProject(projectFile);
            var optionsA = new PackageOptions(
                Path.Combine(testRoot, "OutputA"),
                "1.2.3",
                AssetPackToolPath: assetPackTool);
            var optionsB = optionsA with
            {
                OutputDirectory = Path.Combine(testRoot, "OutputB")
            };
            var firstProgress = new CaptureProgress<PackageProgress>();
            PackageResult first = await PackageCore.CreatePackageAsync(
                project,
                optionsA,
                executable,
                [runtime],
                firstProgress);
            var secondProgress = new CaptureProgress<PackageProgress>();
            PackageResult second = await PackageCore.CreatePackageAsync(
                project,
                optionsB,
                executable,
                [runtime],
                secondProgress);

            Assert(
                SHA256.HashData(File.ReadAllBytes(first.ZipPath))
                    .SequenceEqual(SHA256.HashData(File.ReadAllBytes(second.ZipPath))),
                "same inputs must produce byte-identical ZIP files");
            Assert(
                firstProgress.Items
                    .Where(item =>
                        item.Phase == "Cook" &&
                        item.Message.StartsWith("Cook (", StringComparison.Ordinal))
                    .Count() == first.CookedAssetCount &&
                firstProgress.Items
                    .Where(item =>
                        item.Phase == "Cook" &&
                        item.Message.StartsWith("Cook (", StringComparison.Ordinal))
                    .All(item =>
                        item.Message.Contains("(Miss)", StringComparison.Ordinal)) &&
                secondProgress.Items
                    .Where(item =>
                        item.Phase == "Cook" &&
                        item.Message.StartsWith("Cook (", StringComparison.Ordinal))
                    .Count() == second.CookedAssetCount &&
                secondProgress.Items
                    .Where(item =>
                        item.Phase == "Cook" &&
                        item.Message.StartsWith("Cook (", StringComparison.Ordinal))
                    .All(item =>
                        item.Message.Contains("(Hit)", StringComparison.Ordinal)),
                "first Cook populates DDC and repeated Cook reuses verified entries");

            using ZipArchive zip = ZipFile.OpenRead(first.ZipPath);
            string prefix = "Game-1.2.3-win64/";
            string[] names = zip.Entries.Select(entry => entry.FullName).ToArray();
            Assert(names.SequenceEqual(names.OrderBy(name => name, StringComparer.Ordinal)),
                "ZIP entries must use stable ordinal order");
            Assert(names.Contains(prefix + "Game.exe"), "game executable missing");
            Assert(names.Contains(prefix + "Runtime.dll"), "runtime DLL missing");
            Assert(!names.Any(name => name.EndsWith(".pdb", StringComparison.OrdinalIgnoreCase)),
                "PDB must be excluded by default");
            Assert(!names.Any(name => name.EndsWith("_reflect.dll", StringComparison.OrdinalIgnoreCase)),
                "reflection DLL must never be staged");
            Assert(names.Contains(prefix + "game.acpak"), "cooked asset pack missing");
            Assert(!names.Any(name => name.StartsWith(prefix + "Assets/", StringComparison.Ordinal)),
                "Shipping must not include redundant loose Assets");
            Assert(names.Contains(prefix + "Config/ProjectSettings.ini"), "config missing");
            Assert(!names.Contains(prefix + "main.acscene"),
                "Shipping initial scene must live only in game.acpak");
            Assert(zip.Entries.All(entry =>
                    entry.LastWriteTime.DateTime == new DateTime(1980, 1, 1, 0, 0, 0)),
                "ZIP timestamps must be fixed");

            string extractedPack = Path.Combine(testRoot, "game.acpak");
            await using (Stream source =
                (zip.GetEntry(prefix + "game.acpak")
                    ?? throw new InvalidDataException("game.acpak ZIP entry missing"))
                .Open())
            await using (FileStream destination = File.Create(extractedPack))
                await source.CopyToAsync(destination);
            Assert(
                await RunProcessAsync(
                    assetPackTool,
                    ["verify", extractedPack],
                    testRoot) == 0,
                "emitted game.acpak must pass native verify");
            string unpacked = Path.Combine(testRoot, "Unpacked");
            Assert(
                await RunProcessAsync(
                    assetPackTool,
                    ["unpack", extractedPack, unpacked],
                    testRoot) == 0,
                "native acpak unpack failed");

            string stagedScene = File.ReadAllText(
                Path.Combine(unpacked, "main.acscene"),
                Encoding.UTF8);
            string stagedMaterial = File.ReadAllText(
                Path.Combine(unpacked, "Assets", "Water.acsmat"),
                Encoding.UTF8);
            Assert(stagedScene.Contains("SPRT 1 Assets/Textures/albedo.png", StringComparison.Ordinal),
                "scene sprite path was not made portable");
            Assert(stagedScene.Contains("MAT 1 Assets/Water.acsmat", StringComparison.Ordinal),
                "scene material path was not made portable");
            Assert(stagedMaterial.Contains("normal Assets/Textures/normal detail.png", StringComparison.Ordinal),
                "material normal path was not made portable");
            Assert(!stagedScene.Contains(projectRoot, StringComparison.OrdinalIgnoreCase) &&
                   !stagedMaterial.Contains(projectRoot, StringComparison.OrdinalIgnoreCase),
                "source absolute paths leaked into package");
            Assert(!Directory.EnumerateFiles(
                       unpacked,
                       "*",
                       SearchOption.AllDirectories)
                   .Any(path =>
                       path.EndsWith(".acsmeta", StringComparison.OrdinalIgnoreCase) ||
                       path.Contains(
                           Path.DirectorySeparatorChar + ".acsdb" +
                           Path.DirectorySeparatorChar,
                           StringComparison.OrdinalIgnoreCase) ||
                       Path.GetFileName(path).Contains(
                           ".tmp-",
                           StringComparison.OrdinalIgnoreCase)),
                "asset database metadata/temp files leaked into Cook");
            foreach ((string relative, byte[] content) in passThroughAssets)
            {
                string cookedPath = Path.Combine(
                    unpacked,
                    "Assets",
                    relative.Replace('/', Path.DirectorySeparatorChar));
                Assert(
                    !File.Exists(cookedPath),
                    "unreachable allowlisted asset leaked into dependency-closure Cook: " +
                    relative);
            }

            using JsonDocument manifest = JsonDocument.Parse(
                ReadEntry(zip, prefix + "package-manifest.json"));
            Assert(
                manifest.RootElement.GetProperty("productVersion").GetString() == "1.2.3",
                "product version missing from manifest");
            Assert(
                manifest.RootElement.GetProperty("projectSchemaVersion").GetInt32() == 1,
                "project schema version missing from manifest");
            Assert(
                manifest.RootElement.GetProperty("buildId").GetString() == first.BuildId,
                "build ID mismatch");
            Assert(
                manifest.RootElement.GetProperty("canonicalSceneAssetId").GetString() ==
                    sceneAsset.AssetId &&
                manifest.RootElement.GetProperty("canonicalSceneKind").GetString() ==
                    "scene" &&
                manifest.RootElement.GetProperty("canonicalSceneImporter").GetString() ==
                    "legacy-acscene" &&
                manifest.RootElement.GetProperty("assetGraphHash").GetString() is
                    { Length: 64 },
                "manifest canonical scene identity, kind, importer provenance, and graph hash missing");
            JsonElement sceneBootstrap =
                manifest.RootElement.GetProperty("sceneBootstrap");
            Assert(
                sceneBootstrap.GetProperty("path").GetString() == "main.acscene" &&
                sceneBootstrap.GetProperty("contract").GetString() ==
                    "acs.scene.bootstrap.v1" &&
                sceneBootstrap.GetProperty("sourceFormat").GetString() ==
                    "legacy-acscene-v1" &&
                sceneBootstrap.GetProperty("adapterProjectionHint").GetString() ==
                    "orthographic",
                "reversible legacy scene bootstrap envelope missing from manifest");
            JsonElement manifestPack =
                manifest.RootElement.GetProperty("assetPack");
            string expectedPackHash = Convert.ToHexString(
                    SHA256.HashData(File.ReadAllBytes(extractedPack)))
                .ToLowerInvariant();
            Assert(
                manifestPack.GetProperty("sha256").GetString() ==
                    expectedPackHash &&
                first.AssetPackSha256 == expectedPackHash,
                "cooked pack hash missing or mismatched");
            Assert(
                manifest.RootElement.GetProperty("profile").GetString() ==
                    "Shipping" &&
                manifestPack.GetProperty("compressed").GetBoolean(),
                "Shipping profile semantics missing from manifest");

            var symbolsOptions = new PackageOptions(
                Path.Combine(testRoot, "OutputSymbols"),
                "1.2.3",
                IncludeDebugSymbols: true,
                Profile: PackageProfile.Test,
                AssetPackToolPath: assetPackTool);
            PackageResult symbolsResult = await PackageCore.CreatePackageAsync(
                project,
                symbolsOptions,
                executable,
                [runtime]);
            using (ZipArchive symbolsZip = ZipFile.OpenRead(symbolsResult.ZipPath))
            {
                string symbolsPrefix = symbolsResult.PackageId + "/";
                Assert(
                    symbolsZip.Entries.Any(entry =>
                        entry.FullName == symbolsPrefix + "Game.pdb"),
                    "game PDB must be included when explicitly requested and available");
                Assert(
                    !symbolsZip.Entries.Any(entry =>
                        entry.FullName.EndsWith("_reflect.dll", StringComparison.OrdinalIgnoreCase)),
                    "reflection DLL must remain excluded when symbols are enabled");
            }

            File.Delete(Path.Combine(binaries, "Game.pdb"));
            IReadOnlyList<PackageIssue> missingSymbolIssues =
                PackageCore.Validate(project, symbolsOptions, executable, [runtime]);
            Assert(
                missingSymbolIssues.Any(issue => issue.Code == "DEBUG_SYMBOLS_MISSING"),
                "missing opt-in PDB must produce an explicit warning");

            var developmentOptions = new PackageOptions(
                Path.Combine(testRoot, "OutputDevelopment"),
                "1.2.3",
                Profile: PackageProfile.Development,
                AssetPackToolPath: assetPackTool);
            PackageResult development =
                await PackageCore.CreatePackageAsync(
                    project,
                    developmentOptions,
                    executable,
                    [runtime]);
            using (ZipArchive developmentZip =
                   ZipFile.OpenRead(development.ZipPath))
            {
                string developmentPrefix = development.PackageId + "/";
                Assert(
                    developmentZip.Entries.Any(entry =>
                        entry.FullName ==
                        developmentPrefix + "Assets/Textures/albedo.png"),
                    "Development profile must retain loose cooked assets");
                Assert(
                    developmentZip.Entries.Any(entry =>
                        entry.FullName == developmentPrefix + "game.acpak"),
                    "Development profile must still emit game.acpak");
            }

            var traversalProject = project with { InitialScene = "../outside.acscene" };
            IReadOnlyList<PackageIssue> traversalIssues =
                PackageCore.Validate(traversalProject, optionsA, executable, [runtime]);
            Assert(traversalIssues.Any(issue => issue.Code == "INITIAL_SCENE_ESCAPE"),
                "initial-scene traversal must be rejected");

            var absoluteProject = project with { InitialScene = scene };
            IReadOnlyList<PackageIssue> absoluteIssues =
                PackageCore.Validate(absoluteProject, optionsA, executable, [runtime]);
            Assert(absoluteIssues.Any(issue => issue.Code == "INITIAL_SCENE_ABSOLUTE"),
                "absolute initial-scene references must be rejected");

            var identityMismatchProject = project with
            {
                CanonicalSceneAssetId = materialAsset.AssetId,
            };
            IReadOnlyList<PackageIssue> identityMismatchIssues =
                PackageCore.Validate(
                    identityMismatchProject,
                    optionsA,
                    executable,
                    [runtime]);
            Assert(
                identityMismatchIssues.Any(issue =>
                    issue.Code == "CANONICAL_SCENE_PATH_MISMATCH"),
                "canonical scene Asset ID/path divergence must fail closed");

            var nonAssetProject = project with { InitialScene = "Config/main.acscene" };
            IReadOnlyList<PackageIssue> nonAssetIssues =
                PackageCore.Validate(nonAssetProject, optionsA, executable, [runtime]);
            Assert(nonAssetIssues.Any(issue => issue.Code == "INITIAL_SCENE_ESCAPE"),
                "initial scene inside the project but outside Assets must be rejected");

            var wrongExtensionProject = project with { InitialScene = "Assets/main.txt" };
            IReadOnlyList<PackageIssue> wrongExtensionIssues =
                PackageCore.Validate(wrongExtensionProject, optionsA, executable, [runtime]);
            Assert(
                wrongExtensionIssues.Any(issue =>
                    issue.Code == "INITIAL_SCENE_EXTENSION"),
                "initial scene with an unsupported extension must be rejected");

            string settings = Path.Combine(config, "ProjectSettings.ini");
            File.WriteAllText(
                settings,
                "[Game]\nDefaultScene=Assets/other.acscene\n",
                new UTF8Encoding(false));
            IReadOnlyList<PackageIssue> defaultSceneIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                defaultSceneIssues.Any(issue =>
                    issue.Code == "DEFAULT_SCENE_MISMATCH"),
                "configured DefaultScene/project initialScene mismatch must fail closed");

            File.WriteAllText(
                settings,
                $"[Game]\nDefaultScene={scene}\n",
                new UTF8Encoding(false));
            IReadOnlyList<PackageIssue> absoluteDefaultSceneIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                absoluteDefaultSceneIssues.Any(issue =>
                    issue.Code == "DEFAULT_SCENE_INVALID"),
                "absolute configured DefaultScene must fail closed");

            File.WriteAllText(
                settings,
                "[Game]\nDefaultScene=Config/main.acscene\n",
                new UTF8Encoding(false));
            IReadOnlyList<PackageIssue> nonAssetDefaultSceneIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                nonAssetDefaultSceneIssues.Any(issue =>
                    issue.Code == "DEFAULT_SCENE_INVALID"),
                "configured DefaultScene outside Assets must fail closed");

            File.WriteAllText(
                settings,
                "[Game]\nDefaultScene=Assets/main.txt\n",
                new UTF8Encoding(false));
            IReadOnlyList<PackageIssue> wrongExtensionDefaultSceneIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                wrongExtensionDefaultSceneIssues.Any(issue =>
                    issue.Code == "DEFAULT_SCENE_INVALID"),
                "configured DefaultScene with an unsupported extension must fail closed");

            File.WriteAllText(
                settings,
                "[Game]\nDefaultScene=Assets/main.acscene\n",
                new UTF8Encoding(false));

            var unsafeOutput = optionsA with
            {
                OutputDirectory = Path.Combine(assets, "Packages")
            };
            IReadOnlyList<PackageIssue> outputIssues =
                PackageCore.Validate(project, unsafeOutput, executable, [runtime]);
            Assert(outputIssues.Any(issue => issue.Code == "OUTPUT_INSIDE_INPUT"),
                "output inside Assets must be rejected");

            string scene3d = Path.Combine(assets, "scene3d.acs3d");
            File.WriteAllText(
                scene3d,
                "ACS3D v2\n" +
                "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n",
                new UTF8Encoding(false));
            assetDatabase.Refresh(verifyContent: true);
            AssetRecord scene3DAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "scene3d.acs3d");
            assetDatabase.UpdateImportMetadata(
                scene3DAsset.AssetId,
                scene3DAsset.Metadata.Source,
                "legacy-acs3d",
                2,
                [],
                new Dictionary<string, string>
                {
                    ["scene.subsystems"] = "renderer3d",
                });
            var project3D = project with
            {
                InitialScene = "Assets/scene3d.acs3d",
                CanonicalSceneAssetId = scene3DAsset.AssetId,
            };
            IReadOnlyList<PackageIssue> scene3dIssues =
                PackageCore.Validate(project3D, optionsA, executable, [runtime]);
            Assert(
                !scene3dIssues.Any(issue =>
                    issue.Code == "RUNTIME_3D_SCENE_UNSUPPORTED" ||
                    issue.Code.StartsWith(
                        "SCENE3D_",
                        StringComparison.Ordinal) &&
                    issue.Severity == PackageIssueSeverity.Error),
                "supported ACS3D v2 subset must pass the reversible runtime adapter");

            File.WriteAllText(
                settings,
                "[Game]\nDefaultScene=Assets/scene3d.acs3d\n",
                new UTF8Encoding(false));
            File.WriteAllText(
                projectFile,
                $$"""
                {
                  "version": 1,
                  "name": "Game",
                  "engineVersion": "self-test",
                  "initialScene": "Assets/scene3d.acs3d",
                  "canonicalSceneAssetId": "{{scene3DAsset.AssetId}}"
                }
                """,
                new UTF8Encoding(false));
            PackageResult package3D = await PackageCore.CreatePackageAsync(
                project3D,
                optionsA with
                {
                    OutputDirectory = Path.Combine(testRoot, "Output3D"),
                },
                executable,
                [runtime]);
            using (ZipArchive zip3D = ZipFile.OpenRead(package3D.ZipPath))
            {
                string prefix3D = package3D.PackageId + "/";
                using JsonDocument manifest3D = JsonDocument.Parse(
                    ReadEntry(zip3D, prefix3D + "package-manifest.json"));
                JsonElement bootstrap3D =
                    manifest3D.RootElement.GetProperty("sceneBootstrap");
                Assert(
                    bootstrap3D.GetProperty("sourceFormat").GetString() ==
                        "legacy-acs3d-v2" &&
                    bootstrap3D.GetProperty("adapterProjectionHint").GetString() ==
                        "perspective" &&
                    manifest3D.RootElement.GetProperty(
                        "canonicalSceneAssetId").GetString() ==
                        scene3DAsset.AssetId,
                    "supported ACS3D scene must package with canonical identity and reversible adapter envelope");
            }

            File.WriteAllText(
                scene3d,
                "ACS3D v2\n" +
                "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n" +
                "SPR3D 1 Assets/Textures/albedo.png\n",
                new UTF8Encoding(false));
            assetDatabase.Refresh(verifyContent: true);
            scene3DAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "scene3d.acs3d");
            assetDatabase.UpdateImportMetadata(
                scene3DAsset.AssetId,
                scene3DAsset.Metadata.Source,
                "legacy-acs3d",
                2,
                [albedoAsset.AssetId],
                scene3DAsset.Metadata.ImportSettings);
            IReadOnlyList<PackageIssue> unsupported3DIssues =
                PackageCore.Validate(project3D, optionsA, executable, [runtime]);
            Assert(
                unsupported3DIssues.Any(issue =>
                    issue.Code == "SCENE3D_RUNTIME_DIRECTIVE_UNSUPPORTED"),
                "editor-only ACS3D directives must fail closed with an explicit adapter diagnostic");
            File.WriteAllText(
                settings,
                "[Game]\nDefaultScene=Assets/main.acscene\n",
                new UTF8Encoding(false));
            File.Delete(scene3d);
            File.Delete(scene3d + AssetDatabase.MetadataSuffix);
            assetDatabase.Refresh(verifyContent: true);
            File.WriteAllText(
                projectFile,
                $$"""
                {
                  "version": 1,
                  "name": "Game",
                  "engineVersion": "self-test",
                  "initialScene": "Assets/main.acscene",
                  "canonicalSceneAssetId": "{{sceneAsset.AssetId}}"
                }
                """,
                new UTF8Encoding(false));

            string unsupported = Path.Combine(assets, "payload.unsupported");
            File.WriteAllText(unsupported, "not cookable");
            IReadOnlyList<PackageIssue> unsupportedIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                unsupportedIssues.Any(issue =>
                    issue.Code == "ASSET_TYPE_UNSUPPORTED"),
                "unsupported Cook input must fail closed");
            File.Delete(unsupported);

            string externalGltf = Path.Combine(assets, "external.gltf");
            File.WriteAllText(
                externalGltf,
                "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"C:/outside.bin\",\"byteLength\":4}]}");
            IReadOnlyList<PackageIssue> gltfIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                gltfIssues.Any(issue =>
                    issue.Code == "GLTF_EXTERNAL_URI_UNSUPPORTED"),
                "external glTF URI must fail closed");
            File.Delete(externalGltf);

            await TestReparsePointIfSupportedAsync(
                testRoot,
                assets,
                executable,
                runtime,
                project,
                optionsA);

            Console.WriteLine(
                "SELF-TEST PASS: canonical Asset-ID dependency closure, DDC miss/hit reuse, " +
                "deterministic Cook/acpak+ZIP, native verify, canonical/bootstrap manifest, " +
                "2D+supported 3D package smoke, metadata/source exclusions, path rewrite, and " +
                "identity-mismatch/traversal/reparse/runtime-adapter guards.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("SELF-TEST FAIL: " + error);
            return 1;
        }
        finally
        {
            if (Directory.Exists(testRoot) &&
                Path.GetFullPath(testRoot).StartsWith(
                    Path.GetFullPath(Path.GetTempPath()),
                    StringComparison.OrdinalIgnoreCase))
            {
                try { Directory.Delete(testRoot, recursive: true); } catch { }
            }
        }
    }

    private static async Task TestReparsePointIfSupportedAsync(
        string testRoot,
        string assets,
        string executable,
        string runtime,
        PackageProjectInfo project,
        PackageOptions options)
    {
        string external = Path.Combine(Path.GetDirectoryName(assets)!, "External");
        string assetLink = Path.Combine(assets, "UnsafeLink");
        string tempLink = project.TempDirectory;
        string externalTemp = Path.Combine(testRoot, "ExternalTemp");
        string ddcLink = Path.Combine(tempLink, "DerivedDataCache");
        string externalDdc = Path.Combine(testRoot, "ExternalDdc");
        string outputLink = Path.Combine(testRoot, "OutputLink");
        string externalOutput = Path.Combine(testRoot, "ExternalOutput");
        bool assetLinkCreated = false;
        bool tempLinkCreated = false;
        bool ddcLinkCreated = false;
        bool outputLinkCreated = false;
        string linkedProjectRoot = Path.Combine(testRoot, "LinkedProjectRoot");
        bool projectRootLinkCreated = false;
        Directory.CreateDirectory(external);
        File.WriteAllText(Path.Combine(external, "secret.txt"), "must not escape");
        try
        {
            Directory.CreateSymbolicLink(assetLink, external);
            assetLinkCreated = true;
            IReadOnlyList<PackageIssue> issues =
                PackageCore.Validate(project, options, executable, [runtime]);
            Assert(issues.Any(issue => issue.Code is "REPARSE_POINT" or "INPUT_TREE_UNSAFE"),
                "asset reparse point must be rejected");

            Directory.Delete(assetLink);
            assetLinkCreated = false;

            Directory.CreateSymbolicLink(linkedProjectRoot, project.RootDirectory);
            projectRootLinkCreated = true;
            PackageProjectInfo linkedProject = project with
            {
                ProjectFilePath = Path.Combine(
                    linkedProjectRoot,
                    Path.GetFileName(project.ProjectFilePath))
            };
            IReadOnlyList<PackageIssue> linkedRootIssues =
                PackageCore.Validate(linkedProject, options, executable, [runtime]);
            Assert(
                linkedRootIssues.Any(issue => issue.Code == "INPUT_TREE_UNSAFE"),
                "project root reparse point must be rejected before Assets traversal");
            Directory.Delete(linkedProjectRoot);
            projectRootLinkCreated = false;

            if (Directory.Exists(tempLink))
            {
                Assert(
                    (File.GetAttributes(tempLink) & FileAttributes.ReparsePoint) == 0,
                    "self-test Temp must not already be a reparse point");
                Directory.Delete(tempLink, recursive: true);
            }
            Directory.CreateDirectory(externalTemp);
            Directory.CreateSymbolicLink(tempLink, externalTemp);
            tempLinkCreated = true;

            IReadOnlyList<PackageIssue> stagingIssues =
                PackageCore.Validate(project, options, executable, [runtime]);
            Assert(stagingIssues.Any(issue => issue.Code == "STAGING_REPARSE"),
                "staging ancestor reparse point must be rejected");
            bool stagingRejected = false;
            try
            {
                await PackageCore.CreatePackageAsync(
                    project,
                    options,
                    executable,
                    [runtime]);
            }
            catch (PackageValidationException error)
            {
                stagingRejected =
                    error.Issues.Any(issue => issue.Code == "STAGING_REPARSE");
            }
            Assert(stagingRejected,
                "packaging must fail before traversing a staging ancestor reparse point");
            Assert(!Directory.Exists(Path.Combine(externalTemp, "PackageStaging")),
                "staging validation must not create a directory through a reparse point");

            Directory.Delete(tempLink);
            tempLinkCreated = false;
            Directory.CreateDirectory(tempLink);

            Directory.CreateDirectory(externalDdc);
            Directory.CreateSymbolicLink(ddcLink, externalDdc);
            ddcLinkCreated = true;
            IReadOnlyList<PackageIssue> ddcIssues =
                PackageCore.Validate(project, options, executable, [runtime]);
            Assert(
                ddcIssues.Any(issue => issue.Code == "DDC_REPARSE"),
                "Derived Data Cache ancestor reparse point must be rejected");
            Directory.Delete(ddcLink);
            ddcLinkCreated = false;

            Directory.CreateDirectory(externalOutput);
            Directory.CreateSymbolicLink(outputLink, externalOutput);
            outputLinkCreated = true;
            var redirectedOutput = options with
            {
                OutputDirectory = Path.Combine(outputLink, "Nested")
            };
            IReadOnlyList<PackageIssue> outputIssues =
                PackageCore.Validate(project, redirectedOutput, executable, [runtime]);
            Assert(outputIssues.Any(issue => issue.Code == "OUTPUT_REPARSE"),
                "output ancestor reparse point must be rejected");
            bool outputRejected = false;
            try
            {
                await PackageCore.CreatePackageAsync(
                    project,
                    redirectedOutput,
                    executable,
                    [runtime]);
            }
            catch (PackageValidationException error)
            {
                outputRejected =
                    error.Issues.Any(issue => issue.Code == "OUTPUT_REPARSE");
            }
            Assert(outputRejected,
                "packaging must fail before traversing an output ancestor reparse point");
            Assert(!Directory.Exists(Path.Combine(externalOutput, "Nested")),
                "output validation must not create a directory through a reparse point");
        }
        catch (UnauthorizedAccessException)
        {
            Console.WriteLine("SELF-TEST NOTE: symlink test skipped (developer mode/privilege unavailable).");
        }
        catch (PlatformNotSupportedException)
        {
            Console.WriteLine("SELF-TEST NOTE: symlink test skipped on this platform.");
        }
        finally
        {
            try
            {
                if (assetLinkCreated && Directory.Exists(assetLink) &&
                    (File.GetAttributes(assetLink) & FileAttributes.ReparsePoint) != 0)
                    Directory.Delete(assetLink);
            }
            catch { }
            try
            {
                if (tempLinkCreated && Directory.Exists(tempLink) &&
                    (File.GetAttributes(tempLink) & FileAttributes.ReparsePoint) != 0)
                    Directory.Delete(tempLink);
            }
            catch { }
            try
            {
                if (ddcLinkCreated && Directory.Exists(ddcLink) &&
                    (File.GetAttributes(ddcLink) & FileAttributes.ReparsePoint) != 0)
                    Directory.Delete(ddcLink);
            }
            catch { }
            try
            {
                if (outputLinkCreated && Directory.Exists(outputLink) &&
                    (File.GetAttributes(outputLink) & FileAttributes.ReparsePoint) != 0)
                    Directory.Delete(outputLink);
            }
            catch { }
            try
            {
                if (projectRootLinkCreated && Directory.Exists(linkedProjectRoot) &&
                    (File.GetAttributes(linkedProjectRoot) & FileAttributes.ReparsePoint) != 0)
                    Directory.Delete(linkedProjectRoot);
            }
            catch { }
        }
    }

    private static string ReadEntry(ZipArchive zip, string name)
    {
        ZipArchiveEntry entry = zip.GetEntry(name)
            ?? throw new InvalidDataException("ZIP entry not found: " + name);
        using StreamReader reader = new(entry.Open(), Encoding.UTF8);
        return reader.ReadToEnd();
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }
}
