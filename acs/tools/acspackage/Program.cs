// SPDX-License-Identifier: Apache-2.0

using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using AcsEditor;
using AcsEditor.Packaging;

namespace AcsPackage;

internal static partial class Program
{
    private const int InspectArchiveEntryLimit = 100_000;
    private const int InspectManifestLimitBytes = 4 * 1024 * 1024;
    private const long InspectCentralDirectoryLimitBytes = 128L * 1024 * 1024;
    private const long InspectArchiveUncompressedLimitBytes = 128L << 30;
    private const long InspectCompressionRatioCheckThresholdBytes = 16L << 20;
    private const long InspectMaximumCompressionRatio = 200;
    private const long InspectArchiveFileLimitBytes =
        129L << 30;
    private const int TerminalTextLimit = 512;
    private static readonly UTF8Encoding Utf8Strict = new(false, true);

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

    private sealed record VerifyCommandOptions(
        string ArchivePath,
        string? ReportPath,
        bool Quiet);

    private sealed record InspectCommandOptions(
        string ArchivePath,
        string? JsonPath,
        bool Quiet);

    private sealed record DiffCommandOptions(
        string LeftArchivePath,
        string RightArchivePath,
        string? JsonPath,
        bool Quiet);

    private sealed record VerificationReport(
        int schemaVersion,
        bool verified,
        string archivePath,
        DateTimeOffset verifiedUtc,
        string packageId,
        string buildId,
        int fileCount,
        long uncompressedBytes,
        string assetPackSha256,
        int cookedAssetCount,
        string profile,
        PackageProductMetadata? productMetadata,
        string? errorCode,
        string? errorMessage);

    private sealed record InspectedManifestFile(
        string path,
        long size,
        string sha256);

    private sealed record InspectedManifestAssetPack(
        string path,
        long size,
        string sha256,
        int formatVersion,
        bool compressed,
        int sourceFileCount);

    private sealed record InspectedPackageManifest(
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
        CanonicalSceneBootstrapEnvelope? sceneBootstrap,
        PackageProductMetadata? productMetadata,
        InspectedManifestAssetPack? assetPack,
        IReadOnlyList<InspectedManifestFile> files);

    private sealed record PackageFileInspection(
        string path,
        long size,
        string sha256);

    private sealed record PackageInspectionReport(
        int schemaVersion,
        bool verified,
        string archivePath,
        string archiveSha256,
        string packageId,
        int manifestSchemaVersion,
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
        CanonicalSceneBootstrapEnvelope? sceneBootstrap,
        PackageProductMetadata? productMetadata,
        InspectedManifestAssetPack? assetPack,
        int fileCount,
        long uncompressedBytes,
        IReadOnlyList<PackageFileInspection> files,
        string? errorCode,
        string? errorMessage);

    private sealed record PackageDiffSide(
        string archivePath,
        string archiveSha256,
        string packageId,
        string buildId,
        string productName,
        string productVersion,
        string profile,
        int fileCount,
        long uncompressedBytes);

    private sealed record PackageMetadataChange(
        string field,
        string left,
        string right);

    private sealed record PackageFileModification(
        string path,
        long leftSize,
        long rightSize,
        string leftSha256,
        string rightSha256);

    private sealed record PackageDiffReport(
        int schemaVersion,
        bool compared,
        bool identical,
        bool archiveBytesEqual,
        bool provenanceEqual,
        bool payloadsEqual,
        PackageDiffSide? left,
        PackageDiffSide? right,
        IReadOnlyList<PackageMetadataChange> metadataChanges,
        IReadOnlyList<PackageFileInspection> added,
        IReadOnlyList<PackageFileInspection> removed,
        IReadOnlyList<PackageFileModification> modified,
        int unchangedFileCount,
        string? errorCode,
        string? errorMessage);

    public static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        if (args.Length >= 1 &&
            args[0] == "--launch-smoke-self-test-child")
        {
            return await RunLaunchSmokeSelfTestChildAsync(args);
        }
        if (args.Length == 1 && args[0] == "--self-test")
            return await RunSelfTestAsync();
        if (args.Length >= 1 && args[0] == "distribution-e2e")
            return await RunDistributionE2eCommandAsync(args);
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
        if (args.Length >= 1 && args[0] == "verify")
            return await RunVerifyCommandWithDiagnosticsAsync(args);
        if (args.Length >= 1 && args[0] == "inspect")
            return await RunInspectCommandWithDiagnosticsAsync(args);
        if (args.Length >= 1 && args[0] == "diff")
            return await RunDiffCommandWithDiagnosticsAsync(args);
        if (args.Length >= 1 && args[0] == "smoke")
            return await RunSmokeCommandWithDiagnosticsAsync(args);
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
            Console.WriteLine(
                $"Archive verification: {(result.ArchiveVerified ? "PASS" : "NOT RUN")}");
            using var smokeCancellation = new CancellationTokenSource();
            ConsoleCancelEventHandler smokeCancelHandler = (_, eventArgs) =>
            {
                eventArgs.Cancel = true;
                smokeCancellation.Cancel();
            };
            Console.CancelKeyPress += smokeCancelHandler;
            try
            {
                PackageLaunchSmokeResult launch =
                    await RunPublishedPackageSmokeAsync(
                        result.ZipPath,
                        quiet: false,
                        smokeCancellation.Token);
                return launch.Report.Passed ? 0 : 1;
            }
            finally
            {
                Console.CancelKeyPress -= smokeCancelHandler;
            }
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine(
                "ERROR: Package launch smoke was cancelled; its process tree " +
                "was terminated and a cancellation report was written.");
            return 130;
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

    private static async Task<int> RunVerifyCommandWithDiagnosticsAsync(
        string[] args)
    {
        VerifyCommandOptions options;
        try
        {
            options = ParseVerifyOptions(args);
            if (options.ReportPath != null)
                ValidateNewReportDestination(options.ReportPath);
        }
        catch (ArgumentException error)
        {
            Console.Error.WriteLine("ERROR: " + error.Message);
            PrintVerifyUsage();
            return 2;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("ERROR: " + error.Message);
            return 1;
        }

        PackageVerificationResult verification;
        try
        {
            IProgress<PackageProgress>? progress = options.Quiet
                ? null
                : new Progress<PackageProgress>(item =>
                    Console.WriteLine($"[{item.Phase}] {item.Message}"));
            verification = await PackageCore.VerifyPackageArchiveAsync(
                options.ArchivePath,
                progress);
        }
        catch (Exception error)
        {
            if (options.ReportPath != null)
            {
                try
                {
                    await WriteVerificationReportAsync(
                        options.ReportPath,
                        CreateFailureVerificationReport(
                            options.ArchivePath,
                            error));
                }
                catch (Exception reportError)
                {
                    Console.Error.WriteLine(
                        "ERROR: 検証失敗レポートを書き込めません: " +
                        reportError.Message);
                }
            }

            Console.Error.WriteLine("ERROR: Package verification failed: " + error.Message);
            return 1;
        }

        if (options.ReportPath != null)
        {
            try
            {
                await WriteVerificationReportAsync(
                    options.ReportPath,
                    CreateSuccessfulVerificationReport(verification));
            }
            catch (Exception error)
            {
                Console.Error.WriteLine(
                    "ERROR: 検証レポートを書き込めません: " + error.Message);
                return 1;
            }
        }

        if (!options.Quiet)
        {
            Console.WriteLine("Package verification: PASS");
            Console.WriteLine($"Archive: {verification.ZipPath}");
            Console.WriteLine($"Package ID: {verification.PackageId}");
            Console.WriteLine($"Build ID: {verification.BuildId}");
            Console.WriteLine(
                $"Files: {verification.FileCount}, bytes: " +
                verification.UncompressedBytes);
            Console.WriteLine(
                $"Cooked assets: {verification.CookedAssetCount}, profile: " +
                verification.Profile);
            Console.WriteLine(
                "Asset pack SHA-256: " + verification.AssetPackSha256);
            if (options.ReportPath != null)
                Console.WriteLine("Report: " + options.ReportPath);
        }
        return 0;
    }

    private static async Task<int> RunInspectCommandWithDiagnosticsAsync(
        string[] args)
    {
        InspectCommandOptions options;
        try
        {
            options = ParseInspectOptions(args);
            if (options.JsonPath != null)
                ValidateNewReportDestination(options.JsonPath);
        }
        catch (ArgumentException error)
        {
            Console.Error.WriteLine(FormatTerminalText(
                "ERROR: " + error.Message));
            PrintInspectUsage();
            return 2;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(FormatTerminalText(
                "ERROR: " + error.Message));
            return 1;
        }

        bool jsonWriteStarted = false;
        PackageInspectionReport inspection;
        try
        {
            IProgress<PackageProgress>? progress = options.Quiet
                ? null
                : new Progress<PackageProgress>(item =>
                    Console.WriteLine(FormatTerminalText(
                        $"[{item.Phase}] {item.Message}")));
            inspection = await InspectPackageArchiveAsync(
                options.ArchivePath,
                progress);
            if (options.JsonPath != null)
            {
                jsonWriteStarted = true;
                await WriteNewJsonDocumentAsync(options.JsonPath, inspection);
            }
        }
        catch (Exception error)
        {
            if (options.JsonPath != null && !jsonWriteStarted)
            {
                try
                {
                    await WriteNewJsonDocumentAsync(
                        options.JsonPath,
                        CreateFailedInspectionReport(
                            options.ArchivePath,
                            error));
                }
                catch (Exception reportError)
                {
                    Console.Error.WriteLine(FormatTerminalText(
                        "ERROR: Could not write inspection failure JSON: " +
                        reportError.Message));
                }
            }

            Console.Error.WriteLine(FormatTerminalText(
                "ERROR: Package inspection failed: " + error.Message));
            return 1;
        }

        if (!options.Quiet)
        {
            Console.WriteLine(FormatTerminalText("Package inspection: PASS"));
            Console.WriteLine(FormatTerminalText(
                $"Archive: {inspection.archivePath}"));
            Console.WriteLine(FormatTerminalText(
                $"Archive SHA-256: {inspection.archiveSha256}"));
            Console.WriteLine(FormatTerminalText(
                $"Package ID: {inspection.packageId}"));
            Console.WriteLine(FormatTerminalText(
                $"Build ID: {inspection.buildId}"));
            Console.WriteLine(FormatTerminalText(
                $"Product: {inspection.productName} " +
                inspection.productVersion));
            if (inspection.productMetadata is { IsEmpty: false } metadata)
            {
                Console.WriteLine(FormatTerminalText(
                    $"Publisher: {metadata.Publisher}"));
                if (metadata.SupportUrl.Length > 0)
                {
                    Console.WriteLine(FormatTerminalText(
                        $"Support: {metadata.SupportUrl}"));
                }
            }
            Console.WriteLine(FormatTerminalText(
                $"Engine: {inspection.engineVersion}, profile: " +
                inspection.profile));
            Console.WriteLine(FormatTerminalText(
                $"Files: {inspection.fileCount}, bytes: " +
                inspection.uncompressedBytes));
            Console.WriteLine(FormatTerminalText(
                "Asset pack SHA-256: " +
                (inspection.assetPack?.sha256 ?? "")));
            if (options.JsonPath != null)
            {
                Console.WriteLine(FormatTerminalText(
                    "JSON: " + options.JsonPath));
            }
        }
        return 0;
    }

    private static async Task<int> RunDiffCommandWithDiagnosticsAsync(
        string[] args)
    {
        DiffCommandOptions options;
        try
        {
            options = ParseDiffOptions(args);
            if (options.JsonPath != null)
                ValidateNewReportDestination(options.JsonPath);
        }
        catch (ArgumentException error)
        {
            Console.Error.WriteLine(FormatTerminalText(
                "ERROR: " + error.Message));
            PrintDiffUsage();
            return 2;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(FormatTerminalText(
                "ERROR: " + error.Message));
            return 3;
        }

        bool jsonWriteStarted = false;
        PackageDiffReport comparison;
        try
        {
            IProgress<PackageProgress>? leftProgress = options.Quiet
                ? null
                : new Progress<PackageProgress>(item =>
                    Console.WriteLine(FormatTerminalText(
                        $"[left:{item.Phase}] {item.Message}")));
            IProgress<PackageProgress>? rightProgress = options.Quiet
                ? null
                : new Progress<PackageProgress>(item =>
                    Console.WriteLine(FormatTerminalText(
                        $"[right:{item.Phase}] {item.Message}")));
            PackageInspectionReport left = await InspectPackageArchiveAsync(
                options.LeftArchivePath,
                leftProgress);
            // A lexical same-path comparison is one point-in-time input, not two
            // sequential reads with an exchange window between them.
            PackageInspectionReport right = PathsEqual(
                options.LeftArchivePath,
                options.RightArchivePath)
                ? left
                : await InspectPackageArchiveAsync(
                    options.RightArchivePath,
                    rightProgress);
            comparison = CreatePackageDiffReport(left, right);
            if (options.JsonPath != null)
            {
                jsonWriteStarted = true;
                await WriteNewJsonDocumentAsync(options.JsonPath, comparison);
            }
        }
        catch (Exception error)
        {
            if (options.JsonPath != null && !jsonWriteStarted)
            {
                try
                {
                    await WriteNewJsonDocumentAsync(
                        options.JsonPath,
                        CreateFailedDiffReport(error));
                }
                catch (Exception reportError)
                {
                    Console.Error.WriteLine(FormatTerminalText(
                        "ERROR: Could not write diff failure JSON: " +
                        reportError.Message));
                }
            }

            Console.Error.WriteLine(FormatTerminalText(
                "ERROR: Package diff failed: " + error.Message));
            return 3;
        }

        if (!options.Quiet)
        {
            Console.WriteLine(FormatTerminalText(
                "Package diff: " +
                (comparison.identical ? "IDENTICAL" : "DIFFERENT")));
            Console.WriteLine(FormatTerminalText(
                "Archive bytes: " +
                (comparison.archiveBytesEqual ? "equal" : "different")));
            Console.WriteLine(FormatTerminalText(
                "Provenance: " +
                (comparison.provenanceEqual ? "equal" : "different")));
            Console.WriteLine(FormatTerminalText(
                "Payloads: " +
                (comparison.payloadsEqual ? "equal" : "different")));
            Console.WriteLine(FormatTerminalText(
                $"Metadata changes: {comparison.metadataChanges.Count}, " +
                $"added: {comparison.added.Count}, " +
                $"removed: {comparison.removed.Count}, " +
                $"modified: {comparison.modified.Count}, " +
                $"unchanged: {comparison.unchangedFileCount}"));
            foreach (PackageMetadataChange change in comparison.metadataChanges)
            {
                Console.WriteLine(FormatTerminalText(
                    $"META {change.field}: {change.left} -> {change.right}"));
            }
            foreach (PackageFileInspection file in comparison.added)
            {
                Console.WriteLine(FormatTerminalText(
                    "ADD " + file.path));
            }
            foreach (PackageFileInspection file in comparison.removed)
            {
                Console.WriteLine(FormatTerminalText(
                    "REMOVE " + file.path));
            }
            foreach (PackageFileModification file in comparison.modified)
            {
                Console.WriteLine(FormatTerminalText(
                    "MODIFY " + file.path));
            }
            if (options.JsonPath != null)
            {
                Console.WriteLine(FormatTerminalText(
                    "JSON: " + options.JsonPath));
            }
        }

        return comparison.identical ? 0 : 1;
    }

    private static string FormatTerminalText(string? value)
    {
        value ??= "";
        const string truncationSuffix = "...";
        int contentLimit = TerminalTextLimit - truncationSuffix.Length;
        var output = new StringBuilder(
            Math.Min(value.Length, TerminalTextLimit));
        bool truncated = false;

        foreach (char character in value)
        {
            string? escaped = character switch
            {
                '\r' => "\\r",
                '\n' => "\\n",
                '\t' => "\\t",
                '\u001B' => "\\x1B",
                >= '\0' and <= '\u001F' =>
                    $"\\x{(int)character:X2}",
                >= '\u007F' and <= '\u009F' =>
                    $"\\x{(int)character:X2}",
                _ when char.GetUnicodeCategory(character) is
                    System.Globalization.UnicodeCategory.Format or
                    System.Globalization.UnicodeCategory.LineSeparator or
                    System.Globalization.UnicodeCategory.ParagraphSeparator or
                    System.Globalization.UnicodeCategory.Surrogate =>
                    $"\\u{(int)character:X4}",
                _ => null,
            };

            int length = escaped?.Length ?? 1;
            if (output.Length + length > contentLimit)
            {
                truncated = true;
                break;
            }
            if (escaped is null)
                output.Append(character);
            else
                output.Append(escaped);
        }

        if (truncated)
            output.Append(truncationSuffix);
        return output.ToString();
    }

    private static InspectCommandOptions ParseInspectOptions(string[] args)
    {
        if (args.Length < 2 ||
            !string.Equals(args[0], "inspect", StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(args[1]) ||
            args[1].StartsWith("--", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "inspect requires one package.zip path.");
        }

        string archive = Path.GetFullPath(args[1]);
        string? json = null;
        bool quiet = false;
        for (int index = 2; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--json":
                    if (json != null)
                        throw new ArgumentException(
                            "--json may be specified only once.");
                    string value = NextValue(args, ref index, "--json");
                    if (string.IsNullOrWhiteSpace(value) ||
                        value.StartsWith("--", StringComparison.Ordinal))
                    {
                        throw new ArgumentException(
                            "--json requires a new output file path.");
                    }
                    json = Path.GetFullPath(value);
                    break;
                case "--quiet":
                    if (quiet)
                        throw new ArgumentException(
                            "--quiet may be specified only once.");
                    quiet = true;
                    break;
                default:
                    throw new ArgumentException(
                        $"Unknown inspect argument: {args[index]}");
            }
        }

        if (json != null && PathsEqual(archive, json))
        {
            throw new ArgumentException(
                "Inspection JSON must not replace the package archive.");
        }
        return new(archive, json, quiet);
    }

    private static DiffCommandOptions ParseDiffOptions(string[] args)
    {
        if (args.Length < 3 ||
            !string.Equals(args[0], "diff", StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(args[1]) ||
            string.IsNullOrWhiteSpace(args[2]) ||
            args[1].StartsWith("--", StringComparison.Ordinal) ||
            args[2].StartsWith("--", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "diff requires left-package.zip and right-package.zip paths.");
        }

        string left = Path.GetFullPath(args[1]);
        string right = Path.GetFullPath(args[2]);
        string? json = null;
        bool quiet = false;
        for (int index = 3; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--json":
                    if (json != null)
                        throw new ArgumentException(
                            "--json may be specified only once.");
                    string value = NextValue(args, ref index, "--json");
                    if (string.IsNullOrWhiteSpace(value) ||
                        value.StartsWith("--", StringComparison.Ordinal))
                    {
                        throw new ArgumentException(
                            "--json requires a new output file path.");
                    }
                    json = Path.GetFullPath(value);
                    break;
                case "--quiet":
                    if (quiet)
                        throw new ArgumentException(
                            "--quiet may be specified only once.");
                    quiet = true;
                    break;
                default:
                    throw new ArgumentException(
                        $"Unknown diff argument: {args[index]}");
            }
        }

        if (json != null &&
            (PathsEqual(left, json) || PathsEqual(right, json)))
        {
            throw new ArgumentException(
                "Diff JSON must not replace either package archive.");
        }
        return new(left, right, json, quiet);
    }

    private static void ValidateInspectionArchiveEnvelope(
        FileStream archiveLease,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        (long declaredEntryCount, _) =
            ReadZipCentralDirectoryEnvelope(archiveLease);
        archiveLease.Position = 0;
        using (var archive = new ZipArchive(
                   archiveLease,
                   ZipArchiveMode.Read,
                   leaveOpen: true,
                   entryNameEncoding: Utf8Strict))
        {
            if (archive.Entries.Count != declaredEntryCount ||
                archive.Entries.Count is < 2 or > InspectArchiveEntryLimit)
            {
                throw new InvalidDataException(
                    "Package archive entry count is outside inspection limits.");
            }

            long uncompressedBytes = 0;
            long compressedBytes = 0;
            int entryIndex = 0;
            foreach (ZipArchiveEntry entry in archive.Entries)
            {
                if ((entryIndex++ & 1023) == 0)
                    cancellationToken.ThrowIfCancellationRequested();

                long length = entry.Length;
                long compressedLength = entry.CompressedLength;
                if (length < 0 ||
                    compressedLength < 0 ||
                    compressedLength > archiveLease.Length ||
                    length > InspectArchiveUncompressedLimitBytes -
                        uncompressedBytes)
                {
                    throw new InvalidDataException(
                        "Package archive exceeds inspection size limits.");
                }
                uncompressedBytes += length;
                if (compressedLength > long.MaxValue - compressedBytes)
                {
                    throw new InvalidDataException(
                        "Package archive exceeds inspection size limits.");
                }
                compressedBytes += compressedLength;

                if (length >= InspectCompressionRatioCheckThresholdBytes &&
                    ExceedsMaximumCompressionRatio(
                        length,
                        compressedLength))
                {
                    throw new InvalidDataException(
                        "Package archive contains a suspicious compression ratio.");
                }
            }
            if (uncompressedBytes >=
                    InspectCompressionRatioCheckThresholdBytes &&
                ExceedsMaximumCompressionRatio(
                    uncompressedBytes,
                    compressedBytes))
            {
                throw new InvalidDataException(
                    "Package archive has a suspicious aggregate compression ratio.");
            }
        }
        archiveLease.Position = 0;
    }

    private static bool ExceedsMaximumCompressionRatio(
        long uncompressedBytes,
        long compressedBytes)
    {
        if (uncompressedBytes <= 0) return false;
        if (compressedBytes <= 0) return true;

        // Compare uncompressedBytes > compressedBytes * ratio without
        // multiplying attacker-controlled declared sizes.
        long quotient =
            uncompressedBytes / InspectMaximumCompressionRatio;
        long remainder =
            uncompressedBytes % InspectMaximumCompressionRatio;
        return quotient > compressedBytes ||
               (quotient == compressedBytes && remainder > 0);
    }

    private static (long EntryCount, long CentralDirectoryBytes)
        ReadZipCentralDirectoryEnvelope(FileStream archiveLease)
    {
        const uint endOfCentralDirectorySignature = 0x06054b50u;
        const uint zip64LocatorSignature = 0x07064b50u;
        const uint zip64EndOfCentralDirectorySignature = 0x06064b50u;
        const int endOfCentralDirectorySize = 22;
        const int zip64LocatorSize = 20;
        const int zip64EndOfCentralDirectoryMinimumSize = 56;

        long archiveLength = archiveLease.Length;
        int tailLength = checked((int)Math.Min(
            archiveLength,
            ushort.MaxValue +
                endOfCentralDirectorySize +
                zip64LocatorSize));
        if (tailLength < endOfCentralDirectorySize)
        {
            throw new InvalidDataException(
                "Package archive has no ZIP central directory.");
        }

        byte[] tail = new byte[tailLength];
        archiveLease.Position = archiveLength - tailLength;
        archiveLease.ReadExactly(tail);
        int endIndex = -1;
        for (int index = tail.Length - endOfCentralDirectorySize;
             index >= 0;
             index--)
        {
            ReadOnlySpan<byte> candidate = tail.AsSpan(index);
            if (BinaryPrimitives.ReadUInt32LittleEndian(candidate) !=
                endOfCentralDirectorySignature)
            {
                continue;
            }

            int commentLength =
                BinaryPrimitives.ReadUInt16LittleEndian(candidate[20..]);
            if (index + endOfCentralDirectorySize + commentLength ==
                tail.Length)
            {
                endIndex = index;
                break;
            }
        }
        if (endIndex < 0)
        {
            throw new InvalidDataException(
                "Package archive has an invalid ZIP end record.");
        }

        ReadOnlySpan<byte> endRecord = tail.AsSpan(
            endIndex,
            endOfCentralDirectorySize);
        ushort diskNumber =
            BinaryPrimitives.ReadUInt16LittleEndian(endRecord[4..]);
        ushort centralDirectoryDisk =
            BinaryPrimitives.ReadUInt16LittleEndian(endRecord[6..]);
        ushort diskEntryCount =
            BinaryPrimitives.ReadUInt16LittleEndian(endRecord[8..]);
        ushort totalEntryCount =
            BinaryPrimitives.ReadUInt16LittleEndian(endRecord[10..]);
        uint centralDirectorySize =
            BinaryPrimitives.ReadUInt32LittleEndian(endRecord[12..]);
        uint centralDirectoryOffset =
            BinaryPrimitives.ReadUInt32LittleEndian(endRecord[16..]);
        if (diskNumber != 0 ||
            centralDirectoryDisk != 0 ||
            diskEntryCount != totalEntryCount)
        {
            throw new InvalidDataException(
                "Multi-disk ZIP archives are not inspectable packages.");
        }

        long endRecordOffset =
            archiveLength - tailLength + endIndex;
        bool usesZip64 =
            totalEntryCount == ushort.MaxValue ||
            centralDirectorySize == uint.MaxValue ||
            centralDirectoryOffset == uint.MaxValue;
        if (!usesZip64)
        {
            return ValidateZipCentralDirectoryBounds(
                totalEntryCount,
                centralDirectorySize,
                centralDirectoryOffset,
                endRecordOffset);
        }

        long locatorOffset = endRecordOffset - zip64LocatorSize;
        if (locatorOffset < 0)
        {
            throw new InvalidDataException(
                "ZIP64 locator is missing.");
        }
        Span<byte> locator = stackalloc byte[zip64LocatorSize];
        archiveLease.Position = locatorOffset;
        archiveLease.ReadExactly(locator);
        if (BinaryPrimitives.ReadUInt32LittleEndian(locator) !=
                zip64LocatorSignature ||
            BinaryPrimitives.ReadUInt32LittleEndian(locator[4..]) != 0 ||
            BinaryPrimitives.ReadUInt32LittleEndian(locator[16..]) != 1)
        {
            throw new InvalidDataException(
                "ZIP64 locator is invalid or multi-disk.");
        }

        ulong zip64EndOffset =
            BinaryPrimitives.ReadUInt64LittleEndian(locator[8..]);
        if (zip64EndOffset > (ulong)long.MaxValue ||
            zip64EndOffset + zip64EndOfCentralDirectoryMinimumSize >
                (ulong)archiveLength)
        {
            throw new InvalidDataException(
                "ZIP64 end record is outside the archive.");
        }

        Span<byte> zip64End =
            stackalloc byte[zip64EndOfCentralDirectoryMinimumSize];
        archiveLease.Position = checked((long)zip64EndOffset);
        archiveLease.ReadExactly(zip64End);
        ulong zip64RecordPayloadSize =
            BinaryPrimitives.ReadUInt64LittleEndian(zip64End[4..]);
        if (BinaryPrimitives.ReadUInt32LittleEndian(zip64End) !=
                zip64EndOfCentralDirectorySignature ||
            zip64RecordPayloadSize < 44 ||
            zip64EndOffset + 12 > (ulong)locatorOffset ||
            zip64RecordPayloadSize !=
                (ulong)locatorOffset - zip64EndOffset - 12 ||
            BinaryPrimitives.ReadUInt32LittleEndian(zip64End[16..]) != 0 ||
            BinaryPrimitives.ReadUInt32LittleEndian(zip64End[20..]) != 0)
        {
            throw new InvalidDataException(
                "ZIP64 end record is invalid or multi-disk.");
        }

        ulong zip64DiskEntryCount =
            BinaryPrimitives.ReadUInt64LittleEndian(zip64End[24..]);
        ulong zip64TotalEntryCount =
            BinaryPrimitives.ReadUInt64LittleEndian(zip64End[32..]);
        if (zip64DiskEntryCount != zip64TotalEntryCount)
        {
            throw new InvalidDataException(
                "Multi-disk ZIP64 archives are not inspectable packages.");
        }
        return ValidateZipCentralDirectoryBounds(
            zip64TotalEntryCount,
            BinaryPrimitives.ReadUInt64LittleEndian(zip64End[40..]),
            BinaryPrimitives.ReadUInt64LittleEndian(zip64End[48..]),
            checked((long)zip64EndOffset));
    }

    private static (long EntryCount, long CentralDirectoryBytes)
        ValidateZipCentralDirectoryBounds(
            ulong entryCount,
            ulong centralDirectorySize,
            ulong centralDirectoryOffset,
            long expectedEndOffset)
    {
        if (entryCount is < 2 or > InspectArchiveEntryLimit ||
            centralDirectorySize is 0 ||
            centralDirectorySize >
                (ulong)InspectCentralDirectoryLimitBytes ||
            centralDirectoryOffset > (ulong)long.MaxValue ||
            centralDirectorySize > (ulong)long.MaxValue ||
            centralDirectoryOffset + centralDirectorySize !=
                (ulong)expectedEndOffset)
        {
            throw new InvalidDataException(
                "ZIP central directory exceeds inspection limits or is non-canonical.");
        }
        return (checked((long)entryCount), checked((long)centralDirectorySize));
    }

    private static void RejectDuplicateJsonProperties(JsonElement root)
    {
        static void Visit(JsonElement element, int depth)
        {
            if (depth > 64)
            {
                throw new InvalidDataException(
                    "Package manifest JSON exceeds the inspection depth limit.");
            }
            if (element.ValueKind == JsonValueKind.Object)
            {
                var names = new HashSet<string>(StringComparer.Ordinal);
                foreach (JsonProperty property in element.EnumerateObject())
                {
                    if (!names.Add(property.Name))
                    {
                        throw new InvalidDataException(
                            "Package manifest JSON contains a duplicate property.");
                    }
                    Visit(property.Value, depth + 1);
                }
            }
            else if (element.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement item in element.EnumerateArray())
                    Visit(item, depth + 1);
            }
        }

        Visit(root, 0);
    }

    private static async Task<PackageInspectionReport>
        InspectPackageArchiveAsync(
            string archivePath,
            IProgress<PackageProgress>? progress = null,
            CancellationToken cancellationToken = default)
    {
        string fullArchivePath = Path.GetFullPath(archivePath);
        RejectExistingReparsePointsInPath(
            fullArchivePath,
            "Package inspection archive");
        if (!File.Exists(fullArchivePath))
        {
            throw new FileNotFoundException(
                "Package archive was not found.",
                fullArchivePath);
        }

        await using var archiveLease = new FileStream(
            fullArchivePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 128 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (archiveLease.Length is <= 0 or > InspectArchiveFileLimitBytes)
        {
            throw new InvalidDataException(
                "Package archive exceeds the inspectable compressed-size limit.");
        }
        ValidateInspectionArchiveEnvelope(archiveLease, cancellationToken);

        PackageVerificationResult verification =
            await PackageCore.VerifyPackageArchiveAsync(
                fullArchivePath,
                progress,
                cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        RejectExistingReparsePointsInPath(
            fullArchivePath,
            "Package inspection archive");

        archiveLease.Position = 0;
        byte[] archiveDigest = await SHA256.HashDataAsync(
            archiveLease,
            cancellationToken);
        string archiveSha256 =
            Convert.ToHexString(archiveDigest).ToLowerInvariant();

        archiveLease.Position = 0;
        using var archive = new ZipArchive(
            archiveLease,
            ZipArchiveMode.Read,
            leaveOpen: true,
            entryNameEncoding: Utf8Strict);
        if (archive.Entries.Count is < 2 or > InspectArchiveEntryLimit)
        {
            throw new InvalidDataException(
                "Verified package entry count is outside inspection limits.");
        }

        string manifestPath =
            verification.PackageId + "/package-manifest.json";
        ZipArchiveEntry[] manifestEntries = archive.Entries
            .Where(entry => string.Equals(
                entry.FullName,
                manifestPath,
                StringComparison.Ordinal))
            .ToArray();
        if (manifestEntries.Length != 1 ||
            manifestEntries[0].Length is < 2 or > InspectManifestLimitBytes)
        {
            throw new InvalidDataException(
                "Verified package manifest is missing or exceeds inspection limits.");
        }

        InspectedPackageManifest? manifest;
        await using (Stream manifestStream = manifestEntries[0].Open())
        {
            using JsonDocument manifestDocument = await JsonDocument.ParseAsync(
                manifestStream,
                new JsonDocumentOptions
                {
                    MaxDepth = 64,
                },
                cancellationToken);
            RejectDuplicateJsonProperties(manifestDocument.RootElement);
            manifest = manifestDocument.RootElement.Deserialize<
                InspectedPackageManifest>(
                new JsonSerializerOptions
                {
                    PropertyNameCaseInsensitive = false,
                    UnmappedMemberHandling =
                        System.Text.Json.Serialization.
                            JsonUnmappedMemberHandling.Disallow,
                    MaxDepth = 64,
                });
        }
        if (manifest is null ||
            manifest.sceneBootstrap is null ||
            manifest.assetPack is null ||
            manifest.files is null)
        {
            throw new InvalidDataException(
                "Verified package manifest is incomplete.");
        }

        long manifestBytes = 0;
        var files = new List<PackageFileInspection>(manifest.files.Count);
        foreach (InspectedManifestFile? file in manifest.files)
        {
            if (file is null ||
                string.IsNullOrWhiteSpace(file.path) ||
                file.size < 0 ||
                !IsLowerHexSha256(file.sha256) ||
                file.size > long.MaxValue - manifestBytes)
            {
                throw new InvalidDataException(
                    "Verified package file inventory is invalid.");
            }
            manifestBytes += file.size;
            files.Add(new(file.path, file.size, file.sha256));
        }

        if (manifest.schemaVersion != 3 ||
            manifest.files.Count != verification.FileCount ||
            manifestBytes != verification.UncompressedBytes ||
            !string.Equals(
                manifest.buildId,
                verification.BuildId,
                StringComparison.Ordinal) ||
            !string.Equals(
                manifest.profile,
                verification.Profile.ToString(),
                StringComparison.Ordinal) ||
            !string.Equals(
                manifest.assetPack.sha256,
                verification.AssetPackSha256,
                StringComparison.Ordinal) ||
            manifest.assetPack.sourceFileCount !=
                verification.CookedAssetCount ||
            manifest.productMetadata != verification.ProductMetadata)
        {
            throw new InvalidDataException(
                "Verified package result and inspected manifest disagree.");
        }

        return new(
            schemaVersion: 1,
            verified: true,
            archivePath: fullArchivePath,
            archiveSha256,
            packageId: verification.PackageId,
            manifestSchemaVersion: manifest.schemaVersion,
            productName: manifest.productName,
            productVersion: manifest.productVersion,
            projectSchemaVersion: manifest.projectSchemaVersion,
            engineVersion: manifest.engineVersion,
            platform: manifest.platform,
            configuration: manifest.configuration,
            profile: manifest.profile,
            executable: manifest.executable,
            buildId: manifest.buildId,
            canonicalSceneAssetId: manifest.canonicalSceneAssetId,
            canonicalSceneKind: manifest.canonicalSceneKind,
            canonicalSceneImporter: manifest.canonicalSceneImporter,
            canonicalSceneImporterVersion:
                manifest.canonicalSceneImporterVersion,
            assetGraphHash: manifest.assetGraphHash,
            sceneBootstrap: manifest.sceneBootstrap,
            productMetadata: manifest.productMetadata,
            assetPack: manifest.assetPack,
            fileCount: verification.FileCount,
            uncompressedBytes: verification.UncompressedBytes,
            files,
            errorCode: null,
            errorMessage: null);
    }

    private static PackageInspectionReport CreateFailedInspectionReport(
        string archivePath,
        Exception error) =>
        new(
            schemaVersion: 1,
            verified: false,
            archivePath: Path.GetFullPath(archivePath),
            archiveSha256: "",
            packageId: "",
            manifestSchemaVersion: 0,
            productName: "",
            productVersion: "",
            projectSchemaVersion: 0,
            engineVersion: "",
            platform: "",
            configuration: "",
            profile: "",
            executable: "",
            buildId: "",
            canonicalSceneAssetId: "",
            canonicalSceneKind: "",
            canonicalSceneImporter: "",
            canonicalSceneImporterVersion: 0,
            assetGraphHash: "",
            sceneBootstrap: null,
            productMetadata: null,
            assetPack: null,
            fileCount: 0,
            uncompressedBytes: 0,
            files: Array.Empty<PackageFileInspection>(),
            errorCode: VerificationErrorCode(error),
            errorMessage: error.Message);

    private static PackageDiffReport CreatePackageDiffReport(
        PackageInspectionReport left,
        PackageInspectionReport right)
    {
        var metadata = new List<PackageMetadataChange>();
        void Compare(string field, string leftValue, string rightValue)
        {
            if (!string.Equals(
                    leftValue,
                    rightValue,
                    StringComparison.Ordinal))
            {
                metadata.Add(new(field, leftValue, rightValue));
            }
        }

        string Number(int value) =>
            value.ToString(System.Globalization.CultureInfo.InvariantCulture);
        string Number64(long value) =>
            value.ToString(System.Globalization.CultureInfo.InvariantCulture);
        string Boolean(bool value) => value ? "true" : "false";

        Compare(
            "manifestSchemaVersion",
            Number(left.manifestSchemaVersion),
            Number(right.manifestSchemaVersion));
        Compare("packageId", left.packageId, right.packageId);
        Compare("productName", left.productName, right.productName);
        Compare("productVersion", left.productVersion, right.productVersion);
        Compare(
            "projectSchemaVersion",
            Number(left.projectSchemaVersion),
            Number(right.projectSchemaVersion));
        Compare("engineVersion", left.engineVersion, right.engineVersion);
        Compare("platform", left.platform, right.platform);
        Compare("configuration", left.configuration, right.configuration);
        Compare("profile", left.profile, right.profile);
        Compare("executable", left.executable, right.executable);
        Compare("buildId", left.buildId, right.buildId);
        Compare(
            "canonicalSceneAssetId",
            left.canonicalSceneAssetId,
            right.canonicalSceneAssetId);
        Compare(
            "canonicalSceneKind",
            left.canonicalSceneKind,
            right.canonicalSceneKind);
        Compare(
            "canonicalSceneImporter",
            left.canonicalSceneImporter,
            right.canonicalSceneImporter);
        Compare(
            "canonicalSceneImporterVersion",
            Number(left.canonicalSceneImporterVersion),
            Number(right.canonicalSceneImporterVersion));
        Compare("assetGraphHash", left.assetGraphHash, right.assetGraphHash);
        Compare(
            "sceneBootstrap.path",
            left.sceneBootstrap?.path ?? "",
            right.sceneBootstrap?.path ?? "");
        Compare(
            "sceneBootstrap.contract",
            left.sceneBootstrap?.contract ?? "",
            right.sceneBootstrap?.contract ?? "");
        Compare(
            "sceneBootstrap.sourceFormat",
            left.sceneBootstrap?.sourceFormat ?? "",
            right.sceneBootstrap?.sourceFormat ?? "");
        Compare(
            "sceneBootstrap.adapterProjectionHint",
            left.sceneBootstrap?.adapterProjectionHint ?? "",
            right.sceneBootstrap?.adapterProjectionHint ?? "");
        Compare(
            "productMetadata.publisher",
            left.productMetadata?.Publisher ?? "",
            right.productMetadata?.Publisher ?? "");
        Compare(
            "productMetadata.description",
            left.productMetadata?.Description ?? "",
            right.productMetadata?.Description ?? "");
        Compare(
            "productMetadata.copyright",
            left.productMetadata?.Copyright ?? "",
            right.productMetadata?.Copyright ?? "");
        Compare(
            "productMetadata.supportUrl",
            left.productMetadata?.SupportUrl ?? "",
            right.productMetadata?.SupportUrl ?? "");
        Compare(
            "assetPack.path",
            left.assetPack?.path ?? "",
            right.assetPack?.path ?? "");
        Compare(
            "assetPack.size",
            Number64(left.assetPack?.size ?? 0),
            Number64(right.assetPack?.size ?? 0));
        Compare(
            "assetPack.sha256",
            left.assetPack?.sha256 ?? "",
            right.assetPack?.sha256 ?? "");
        Compare(
            "assetPack.formatVersion",
            Number(left.assetPack?.formatVersion ?? 0),
            Number(right.assetPack?.formatVersion ?? 0));
        Compare(
            "assetPack.compressed",
            Boolean(left.assetPack?.compressed ?? false),
            Boolean(right.assetPack?.compressed ?? false));
        Compare(
            "assetPack.sourceFileCount",
            Number(left.assetPack?.sourceFileCount ?? 0),
            Number(right.assetPack?.sourceFileCount ?? 0));

        Dictionary<string, PackageFileInspection> leftFiles =
            left.files.ToDictionary(file => file.path, StringComparer.Ordinal);
        Dictionary<string, PackageFileInspection> rightFiles =
            right.files.ToDictionary(file => file.path, StringComparer.Ordinal);
        var added = new List<PackageFileInspection>();
        var removed = new List<PackageFileInspection>();
        var modified = new List<PackageFileModification>();
        int unchanged = 0;

        foreach (string path in leftFiles.Keys
                     .Union(rightFiles.Keys, StringComparer.Ordinal)
                     .OrderBy(path => path, StringComparer.Ordinal))
        {
            bool hasLeft = leftFiles.TryGetValue(
                path,
                out PackageFileInspection? leftFile);
            bool hasRight = rightFiles.TryGetValue(
                path,
                out PackageFileInspection? rightFile);
            if (!hasLeft)
            {
                added.Add(rightFile!);
            }
            else if (!hasRight)
            {
                removed.Add(leftFile!);
            }
            else if (leftFile!.size != rightFile!.size ||
                     !string.Equals(
                         leftFile.sha256,
                         rightFile.sha256,
                         StringComparison.Ordinal))
            {
                modified.Add(new(
                    path,
                    leftFile.size,
                    rightFile.size,
                    leftFile.sha256,
                    rightFile.sha256));
            }
            else
            {
                unchanged++;
            }
        }

        bool archiveBytesEqual = string.Equals(
            left.archiveSha256,
            right.archiveSha256,
            StringComparison.Ordinal);
        bool provenanceEqual = metadata.Count == 0;
        bool payloadsEqual =
            added.Count == 0 && removed.Count == 0 && modified.Count == 0;
        bool identical =
            archiveBytesEqual && provenanceEqual && payloadsEqual;
        return new(
            schemaVersion: 1,
            compared: true,
            identical,
            archiveBytesEqual,
            provenanceEqual,
            payloadsEqual,
            left: CreateDiffSide(left),
            right: CreateDiffSide(right),
            metadataChanges: metadata,
            added,
            removed,
            modified,
            unchangedFileCount: unchanged,
            errorCode: null,
            errorMessage: null);
    }

    private static PackageDiffSide CreateDiffSide(
        PackageInspectionReport inspection) =>
        new(
            inspection.archivePath,
            inspection.archiveSha256,
            inspection.packageId,
            inspection.buildId,
            inspection.productName,
            inspection.productVersion,
            inspection.profile,
            inspection.fileCount,
            inspection.uncompressedBytes);

    private static PackageDiffReport CreateFailedDiffReport(Exception error) =>
        new(
            schemaVersion: 1,
            compared: false,
            identical: false,
            archiveBytesEqual: false,
            provenanceEqual: false,
            payloadsEqual: false,
            left: null,
            right: null,
            metadataChanges: Array.Empty<PackageMetadataChange>(),
            added: Array.Empty<PackageFileInspection>(),
            removed: Array.Empty<PackageFileInspection>(),
            modified: Array.Empty<PackageFileModification>(),
            unchangedFileCount: 0,
            errorCode: VerificationErrorCode(error),
            errorMessage: error.Message);

    private static bool IsLowerHexSha256(string value) =>
        value is { Length: 64 } &&
        value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static VerifyCommandOptions ParseVerifyOptions(string[] args)
    {
        if (args.Length < 2 ||
            !string.Equals(args[0], "verify", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "verify には検証対象の package.zip が必要です。");
        }

        if (string.IsNullOrWhiteSpace(args[1]) ||
            args[1].StartsWith("--", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "verify には検証対象の package.zip が必要です。");
        }

        string archive = Path.GetFullPath(args[1]);
        string? report = null;
        bool quiet = false;
        for (int index = 2; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--report":
                    if (report != null)
                        throw new ArgumentException("--report は一度だけ指定できます。");
                    string value = NextValue(args, ref index, "--report");
                    if (string.IsNullOrWhiteSpace(value) ||
                        value.StartsWith("--", StringComparison.Ordinal))
                        throw new ArgumentException("--report には出力先が必要です。");
                    report = Path.GetFullPath(value);
                    break;
                case "--quiet":
                    if (quiet)
                        throw new ArgumentException("--quiet は一度だけ指定できます。");
                    quiet = true;
                    break;
                default:
                    throw new ArgumentException($"不明な verify 引数です: {args[index]}");
            }
        }

        if (report != null && PathsEqual(archive, report))
        {
            throw new ArgumentException(
                "検証レポートは検証対象のZIPと同じパスには書き込めません。");
        }

        return new(archive, report, quiet);
    }

    private static VerificationReport CreateSuccessfulVerificationReport(
        PackageVerificationResult verification) =>
        new(
            schemaVersion: 1,
            verified: true,
            archivePath: verification.ZipPath,
            verifiedUtc: DateTimeOffset.UtcNow,
            packageId: verification.PackageId,
            buildId: verification.BuildId,
            fileCount: verification.FileCount,
            uncompressedBytes: verification.UncompressedBytes,
            assetPackSha256: verification.AssetPackSha256,
            cookedAssetCount: verification.CookedAssetCount,
            profile: verification.Profile.ToString(),
            productMetadata: verification.ProductMetadata,
            errorCode: null,
            errorMessage: null);

    private static VerificationReport CreateFailureVerificationReport(
        string archivePath,
        Exception error) =>
        new(
            schemaVersion: 1,
            verified: false,
            archivePath: Path.GetFullPath(archivePath),
            verifiedUtc: DateTimeOffset.UtcNow,
            packageId: "",
            buildId: "",
            fileCount: 0,
            uncompressedBytes: 0,
            assetPackSha256: "",
            cookedAssetCount: 0,
            profile: "",
            productMetadata: null,
            errorCode: VerificationErrorCode(error),
            errorMessage: error.Message);

    private static string VerificationErrorCode(Exception error) =>
        error switch
        {
            FileNotFoundException => "ARCHIVE_NOT_FOUND",
            InvalidDataException => "ARCHIVE_INVALID",
            UnauthorizedAccessException => "ACCESS_DENIED",
            IOException => "ARCHIVE_IO_ERROR",
            OperationCanceledException => "CANCELLED",
            _ => "VERIFY_FAILED",
        };

    private static async Task WriteVerificationReportAsync(
        string reportPath,
        VerificationReport report)
    {
        await WriteNewJsonDocumentAsync(reportPath, report);
    }

    private static async Task WriteNewJsonDocumentAsync<T>(
        string reportPath,
        T report)
    {
        string destination = Path.GetFullPath(reportPath);
        ValidateNewReportDestination(destination);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new IOException(
                "検証レポートの親ディレクトリを解決できません。");

        RejectExistingReparsePointsInPath(parent, "Verification report directory");
        Directory.CreateDirectory(parent);
        RejectExistingReparsePointsInPath(parent, "Verification report directory");
        ValidateNewReportDestination(destination);

        string temporary = Path.Combine(
            parent,
            "." + Path.GetFileName(destination) + "." +
            Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            await using (var output = new FileStream(
                temporary,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 16 * 1024,
                FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(
                    output,
                    report,
                    new JsonSerializerOptions { WriteIndented = true });
                await output.FlushAsync();
                output.Flush(flushToDisk: true);
            }

            RejectExistingReparsePointsInPath(parent, "Verification report directory");
            ValidateNewReportDestination(destination);
            File.Move(temporary, destination, overwrite: false);
        }
        finally
        {
            TryDeleteOwnedReportTemporaryFile(temporary, parent);
        }
    }

    private static void ValidateNewReportDestination(string reportPath)
    {
        string destination = Path.GetFullPath(reportPath);
        RejectExistingReparsePointsInPath(destination, "Verification report");
        if (File.Exists(destination) || Directory.Exists(destination))
        {
            throw new IOException(
                "既存の検証レポートは上書きしません: " + destination);
        }
    }

    private static void RejectExistingReparsePointsInPath(
        string path,
        string label)
    {
        string current = Path.GetFullPath(path);
        while (!string.IsNullOrEmpty(current))
        {
            if (TryGetExistingAttributes(current, out FileAttributes attributes) &&
                (attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException(
                    $"{label} が reparse point を経由しています: {current}");
            }

            string? parent = Path.GetDirectoryName(current);
            if (string.IsNullOrEmpty(parent) || PathsEqual(parent, current))
                break;
            current = parent;
        }
    }

    private static void TryDeleteOwnedReportTemporaryFile(
        string temporary,
        string allowedParent)
    {
        try
        {
            string fullTemporary = Path.GetFullPath(temporary);
            string fullParent = Path.GetFullPath(allowedParent);
            if (!PathsEqual(Path.GetDirectoryName(fullTemporary) ?? "", fullParent) ||
                !Path.GetFileName(fullTemporary).StartsWith(
                    ".",
                    StringComparison.Ordinal) ||
                !Path.GetFileName(fullTemporary).EndsWith(
                    ".tmp",
                    StringComparison.Ordinal))
            {
                return;
            }

            // Recheck immediately before deletion. If a same-user process
            // replaced the report directory or the temporary leaf with a
            // junction/symlink, leave the path untouched rather than following
            // it outside the directory that this command created in.
            RejectExistingReparsePointsInPath(
                fullParent,
                "Verification report cleanup directory");
            RejectExistingReparsePointsInPath(
                fullTemporary,
                "Verification report cleanup file");
            if (!TryGetExistingAttributes(
                    fullTemporary,
                    out FileAttributes attributes) ||
                (attributes & (FileAttributes.Directory |
                               FileAttributes.ReparsePoint)) != 0)
            {
                return;
            }
            File.Delete(fullTemporary);
        }
        catch
        {
            // Cleanup is best-effort and is constrained to our private sibling file.
        }
    }

    private static bool TryGetExistingAttributes(
        string path,
        out FileAttributes attributes)
    {
        try
        {
            attributes = File.GetAttributes(path);
            return true;
        }
        catch (FileNotFoundException)
        {
            attributes = default;
            return false;
        }
        catch (DirectoryNotFoundException)
        {
            attributes = default;
            return false;
        }
    }

    private static bool PathsEqual(string left, string right) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
            OperatingSystem.IsWindows()
                ? StringComparison.OrdinalIgnoreCase
                : StringComparison.Ordinal);

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
              acspackage verify <package.zip> [--report <new-report.json>] [--quiet]
              acspackage inspect <package.zip> [--json <new-file.json>] [--quiet]
              acspackage diff <left.zip> <right.zip> [--json <new-file.json>] [--quiet]
              acspackage smoke <package.zip> [--report <report.json>] [--quiet]
              acspackage deps <game.exe> [additional-search-dir ...]
              acspackage distribution-e2e [--artifacts <new-temp-directory>]
              acspackage --self-test

            Project/package options:
              --output <dir>          ZIP output directory (default: Build/Packages)
              --version <semver>      Product/package version (default: 0.1.0)
              --profile <name>        Development, Test, or Shipping (default)
              --engine-root <dir>     ACS directory containing engine/CMakeLists.txt
              --skip-build            Package an existing Release executable
              --include-symbols        Include game PDB (Development/Test only)

            Verify options:
              --report <new-file>      Atomically create a JSON verification report
              --quiet                  Suppress progress and PASS summary

            Inspect/diff options:
              --json <new-file>        Atomically create a machine-readable JSON result
              --quiet                  Suppress progress and human-readable summary

            Smoke options:
              --report <file>           Atomic package/launch report
              --timeout-seconds <1..300>
                                        Startup deadline (default: 45)
              --max-extract-mib <mib>   Private extraction bound (default: 16384)
              --quiet                  Suppress progress and human-readable summary

            Distribution E2E:
              --artifacts <new-dir>     Retain the full audit under a new directory inside TEMP
                                        (default: a unique acs-distribution-e2e-* directory)
            """);
    }

    private static void PrintVerifyUsage()
    {
        Console.WriteLine(
            """
            Usage:
              acspackage verify <package.zip> [--report <new-report.json>] [--quiet]
            """);
    }

    private static void PrintInspectUsage()
    {
        Console.WriteLine(
            """
            Usage:
              acspackage inspect <package.zip> [--json <new-file.json>] [--quiet]
            """);
    }

    private static void PrintDiffUsage()
    {
        Console.WriteLine(
            """
            Usage:
              acspackage diff <left.zip> <right.zip> [--json <new-file.json>] [--quiet]
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
            File.WriteAllText(
                Path.Combine(
                    config,
                    PackageProductMetadataContract.FileName),
                """
                {
                  "schemaVersion": 1,
                  "publisher": "ACS Package Self-Test",
                  "description": "Deterministic package fixture.",
                  "copyright": "",
                  "supportUrl": "https://example.invalid/acs-package"
                }
                """,
                new UTF8Encoding(false));

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
            File.Copy(
                Environment.ProcessPath ??
                throw new InvalidOperationException(
                    "Self-test process path is unavailable."),
                executable);
            File.WriteAllBytes(runtime, Encoding.ASCII.GetBytes("dummy runtime"));
            File.WriteAllText(Path.Combine(binaries, "Game.pdb"), "debug only");
            File.WriteAllText(Path.Combine(binaries, "Game_reflect.dll"), "editor only");

            PackageProjectInfo project = LoadProject(projectFile);
            PackageProductMetadata fixtureProductMetadata =
                PackageProductMetadataContract.LoadOptional(config);
            PackageExecutableProductMetadata expectedExecutableMetadata =
                PackageExecutableMetadataContract.Create(
                    project.Name,
                    "1.2.3",
                    fixtureProductMetadata,
                    "Game.exe");
            string executableMetadataRoot =
                Path.Combine(testRoot, "ExecutableMetadata");
            Directory.CreateDirectory(executableMetadataRoot);
            string metadataCopyA =
                Path.Combine(executableMetadataRoot, "metadata-a.exe");
            string metadataCopyB =
                Path.Combine(executableMetadataRoot, "metadata-b.exe");
            File.Copy(executable, metadataCopyA);
            File.Copy(executable, metadataCopyB);
            int volatileDebugEntryCount = PackageExecutableMetadataContract
                .SetVolatilePeHeaderFieldsForSelfTest(
                    metadataCopyB,
                    timeDateStamp: 0xa1b2c3d4u,
                    checksum: 0x11223344u,
                    debugTimeDateStamp: 0x55667788u);
            Assert(
                volatileDebugEntryCount > 0,
                "the executable metadata fixture must expose at least one " +
                "IMAGE_DEBUG_DIRECTORY entry");
            PackageExecutableInspection metadataInspectionA =
                PackageExecutableMetadataContract.ApplyFile(
                    metadataCopyA,
                    expectedExecutableMetadata);
            PackageExecutableInspection metadataInspectionB =
                PackageExecutableMetadataContract.ApplyFile(
                    metadataCopyB,
                    expectedExecutableMetadata);
            Assert(
                SHA256.HashData(File.ReadAllBytes(metadataCopyA))
                    .SequenceEqual(
                        SHA256.HashData(File.ReadAllBytes(metadataCopyB))) &&
                metadataInspectionA.ProductMetadata ==
                    expectedExecutableMetadata &&
                metadataInspectionB.ProductMetadata ==
                    expectedExecutableMetadata,
                "independently patched executable copies with distinct COFF, checksum, " +
                "and IMAGE_DEBUG_DIRECTORY timestamps must be byte-identical and " +
                "preserve exact product metadata");
            FileVersionInfo shellVersionInfo =
                FileVersionInfo.GetVersionInfo(metadataCopyA);
            Assert(
                shellVersionInfo.ProductName ==
                    expectedExecutableMetadata.ProductName &&
                shellVersionInfo.ProductVersion ==
                    expectedExecutableMetadata.ProductVersion &&
                shellVersionInfo.CompanyName ==
                    expectedExecutableMetadata.CompanyName &&
                shellVersionInfo.FileDescription ==
                    expectedExecutableMetadata.FileDescription &&
                shellVersionInfo.LegalCopyright ==
                    expectedExecutableMetadata.LegalCopyright &&
                shellVersionInfo.FileVersion ==
                    expectedExecutableMetadata.FileVersion &&
                shellVersionInfo.OriginalFilename ==
                    expectedExecutableMetadata.OriginalFilename,
                "Windows version APIs must observe the canonical packaged VERSIONINFO");

            string generatedManifestCopy =
                Path.Combine(executableMetadataRoot, "generated-manifest.exe");
            File.Copy(executable, generatedManifestCopy);
            PackageExecutableMetadataContract
                .RemoveApplicationManifestForSelfTest(generatedManifestCopy);
            PackageExecutableInspection generatedManifestInspection =
                PackageExecutableMetadataContract.ApplyFile(
                    generatedManifestCopy,
                    expectedExecutableMetadata);
            Assert(
                generatedManifestInspection.ApplicationManifest is
                {
                    AssemblyName: "Game",
                    AssemblyVersion: "1.2.3.0",
                    ProcessorArchitecture: "amd64",
                    RequestedExecutionLevel: "asInvoker",
                    UiAccess: false,
                },
                "a missing application manifest must receive one deterministic " +
                "Windows x64 asInvoker identity");

            string identityFreeManifestCopy =
                Path.Combine(
                    executableMetadataRoot,
                    "identity-free-manifest.exe");
            File.Copy(executable, identityFreeManifestCopy);
            PackageExecutableMetadataContract
                .ReplaceApplicationManifestForSelfTest(
                    identityFreeManifestCopy,
                    Encoding.UTF8.GetBytes(
                        """
                        <?xml version="1.0" encoding="UTF-8"?>
                        <assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
                          <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
                            <security>
                              <requestedPrivileges>
                                <requestedExecutionLevel level="asInvoker"
                                                         uiAccess="false" />
                              </requestedPrivileges>
                            </security>
                          </trustInfo>
                        </assembly>
                        """));
            PackageExecutableInspection identityFreeManifestInspection =
                PackageExecutableMetadataContract.ApplyFile(
                    identityFreeManifestCopy,
                    expectedExecutableMetadata);
            Assert(
                identityFreeManifestInspection.ApplicationManifest is
                {
                    AssemblyName: "",
                    AssemblyVersion: "",
                    ProcessorArchitecture: "*",
                    RequestedExecutionLevel: "asInvoker",
                    UiAccess: false,
                },
                "a valid process manifest without assemblyIdentity must be " +
                "preserved while retaining strict execution-level safety");

            bool metadataMismatchRejected = false;
            try
            {
                PackageExecutableMetadataContract.ValidateInspection(
                    metadataInspectionA,
                    expectedExecutableMetadata with
                    {
                        CompanyName = "Different Publisher"
                    });
            }
            catch (InvalidDataException)
            {
                metadataMismatchRejected = true;
            }
            Assert(
                metadataMismatchRejected,
                "manifest-to-PE product metadata mismatch must fail closed");

            bool directMetadataValidationRejected = false;
            try
            {
                PackageExecutableMetadataContract.ValidateInspection(
                    metadataInspectionA,
                    expectedExecutableMetadata with
                    {
                        SupportUrl = "http://example.invalid/not-https"
                    });
            }
            catch (InvalidDataException)
            {
                directMetadataValidationRejected = true;
            }
            Assert(
                directMetadataValidationRejected,
                "direct executable metadata validation must retain bounded HTTPS semantics");

            byte[] canonicalVersionInfo =
                PackageExecutableMetadataContract.BuildVersionInfoForSelfTest(
                    expectedExecutableMetadata);
            string extraLanguageCopy =
                Path.Combine(executableMetadataRoot, "extra-language.exe");
            File.Copy(metadataCopyA, extraLanguageCopy);
            PackageExecutableMetadataContract.AddVersionInfoForSelfTest(
                extraLanguageCopy,
                name: 1,
                language: 0x0411,
                canonicalVersionInfo);
            bool extraLanguageRejected = false;
            try
            {
                PackageExecutableMetadataContract.ValidateInspection(
                    PackageExecutableContract.InspectFile(extraLanguageCopy),
                    expectedExecutableMetadata);
            }
            catch (InvalidDataException)
            {
                extraLanguageRejected = true;
            }
            Assert(
                extraLanguageRejected,
                "extra VERSIONINFO languages must fail canonical verification");

            string extraNameCopy =
                Path.Combine(executableMetadataRoot, "extra-name.exe");
            File.Copy(metadataCopyA, extraNameCopy);
            PackageExecutableMetadataContract.AddVersionInfoForSelfTest(
                extraNameCopy,
                name: 2,
                language: 0x0409,
                canonicalVersionInfo);
            bool extraNameRejected = false;
            try
            {
                _ = PackageExecutableContract.InspectFile(extraNameCopy);
            }
            catch (InvalidDataException)
            {
                extraNameRejected = true;
            }
            Assert(
                extraNameRejected,
                "VERSIONINFO resources outside ID 1 must fail closed");

            string undeclaredResourcesCopy =
                Path.Combine(executableMetadataRoot, "undeclared-resources.exe");
            File.Copy(metadataCopyA, undeclaredResourcesCopy);
            SetPeNumberOfRvaAndSizesForSelfTest(
                undeclaredResourcesCopy,
                2);
            PackageExecutableInspection undeclaredResourcesInspection =
                PackageExecutableContract.InspectFile(
                    undeclaredResourcesCopy);
            Assert(
                undeclaredResourcesInspection.ProductMetadata is null &&
                undeclaredResourcesInspection.ApplicationManifest is null,
                "PE resource bytes outside NumberOfRvaAndSizes must not be treated as declared data directories");

            string truncatedDirectoriesCopy =
                Path.Combine(executableMetadataRoot, "truncated-directories.exe");
            File.Copy(metadataCopyA, truncatedDirectoriesCopy);
            SetPeNumberOfRvaAndSizesForSelfTest(
                truncatedDirectoriesCopy,
                uint.MaxValue);
            bool truncatedDirectoriesRejected = false;
            try
            {
                _ = PackageExecutableContract.InspectFile(
                    truncatedDirectoriesCopy);
            }
            catch (InvalidDataException)
            {
                truncatedDirectoriesRejected = true;
            }
            Assert(
                truncatedDirectoriesRejected,
                "PE data-directory count must fit inside the declared optional header");

            bool cyclicResourceDirectoryRejected = false;
            try
            {
                PackageExecutableContract.ValidateResourceDirectoryForSelfTest(
                    BuildCyclicResourceDirectoryForSelfTest());
            }
            catch (InvalidDataException)
            {
                cyclicResourceDirectoryRejected = true;
            }
            Assert(
                cyclicResourceDirectoryRejected,
                "cyclic or multiply visited PE resource directories must fail closed");

            bool resourceEntryBudgetRejected = false;
            try
            {
                PackageExecutableContract.ValidateResourceDirectoryForSelfTest(
                    BuildResourceDirectoryBudgetAttackForSelfTest());
            }
            catch (InvalidDataException)
            {
                resourceEntryBudgetRejected = true;
            }
            Assert(
                resourceEntryBudgetRejected,
                "PE resource traversal must enforce one strict global entry budget");

            string incompatibleManifestCopy =
                Path.Combine(executableMetadataRoot, "incompatible-manifest.exe");
            File.Copy(executable, incompatibleManifestCopy);
            PackageExecutableMetadataContract
                .ReplaceApplicationManifestForSelfTest(
                    incompatibleManifestCopy,
                    Encoding.UTF8.GetBytes(
                        """
                        <?xml version="1.0" encoding="UTF-8"?>
                        <assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
                          <assemblyIdentity name="Incompatible" version="1.0.0.0"
                                            processorArchitecture="amd64" type="win32" />
                          <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
                            <security>
                              <requestedPrivileges>
                                <requestedExecutionLevel level="requireAdministrator"
                                                         uiAccess="false" />
                              </requestedPrivileges>
                            </security>
                          </trustInfo>
                        </assembly>
                        """));
            bool incompatibleManifestRejected = false;
            try
            {
                _ = PackageExecutableContract.InspectFile(
                    incompatibleManifestCopy);
            }
            catch (PackageApplicationManifestException)
            {
                incompatibleManifestRejected = true;
            }
            Assert(
                incompatibleManifestRejected,
                "an embedded elevation manifest must fail package inspection");

            string malformedManifestCopy =
                Path.Combine(executableMetadataRoot, "malformed-manifest.exe");
            File.Copy(executable, malformedManifestCopy);
            PackageExecutableMetadataContract
                .ReplaceApplicationManifestForSelfTest(
                    malformedManifestCopy,
                    Encoding.UTF8.GetBytes("<assembly"));
            bool malformedManifestRejected = false;
            try
            {
                _ = PackageExecutableContract.InspectFile(
                    malformedManifestCopy);
            }
            catch (PackageApplicationManifestException)
            {
                malformedManifestRejected = true;
            }
            Assert(
                malformedManifestRejected,
                "a malformed embedded application manifest must fail closed");

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
                first.ArchiveVerified &&
                second.ArchiveVerified &&
                first.ProductMetadata?.Publisher == "ACS Package Self-Test" &&
                first.ProductMetadata == second.ProductMetadata,
                "completed packages must verify PE32+ x64 and preserve deterministic metadata");
            await RunPackageLaunchSmokeContractSelfTestAsync(
                first.ZipPath,
                testRoot);
            string verificationReport = Path.Combine(
                testRoot,
                "package-verification.json");
            int verifyExit = await RunVerifyCommandWithDiagnosticsAsync(
                [
                    "verify",
                    first.ZipPath,
                    "--report",
                    verificationReport,
                    "--quiet",
                ]);
            Assert(
                verifyExit == 0 && File.Exists(verificationReport),
                "standalone verify command must create a report for a valid package");
            using (JsonDocument reportDocument = JsonDocument.Parse(
                File.ReadAllText(verificationReport, Encoding.UTF8)))
            {
                JsonElement report = reportDocument.RootElement;
                Assert(
                    report.GetProperty("schemaVersion").GetInt32() == 1 &&
                    report.GetProperty("verified").GetBoolean() &&
                    report.GetProperty("archivePath").GetString() ==
                        first.ZipPath &&
                    report.GetProperty("packageId").GetString() ==
                        first.PackageId &&
                    report.GetProperty("buildId").GetString() ==
                        first.BuildId &&
                    report.GetProperty("profile").GetString() ==
                        first.Profile.ToString() &&
                    report.GetProperty("productMetadata")
                        .GetProperty("publisher").GetString() ==
                        "ACS Package Self-Test" &&
                    report.GetProperty("errorCode").ValueKind ==
                        JsonValueKind.Null,
                    "standalone verification report must preserve verified package identity");
            }
            bool reportOverwriteRejected = false;
            try
            {
                ValidateNewReportDestination(verificationReport);
            }
            catch (IOException)
            {
                reportOverwriteRejected = true;
            }
            Assert(
                reportOverwriteRejected,
                "standalone verification reports must never overwrite existing files");
            bool archiveOverwriteRejected = false;
            try
            {
                ParseVerifyOptions(
                    [
                        "verify",
                        first.ZipPath,
                        "--report",
                        first.ZipPath,
                    ]);
            }
            catch (ArgumentException)
            {
                archiveOverwriteRejected = true;
            }
            Assert(
                archiveOverwriteRejected,
                "verification report path must not alias the package archive");
            Assert(
                !Directory.EnumerateFiles(
                    testRoot,
                    ".package-verification.json.*.tmp",
                    SearchOption.TopDirectoryOnly).Any(),
                "standalone verification report must not leave temporary files");
            string failureReport = Path.Combine(
                testRoot,
                "failed-package-verification.json");
            await WriteVerificationReportAsync(
                failureReport,
                CreateFailureVerificationReport(
                    Path.Combine(testRoot, "missing-package.zip"),
                    new FileNotFoundException("Package archive was not found.")));
            using (JsonDocument failureDocument = JsonDocument.Parse(
                File.ReadAllText(failureReport, Encoding.UTF8)))
            {
                JsonElement report = failureDocument.RootElement;
                Assert(
                    !report.GetProperty("verified").GetBoolean() &&
                    report.GetProperty("errorCode").GetString() ==
                        "ARCHIVE_NOT_FOUND" &&
                    report.GetProperty("packageId").GetString() == "",
                    "failed verification report must be machine-readable and fail closed");
            }
            Assert(
                !Directory.EnumerateFiles(
                    testRoot,
                    ".failed-package-verification.json.*.tmp",
                    SearchOption.TopDirectoryOnly).Any(),
                "failed verification report must not leave temporary files");

            string inspectionJson = Path.Combine(
                testRoot,
                "package-inspection.json");
            int inspectExit = await RunInspectCommandWithDiagnosticsAsync(
                [
                    "inspect",
                    first.ZipPath,
                    "--json",
                    inspectionJson,
                    "--quiet",
                ]);
            Assert(
                inspectExit == 0 && File.Exists(inspectionJson),
                "inspect must verify the archive and atomically create JSON");
            using (JsonDocument inspectionDocument = JsonDocument.Parse(
                File.ReadAllText(inspectionJson, Encoding.UTF8)))
            {
                JsonElement inspection = inspectionDocument.RootElement;
                JsonElement[] inspectedFiles = inspection
                    .GetProperty("files")
                    .EnumerateArray()
                    .ToArray();
                Assert(
                    inspection.GetProperty("schemaVersion").GetInt32() == 1 &&
                    inspection.GetProperty("verified").GetBoolean() &&
                    inspection.GetProperty("archivePath").GetString() ==
                        first.ZipPath &&
                    inspection.GetProperty("archiveSha256").GetString() is
                        { Length: 64 } &&
                    inspection.GetProperty("packageId").GetString() ==
                        first.PackageId &&
                    inspection.GetProperty("buildId").GetString() ==
                        first.BuildId &&
                    inspection.GetProperty("productVersion").GetString() ==
                        "1.2.3" &&
                    inspection.GetProperty("profile").GetString() ==
                        "Shipping" &&
                    inspection.GetProperty("assetPack")
                        .GetProperty("sha256").GetString() ==
                        first.AssetPackSha256 &&
                    inspectedFiles.Length == first.FileCount &&
                    inspectedFiles
                        .Select(file =>
                            file.GetProperty("path").GetString() ?? "")
                        .SequenceEqual(
                            inspectedFiles
                                .Select(file =>
                                    file.GetProperty("path").GetString() ?? "")
                                .OrderBy(path => path, StringComparer.Ordinal)) &&
                    inspection.GetProperty("errorCode").ValueKind ==
                        JsonValueKind.Null,
                    "inspect JSON must expose stable provenance and the sorted payload ledger");
            }
            bool inspectionArchiveAliasRejected = false;
            try
            {
                ParseInspectOptions(
                    [
                        "inspect",
                        first.ZipPath,
                        "--json",
                        first.ZipPath,
                    ]);
            }
            catch (ArgumentException)
            {
                inspectionArchiveAliasRejected = true;
            }
            Assert(
                inspectionArchiveAliasRejected,
                "inspection JSON must not alias its package archive");

            string identicalDiffJson = Path.Combine(
                testRoot,
                "package-identical-diff.json");
            int identicalDiffExit = await RunDiffCommandWithDiagnosticsAsync(
                [
                    "diff",
                    first.ZipPath,
                    second.ZipPath,
                    "--json",
                    identicalDiffJson,
                    "--quiet",
                ]);
            Assert(
                identicalDiffExit == 0 && File.Exists(identicalDiffJson),
                "byte-identical packages must produce diff exit code 0 and JSON");
            using (JsonDocument diffDocument = JsonDocument.Parse(
                File.ReadAllText(identicalDiffJson, Encoding.UTF8)))
            {
                JsonElement diff = diffDocument.RootElement;
                Assert(
                    diff.GetProperty("schemaVersion").GetInt32() == 1 &&
                    diff.GetProperty("compared").GetBoolean() &&
                    diff.GetProperty("identical").GetBoolean() &&
                    diff.GetProperty("archiveBytesEqual").GetBoolean() &&
                    diff.GetProperty("provenanceEqual").GetBoolean() &&
                    diff.GetProperty("payloadsEqual").GetBoolean() &&
                    diff.GetProperty("metadataChanges").GetArrayLength() == 0 &&
                    diff.GetProperty("added").GetArrayLength() == 0 &&
                    diff.GetProperty("removed").GetArrayLength() == 0 &&
                    diff.GetProperty("modified").GetArrayLength() == 0 &&
                    diff.GetProperty("unchangedFileCount").GetInt32() ==
                        first.FileCount &&
                    diff.GetProperty("errorCode").ValueKind ==
                        JsonValueKind.Null,
                    "identical diff JSON must distinguish archive, provenance, and payload equality");
            }
            bool diffArchiveAliasRejected = false;
            try
            {
                ParseDiffOptions(
                    [
                        "diff",
                        first.ZipPath,
                        second.ZipPath,
                        "--json",
                        second.ZipPath,
                    ]);
            }
            catch (ArgumentException)
            {
                diffArchiveAliasRejected = true;
            }
            Assert(
                diffArchiveAliasRejected,
                "diff JSON must not alias either package archive");
            Assert(
                !Directory.EnumerateFiles(
                    testRoot,
                    ".package-inspection.json.*.tmp",
                    SearchOption.TopDirectoryOnly).Any() &&
                !Directory.EnumerateFiles(
                    testRoot,
                    ".package-identical-diff.json.*.tmp",
                    SearchOption.TopDirectoryOnly).Any(),
                "inspect and diff JSON must not leave private temporary files");

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
            JsonElement productMetadata =
                manifest.RootElement.GetProperty("productMetadata");
            Assert(
                productMetadata.GetProperty("schemaVersion").GetInt32() == 1 &&
                productMetadata.GetProperty("publisher").GetString() ==
                    "ACS Package Self-Test" &&
                productMetadata.GetProperty("supportUrl").GetString() ==
                    "https://example.invalid/acs-package" &&
                !productMetadata.TryGetProperty("isEmpty", out _) &&
                !productMetadata.TryGetProperty("IsEmpty", out _),
                "bounded distribution metadata missing from manifest");
            PackageProductMetadata manifestProductMetadata =
                productMetadata.Deserialize<PackageProductMetadata>() ??
                throw new InvalidDataException(
                    "package manifest product metadata is empty");
            PackageExecutableProductMetadata manifestExecutableMetadata =
                PackageExecutableMetadataContract.Create(
                    manifest.RootElement.GetProperty("productName").GetString() ??
                        throw new InvalidDataException(
                            "package manifest product name is empty"),
                    manifest.RootElement.GetProperty("productVersion").GetString() ??
                        throw new InvalidDataException(
                            "package manifest product version is empty"),
                    manifestProductMetadata,
                    manifest.RootElement.GetProperty("executable").GetString() ??
                        throw new InvalidDataException(
                            "package manifest executable is empty"));
            ZipArchiveEntry packagedExecutableEntry =
                zip.GetEntry(prefix + "Game.exe") ??
                throw new InvalidDataException(
                    "packaged executable ZIP entry is missing");
            PackageExecutableInspection packagedExecutableInspection;
            await using (Stream packagedExecutableStream =
                         packagedExecutableEntry.Open())
            {
                packagedExecutableInspection =
                    PackageExecutableContract.Inspect(
                        packagedExecutableStream,
                        packagedExecutableEntry.Length);
            }
            PackageExecutableMetadataContract.ValidateInspection(
                packagedExecutableInspection,
                manifestExecutableMetadata);
            Assert(
                packagedExecutableInspection.ProductMetadata ==
                    manifestExecutableMetadata &&
                packagedExecutableInspection.ApplicationManifest is
                {
                    RequestedExecutionLevel: "asInvoker",
                    UiAccess: false,
                },
                "package-manifest identity and distribution fields must round-trip " +
                "exactly through PE VERSIONINFO with a compatible manifest");
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

            string invalidExecutable = Path.Combine(binaries, "Invalid.exe");
            File.WriteAllText(invalidExecutable, "MZ but not a PE image");
            IReadOnlyList<PackageIssue> invalidExecutableIssues =
                PackageCore.Validate(
                    project,
                    optionsA,
                    invalidExecutable,
                    [runtime]);
            Assert(
                invalidExecutableIssues.Any(issue =>
                    issue.Code == "EXECUTABLE_INVALID" &&
                    issue.Severity == PackageIssueSeverity.Error),
                "source package preflight must fail closed on a non-PE executable");
            IReadOnlyList<PackageIssue> unrepresentableVersionIssues =
                PackageCore.Validate(
                    project,
                    optionsA with { ProductVersion = "65536.0.0" },
                    executable,
                    [runtime]);
            Assert(
                unrepresentableVersionIssues.Any(issue =>
                    issue.Code == "EXECUTABLE_METADATA_INVALID" &&
                    issue.Severity == PackageIssueSeverity.Error),
                "SemVer components outside VERSIONINFO UInt16 fields must fail preflight");

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

            string differentDiffJson = Path.Combine(
                testRoot,
                "package-different-diff.json");
            int differentDiffExit = await RunDiffCommandWithDiagnosticsAsync(
                [
                    "diff",
                    first.ZipPath,
                    development.ZipPath,
                    "--json",
                    differentDiffJson,
                    "--quiet",
                ]);
            Assert(
                differentDiffExit == 1 && File.Exists(differentDiffJson),
                "valid packages with differences must produce diff exit code 1 and JSON");
            using (JsonDocument diffDocument = JsonDocument.Parse(
                File.ReadAllText(differentDiffJson, Encoding.UTF8)))
            {
                JsonElement diff = diffDocument.RootElement;
                Assert(
                    diff.GetProperty("compared").GetBoolean() &&
                    !diff.GetProperty("identical").GetBoolean() &&
                    !diff.GetProperty("archiveBytesEqual").GetBoolean() &&
                    !diff.GetProperty("provenanceEqual").GetBoolean() &&
                    !diff.GetProperty("payloadsEqual").GetBoolean() &&
                    diff.GetProperty("metadataChanges")
                        .EnumerateArray()
                        .Any(change =>
                            change.GetProperty("field").GetString() ==
                            "profile") &&
                    diff.GetProperty("added").GetArrayLength() > 0 &&
                    diff.GetProperty("errorCode").ValueKind ==
                        JsonValueKind.Null,
                    "different diff JSON must separate provenance changes from payload additions");
            }
            Assert(
                !Directory.EnumerateFiles(
                    testRoot,
                    ".package-different-diff.json.*.tmp",
                    SearchOption.TopDirectoryOnly).Any(),
                "different diff must not leave a private temporary file");

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

            IReadOnlyList<PackageIssue> missingCanonicalIdIssues =
                PackageCore.Validate(
                    project with { CanonicalSceneAssetId = "" },
                    optionsA,
                    executable,
                    [runtime]);
            IReadOnlyList<PackageIssue> invalidCanonicalIdIssues =
                PackageCore.Validate(
                    project with { CanonicalSceneAssetId = "not-a-guid" },
                    optionsA,
                    executable,
                    [runtime]);
            Assert(
                missingCanonicalIdIssues.Any(issue =>
                    issue.Code == "CANONICAL_SCENE_ASSET_ID_REQUIRED") &&
                invalidCanonicalIdIssues.Any(issue =>
                    issue.Code == "CANONICAL_SCENE_ASSET_ID_INVALID"),
                "CLI package preflight must not fall back to the legacy initialScene path");

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
            IReadOnlyList<PackageIssue> sprite3DIssues =
                PackageCore.Validate(project3D, optionsA, executable, [runtime]);
            Assert(
                !sprite3DIssues.Any(issue =>
                    issue.Code.StartsWith("SCENE3D_", StringComparison.Ordinal) &&
                    issue.Severity == PackageIssueSeverity.Error),
                "SPR3D texture references must pass the reversible runtime adapter");
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
            assetDatabase.Refresh(verifyContent: true);
            AssetRecord unsupportedAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "payload.unsupported");
            IReadOnlyList<PackageIssue> unreachableUnsupportedIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                !unreachableUnsupportedIssues.Any(issue =>
                    issue.Code == "ASSET_TYPE_UNSUPPORTED"),
                "unreachable unsupported Asset must not enter closure-driven Cook");
            File.WriteAllText(
                scene,
                $"ACSCENE v1\n1\nSPRT 1 {albedo}\nMAT 1 {material}\n" +
                "SPRT 2 Assets/payload.unsupported\n",
                new UTF8Encoding(false));
            assetDatabase.Refresh(verifyContent: true);
            assetDatabase.UpdateImportMetadata(
                sceneAsset.AssetId,
                sceneAsset.Metadata.Source,
                "legacy-acscene",
                1,
                [albedoAsset.AssetId, materialAsset.AssetId, unsupportedAsset.AssetId],
                sceneAsset.Metadata.ImportSettings);
            IReadOnlyList<PackageIssue> requiredUnsupportedIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                requiredUnsupportedIssues.Any(issue =>
                    issue.Code == "ASSET_TYPE_UNSUPPORTED"),
                "required unsupported Cook input must fail closed");
            File.WriteAllText(
                scene,
                $"ACSCENE v1\n1\nSPRT 1 {albedo}\nMAT 1 {material}\n",
                new UTF8Encoding(false));
            assetDatabase.Refresh(verifyContent: true);
            assetDatabase.UpdateImportMetadata(
                sceneAsset.AssetId,
                sceneAsset.Metadata.Source,
                "legacy-acscene",
                1,
                [albedoAsset.AssetId, materialAsset.AssetId],
                sceneAsset.Metadata.ImportSettings);
            File.Delete(unsupported);
            File.Delete(unsupported + AssetDatabase.MetadataSuffix);
            assetDatabase.Refresh(verifyContent: true);

            string externalGltf = Path.Combine(assets, "external.gltf");
            File.WriteAllText(
                externalGltf,
                "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"C:/outside.bin\",\"byteLength\":4}]}");
            assetDatabase.Refresh(verifyContent: true);
            AssetRecord externalGltfAsset = assetDatabase.Snapshot().Single(item =>
                item.RelativePath == "external.gltf");
            IReadOnlyList<PackageIssue> unreachableGltfIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                !unreachableGltfIssues.Any(issue =>
                    issue.Code is "ASSET_REFERENCE_INVALID" or
                        "GLTF_EXTERNAL_URI_UNSUPPORTED"),
                "unreachable invalid glTF must not enter closure-driven Cook");
            File.WriteAllText(
                scene,
                $"ACSCENE v1\n1\nSPRT 1 {albedo}\nMAT 1 {material}\n" +
                "SPRT 2 Assets/external.gltf\n",
                new UTF8Encoding(false));
            assetDatabase.Refresh(verifyContent: true);
            assetDatabase.UpdateImportMetadata(
                sceneAsset.AssetId,
                sceneAsset.Metadata.Source,
                "legacy-acscene",
                1,
                [albedoAsset.AssetId, materialAsset.AssetId, externalGltfAsset.AssetId],
                sceneAsset.Metadata.ImportSettings);
            IReadOnlyList<PackageIssue> requiredGltfIssues =
                PackageCore.Validate(project, optionsA, executable, [runtime]);
            Assert(
                requiredGltfIssues.Any(issue =>
                    issue.Code == "ASSET_REFERENCE_INVALID"),
                "required glTF external URI must fail closed in dependency discovery");
            File.WriteAllText(
                scene,
                $"ACSCENE v1\n1\nSPRT 1 {albedo}\nMAT 1 {material}\n",
                new UTF8Encoding(false));
            assetDatabase.Refresh(verifyContent: true);
            assetDatabase.UpdateImportMetadata(
                sceneAsset.AssetId,
                sceneAsset.Metadata.Source,
                "legacy-acscene",
                1,
                [albedoAsset.AssetId, materialAsset.AssetId],
                sceneAsset.Metadata.ImportSettings);
            File.Delete(externalGltf);
            File.Delete(externalGltf + AssetDatabase.MetadataSuffix);
            assetDatabase.Refresh(verifyContent: true);

            await TestInspectionAdversarialGuardsAsync(
                testRoot,
                first.ZipPath);
            await TestReparsePointIfSupportedAsync(
                testRoot,
                assets,
                executable,
                runtime,
                project,
                optionsA);

            Console.WriteLine(
                "SELF-TEST PASS: canonical Asset-ID dependency closure, DDC miss/hit reuse, " +
                "deterministic Cook/acpak+ZIP, native and archive SHA-256 verify, " +
                "canonical/bootstrap/product manifest, PE32+ AMD64 preflight, " +
                "byte-identical VERSIONINFO publication, manifest-to-PE metadata " +
                "round-trip, compatible/generated asInvoker manifests, " +
                "2D+supported 3D package smoke, authenticated hidden first-frame " +
                "launch/timeout/cancel reports with pre-instruction Job containment, " +
                "metadata/source exclusions, path rewrite, and " +
                "metadata/version/resource/identity-mismatch, malformed-manifest, " +
                "traversal/reparse/runtime-adapter guards, plus standalone " +
                "verification/inspection reports, adversarial ZIP/JSON/terminal and " +
                "exact-length guards, and " +
                "deterministic package diff safety.");
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

    private static async Task TestInspectionAdversarialGuardsAsync(
        string testRoot,
        string validArchive)
    {
        string adversarialRoot = Path.Combine(
            testRoot,
            "InspectionAdversarial");
        Directory.CreateDirectory(adversarialRoot);

        async Task<Exception?> CaptureInspectionFailureAsync(string path)
        {
            try
            {
                _ = await InspectPackageArchiveAsync(path);
                return null;
            }
            catch (Exception error)
            {
                return error;
            }
        }

        async Task<int> RunWithCapturedErrorAsync(Func<Task<int>> action)
        {
            TextWriter original = Console.Error;
            using var captured = new StringWriter(
                System.Globalization.CultureInfo.InvariantCulture);
            try
            {
                Console.SetError(captured);
                return await action();
            }
            finally
            {
                Console.SetError(original);
            }
        }

        async Task<(int ExitCode, string Output, string Error)>
            RunWithCapturedConsoleAsync(Func<Task<int>> action)
        {
            TextWriter originalOutput = Console.Out;
            TextWriter originalError = Console.Error;
            using var capturedOutput = new StringWriter(
                System.Globalization.CultureInfo.InvariantCulture);
            using var capturedError = new StringWriter(
                System.Globalization.CultureInfo.InvariantCulture);
            try
            {
                Console.SetOut(capturedOutput);
                Console.SetError(capturedError);
                int exitCode = await action();
                return (
                    exitCode,
                    capturedOutput.ToString(),
                    capturedError.ToString());
            }
            finally
            {
                Console.SetOut(originalOutput);
                Console.SetError(originalError);
            }
        }

        string terminalAttack =
            "Product\u001B[31m\nForged\t\u009B\u202E";
        string terminalSafe = FormatTerminalText(terminalAttack);
        Assert(
            terminalSafe ==
                "Product\\x1B[31m\\nForged\\t\\x9B\\u202E" &&
            !terminalSafe.Any(character =>
                character is '\u001B' or '\n' or '\r' or '\u009B'),
            "terminal formatter must escape control, C1, and format characters");
        string terminalTruncated = FormatTerminalText(
            new string('x', TerminalTextLimit + 64));
        Assert(
            terminalTruncated.Length == TerminalTextLimit &&
            terminalTruncated.EndsWith("...", StringComparison.Ordinal),
            "terminal formatter output must be bounded and visibly truncated");

        string firstJson = Path.Combine(adversarialRoot, "first.json");
        string secondJson = Path.Combine(adversarialRoot, "second.json");
        Assert(
            await RunInspectCommandWithDiagnosticsAsync(
                ["inspect", validArchive, "--json", firstJson, "--quiet"]) == 0 &&
            await RunInspectCommandWithDiagnosticsAsync(
                ["inspect", validArchive, "--json", secondJson, "--quiet"]) == 0 &&
            File.ReadAllBytes(firstJson).SequenceEqual(
                File.ReadAllBytes(secondJson)),
            "repeated inspection JSON for one archive must be byte-deterministic");
        Assert(
            await RunDiffCommandWithDiagnosticsAsync(
                ["diff", validArchive, validArchive, "--quiet"]) == 0,
            "same-path diff must use one stable inspection snapshot");

        string terminalMetadataArchive = Path.Combine(
            adversarialRoot,
            "terminal-metadata.zip");
        File.Copy(validArchive, terminalMetadataArchive);
        using (ZipArchive archive = ZipFile.Open(
                   terminalMetadataArchive,
                   ZipArchiveMode.Update,
                   entryNameEncoding: Utf8Strict))
        {
            ZipArchiveEntry manifestEntry = archive.Entries.Single(entry =>
                entry.FullName.EndsWith(
                    "/package-manifest.json",
                    StringComparison.Ordinal));
            string manifestName = manifestEntry.FullName;
            JsonNode manifestNode;
            using (var reader = new StreamReader(
                       manifestEntry.Open(),
                       Utf8Strict,
                       detectEncodingFromByteOrderMarks: false,
                       leaveOpen: false))
            {
                manifestNode = JsonNode.Parse(reader.ReadToEnd()) ??
                    throw new InvalidDataException(
                        "Self-test package manifest is empty.");
            }
            // engineVersion is printed by inspect but does not derive the
            // package root identity, so this remains a valid success archive.
            manifestNode["engineVersion"] = terminalAttack;
            manifestEntry.Delete();
            ZipArchiveEntry replacement = archive.CreateEntry(
                manifestName,
                CompressionLevel.Optimal);
            replacement.ExternalAttributes = 0;
            using var writer = new StreamWriter(
                replacement.Open(),
                Utf8Strict,
                leaveOpen: false);
            writer.Write(manifestNode.ToJsonString());
        }
        string terminalMetadataJson = Path.Combine(
            adversarialRoot,
            "terminal-metadata.json");
        (int terminalMetadataExit, string terminalMetadataOutput,
            string terminalMetadataError) =
            await RunWithCapturedConsoleAsync(() =>
                RunInspectCommandWithDiagnosticsAsync(
                    [
                        "inspect",
                        terminalMetadataArchive,
                        "--json",
                        terminalMetadataJson,
                    ]));
        Assert(
            terminalMetadataExit == 0 &&
            terminalMetadataOutput.Contains(
                "Product\\x1B[31m\\nForged\\t\\x9B\\u202E",
                StringComparison.Ordinal) &&
            !terminalMetadataOutput.Contains('\u001B') &&
            !terminalMetadataOutput.Contains('\u009B'),
            "inspect stdout must terminal-escape archive-controlled metadata; " +
            $"exit={terminalMetadataExit}, output=" +
            FormatTerminalText(terminalMetadataOutput) +
            ", error=" + FormatTerminalText(terminalMetadataError));
        using (JsonDocument terminalMetadataDocument = JsonDocument.Parse(
                   File.ReadAllText(terminalMetadataJson, Encoding.UTF8)))
        {
            Assert(
                terminalMetadataDocument.RootElement
                    .GetProperty("engineVersion")
                    .GetString() == terminalAttack,
                "inspection JSON must preserve raw metadata semantics");
        }

        string terminalFailureArchive = Path.Combine(
            adversarialRoot,
            "terminal-failure.zip");
        string terminalFailurePath =
            "Terminal/evil\u001B[31m\nname\u009B.txt";
        using (FileStream output = new(
                   terminalFailureArchive,
                   FileMode.CreateNew,
                   FileAccess.Write,
                   FileShare.None))
        using (var archive = new ZipArchive(
                   output,
                   ZipArchiveMode.Create,
                   leaveOpen: false,
                   entryNameEncoding: Utf8Strict))
        {
            WriteTestZipEntry(
                archive,
                "Terminal/package-manifest.json",
                "{}");
            WriteTestZipEntry(
                archive,
                terminalFailurePath,
                "unsafe");
        }
        string terminalFailureJson = Path.Combine(
            adversarialRoot,
            "terminal-failure.json");
        (int terminalFailureExit, _, string terminalFailureError) =
            await RunWithCapturedConsoleAsync(() =>
                RunInspectCommandWithDiagnosticsAsync(
                    [
                        "inspect",
                        terminalFailureArchive,
                        "--json",
                        terminalFailureJson,
                        "--quiet",
                    ]));
        string terminalFailureBody =
            terminalFailureError.TrimEnd('\r', '\n');
        Assert(
            terminalFailureExit == 1 &&
            terminalFailureBody.Contains("\\x1B", StringComparison.Ordinal) &&
            terminalFailureBody.Contains("\\n", StringComparison.Ordinal) &&
            terminalFailureBody.Contains("\\x9B", StringComparison.Ordinal) &&
            !terminalFailureBody.Any(character =>
                character is '\u001B' or '\n' or '\r' or '\u009B'),
            "inspect stderr must terminal-escape archive-controlled failures");
        using (JsonDocument terminalFailureDocument = JsonDocument.Parse(
                   File.ReadAllText(terminalFailureJson, Encoding.UTF8)))
        {
            Assert(
                terminalFailureDocument.RootElement
                    .GetProperty("errorMessage")
                    .GetString()?
                    .Contains(terminalFailurePath, StringComparison.Ordinal) ==
                    true,
                "inspection failure JSON must preserve the raw error value");
        }
        (int terminalDiffExit, _, string terminalDiffError) =
            await RunWithCapturedConsoleAsync(() =>
                RunDiffCommandWithDiagnosticsAsync(
                    [
                        "diff",
                        terminalFailureArchive,
                        validArchive,
                        "--quiet",
                    ]));
        string terminalDiffBody =
            terminalDiffError.TrimEnd('\r', '\n');
        Assert(
            terminalDiffExit == 3 &&
            terminalDiffBody.Contains("\\x1B", StringComparison.Ordinal) &&
            terminalDiffBody.Contains("\\n", StringComparison.Ordinal) &&
            terminalDiffBody.Contains("\\x9B", StringComparison.Ordinal) &&
            !terminalDiffBody.Any(character =>
                character is '\u001B' or '\n' or '\r' or '\u009B'),
            "diff stderr must terminal-escape archive-controlled failures");

        byte[] exposedOverflow = new byte[4096];
        using (var manifestOverflowStream = new MemoryStream(
                   exposedOverflow,
                   writable: false))
        {
            Exception? manifestOverflowFailure = null;
            try
            {
                _ = await PackageCore
                    .ReadManifestStreamExactForSelfTestAsync(
                        manifestOverflowStream,
                        declaredLength: 16);
            }
            catch (Exception error)
            {
                manifestOverflowFailure = error;
            }
            Assert(
                manifestOverflowFailure is InvalidDataException &&
                manifestOverflowStream.Position == 17,
                "manifest exact reader must abort after declared length plus one byte");
        }
        using (var payloadOverflowStream = new MemoryStream(
                   exposedOverflow,
                   writable: false))
        {
            Exception? payloadOverflowFailure = null;
            try
            {
                _ = await PackageCore
                    .HashArchiveStreamExactForSelfTestAsync(
                        payloadOverflowStream,
                        expectedBytes: 16);
            }
            catch (Exception error)
            {
                payloadOverflowFailure = error;
            }
            Assert(
                payloadOverflowFailure is InvalidDataException &&
                payloadOverflowStream.Position == 17,
                "payload hashing must abort after declared length plus one byte");
        }

        string forgedManifestLength = Path.Combine(
            adversarialRoot,
            "forged-manifest-length.zip");
        File.Copy(validArchive, forgedManifestLength);
        string forgedManifestEntryName;
        using (ZipArchive archive = ZipFile.OpenRead(forgedManifestLength))
        {
            forgedManifestEntryName = archive.Entries.Single(entry =>
                entry.FullName.EndsWith(
                    "/package-manifest.json",
                    StringComparison.Ordinal)).FullName;
        }
        byte[] forgedManifestBytes =
            File.ReadAllBytes(forgedManifestLength);
        PatchZipDeclaredUncompressedSize(
            forgedManifestBytes,
            forgedManifestEntryName,
            declaredSize: 2);
        File.WriteAllBytes(forgedManifestLength, forgedManifestBytes);
        Exception? forgedManifestFailure = null;
        try
        {
            _ = await PackageCore.VerifyPackageArchiveAsync(
                forgedManifestLength);
        }
        catch (Exception error)
        {
            forgedManifestFailure = error;
        }
        Assert(
            forgedManifestFailure is JsonException or InvalidDataException,
            "manifest verification must reject a forged smaller central-directory " +
            "length; actual=" +
            FormatTerminalText(forgedManifestFailure?.GetType().Name));

        string forgedPayloadLength = Path.Combine(
            adversarialRoot,
            "forged-payload-length.zip");
        File.Copy(validArchive, forgedPayloadLength);
        string forgedPayloadEntryName;
        using (ZipArchive archive = ZipFile.Open(
                   forgedPayloadLength,
                   ZipArchiveMode.Update,
                   entryNameEncoding: Utf8Strict))
        {
            ZipArchiveEntry manifestEntry = archive.Entries.Single(entry =>
                entry.FullName.EndsWith(
                    "/package-manifest.json",
                    StringComparison.Ordinal));
            string manifestName = manifestEntry.FullName;
            string packageRoot =
                manifestName[..manifestName.IndexOf('/')];
            JsonNode manifestNode;
            using (var reader = new StreamReader(
                       manifestEntry.Open(),
                       Utf8Strict,
                       detectEncodingFromByteOrderMarks: false,
                       leaveOpen: false))
            {
                manifestNode = JsonNode.Parse(reader.ReadToEnd()) ??
                    throw new InvalidDataException(
                        "Self-test package manifest is empty.");
            }
            JsonArray files =
                manifestNode["files"]?.AsArray() ??
                throw new InvalidDataException(
                    "Self-test package manifest has no files.");
            JsonObject payloadRecord = files
                .Select(node => node?.AsObject())
                .First(record =>
                    record?["size"]?.GetValue<long>() > 1)!;
            string payloadPath =
                payloadRecord["path"]?.GetValue<string>() ??
                throw new InvalidDataException(
                    "Self-test package payload path is missing.");
            payloadRecord["size"] = 1;
            forgedPayloadEntryName = packageRoot + "/" + payloadPath;

            manifestEntry.Delete();
            ZipArchiveEntry replacement = archive.CreateEntry(
                manifestName,
                CompressionLevel.Optimal);
            replacement.ExternalAttributes = 0;
            using var writer = new StreamWriter(
                replacement.Open(),
                Utf8Strict,
                leaveOpen: false);
            writer.Write(manifestNode.ToJsonString());
        }
        byte[] forgedPayloadBytes =
            File.ReadAllBytes(forgedPayloadLength);
        PatchZipDeclaredUncompressedSize(
            forgedPayloadBytes,
            forgedPayloadEntryName,
            declaredSize: 1);
        File.WriteAllBytes(forgedPayloadLength, forgedPayloadBytes);
        Exception? forgedPayloadFailure = null;
        try
        {
            _ = await PackageCore.VerifyPackageArchiveAsync(
                forgedPayloadLength);
        }
        catch (Exception error)
        {
            forgedPayloadFailure = error;
        }
        Assert(
            forgedPayloadFailure is InvalidDataException,
            "payload verification must reject a forged smaller central-directory " +
            "length; actual=" +
            FormatTerminalText(forgedPayloadFailure?.ToString()));

        string compressionBomb = Path.Combine(
            adversarialRoot,
            "compression-bomb.zip");
        using (FileStream output = new(
                   compressionBomb,
                   FileMode.CreateNew,
                   FileAccess.Write,
                   FileShare.None))
        using (var archive = new ZipArchive(
                   output,
                   ZipArchiveMode.Create,
                   leaveOpen: false,
                   entryNameEncoding: Utf8Strict))
        {
            WriteTestZipEntry(
                archive,
                "Bomb/package-manifest.json",
                "{}");
            ZipArchiveEntry payload = archive.CreateEntry(
                "Bomb/payload.bin",
                CompressionLevel.Optimal);
            payload.ExternalAttributes = 0;
            byte[] zeros = new byte[64 * 1024];
            using Stream payloadStream = payload.Open();
            long remaining =
                InspectCompressionRatioCheckThresholdBytes + 1024 * 1024;
            while (remaining > 0)
            {
                int count = (int)Math.Min(zeros.Length, remaining);
                payloadStream.Write(zeros, 0, count);
                remaining -= count;
            }
        }
        await using (FileStream bombInput = new(
                         compressionBomb,
                         FileMode.Open,
                         FileAccess.Read,
                         FileShare.Read))
        {
            Exception? ratioFailure = null;
            try
            {
                ValidateInspectionArchiveEnvelope(bombInput);
            }
            catch (Exception error)
            {
                ratioFailure = error;
            }
            Assert(
                ratioFailure is InvalidDataException &&
                ratioFailure.Message.Contains(
                    "suspicious compression ratio",
                    StringComparison.Ordinal),
                "inspection must reject a high-ratio ZIP before payload verification");
        }
        string bombFailureJson = Path.Combine(
            adversarialRoot,
            "compression-bomb-failure.json");
        int bombExit = await RunWithCapturedErrorAsync(() =>
            RunInspectCommandWithDiagnosticsAsync(
                [
                    "inspect",
                    compressionBomb,
                    "--json",
                    bombFailureJson,
                    "--quiet",
                ]));
        using (JsonDocument failure = JsonDocument.Parse(
                   File.ReadAllText(bombFailureJson, Encoding.UTF8)))
        {
            Assert(
                bombExit == 1 &&
                !failure.RootElement.GetProperty("verified").GetBoolean() &&
                failure.RootElement.GetProperty("errorCode").GetString() ==
                    "ARCHIVE_INVALID",
                "invalid inspection must return exit 1 and atomically publish failure JSON");
        }
        string bombDiffFailureJson = Path.Combine(
            adversarialRoot,
            "compression-bomb-diff-failure.json");
        int bombDiffExit = await RunWithCapturedErrorAsync(() =>
            RunDiffCommandWithDiagnosticsAsync(
                [
                    "diff",
                    compressionBomb,
                    validArchive,
                    "--json",
                    bombDiffFailureJson,
                    "--quiet",
                ]));
        using (JsonDocument failure = JsonDocument.Parse(
                   File.ReadAllText(bombDiffFailureJson, Encoding.UTF8)))
        {
            Assert(
                bombDiffExit == 3 &&
                !failure.RootElement.GetProperty("compared").GetBoolean() &&
                failure.RootElement.GetProperty("errorCode").GetString() ==
                    "ARCHIVE_INVALID",
                "invalid diff must return exit 3 and atomically publish failure JSON");
        }

        string aggregateCompressionBomb = Path.Combine(
            adversarialRoot,
            "aggregate-compression-bomb.zip");
        using (FileStream output = new(
                   aggregateCompressionBomb,
                   FileMode.CreateNew,
                   FileAccess.Write,
                   FileShare.None))
        using (var archive = new ZipArchive(
                   output,
                   ZipArchiveMode.Create,
                   leaveOpen: false,
                   entryNameEncoding: Utf8Strict))
        {
            WriteTestZipEntry(
                archive,
                "AggregateBomb/package-manifest.json",
                "{}");
            byte[] zeros = new byte[64 * 1024];
            for (int entryIndex = 0; entryIndex < 3; entryIndex++)
            {
                ZipArchiveEntry payload = archive.CreateEntry(
                    $"AggregateBomb/payload-{entryIndex}.bin",
                    CompressionLevel.Optimal);
                payload.ExternalAttributes = 0;
                using Stream payloadStream = payload.Open();
                long remaining =
                    InspectCompressionRatioCheckThresholdBytes / 2;
                while (remaining > 0)
                {
                    int count = (int)Math.Min(zeros.Length, remaining);
                    payloadStream.Write(zeros, 0, count);
                    remaining -= count;
                }
            }
        }
        await using (FileStream aggregateBombInput = new(
                         aggregateCompressionBomb,
                         FileMode.Open,
                         FileAccess.Read,
                         FileShare.Read))
        {
            Exception? aggregateRatioFailure = null;
            try
            {
                ValidateInspectionArchiveEnvelope(aggregateBombInput);
            }
            catch (Exception error)
            {
                aggregateRatioFailure = error;
            }
            Assert(
                aggregateRatioFailure is InvalidDataException &&
                aggregateRatioFailure.Message.Contains(
                    "aggregate compression ratio",
                    StringComparison.Ordinal),
                "inspection must reject aggregate high-ratio ZIPs composed of sub-threshold entries");
        }

        string invalidEnvelope = Path.Combine(
            adversarialRoot,
            "invalid-envelope.zip");
        File.Copy(validArchive, invalidEnvelope);
        byte[] envelopeBytes = File.ReadAllBytes(invalidEnvelope);
        int endRecord = FindZipEndRecord(envelopeBytes);
        BinaryPrimitives.WriteUInt32LittleEndian(
            envelopeBytes.AsSpan(endRecord + 12),
            checked((uint)InspectCentralDirectoryLimitBytes + 1));
        File.WriteAllBytes(invalidEnvelope, envelopeBytes);
        await using (FileStream invalidEnvelopeInput = new(
                         invalidEnvelope,
                         FileMode.Open,
                         FileAccess.Read,
                         FileShare.Read))
        {
            Exception? envelopeFailure = null;
            try
            {
                ValidateInspectionArchiveEnvelope(invalidEnvelopeInput);
            }
            catch (Exception error)
            {
                envelopeFailure = error;
            }
            Assert(
                envelopeFailure is InvalidDataException &&
                envelopeFailure.Message.Contains(
                    "central directory",
                    StringComparison.OrdinalIgnoreCase),
                "inspection must reject an oversized/non-canonical central directory envelope");
        }

        string duplicateManifest = Path.Combine(
            adversarialRoot,
            "duplicate-manifest-property.zip");
        File.Copy(validArchive, duplicateManifest);
        using (ZipArchive archive = ZipFile.Open(
                   duplicateManifest,
                   ZipArchiveMode.Update,
                   entryNameEncoding: Utf8Strict))
        {
            ZipArchiveEntry manifestEntry = archive.Entries.Single(entry =>
                entry.FullName.EndsWith(
                    "/package-manifest.json",
                    StringComparison.Ordinal));
            string manifestName = manifestEntry.FullName;
            string manifestText;
            using (var reader = new StreamReader(
                       manifestEntry.Open(),
                       Utf8Strict,
                       detectEncodingFromByteOrderMarks: false,
                       leaveOpen: false))
            {
                manifestText = reader.ReadToEnd();
            }
            using JsonDocument document = JsonDocument.Parse(manifestText);
            string bootstrapPath = document.RootElement
                .GetProperty("sceneBootstrap")
                .GetProperty("path")
                .GetString()!;
            int bootstrapProperty = manifestText.IndexOf(
                "\"sceneBootstrap\"",
                StringComparison.Ordinal);
            int bootstrapObject = manifestText.IndexOf(
                '{',
                bootstrapProperty);
            Assert(
                bootstrapProperty >= 0 && bootstrapObject > bootstrapProperty,
                "self-test manifest sceneBootstrap object is missing");
            manifestText = manifestText.Insert(
                bootstrapObject + 1,
                "\"path\":" +
                JsonSerializer.Serialize(bootstrapPath) +
                ",");

            manifestEntry.Delete();
            ZipArchiveEntry replacement = archive.CreateEntry(
                manifestName,
                CompressionLevel.Optimal);
            replacement.ExternalAttributes = 0;
            using var writer = new StreamWriter(
                replacement.Open(),
                Utf8Strict,
                leaveOpen: false);
            writer.Write(manifestText);
        }
        Exception? duplicateVerificationFailure = null;
        try
        {
            _ = await PackageCore.VerifyPackageArchiveAsync(
                duplicateManifest);
        }
        catch (Exception error)
        {
            duplicateVerificationFailure = error;
        }
        Assert(
            duplicateVerificationFailure is InvalidDataException &&
            duplicateVerificationFailure.Message.Contains(
                "duplicate property",
                StringComparison.Ordinal),
            "the shared package verification gate must recursively reject " +
            "duplicate manifest JSON properties");
        Exception? duplicatePropertyFailure =
            await CaptureInspectionFailureAsync(duplicateManifest);
        Assert(
            duplicatePropertyFailure is InvalidDataException &&
            duplicatePropertyFailure.Message.Contains(
                "duplicate property",
                StringComparison.Ordinal),
            "inspection must recursively reject duplicate manifest JSON properties");

        string caseCollision = Path.Combine(
            adversarialRoot,
            "case-collision.zip");
        File.Copy(validArchive, caseCollision);
        using (ZipArchive archive = ZipFile.Open(
                   caseCollision,
                   ZipArchiveMode.Update,
                   entryNameEncoding: Utf8Strict))
        {
            string existing = archive.Entries
                .First(entry => !entry.FullName.EndsWith(
                    "/package-manifest.json",
                    StringComparison.Ordinal))
                .FullName;
            string collision = existing.ToUpperInvariant();
            Assert(
                !string.Equals(existing, collision, StringComparison.Ordinal),
                "self-test payload path must have a distinct case variant");
            WriteTestZipEntry(archive, collision, "collision");
        }
        Exception? collisionFailure =
            await CaptureInspectionFailureAsync(caseCollision);
        Assert(
            collisionFailure is InvalidDataException &&
            collisionFailure.Message.Contains(
                "duplicate Windows path",
                StringComparison.Ordinal),
            "inspection verification must reject case-insensitive ZIP collisions");

        string traversal = Path.Combine(
            adversarialRoot,
            "traversal.zip");
        File.Copy(validArchive, traversal);
        using (ZipArchive archive = ZipFile.Open(
                   traversal,
                   ZipArchiveMode.Update,
                   entryNameEncoding: Utf8Strict))
        {
            string root = archive.Entries[0].FullName.Split('/')[0];
            WriteTestZipEntry(
                archive,
                root + "/../escape.txt",
                "escape");
        }
        Exception? traversalFailure =
            await CaptureInspectionFailureAsync(traversal);
        Assert(
            traversalFailure is InvalidDataException &&
            traversalFailure.Message.Contains(
                "path is invalid",
                StringComparison.Ordinal),
            "inspection verification must reject ZIP traversal entries");

        string symlinkEntry = Path.Combine(
            adversarialRoot,
            "symlink-entry.zip");
        File.Copy(validArchive, symlinkEntry);
        using (ZipArchive archive = ZipFile.Open(
                   symlinkEntry,
                   ZipArchiveMode.Update,
                   entryNameEncoding: Utf8Strict))
        {
            string root = archive.Entries[0].FullName.Split('/')[0];
            ZipArchiveEntry link = archive.CreateEntry(
                root + "/payload-link",
                CompressionLevel.NoCompression);
            link.ExternalAttributes = unchecked((int)0xA0000000);
            using var writer = new StreamWriter(
                link.Open(),
                Utf8Strict,
                leaveOpen: false);
            writer.Write("target");
        }
        Exception? symlinkFailure =
            await CaptureInspectionFailureAsync(symlinkEntry);
        Assert(
            symlinkFailure is InvalidDataException &&
            symlinkFailure.Message.Contains(
                "ordinary files",
                StringComparison.Ordinal),
            "inspection verification must reject ZIP symlink attributes");
        Assert(
            !Directory.EnumerateFiles(
                    adversarialRoot,
                    ".*.tmp",
                    SearchOption.TopDirectoryOnly)
                .Any(),
            "inspection and diff reports must not leave sibling temp files");
    }

    private static void WriteTestZipEntry(
        ZipArchive archive,
        string path,
        string content)
    {
        ZipArchiveEntry entry = archive.CreateEntry(
            path,
            CompressionLevel.NoCompression);
        entry.ExternalAttributes = 0;
        using var writer = new StreamWriter(
            entry.Open(),
            Utf8Strict,
            leaveOpen: false);
        writer.Write(content);
    }

    private static int FindZipEndRecord(byte[] archive)
    {
        const uint signature = 0x06054b50u;
        for (int index = archive.Length - 22; index >= 0; index--)
        {
            if (BinaryPrimitives.ReadUInt32LittleEndian(
                    archive.AsSpan(index)) != signature)
            {
                continue;
            }
            int commentLength = BinaryPrimitives.ReadUInt16LittleEndian(
                archive.AsSpan(index + 20));
            if (index + 22 + commentLength == archive.Length)
                return index;
        }
        throw new InvalidDataException(
            "Self-test ZIP end record was not found.");
    }

    private static void PatchZipDeclaredUncompressedSize(
        byte[] archive,
        string entryName,
        uint declaredSize)
    {
        const uint centralSignature = 0x02014b50u;
        const int centralHeaderSize = 46;

        int endRecord = FindZipEndRecord(archive);
        int entryCount = BinaryPrimitives.ReadUInt16LittleEndian(
            archive.AsSpan(endRecord + 10));
        int offset = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(
            archive.AsSpan(endRecord + 16)));
        for (int index = 0; index < entryCount; index++)
        {
            if (offset < 0 ||
                offset > archive.Length - centralHeaderSize ||
                BinaryPrimitives.ReadUInt32LittleEndian(
                    archive.AsSpan(offset)) != centralSignature)
            {
                throw new InvalidDataException(
                    "Self-test ZIP central directory is invalid.");
            }

            int nameLength = BinaryPrimitives.ReadUInt16LittleEndian(
                archive.AsSpan(offset + 28));
            int extraLength = BinaryPrimitives.ReadUInt16LittleEndian(
                archive.AsSpan(offset + 30));
            int commentLength = BinaryPrimitives.ReadUInt16LittleEndian(
                archive.AsSpan(offset + 32));
            int recordLength = checked(
                centralHeaderSize +
                nameLength +
                extraLength +
                commentLength);
            if (recordLength > archive.Length - offset)
            {
                throw new InvalidDataException(
                    "Self-test ZIP central record exceeds the archive.");
            }

            string candidate = Utf8Strict.GetString(
                archive,
                offset + centralHeaderSize,
                nameLength);
            if (string.Equals(
                    candidate,
                    entryName,
                    StringComparison.Ordinal))
            {
                BinaryPrimitives.WriteUInt32LittleEndian(
                    archive.AsSpan(offset + 24),
                    declaredSize);
                return;
            }
            offset += recordLength;
        }

        throw new InvalidDataException(
            "Self-test ZIP entry was not found in the central directory.");
    }

    private static void SetPeNumberOfRvaAndSizesForSelfTest(
        string path,
        uint count)
    {
        byte[] bytes = File.ReadAllBytes(path);
        if (bytes.Length < 64 ||
            bytes[0] != (byte)'M' ||
            bytes[1] != (byte)'Z')
        {
            throw new InvalidDataException(
                "Self-test executable is not an MZ image.");
        }
        int peOffset = BinaryPrimitives.ReadInt32LittleEndian(
            bytes.AsSpan(0x3c));
        int optionalHeaderOffset = checked(peOffset + 4 + 20);
        int countOffset = checked(optionalHeaderOffset + 108);
        if (peOffset < 64 ||
            countOffset < 0 ||
            countOffset > bytes.Length - sizeof(uint) ||
            BinaryPrimitives.ReadUInt32LittleEndian(
                bytes.AsSpan(peOffset)) != 0x00004550u ||
            BinaryPrimitives.ReadUInt16LittleEndian(
                bytes.AsSpan(optionalHeaderOffset)) != 0x020b)
        {
            throw new InvalidDataException(
                "Self-test executable is not a bounded PE32+ image.");
        }
        BinaryPrimitives.WriteUInt32LittleEndian(
            bytes.AsSpan(countOffset),
            count);
        File.WriteAllBytes(path, bytes);
    }

    private static byte[] BuildCyclicResourceDirectoryForSelfTest()
    {
        byte[] directory = new byte[24];
        BinaryPrimitives.WriteUInt16LittleEndian(
            directory.AsSpan(14),
            1);
        BinaryPrimitives.WriteUInt32LittleEndian(
            directory.AsSpan(16),
            24);
        BinaryPrimitives.WriteUInt32LittleEndian(
            directory.AsSpan(20),
            0x80000000u);
        return directory;
    }

    private static byte[] BuildResourceDirectoryBudgetAttackForSelfTest()
    {
        const int rootEntryCount = 4;
        const int branchEntryCount = 4096;
        const int rootSize = 16 + rootEntryCount * 8;
        const int branchSize = 16 + branchEntryCount * 8;
        byte[] directory =
            new byte[rootSize + rootEntryCount * branchSize];
        BinaryPrimitives.WriteUInt16LittleEndian(
            directory.AsSpan(14),
            rootEntryCount);

        for (int rootIndex = 0;
             rootIndex < rootEntryCount;
             rootIndex++)
        {
            int branchOffset = rootSize + rootIndex * branchSize;
            int rootEntryOffset = 16 + rootIndex * 8;
            BinaryPrimitives.WriteUInt32LittleEndian(
                directory.AsSpan(rootEntryOffset),
                24);
            BinaryPrimitives.WriteUInt32LittleEndian(
                directory.AsSpan(rootEntryOffset + 4),
                0x80000000u | checked((uint)branchOffset));
            BinaryPrimitives.WriteUInt16LittleEndian(
                directory.AsSpan(branchOffset + 14),
                branchEntryCount);
            for (int branchIndex = 0;
                 branchIndex < branchEntryCount;
                 branchIndex++)
            {
                int entryOffset =
                    branchOffset + 16 + branchIndex * 8;
                // Non-process manifest ID 2 is intentionally ignored after
                // the directory has been charged to the global budget.
                BinaryPrimitives.WriteUInt32LittleEndian(
                    directory.AsSpan(entryOffset),
                    2);
                BinaryPrimitives.WriteUInt32LittleEndian(
                    directory.AsSpan(entryOffset + 4),
                    0);
            }
        }
        return directory;
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
        string jsonOutputLink = Path.Combine(testRoot, "JsonOutputLink");
        string externalJsonOutput =
            Path.Combine(testRoot, "ExternalJsonOutput");
        string executableMetadataLink =
            Path.Combine(testRoot, "ExecutableMetadataLink.exe");
        bool assetLinkCreated = false;
        bool tempLinkCreated = false;
        bool ddcLinkCreated = false;
        bool outputLinkCreated = false;
        bool jsonOutputLinkCreated = false;
        bool executableMetadataLinkCreated = false;
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

            File.CreateSymbolicLink(executableMetadataLink, executable);
            executableMetadataLinkCreated = true;
            bool executableMetadataLinkRejected = false;
            try
            {
                PackageProductMetadata metadata =
                    PackageProductMetadataContract.LoadOptional(
                        project.ConfigDirectory);
                _ = PackageExecutableMetadataContract.ApplyFile(
                    executableMetadataLink,
                    PackageExecutableMetadataContract.Create(
                        project.Name,
                        options.ProductVersion,
                        metadata,
                        Path.GetFileName(executable)));
            }
            catch (InvalidDataException)
            {
                executableMetadataLinkRejected = true;
            }
            Assert(
                executableMetadataLinkRejected,
                "executable metadata publication must reject a reparse-point target");
            File.Delete(executableMetadataLink);
            executableMetadataLinkCreated = false;

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

            Directory.CreateDirectory(externalJsonOutput);
            Directory.CreateSymbolicLink(
                jsonOutputLink,
                externalJsonOutput);
            jsonOutputLinkCreated = true;
            bool jsonOutputRejected = false;
            try
            {
                await WriteNewJsonDocumentAsync(
                    Path.Combine(jsonOutputLink, "inspection.json"),
                    new { schemaVersion = 1, verified = true });
            }
            catch (IOException)
            {
                jsonOutputRejected = true;
            }
            Assert(
                jsonOutputRejected,
                "machine-readable output must reject a reparse-point ancestor");
            Assert(
                !File.Exists(
                    Path.Combine(externalJsonOutput, "inspection.json")),
                "JSON validation must not publish through a reparse point");
            Directory.Delete(jsonOutputLink);
            jsonOutputLinkCreated = false;
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
                if (jsonOutputLinkCreated &&
                    Directory.Exists(jsonOutputLink) &&
                    (File.GetAttributes(jsonOutputLink) &
                     FileAttributes.ReparsePoint) != 0)
                {
                    Directory.Delete(jsonOutputLink);
                }
            }
            catch { }
            try
            {
                if (executableMetadataLinkCreated &&
                    File.Exists(executableMetadataLink) &&
                    (File.GetAttributes(executableMetadataLink) &
                     FileAttributes.ReparsePoint) != 0)
                {
                    File.Delete(executableMetadataLink);
                }
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
