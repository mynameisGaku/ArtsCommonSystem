// SPDX-License-Identifier: Apache-2.0
// Retained, TEMP-only end-to-end distribution audit.

using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using AcsEditor;
using AcsEditor.Packaging;

namespace AcsPackage;

internal static partial class Program
{
    private const string DistributionE2eVersion = "9.8.7";
    private const string DistributionE2ePublisher = "ACS Distribution E2E";
    private const string DistributionE2eDescription =
        "Retained TEMP-only package audit fixture.";
    private const string DistributionE2eCopyright =
        "Copyright (c) ACS Distribution E2E";
    private const string DistributionE2eSupportUrl =
        "https://example.invalid/acs-distribution-e2e";

    private sealed record DistributionE2eCommandResult(
        string command,
        int exitCode,
        string output);

    private sealed record DistributionE2ePeResult(
        string archiveEntry,
        string machine,
        ushort subsystem,
        ushort sectionCount,
        uint entryPointRva,
        uint imageSize,
        uint headerSize);

    private sealed record DistributionE2eSummary(
        int schemaVersion,
        bool passed,
        string artifactRoot,
        string projectFile,
        string packageA,
        string packageB,
        string tamperedPackage,
        string packageSha256,
        string tamperedPackageSha256,
        PackageProductMetadata productMetadata,
        DistributionE2ePeResult executable,
        IReadOnlyList<DistributionE2eCommandResult> commands);

    private sealed record DistributionE2eFailureSummary(
        int schemaVersion,
        bool passed,
        string artifactRoot,
        string errorType,
        string errorMessage);

    private static async Task<int> RunDistributionE2eCommandAsync(
        string[] args)
    {
        string artifactRoot;
        try
        {
            artifactRoot = ParseDistributionE2eArtifactRoot(args);
            PrepareDistributionE2eArtifactRoot(artifactRoot);
        }
        catch (ArgumentException error)
        {
            Console.Error.WriteLine("ERROR: " + error.Message);
            PrintDistributionE2eUsage();
            return 2;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(
                "ERROR: Distribution E2E artifact root could not be created: " +
                error.Message);
            return 1;
        }

        try
        {
            string engineRoot = FindEngineRoot(null)
                ?? throw new DirectoryNotFoundException(
                    "Distribution E2E requires an ACS engine root. Run it from the repository.");
            string projectFile = CreateDistributionE2eProject(artifactRoot);
            var commands = new List<DistributionE2eCommandResult>();

            string outputA = Path.Combine(artifactRoot, "PackageA");
            string outputB = Path.Combine(artifactRoot, "PackageB");
            int packageAExit = await Main(
                DistributionPackageArguments(
                    projectFile,
                    outputA,
                    engineRoot));
            commands.Add(new("package-a", packageAExit, outputA));
            RequireDistributionExit("package-a", packageAExit, 0);
            string packageA = FindSingleDistributionPackage(outputA);

            int packageBExit = await Main(
                DistributionPackageArguments(
                    projectFile,
                    outputB,
                    engineRoot));
            commands.Add(new("package-b", packageBExit, outputB));
            RequireDistributionExit("package-b", packageBExit, 0);
            string packageB = FindSingleDistributionPackage(outputB);

            string verifyReport = Path.Combine(
                artifactRoot,
                "verify-valid.json");
            int verifyExit = await Main(
                [
                    "verify",
                    packageA,
                    "--report",
                    verifyReport,
                    "--quiet",
                ]);
            commands.Add(new("verify-valid", verifyExit, verifyReport));
            RequireDistributionExit("verify-valid", verifyExit, 0);
            RequireJsonBoolean(
                verifyReport,
                "verified",
                expected: true,
                "valid verification report");

            string inspectionJson = Path.Combine(
                artifactRoot,
                "inspect-valid.json");
            int inspectExit = await Main(
                [
                    "inspect",
                    packageA,
                    "--json",
                    inspectionJson,
                    "--quiet",
                ]);
            commands.Add(new("inspect-valid", inspectExit, inspectionJson));
            RequireDistributionExit("inspect-valid", inspectExit, 0);
            PackageProductMetadata metadata =
                ReadAndValidateDistributionMetadata(inspectionJson);

            string identicalDiffJson = Path.Combine(
                artifactRoot,
                "diff-identical.json");
            int identicalDiffExit = await Main(
                [
                    "diff",
                    packageA,
                    packageB,
                    "--json",
                    identicalDiffJson,
                    "--quiet",
                ]);
            commands.Add(new(
                "diff-identical",
                identicalDiffExit,
                identicalDiffJson));
            RequireDistributionExit(
                "diff-identical",
                identicalDiffExit,
                0);
            RequireJsonBoolean(
                identicalDiffJson,
                "identical",
                expected: true,
                "identical package diff");

            byte[] packageAHash = SHA256.HashData(
                await File.ReadAllBytesAsync(packageA));
            byte[] packageBHash = SHA256.HashData(
                await File.ReadAllBytesAsync(packageB));
            if (!packageAHash.SequenceEqual(packageBHash))
            {
                throw new InvalidDataException(
                    "Repeated package commands did not produce byte-identical archives.");
            }

            DistributionE2ePeResult pe = InspectDistributionExecutable(
                packageA);

            string tamperedPackage = Path.Combine(
                artifactRoot,
                "Game-9.8.7-win64-tampered.zip");
            File.Copy(packageA, tamperedPackage, overwrite: false);
            TamperDistributionPackage(tamperedPackage);

            string tamperedVerifyReport = Path.Combine(
                artifactRoot,
                "verify-tampered.json");
            int tamperedVerifyExit = await Main(
                [
                    "verify",
                    tamperedPackage,
                    "--report",
                    tamperedVerifyReport,
                    "--quiet",
                ]);
            commands.Add(new(
                "verify-tampered",
                tamperedVerifyExit,
                tamperedVerifyReport));
            RequireDistributionExit(
                "verify-tampered",
                tamperedVerifyExit,
                1);
            RequireJsonBoolean(
                tamperedVerifyReport,
                "verified",
                expected: false,
                "tampered verification report");

            string tamperedInspectionJson = Path.Combine(
                artifactRoot,
                "inspect-tampered.json");
            int tamperedInspectExit = await Main(
                [
                    "inspect",
                    tamperedPackage,
                    "--json",
                    tamperedInspectionJson,
                    "--quiet",
                ]);
            commands.Add(new(
                "inspect-tampered",
                tamperedInspectExit,
                tamperedInspectionJson));
            RequireDistributionExit(
                "inspect-tampered",
                tamperedInspectExit,
                1);
            RequireJsonBoolean(
                tamperedInspectionJson,
                "verified",
                expected: false,
                "tampered inspection report");

            string tamperedDiffJson = Path.Combine(
                artifactRoot,
                "diff-tampered.json");
            int tamperedDiffExit = await Main(
                [
                    "diff",
                    packageA,
                    tamperedPackage,
                    "--json",
                    tamperedDiffJson,
                    "--quiet",
                ]);
            commands.Add(new(
                "diff-tampered",
                tamperedDiffExit,
                tamperedDiffJson));
            RequireDistributionExit(
                "diff-tampered",
                tamperedDiffExit,
                3);
            RequireJsonBoolean(
                tamperedDiffJson,
                "compared",
                expected: false,
                "tampered package diff");

            string packageHash = Convert.ToHexString(packageAHash)
                .ToLowerInvariant();
            string tamperedHash = Convert.ToHexString(SHA256.HashData(
                    await File.ReadAllBytesAsync(tamperedPackage)))
                .ToLowerInvariant();
            if (string.Equals(
                    packageHash,
                    tamperedHash,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "Tampered archive unexpectedly retained the original SHA-256.");
            }

            string summaryPath = Path.Combine(
                artifactRoot,
                "distribution-e2e-summary.json");
            await WriteNewJsonDocumentAsync(
                summaryPath,
                new DistributionE2eSummary(
                    1,
                    true,
                    artifactRoot,
                    projectFile,
                    packageA,
                    packageB,
                    tamperedPackage,
                    packageHash,
                    tamperedHash,
                    metadata,
                    pe,
                    commands));

            Console.WriteLine("DISTRIBUTION E2E PASS");
            Console.WriteLine("Artifacts retained: " + artifactRoot);
            Console.WriteLine("Summary: " + summaryPath);
            return 0;
        }
        catch (Exception error)
        {
            string failurePath = Path.Combine(
                artifactRoot,
                "distribution-e2e-failure.json");
            try
            {
                await WriteNewJsonDocumentAsync(
                    failurePath,
                    new DistributionE2eFailureSummary(
                        1,
                        false,
                        artifactRoot,
                        error.GetType().Name,
                        error.Message));
            }
            catch (Exception reportError)
            {
                Console.Error.WriteLine(
                    "ERROR: Distribution E2E failure report could not be written: " +
                    reportError.Message);
            }

            Console.Error.WriteLine("DISTRIBUTION E2E FAIL: " + error);
            Console.Error.WriteLine(
                "Partial artifacts retained: " + artifactRoot);
            return 1;
        }
    }

    private static string[] DistributionPackageArguments(
        string projectFile,
        string outputDirectory,
        string engineRoot) =>
        [
            "package",
            projectFile,
            "--version",
            DistributionE2eVersion,
            "--profile",
            "Shipping",
            "--output",
            outputDirectory,
            "--engine-root",
            engineRoot,
            "--skip-build",
        ];

    private static string ParseDistributionE2eArtifactRoot(string[] args)
    {
        if (args.Length == 1)
        {
            return Path.Combine(
                Path.GetTempPath(),
                "acs-distribution-e2e-" + Guid.NewGuid().ToString("N"));
        }
        if (args.Length != 3 ||
            !string.Equals(
                args[1],
                "--artifacts",
                StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "distribution-e2e accepts only --artifacts <new-temp-directory>.");
        }
        if (string.IsNullOrWhiteSpace(args[2]))
        {
            throw new ArgumentException(
                "Distribution E2E artifact directory is empty.");
        }
        return Path.GetFullPath(args[2]);
    }

    private static void PrepareDistributionE2eArtifactRoot(string path)
    {
        string full = Path.GetFullPath(path);
        string temp = Path.GetFullPath(Path.GetTempPath());
        string relative = Path.GetRelativePath(temp, full);
        if (relative == "." ||
            Path.IsPathRooted(relative) ||
            relative == ".." ||
            relative.StartsWith(
                ".." + Path.DirectorySeparatorChar,
                StringComparison.Ordinal) ||
            relative.StartsWith(
                ".." + Path.AltDirectorySeparatorChar,
                StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Distribution E2E artifacts must use a dedicated directory strictly inside TEMP.");
        }

        RejectExistingReparsePointsInPath(
            full,
            "Distribution E2E artifact root");
        if (File.Exists(full) || Directory.Exists(full))
        {
            throw new IOException(
                "Distribution E2E refuses to reuse an existing path: " + full);
        }
        Directory.CreateDirectory(full);
        RejectExistingReparsePointsInPath(
            full,
            "Distribution E2E artifact root");
    }

    private static string CreateDistributionE2eProject(string artifactRoot)
    {
        string projectRoot = Path.Combine(artifactRoot, "Project");
        string assets = Path.Combine(projectRoot, "Assets");
        string config = Path.Combine(projectRoot, "Config");
        string binaries = Path.Combine(
            projectRoot,
            "Binaries",
            "Release");
        Directory.CreateDirectory(assets);
        Directory.CreateDirectory(config);
        Directory.CreateDirectory(binaries);

        string scene = Path.Combine(assets, "main.acs3d");
        File.WriteAllText(
            scene,
            "ACS3D v2\n" +
            "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n",
            new UTF8Encoding(false));
        var assetDatabase = new AssetDatabase(projectRoot, assets);
        assetDatabase.Refresh(verifyContent: true);
        AssetRecord sceneAsset = assetDatabase.Snapshot().Single(item =>
            item.RelativePath == "main.acs3d");
        assetDatabase.UpdateImportMetadata(
            sceneAsset.AssetId,
            sceneAsset.Metadata.Source,
            "legacy-acs3d",
            2,
            [],
            new Dictionary<string, string>
            {
                ["scene.subsystems"] = "renderer3d",
            });

        File.WriteAllText(
            Path.Combine(config, "ProjectSettings.ini"),
            "[Game]\nDefaultScene=Assets/main.acs3d\nQuality=High\n",
            new UTF8Encoding(false));
        File.WriteAllText(
            Path.Combine(config, PackageProductMetadataContract.FileName),
            $$"""
            {
              "schemaVersion": 1,
              "publisher": "{{DistributionE2ePublisher}}",
              "description": "{{DistributionE2eDescription}}",
              "copyright": "{{DistributionE2eCopyright}}",
              "supportUrl": "{{DistributionE2eSupportUrl}}"
            }
            """,
            new UTF8Encoding(false));

        string projectFile = Path.Combine(
            projectRoot,
            "Game.acsproject");
        File.WriteAllText(
            projectFile,
            $$"""
            {
              "version": 1,
              "name": "Game",
              "engineVersion": "distribution-e2e",
              "initialScene": "Assets/main.acs3d",
              "canonicalSceneAssetId": "{{sceneAsset.AssetId}}"
            }
            """,
            new UTF8Encoding(false));

        string sourceExecutable = Environment.ProcessPath
            ?? throw new InvalidOperationException(
                "Distribution E2E process path is unavailable.");
        string executable = Path.Combine(binaries, "Game.exe");
        File.Copy(sourceExecutable, executable, overwrite: false);
        _ = PackageExecutableContract.InspectFile(executable);
        return projectFile;
    }

    private static string FindSingleDistributionPackage(
        string outputDirectory)
    {
        string[] packages = Directory
            .EnumerateFiles(
                outputDirectory,
                "*.zip",
                SearchOption.TopDirectoryOnly)
            .ToArray();
        if (packages.Length != 1)
        {
            throw new InvalidDataException(
                $"Expected one package in {outputDirectory}, found {packages.Length}.");
        }
        return Path.GetFullPath(packages[0]);
    }

    private static PackageProductMetadata
        ReadAndValidateDistributionMetadata(string inspectionJson)
    {
        using JsonDocument document = JsonDocument.Parse(
            File.ReadAllText(inspectionJson, Encoding.UTF8));
        JsonElement root = document.RootElement;
        if (!root.GetProperty("verified").GetBoolean())
        {
            throw new InvalidDataException(
                "Inspection JSON did not report a verified package.");
        }
        JsonElement metadata = root.GetProperty("productMetadata");
        var parsed = new PackageProductMetadata(
            metadata.GetProperty("schemaVersion").GetInt32(),
            metadata.GetProperty("publisher").GetString() ?? "",
            metadata.GetProperty("description").GetString() ?? "",
            metadata.GetProperty("copyright").GetString() ?? "",
            metadata.GetProperty("supportUrl").GetString() ?? "");
        var expected = new PackageProductMetadata(
            1,
            DistributionE2ePublisher,
            DistributionE2eDescription,
            DistributionE2eCopyright,
            DistributionE2eSupportUrl);
        if (parsed != expected)
        {
            throw new InvalidDataException(
                "Package inspection JSON did not preserve product metadata exactly.");
        }
        return parsed;
    }

    private static DistributionE2ePeResult InspectDistributionExecutable(
        string package)
    {
        using ZipArchive archive = ZipFile.OpenRead(package);
        ZipArchiveEntry executable = archive.Entries.Single(entry =>
            entry.FullName.EndsWith(
                "/Game.exe",
                StringComparison.Ordinal));
        using Stream stream = executable.Open();
        PackageExecutableInspection inspection =
            PackageExecutableContract.Inspect(stream, executable.Length);
        return new(
            executable.FullName,
            "0x" + inspection.Machine.ToString("x4"),
            inspection.Subsystem,
            inspection.SectionCount,
            inspection.EntryPointRva,
            inspection.ImageSize,
            inspection.HeaderSize);
    }

    private static void TamperDistributionPackage(string package)
    {
        using ZipArchive archive = ZipFile.Open(
            package,
            ZipArchiveMode.Update,
            entryNameEncoding: Utf8Strict);
        ZipArchiveEntry original = archive.Entries.Single(entry =>
            entry.FullName.EndsWith(
                "/Config/ProjectSettings.ini",
                StringComparison.Ordinal));
        string name = original.FullName;
        byte[] content;
        using (Stream input = original.Open())
        using (var buffer = new MemoryStream())
        {
            input.CopyTo(buffer);
            content = buffer.ToArray();
        }
        original.Delete();

        ZipArchiveEntry replacement = archive.CreateEntry(
            name,
            CompressionLevel.Optimal);
        replacement.ExternalAttributes = 0;
        replacement.LastWriteTime =
            new DateTimeOffset(1980, 1, 1, 0, 0, 0, TimeSpan.Zero);
        using Stream output = replacement.Open();
        output.Write(content);
        output.Write(
            Encoding.UTF8.GetBytes("\nTampered=true\n"));
    }

    private static void RequireJsonBoolean(
        string path,
        string property,
        bool expected,
        string label)
    {
        using JsonDocument document = JsonDocument.Parse(
            File.ReadAllText(path, Encoding.UTF8));
        if (document.RootElement.GetProperty(property).GetBoolean() != expected)
        {
            throw new InvalidDataException(
                $"{label} did not set {property}={expected.ToString().ToLowerInvariant()}.");
        }
    }

    private static void RequireDistributionExit(
        string command,
        int actual,
        int expected)
    {
        if (actual != expected)
        {
            throw new InvalidDataException(
                $"{command} returned {actual}; expected {expected}.");
        }
    }

    private static void PrintDistributionE2eUsage()
    {
        Console.WriteLine(
            """
            Usage:
              acspackage distribution-e2e [--artifacts <new-temp-directory>]

            The artifact directory must be a new, non-reparse-point path strictly
            inside the operating-system TEMP directory. Artifacts are retained.
            """);
    }
}
