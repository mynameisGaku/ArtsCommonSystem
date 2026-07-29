// SPDX-License-Identifier: Apache-2.0

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using AcsEditor.Packaging;

namespace AcsEditor;

internal static class PackageResponsivenessSelfTest
{
    private static int _failures;
    private static TextWriter _output = TextWriter.Null;
    private sealed record VerifierManifestFile(
        string path,
        long size,
        string sha256);

    internal static int Run(TextWriter output)
    {
        _failures = 0;
        _output = output;
        try
        {
            // OnStartup owns a WPF synchronization context even though this
            // switch creates no window. Run the async harness on a worker so a
            // synchronous CLI exit code cannot deadlock its continuations.
            Task.Run(RunAsync).GetAwaiter().GetResult();
        }
        catch (Exception error)
        {
            Fail("unhandled package responsiveness self-test error: " + error);
        }
        return _failures;
    }

    internal static int RunOutputWorker()
    {
        // Deliberately emit more than the capture limit without a newline. Line-oriented
        // Process APIs buffer this whole payload internally before raising one callback, so this
        // fixture protects the fixed-size BaseStream reader rather than only ordinary log lines.
        using Stream output = Console.OpenStandardOutput();
        byte[] payload = Encoding.ASCII.GetBytes(new string('x', 32 * 1024));
        int chunks = PackageProcessRunner.CaptureLimitBytesPerStream /
                     payload.Length + 64;
        for (int index = 0; index < chunks; index++)
            output.Write(payload);
        output.Flush();
        Thread.Sleep(TimeSpan.FromMilliseconds(500));
        return 0;
    }

    internal static int RunWaitWorker()
    {
        Thread.Sleep(TimeSpan.FromSeconds(30));
        return 0;
    }

    private static async Task RunAsync()
    {
        await VerifyLatestOnlyValidationAsync();
        await VerifyPackageDurabilityBoundaryAsync();
        await VerifyConfigSnapshotAsync();
        VerifyProductMetadataContract();
        VerifyExecutableStructuralContract();
        await VerifyArchiveIntegrityAsync();
        VerifyPrefabCookRewrite();
        VerifyBlueprintCookRewrite();
        VerifyBlueprintCookCacheVersion();
        VerifyBlueprintParentPathPolicy();
        VerifyCanonicalSceneSnapshotIsolation();
        VerifyCanonicalCookClosureContract();
        VerifyBlueprintInheritanceClosure();
        await VerifyProcessOutputBoundAsync();
        await VerifyProcessCancellationAsync();
        await VerifyPriorGameProcessDrainAsync();
        await VerifyOwnerCloseDrainAsync();
        await VerifyCleanupRetryAsync();
        VerifyValidateCancellation();
    }

    private static async Task VerifyPackageDurabilityBoundaryAsync()
    {
        int calls = 0;
        ProjectSettingsDurabilityCheckpoint expected =
            ProjectSettingsDurabilityCheckpoint.Create(
                "[Game]\nDefaultScene=Assets/main.acscene\n");
        ProjectSettingsDurabilityCheckpoint? accepted =
            await PackageDurabilityBoundary.VerifyAsync(
                _ =>
                {
                    calls++;
                    return Task.FromResult<ProjectSettingsDurabilityCheckpoint?>(
                        expected);
                },
                CancellationToken.None);
        ProjectSettingsDurabilityCheckpoint? rejected =
            await PackageDurabilityBoundary.VerifyAsync(
                _ =>
                {
                    calls++;
                    return Task.FromResult<ProjectSettingsDurabilityCheckpoint?>(
                        null);
                },
                CancellationToken.None);
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await CheckThrowsAsync<OperationCanceledException>(
            () => PackageDurabilityBoundary.VerifyAsync(
                _ =>
                {
                    calls++;
                    return Task.FromResult<ProjectSettingsDurabilityCheckpoint?>(
                        expected);
                },
                cancelled.Token),
            "Package action durability boundary honors pre-cancellation");
        Check(
            accepted == expected && rejected == null && calls == 2,
            "Package action returns the exact Settings durability checkpoint");
    }

    private static async Task VerifyArchiveIntegrityAsync()
    {
        using (FileStream positioned = new(
                   Environment.ProcessPath ??
                   throw new InvalidOperationException(
                       "Self-test process path is unavailable."),
                   FileMode.Open,
                   FileAccess.Read,
                   FileShare.Read))
        {
            positioned.Position = 1;
            CheckThrows<InvalidDataException>(
                () => PackageExecutableContract.Inspect(
                    positioned,
                    positioned.Length),
                "package executable inspection rejects a non-zero stream position");
        }

        string root = FixtureRoot("archive-verifier");
        Directory.CreateDirectory(root);
        try
        {
            string valid = CreateVerifierPackage(root, "valid");
            PackageVerificationResult verified =
                await PackageCore.VerifyPackageArchiveAsync(valid);
            Check(
                verified.PackageId == "Verifier-1.2.3-win64" &&
                verified.FileCount == 2 &&
                verified.UncompressedBytes > 0 &&
                verified.Profile == PackageProfile.Shipping &&
                verified.AssetPackSha256.Length == 64 &&
                verified.ProductMetadata?.Publisher == "ACS Self-Test",
                "package archive verifier accepts PE32+ x64 and product metadata");

            string invalidExecutable = CreateVerifierPackage(
                root,
                "invalid-executable",
                invalidExecutable: true);
            await CheckThrowsAsync<InvalidDataException>(
                () => PackageCore.VerifyPackageArchiveAsync(invalidExecutable),
                "package archive verifier rejects a hash-valid non-PE executable");

            string nullMetadata = CreateVerifierPackage(
                root,
                "null-metadata",
                nullMetadataPublisher: true);
            await CheckThrowsAsync<InvalidDataException>(
                () => PackageCore.VerifyPackageArchiveAsync(nullMetadata),
                "package archive verifier rejects null product metadata fields");

            string corrupt = CreateVerifierPackage(
                root,
                "corrupt",
                corruptExecutable: true);
            await CheckThrowsAsync<InvalidDataException>(
                () => PackageCore.VerifyPackageArchiveAsync(corrupt),
                "package archive verifier rejects payload hash corruption");

            string extra = CreateVerifierPackage(
                root,
                "extra",
                addUnlistedPayload: true);
            await CheckThrowsAsync<InvalidDataException>(
                () => PackageCore.VerifyPackageArchiveAsync(extra),
                "package archive verifier rejects unlisted payloads");

            string traversal = CreateVerifierPackage(
                root,
                "traversal",
                addTraversalEntry: true);
            await CheckThrowsAsync<InvalidDataException>(
                () => PackageCore.VerifyPackageArchiveAsync(traversal),
                "package archive verifier rejects traversal entry paths");

            string caseCollision = CreateVerifierPackage(
                root,
                "case-collision",
                addCaseCollision: true);
            await CheckThrowsAsync<InvalidDataException>(
                () => PackageCore.VerifyPackageArchiveAsync(caseCollision),
                "package archive verifier rejects Windows case-colliding paths");

            string attributed = CreateVerifierPackage(
                root,
                "external-attributes",
                addExternalAttributes: true);
            await CheckThrowsAsync<InvalidDataException>(
                () => PackageCore.VerifyPackageArchiveAsync(attributed),
                "package archive verifier rejects link and platform attribute entries");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static void VerifyExecutableStructuralContract()
    {
        string processPath =
            Environment.ProcessPath ??
            throw new InvalidOperationException(
                "Self-test process path is unavailable.");
        byte[] validImage = File.ReadAllBytes(processPath);
        PackageExecutableInspection valid =
            InspectExecutableBytes(validImage);

        int peOffset = BinaryPrimitives.ReadInt32LittleEndian(
            validImage.AsSpan(0x3c, sizeof(int)));
        int coffOffset = checked(peOffset + 4);
        int optionalOffset = checked(coffOffset + 20);
        ushort optionalSize = BinaryPrimitives.ReadUInt16LittleEndian(
            validImage.AsSpan(coffOffset + 16, sizeof(ushort)));
        int sectionTableOffset = checked(optionalOffset + optionalSize);

        byte[] outsideImageEntry = (byte[])validImage.Clone();
        BinaryPrimitives.WriteUInt32LittleEndian(
            outsideImageEntry.AsSpan(optionalOffset + 16, sizeof(uint)),
            valid.ImageSize);
        CheckThrows<InvalidDataException>(
            () => InspectExecutableBytes(outsideImageEntry),
            "package executable inspection rejects an out-of-image entry point");

        int entrySectionOffset = FindEntryPointSectionOffset(
            validImage,
            sectionTableOffset,
            valid.SectionCount,
            valid.EntryPointRva);
        Check(
            entrySectionOffset >= 0,
            "self-test executable entry point resolves to a PE section");
        if (entrySectionOffset < 0)
            return;

        byte[] nonExecutableEntry = (byte[])validImage.Clone();
        uint characteristics = BinaryPrimitives.ReadUInt32LittleEndian(
            nonExecutableEntry.AsSpan(
                entrySectionOffset + 36,
                sizeof(uint)));
        characteristics &= ~(0x00000020u | 0x20000000u);
        BinaryPrimitives.WriteUInt32LittleEndian(
            nonExecutableEntry.AsSpan(
                entrySectionOffset + 36,
                sizeof(uint)),
            characteristics);
        CheckThrows<InvalidDataException>(
            () => InspectExecutableBytes(nonExecutableEntry),
            "package executable inspection rejects an entry point in a non-executable section");

        byte[] unbackedEntry = (byte[])validImage.Clone();
        uint entrySectionRva = BinaryPrimitives.ReadUInt32LittleEndian(
            unbackedEntry.AsSpan(
                entrySectionOffset + 12,
                sizeof(uint)));
        BinaryPrimitives.WriteUInt32LittleEndian(
            unbackedEntry.AsSpan(
                entrySectionOffset + 16,
                sizeof(uint)),
            valid.EntryPointRva - entrySectionRva);
        CheckThrows<InvalidDataException>(
            () => InspectExecutableBytes(unbackedEntry),
            "package executable inspection rejects an entry point not backed by section bytes");

        byte[] escapedRawSection = (byte[])validImage.Clone();
        BinaryPrimitives.WriteUInt32LittleEndian(
            escapedRawSection.AsSpan(
                entrySectionOffset + 20,
                sizeof(uint)),
            checked((uint)escapedRawSection.Length - 1u));
        CheckThrows<InvalidDataException>(
            () => InspectExecutableBytes(escapedRawSection),
            "package executable inspection rejects a section raw range outside the file");
    }

    private static PackageExecutableInspection InspectExecutableBytes(
        byte[] image)
    {
        using var stream = new MemoryStream(
            image,
            writable: false);
        return PackageExecutableContract.Inspect(
            stream,
            image.LongLength);
    }

    private static int FindEntryPointSectionOffset(
        byte[] image,
        int sectionTableOffset,
        ushort sectionCount,
        uint entryPointRva)
    {
        const int sectionHeaderSize = 40;
        for (int index = 0; index < sectionCount; index++)
        {
            int offset = checked(
                sectionTableOffset + index * sectionHeaderSize);
            ReadOnlySpan<byte> section =
                image.AsSpan(offset, sectionHeaderSize);
            uint virtualSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section[8..]);
            uint virtualAddress =
                BinaryPrimitives.ReadUInt32LittleEndian(section[12..]);
            uint rawSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section[16..]);
            ulong mappedSize = Math.Max((ulong)virtualSize, rawSize);
            if (entryPointRva >= virtualAddress &&
                (ulong)entryPointRva <
                    (ulong)virtualAddress + mappedSize)
            {
                return offset;
            }
        }
        return -1;
    }

    private static void VerifyProductMetadataContract()
    {
        string root = FixtureRoot("package-product-metadata");
        Directory.CreateDirectory(root);
        try
        {
            string metadataPath = Path.Combine(
                root,
                PackageProductMetadataContract.FileName);
            File.WriteAllText(
                metadataPath,
                """
                {
                  "schemaVersion": 1,
                  "publisher": "ACS Studio",
                  "description": "Deterministic package metadata.",
                  "copyright": "Copyright ACS Studio",
                  "supportUrl": "https://example.invalid/support"
                }
                """,
                new UTF8Encoding(false));
            PackageProductMetadata valid =
                PackageProductMetadataContract.LoadOptional(root);
            Check(
                valid.Publisher == "ACS Studio" &&
                valid.SupportUrl == "https://example.invalid/support",
                "package product metadata accepts bounded canonical fields");

            File.WriteAllText(
                metadataPath,
                """
                {
                  "schemaVersion": 1,
                  "publisher": "first",
                  "publisher": "second"
                }
                """,
                new UTF8Encoding(false));
            CheckThrows<InvalidDataException>(
                () => PackageProductMetadataContract.LoadOptional(root),
                "package product metadata rejects duplicate JSON properties");

            File.WriteAllText(
                metadataPath,
                """
                {
                  "schemaVersion": 1,
                  "supportUrl": "http://example.invalid/support"
                }
                """,
                new UTF8Encoding(false));
            CheckThrows<InvalidDataException>(
                () => PackageProductMetadataContract.LoadOptional(root),
                "package product metadata requires a canonical HTTPS support URL");

            File.WriteAllText(
                metadataPath,
                """
                {
                  "schemaVersion": 1,
                  "supportUrl": "https://user:secret@example.invalid/support"
                }
                """,
                new UTF8Encoding(false));
            CheckThrows<InvalidDataException>(
                () => PackageProductMetadataContract.LoadOptional(root),
                "package product metadata rejects support URL credentials");

            File.Delete(metadataPath);
            Check(
                PackageProductMetadataContract.LoadOptional(root).IsEmpty,
                "missing package metadata preserves legacy package compatibility");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static string CreateVerifierPackage(
        string directory,
        string suffix,
        bool corruptExecutable = false,
        bool invalidExecutable = false,
        bool nullMetadataPublisher = false,
        bool addUnlistedPayload = false,
        bool addTraversalEntry = false,
        bool addCaseCollision = false,
        bool addExternalAttributes = false)
    {
        const string packageId = "Verifier-1.2.3-win64";
        var executableProductMetadata = new PackageProductMetadata(
            1,
            "ACS Self-Test",
            "Package verifier fixture.",
            "",
            "https://example.invalid/acs");
        byte[] executable = invalidExecutable
            ? Encoding.ASCII.GetBytes("MZ not a PE image")
            : CreateVerifierExecutable(
                directory,
                suffix,
                executableProductMetadata);
        byte[] assetPack = Encoding.ASCII.GetBytes("ACPAK verifier fixture");
        var declaredPayloads = new Dictionary<string, byte[]>(StringComparer.Ordinal)
        {
            ["Verifier.exe"] = executable,
            ["game.acpak"] = assetPack,
        };
        VerifierManifestFile[] files = declaredPayloads
            .OrderBy(item => item.Key, StringComparer.Ordinal)
            .Select(item => new VerifierManifestFile(
                item.Key,
                item.Value.LongLength,
                Convert.ToHexString(SHA256.HashData(item.Value)).ToLowerInvariant()))
            .ToArray();
        string buildId = VerifierBuildId(files);
        VerifierManifestFile assetPackFile = files.Single(
            file => file.path == "game.acpak");
        var manifest = new
        {
            schemaVersion = 3,
            productName = "Verifier",
            productVersion = "1.2.3",
            projectSchemaVersion = 1,
            engineVersion = "self-test",
            platform = "win-x64",
            configuration = "Release",
            profile = "Shipping",
            executable = "Verifier.exe",
            buildId,
            canonicalSceneAssetId = "0123456789abcdef0123456789abcdef",
            canonicalSceneKind = "Scene3D",
            canonicalSceneImporter = "CanonicalSceneAdapter",
            canonicalSceneImporterVersion = 1,
            assetGraphHash = new string('a', 64),
            sceneBootstrap = new
            {
                path = CanonicalSceneAdapter.BootstrapPath,
                contract = CanonicalSceneAdapter.BootstrapContract,
                sourceFormat = CanonicalSceneAdapter.LegacyScene3DFormat,
                adapterProjectionHint = "perspective",
            },
            productMetadata = new
            {
                schemaVersion = 1,
                publisher = nullMetadataPublisher
                    ? null
                    : executableProductMetadata.Publisher,
                description = executableProductMetadata.Description,
                copyright = executableProductMetadata.Copyright,
                supportUrl = executableProductMetadata.SupportUrl,
            },
            assetPack = new
            {
                path = "game.acpak",
                size = assetPackFile.size,
                sha256 = assetPackFile.sha256,
                formatVersion = 1,
                compressed = true,
                sourceFileCount = 1,
            },
            files,
        };

        string path = Path.Combine(directory, suffix + ".zip");
        using FileStream output = new(path, FileMode.CreateNew, FileAccess.Write);
        using var archive = new ZipArchive(output, ZipArchiveMode.Create);
        foreach ((string payloadPath, byte[] declaredBytes) in declaredPayloads)
        {
            byte[] bytes = declaredBytes;
            if (corruptExecutable && payloadPath == "Verifier.exe")
            {
                bytes = declaredBytes.ToArray();
                bytes[^1] ^= 0x01;
            }
            ZipArchiveEntry written = WriteVerifierEntry(
                archive,
                packageId + "/" + payloadPath,
                bytes);
            if (addExternalAttributes &&
                payloadPath == "Verifier.exe")
            {
                written.ExternalAttributes = 1;
            }
        }
        if (addUnlistedPayload)
        {
            WriteVerifierEntry(
                archive,
                packageId + "/unexpected.bin",
                [0xde, 0xad, 0xbe, 0xef]);
        }
        if (addTraversalEntry)
        {
            WriteVerifierEntry(
                archive,
                packageId + "/../escape.bin",
                [0x01]);
        }
        if (addCaseCollision)
        {
            WriteVerifierEntry(
                archive,
                packageId + "/GAME.ACPAK",
                [0x02]);
        }
        WriteVerifierEntry(
            archive,
            packageId + "/package-manifest.json",
            JsonSerializer.SerializeToUtf8Bytes(manifest));
        return path;
    }

    private static byte[] CreateVerifierExecutable(
        string directory,
        string suffix,
        PackageProductMetadata productMetadata)
    {
        string source =
            Environment.ProcessPath ??
            throw new InvalidOperationException(
                "Self-test process path is unavailable.");
        string staged = Path.Combine(
            directory,
            suffix + "-staged.exe");
        File.Copy(source, staged);
        PackageExecutableProductMetadata expected =
            PackageExecutableMetadataContract.Create(
                "Verifier",
                "1.2.3",
                productMetadata,
                "Verifier.exe");
        _ = PackageExecutableMetadataContract.ApplyFile(
            staged,
            expected);
        return File.ReadAllBytes(staged);
    }

    private static ZipArchiveEntry WriteVerifierEntry(
        ZipArchive archive,
        string path,
        byte[] payload)
    {
        ZipArchiveEntry entry = archive.CreateEntry(
            path,
            CompressionLevel.NoCompression);
        using Stream stream = entry.Open();
        stream.Write(payload);
        return entry;
    }

    private static string VerifierBuildId(
        IReadOnlyList<VerifierManifestFile> files)
    {
        var canonical = new StringBuilder();
        foreach (VerifierManifestFile file in files)
        {
            canonical.Append(file.path).Append('\0')
                     .Append(file.size).Append('\0')
                     .Append(file.sha256).Append('\n');
        }
        return Convert.ToHexString(
                SHA256.HashData(new UTF8Encoding(false).GetBytes(canonical.ToString())))
            .ToLowerInvariant();
    }

    private static void VerifyPrefabCookRewrite()
    {
        string root = FixtureRoot("prefab-cook");
        string assets = Path.Combine(root, "Assets");
        string mesh = Path.Combine(assets, "Models", "aircraft.glb");
        string sprite = Path.Combine(assets, "Textures", "cloud.png");
        string material = Path.Combine(assets, "Materials", "cloud.acsmat");
        string child = Path.Combine(assets, "Prefabs", "engine.acsprefab");
        Directory.CreateDirectory(Path.GetDirectoryName(mesh)!);
        Directory.CreateDirectory(Path.GetDirectoryName(sprite)!);
        Directory.CreateDirectory(Path.GetDirectoryName(material)!);
        Directory.CreateDirectory(Path.GetDirectoryName(child)!);
        File.WriteAllBytes(mesh, [0x67, 0x6c, 0x54, 0x46]);
        File.WriteAllBytes(sprite, [1, 2, 3, 4]);
        File.WriteAllText(material, "ACSMAT 1\n", new UTF8Encoding(false));
        File.WriteAllText(child, "ACS3D v2\n", new UTF8Encoding(false));

        try
        {
            string source3D =
                "ACS3D v2\n" +
                $"MSH3D 1 {mesh}\n" +
                $"SPR3D 2 {sprite}\n" +
                $"MAT3D 3 {material}\n" +
                $"PFAB3D 4 {child}\n" +
                "MAT3D 5 0.250 0.750\n";
            byte[] cooked3D = PackageCore.RewritePrefabPayloadForSelfTest(
                new UTF8Encoding(false).GetBytes(source3D),
                assets,
                root);
            string rewritten3D = Encoding.UTF8.GetString(cooked3D);
            string portableRoot = root.Replace('\\', '/');
            Check(
                rewritten3D ==
                    "ACS3D v2\n" +
                    "MSH3D 1 Assets/Models/aircraft.glb\n" +
                    "SPR3D 2 Assets/Textures/cloud.png\n" +
                    "MAT3D 3 Assets/Materials/cloud.acsmat\n" +
                    "PFAB3D 4 Assets/Prefabs/engine.acsprefab\n" +
                    "MAT3D 5 0.250 0.750\n" &&
                !rewritten3D.Contains(root, StringComparison.OrdinalIgnoreCase) &&
                !rewritten3D.Contains(
                    portableRoot,
                    StringComparison.OrdinalIgnoreCase),
                "ACS3D prefab Cook rewrites every 3D asset directive and leaks no local absolute path");

            string source2D =
                "ACSCENE v1\n" +
                $"SPRT 1 {sprite}\n" +
                $"MAT 1 {material}\n";
            string rewritten2D = Encoding.UTF8.GetString(
                PackageCore.RewritePrefabPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(source2D),
                    assets,
                    root));
            Check(
                rewritten2D ==
                    "ACSCENE v1\n" +
                    "SPRT 1 Assets/Textures/cloud.png\n" +
                    "MAT 1 Assets/Materials/cloud.acsmat\n",
                "legacy ACSCENE prefab Cook keeps the 2D reference grammar");

            CheckThrows<InvalidDataException>(
                () => PackageCore.RewritePrefabPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSPREFAB 1\nMSH3D 1 " + mesh + "\n"),
                    assets,
                    root),
                "unknown prefab payload headers fail closed");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static void VerifyBlueprintCookRewrite()
    {
        string root = FixtureRoot("blueprint-cook");
        string assets = Path.Combine(root, "Assets");
        string blueprints = Path.Combine(assets, "Blueprints");
        string parent = Path.Combine(blueprints, "Base.acsbp");
        string outside = Path.Combine(root, "Outside.acsbp");
        string mesh = Path.Combine(assets, "Models", "aircraft.glb");
        string sprite = Path.Combine(assets, "Textures", "cloud.png");
        string material = Path.Combine(assets, "Materials", "cloud.acsmat");
        string nestedPrefab = Path.Combine(assets, "Prefabs", "engine.acsprefab");
        Directory.CreateDirectory(blueprints);
        Directory.CreateDirectory(Path.GetDirectoryName(mesh)!);
        Directory.CreateDirectory(Path.GetDirectoryName(sprite)!);
        Directory.CreateDirectory(Path.GetDirectoryName(material)!);
        Directory.CreateDirectory(Path.GetDirectoryName(nestedPrefab)!);
        File.WriteAllText(parent, "ACSBP 1\n", new UTF8Encoding(false));
        File.WriteAllText(outside, "ACSBP 1\n", new UTF8Encoding(false));
        File.WriteAllBytes(mesh, [0x67, 0x6c, 0x54, 0x46]);
        File.WriteAllBytes(sprite, [1, 2, 3, 4]);
        File.WriteAllText(material, "ACSMAT 1\n", new UTF8Encoding(false));
        File.WriteAllText(nestedPrefab, "ACS3D v2\n", new UTF8Encoding(false));

        try
        {
            string source3D =
                "ACSBP 1\n" +
                $"PARENT {parent}\n" +
                "CMP 6\n" +
                "ACS3D v2\n" +
                "N3D 1\n" +
                $"MSH3D 1 {mesh}\n" +
                $"SPR3D 1 {sprite}\n" +
                $"MAT3D 1 {material}\n" +
                $"PFAB3D 1 {nestedPrefab}\n";
            string rewritten3D = Encoding.UTF8.GetString(
                PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(source3D),
                    assets,
                    root));
            string portableRoot = root.Replace('\\', '/');
            Check(
                rewritten3D ==
                    "ACSBP 1\n" +
                    "PARENT Assets/Blueprints/Base.acsbp\n" +
                    "CMP 6\n" +
                    "ACS3D v2\n" +
                    "N3D 1\n" +
                    "MSH3D 1 Assets/Models/aircraft.glb\n" +
                    "SPR3D 1 Assets/Textures/cloud.png\n" +
                    "MAT3D 1 Assets/Materials/cloud.acsmat\n" +
                    "PFAB3D 1 Assets/Prefabs/engine.acsprefab\n" &&
                !rewritten3D.Contains(root, StringComparison.OrdinalIgnoreCase) &&
                !rewritten3D.Contains(
                    portableRoot,
                    StringComparison.OrdinalIgnoreCase),
                "Blueprint Cook rewrites PARENT and every ACS3D CMP reference in one portable transaction");

            string source2D =
                "ACSBP 1\n" +
                "CMP 6\n" +
                "ACSCENE v1\n" +
                "0\n" +
                $"SPRT 1 {sprite}\n" +
                $"MAT 1 {material}\n" +
                $"PFAB 1 {nestedPrefab}\n" +
                "RPLY 1 3 0 0 1 0 0 1\n";
            string rewritten2D = Encoding.UTF8.GetString(
                PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(source2D),
                    assets,
                    root));
            Check(
                rewritten2D ==
                    "ACSBP 1\n" +
                    "CMP 6\n" +
                    "ACSCENE v1\n" +
                    "0\n" +
                    "SPRT 1 Assets/Textures/cloud.png\n" +
                    "MAT 1 Assets/Materials/cloud.acsmat\n" +
                    "PFAB 1 Assets/Prefabs/engine.acsprefab\n" +
                    "RPLY 1 3 0 0 1 0 0 1\n",
                "Blueprint Cook rewrites legacy ACSCENE CMP sprite, material, and prefab references");

            string rewrittenAbsolute = Encoding.UTF8.GetString(
                PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 1\nPARENT " + parent + "\n"),
                    assets,
                    root));
            Check(
                rewrittenAbsolute ==
                    "ACSBP 1\nPARENT Assets/Blueprints/Base.acsbp\n" &&
                !rewrittenAbsolute.Contains(root, StringComparison.OrdinalIgnoreCase),
                "Blueprint Cook rewrites an in-Assets absolute PARENT to a portable Assets path");

            string portableSource =
                "ACSBP 1\nPARENT Assets/Blueprints/Base.acsbp\n";
            byte[] portableBytes = new UTF8Encoding(false).GetBytes(portableSource);
            byte[] portableResult =
                PackageCore.RewriteBlueprintPayloadForSelfTest(
                    portableBytes,
                    assets,
                    root);
            Check(
                ReferenceEquals(portableBytes, portableResult) &&
                Encoding.UTF8.GetString(portableResult) == portableSource,
                "portable Blueprint PARENT remains byte-stable");

            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 2\nPARENT Assets/Blueprints/Base.acsbp\n"),
                    assets,
                    root),
                "unknown Blueprint headers fail closed");
            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 1\nPARENT " + outside + "\n"),
                    assets,
                    root),
                "Blueprint PARENT outside Assets fails closed");
            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 1\nPARENT Assets/Blueprints/Base.acsbp\n" +
                        "PARENT Assets/Blueprints/Base.acsbp\n"),
                    assets,
                    root),
                "duplicate Blueprint PARENT directives fail closed");

            byte[] transactionalSource = new UTF8Encoding(false).GetBytes(
                "ACSBP 1\n" +
                $"PARENT {parent}\n" +
                "CMP 4\n" +
                "ACS3D v2\n" +
                $"MSH3D 1 {mesh}\n" +
                $"SPR3D 1 {sprite}\n" +
                "MAT3D 1 Assets/Materials/Missing.acsmat\n");
            byte[] transactionalSnapshot = transactionalSource.ToArray();
            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    transactionalSource,
                    assets,
                    root),
                "Blueprint PARENT and CMP rewrite fails closed when a later component dependency is missing");
            Check(
                transactionalSource.SequenceEqual(transactionalSnapshot),
                "failed Blueprint Cook leaves the original PARENT and CMP payload byte-for-byte untouched");

            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 1\nCMP nope\nACS3D v2\n"),
                    assets,
                    root),
                "non-numeric Blueprint CMP counts fail closed");
            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 1\nCMP 2147483648\nACS3D v2\n"),
                    assets,
                    root),
                "overflowing Blueprint CMP counts fail closed");
            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 1\nCMP 2\nACS3D v2"),
                    assets,
                    root),
                "truncated Blueprint CMP bodies fail closed");
            CheckThrows<InvalidDataException>(
                () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSBP 1\nCMP 2\nACS3D v2\nBOGUS 1\n"),
                    assets,
                    root),
                "unknown embedded Blueprint CMP grammar fails closed");

            foreach ((string malformedText, string label) in new[]
            {
                (
                    "ACSBP 1\nCMP\t2\nACS3D v2\n" +
                    "PFAB3D 1 Assets/Prefabs/engine.acsprefab\n",
                    "tab-delimited Blueprint CMP cannot hide component dependencies"),
                (
                    "ACSBP 1\nCMP  1\nACS3D v2\n",
                    "multi-space Blueprint CMP is rejected as non-canonical"),
                (
                    "ACSBP 1\nCMP 1 \nACS3D v2\n",
                    "trailing Blueprint CMP tokens are rejected"),
                (
                    "ACSBP 1\nPARENT\tAssets/Blueprints/Base.acsbp\n",
                    "tab-delimited Blueprint PARENT cannot hide inheritance"),
                (
                    "ACSBP 1\nPARENT\n",
                    "empty Blueprint PARENT is rejected"),
                (
                    "ACSBP 1\nPARENT  Assets/Blueprints/Base.acsbp\n",
                    "multi-space Blueprint PARENT is rejected as non-canonical"),
            })
            {
                byte[] malformedBytes =
                    new UTF8Encoding(false).GetBytes(malformedText);
                byte[] malformedSnapshot = malformedBytes.ToArray();
                CheckThrows<InvalidDataException>(
                    () => PackageCore.RewriteBlueprintPayloadForSelfTest(
                        malformedBytes,
                        assets,
                        root),
                    label);
                Check(
                    malformedBytes.SequenceEqual(malformedSnapshot),
                    label + " without mutating the source payload");
            }
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static void VerifyCanonicalCookClosureContract()
    {
        string root = FixtureRoot("canonical-cook-closure");
        string assets = Path.Combine(root, "Assets");
        string materials = Path.Combine(assets, "Materials");
        string textures = Path.Combine(assets, "Textures");
        string scene = Path.Combine(assets, "main.acscene");
        string material = Path.Combine(materials, "Water.acsmat");
        string texture = Path.Combine(textures, "required.png");
        string unused = Path.Combine(assets, "unused.mystery");
        Directory.CreateDirectory(materials);
        Directory.CreateDirectory(textures);
        File.WriteAllText(
            scene,
            "ACSCENE v1\n1\nMAT 1 Assets/Materials/Water.acsmat\n",
            new UTF8Encoding(false));
        File.WriteAllText(
            material,
            "ACSMAT 1\nalbedo Assets/Textures/required.png\n",
            new UTF8Encoding(false));
        File.WriteAllBytes(texture, [1, 2, 3, 4]);
        File.WriteAllText(unused, "not reachable", new UTF8Encoding(false));

        try
        {
            var database = new AssetDatabase(root, assets);
            database.Refresh(verifyContent: true);
            AssetRecord sceneRecord = database.Snapshot().Single(item =>
                item.RelativePath == "main.acscene");
            AssetRecord materialRecord = database.Snapshot().Single(item =>
                item.RelativePath == "Materials/Water.acsmat");
            AssetRecord textureRecord = database.Snapshot().Single(item =>
                item.RelativePath == "Textures/required.png");
            database.UpdateImportMetadata(
                sceneRecord.AssetId,
                sceneRecord.Metadata.Source,
                sceneRecord.Metadata.Importer,
                sceneRecord.Metadata.ImporterVersion,
                [materialRecord.AssetId],
                sceneRecord.Metadata.ImportSettings);
            database.UpdateImportMetadata(
                materialRecord.AssetId,
                materialRecord.Metadata.Source,
                materialRecord.Metadata.Importer,
                materialRecord.Metadata.ImporterVersion,
                [textureRecord.AssetId],
                materialRecord.Metadata.ImportSettings);
            string projectFile = Path.Combine(root, "Fixture.acsproject");
            File.WriteAllText(
                projectFile,
                $$"""
                {
                  "version": 1,
                  "name": "Fixture",
                  "engineVersion": "self-test",
                  "initialScene": "Assets/main.acscene",
                  "canonicalSceneAssetId": "{{sceneRecord.AssetId}}"
                }
                """,
                new UTF8Encoding(false));
            var packageProject = new PackageProjectInfo(
                "Fixture",
                1,
                "self-test",
                projectFile,
                "Assets/main.acscene",
                sceneRecord.AssetId);

            var planner = new AssetCookPlanner(root, assets);
            AssetCookPlan first = planner.BuildByAssetId(sceneRecord.AssetId);
            AssetCookPlan second =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            string[] expected =
            [
                "Materials/Water.acsmat",
                "Textures/required.png",
                "main.acscene",
            ];
            Check(
                !first.HasErrors &&
                !second.HasErrors &&
                first.Assets.Select(static item => item.RelativePath)
                    .SequenceEqual(expected, StringComparer.Ordinal) &&
                second.Assets.Select(static item => item.RelativePath)
                    .SequenceEqual(expected, StringComparer.Ordinal) &&
                string.Equals(first.GraphHash, second.GraphHash, StringComparison.Ordinal),
                "canonical Scene Asset ID produces a stable required-only Cook closure");
            PackageCore.ValidateProjectSceneStateForPublish(
                packageProject,
                sceneRecord.AssetId,
                cookedAssetGraphHash: first.GraphHash);
            Pass("unchanged required Cook graph passes the final publication gate");

            File.WriteAllText(unused, "changed but still unreachable", new UTF8Encoding(false));
            AssetCookPlan unusedChanged =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                !unusedChanged.HasErrors &&
                string.Equals(
                    first.GraphHash,
                    unusedChanged.GraphHash,
                    StringComparison.Ordinal) &&
                unusedChanged.Assets.All(item =>
                    !item.RelativePath.Equals(
                        "unused.mystery",
                        StringComparison.OrdinalIgnoreCase)),
                "unreachable Asset changes do not enter or perturb the Cook graph");
            PackageCore.ValidateProjectSceneStateForPublish(
                packageProject,
                sceneRecord.AssetId,
                cookedAssetGraphHash: first.GraphHash);
            Pass("unreachable Asset changes do not block package publication");

            database.UpdateImportMetadata(
                textureRecord.AssetId,
                textureRecord.Metadata.Source,
                textureRecord.Metadata.Importer,
                textureRecord.Metadata.ImporterVersion,
                textureRecord.Metadata.Dependencies,
                textureRecord.Metadata.ImportSettings.Concat(
                [
                    new KeyValuePair<string, string>(
                        "directSidecarRevision",
                        "changed"),
                ]));
            Check(
                ProjectGraphDriftIsRejected(
                    packageProject,
                    sceneRecord.AssetId,
                    first.GraphHash),
                "required Asset sidecar drift blocks final package publication");
            database.UpdateImportMetadata(
                textureRecord.AssetId,
                textureRecord.Metadata.Source,
                textureRecord.Metadata.Importer,
                textureRecord.Metadata.ImporterVersion,
                textureRecord.Metadata.Dependencies,
                textureRecord.Metadata.ImportSettings);

            string movedTexture = Path.Combine(textures, "required-moved.png");
            string textureMetadata = texture + AssetDatabase.MetadataSuffix;
            string movedTextureMetadata =
                movedTexture + AssetDatabase.MetadataSuffix;
            try
            {
                File.Move(texture, movedTexture);
                File.Move(textureMetadata, movedTextureMetadata);
                Check(
                    ProjectGraphDriftIsRejected(
                        packageProject,
                        sceneRecord.AssetId,
                        first.GraphHash),
                    "required Asset path drift blocks final package publication");
            }
            finally
            {
                if (File.Exists(movedTextureMetadata) &&
                    !File.Exists(textureMetadata))
                {
                    File.Move(movedTextureMetadata, textureMetadata);
                }
                if (File.Exists(movedTexture) && !File.Exists(texture))
                    File.Move(movedTexture, texture);
            }

            File.WriteAllBytes(texture, [1, 2, 3, 4, 5]);
            AssetCookPlan requiredChanged =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                !requiredChanged.HasErrors &&
                !string.Equals(
                    first.GraphHash,
                    requiredChanged.GraphHash,
                    StringComparison.Ordinal),
                "required Asset content changes invalidate the logical Cook graph hash");
            Check(
                ProjectGraphDriftIsRejected(
                    packageProject,
                    sceneRecord.AssetId,
                    first.GraphHash),
                "required Asset graph drift blocks final package publication");

            string missingAssetId = Guid.NewGuid().ToString("N");
            database.UpdateImportMetadata(
                materialRecord.AssetId,
                materialRecord.Metadata.Source,
                materialRecord.Metadata.Importer,
                materialRecord.Metadata.ImporterVersion,
                [missingAssetId],
                materialRecord.Metadata.ImportSettings);
            AssetCookPlan missing =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                missing.HasErrors &&
                missing.Diagnostics.Any(diagnostic =>
                    diagnostic.Code == "ASSET_DEPENDENCY_MISSING" &&
                    diagnostic.AssetId == missingAssetId),
                "missing required dependency GUID fails closed with a structured diagnostic");

            database.UpdateImportMetadata(
                materialRecord.AssetId,
                materialRecord.Metadata.Source,
                materialRecord.Metadata.Importer,
                materialRecord.Metadata.ImporterVersion,
                [textureRecord.AssetId],
                materialRecord.Metadata.ImportSettings);
            database.UpdateImportMetadata(
                textureRecord.AssetId,
                textureRecord.Metadata.Source,
                textureRecord.Metadata.Importer,
                textureRecord.Metadata.ImporterVersion,
                [sceneRecord.AssetId],
                textureRecord.Metadata.ImportSettings);
            AssetCookPlan cyclic =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                cyclic.HasErrors &&
                cyclic.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_DEPENDENCY_CYCLE"),
                "reachable dependency cycle fails closed with a structured diagnostic");

            database.UpdateImportMetadata(
                textureRecord.AssetId,
                textureRecord.Metadata.Source,
                textureRecord.Metadata.Importer,
                textureRecord.Metadata.ImporterVersion,
                [],
                textureRecord.Metadata.ImportSettings);
            File.WriteAllText(
                material,
                "ACSMAT 1\nalbedo ../../outside.png\n",
                new UTF8Encoding(false));
            database.UpdateImportMetadata(
                materialRecord.AssetId,
                materialRecord.Metadata.Source,
                materialRecord.Metadata.Importer,
                materialRecord.Metadata.ImporterVersion,
                [],
                materialRecord.Metadata.ImportSettings);
            AssetCookPlan escaped =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                escaped.HasErrors &&
                escaped.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_REFERENCE_ESCAPE"),
                "reachable path escape fails closed with a structured diagnostic");

            File.WriteAllText(
                material,
                "ACSMAT 1\nalbedo\n",
                new UTF8Encoding(false));
            AssetCookPlan malformed =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                malformed.HasErrors &&
                malformed.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_DEPENDENCY_SCAN_FAILED"),
                "malformed reference directives cannot silently remove required dependencies");

            File.WriteAllText(
                material,
                "ACSMAT 1\nalbedo Assets/Textures/required.png\n",
                new UTF8Encoding(false));
            AssetCookPlan stale =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                stale.HasErrors &&
                stale.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_METADATA_STALE"),
                "source/dependency metadata divergence fails closed as stale Asset DB state");

            database.UpdateImportMetadata(
                materialRecord.AssetId,
                materialRecord.Metadata.Source,
                materialRecord.Metadata.Importer,
                materialRecord.Metadata.ImporterVersion,
                [textureRecord.AssetId],
                materialRecord.Metadata.ImportSettings);
            string indexPath = Path.Combine(
                assets,
                AssetDatabase.InternalDirectoryName,
                "index.v1.json");
            File.WriteAllText(indexPath, "{", new UTF8Encoding(false));
            AssetCookPlan ignoredCache =
                new AssetCookPlanner(root, assets).BuildByAssetId(sceneRecord.AssetId);
            Check(
                !ignoredCache.HasErrors &&
                ignoredCache.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_INDEX_CACHE_IGNORED") &&
                ignoredCache.Assets.Select(static item => item.RelativePath)
                    .SequenceEqual(expected, StringComparer.Ordinal),
                "stale acceleration cache is discarded in favor of authoritative sidecars");

            AssetCookPlan missingId =
                new AssetCookPlanner(root, assets).BuildByAssetId("");
            AssetCookPlan invalidId =
                new AssetCookPlanner(root, assets).BuildByAssetId("not-a-guid");
            AssetCookPlan wrongKind =
                new AssetCookPlanner(root, assets).BuildByAssetId(textureRecord.AssetId);
            Check(
                missingId.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "CANONICAL_SCENE_ASSET_ID_REQUIRED") &&
                invalidId.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "CANONICAL_SCENE_ASSET_ID_INVALID") &&
                wrongKind.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "CANONICAL_SCENE_KIND_INVALID"),
                "canonical Cook root rejects missing, malformed, and non-scene Asset IDs");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static bool ProjectGraphDriftIsRejected(
        PackageProjectInfo project,
        string canonicalSceneAssetId,
        string cookedAssetGraphHash)
    {
        try
        {
            PackageCore.ValidateProjectSceneStateForPublish(
                project,
                canonicalSceneAssetId,
                cookedAssetGraphHash: cookedAssetGraphHash);
            return false;
        }
        catch (PackageValidationException error)
        {
            return error.Issues.Any(static issue =>
                issue.Code == "PROJECT_CHANGED_DURING_PACKAGE");
        }
    }

    private static void VerifyCanonicalSceneSnapshotIsolation()
    {
        string root = FixtureRoot("canonical-scene-snapshot");
        string assets = Path.Combine(root, "Assets");
        string scene = Path.Combine(assets, "main.acscene");
        Directory.CreateDirectory(assets);
        byte[] captured =
            new UTF8Encoding(false).GetBytes("ACSCENE v1\n0\n");
        File.WriteAllBytes(scene, captured);
        var database = new AssetDatabase(root, assets);
        database.Refresh(verifyContent: true);
        AssetRecord capturedRecord = database.Snapshot().Single(item =>
            item.RelativePath == "main.acscene");
        byte[] pathRevision = captured.ToArray();
        pathRevision[0] = 0;
        File.WriteAllBytes(scene, pathRevision);

        try
        {
            CanonicalSceneAdapterInspection pathInspection =
                CanonicalSceneAdapter.InspectFile(scene);
            CanonicalSceneAdapterInspection snapshotInspection =
                PackageCore.InspectCanonicalSceneSnapshotForSelfTest(
                    captured,
                    ".acscene",
                    scene);
            Check(
                pathInspection.HasErrors &&
                !snapshotInspection.HasErrors &&
                snapshotInspection.Envelope.sourceFormat ==
                    CanonicalSceneAdapter.LegacyScene2DFormat,
                "canonical Scene bootstrap inspection uses the verified Cook bytes, " +
                "not a second path read");
            CheckThrows<InvalidDataException>(
                () => new AssetCookPlanner(root, assets)
                    .ReadVerifiedScanSnapshotForSelfTest(capturedRecord),
                "dependency scanning rejects path bytes that differ from the " +
                "content-hash snapshot");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static void VerifyBlueprintInheritanceClosure()
    {
        string root = FixtureRoot("blueprint-inheritance");
        string assets = Path.Combine(root, "Assets");
        string blueprints = Path.Combine(assets, "Blueprints");
        string scene = Path.Combine(assets, "main.acs3d");
        string child = Path.Combine(blueprints, "Child.acsbp");
        string parent = Path.Combine(blueprints, "Parent.acsbp");
        string grandparent = Path.Combine(blueprints, "Grandparent.acsbp");
        string mesh = Path.Combine(assets, "Models", "aircraft.glb");
        string sprite = Path.Combine(assets, "Textures", "cloud.png");
        string material = Path.Combine(assets, "Materials", "cloud.acsmat");
        string nestedPrefab = Path.Combine(assets, "Prefabs", "engine.acsprefab");
        Directory.CreateDirectory(blueprints);
        Directory.CreateDirectory(Path.GetDirectoryName(mesh)!);
        Directory.CreateDirectory(Path.GetDirectoryName(sprite)!);
        Directory.CreateDirectory(Path.GetDirectoryName(material)!);
        Directory.CreateDirectory(Path.GetDirectoryName(nestedPrefab)!);
        File.WriteAllText(
            scene,
            "ACS3D v2\nPFAB3D 1 Assets/Blueprints/Child.acsbp\n",
            new UTF8Encoding(false));
        File.WriteAllText(
            child,
            "ACSBP 1\n" +
            "PARENT Assets/Blueprints/Parent.acsbp\n" +
            "CMP 6\n" +
            "ACS3D v2\n" +
            "N3D 1\n" +
            "MSH3D 1 Assets/Models/aircraft.glb\n" +
            "SPR3D 1 Assets/Textures/cloud.png\n" +
            "MAT3D 1 Assets/Materials/cloud.acsmat\n" +
            "PFAB3D 1 Assets/Prefabs/engine.acsprefab\n",
            new UTF8Encoding(false));
        File.WriteAllText(
            parent,
            "ACSBP 1\nPARENT Assets/Blueprints/Grandparent.acsbp\n",
            new UTF8Encoding(false));
        File.WriteAllText(grandparent, "ACSBP 1\n", new UTF8Encoding(false));
        File.WriteAllBytes(mesh, [0x67, 0x6c, 0x54, 0x46]);
        File.WriteAllBytes(sprite, [1, 2, 3, 4]);
        File.WriteAllText(material, "ACSMAT 1\n", new UTF8Encoding(false));
        File.WriteAllText(nestedPrefab, "ACS3D v2\n", new UTF8Encoding(false));

        try
        {
            var database = new AssetDatabase(root, assets);
            database.Refresh(verifyContent: true);
            database.TryGetByPath(scene, out AssetRecord? sceneRecord);
            database.TryGetByPath(child, out AssetRecord? childRecord);
            if (sceneRecord == null || childRecord == null)
                throw new InvalidDataException(
                    "Blueprint inheritance fixture was not indexed.");
            database.UpdateImportMetadata(
                sceneRecord.AssetId,
                sceneRecord.Metadata.Source,
                sceneRecord.Metadata.Importer,
                sceneRecord.Metadata.ImporterVersion,
                [childRecord.AssetId],
                sceneRecord.Metadata.ImportSettings);

            AssetCookPlan nested = new AssetCookPlanner(root, assets).Build(scene);
            string[] nestedPaths = nested.Assets
                .Select(static asset => asset.RelativePath)
                .OrderBy(static path => path, StringComparer.Ordinal)
                .ToArray();
            Check(
                !nested.HasErrors &&
                nestedPaths.SequenceEqual(
                    new[]
                    {
                        "Blueprints/Child.acsbp",
                        "Blueprints/Grandparent.acsbp",
                        "Blueprints/Parent.acsbp",
                        "Materials/cloud.acsmat",
                        "Models/aircraft.glb",
                        "Prefabs/engine.acsprefab",
                        "Textures/cloud.png",
                        "main.acs3d",
                    },
                    StringComparer.Ordinal),
                "Blueprint PARENT and CMP dependencies join Cook closure without sidecar dependency metadata");

            File.WriteAllText(
                grandparent,
                "ACSBP 1\nPARENT Assets/Blueprints/Child.acsbp\n",
                new UTF8Encoding(false));
            AssetCookPlan cyclic = new AssetCookPlanner(root, assets).Build(scene);
            Check(
                cyclic.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_DEPENDENCY_CYCLE"),
                "source-authored Blueprint inheritance cycle fails closed");

            File.WriteAllText(
                grandparent,
                "ACSBP 1\nPARENT Assets/Blueprints/Missing.acsbp\n",
                new UTF8Encoding(false));
            AssetCookPlan missing = new AssetCookPlanner(root, assets).Build(scene);
            Check(
                missing.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_REFERENCE_MISSING"),
                "missing nested Blueprint parent fails closed");

            File.WriteAllText(grandparent, "ACSBP 1\n", new UTF8Encoding(false));
            File.WriteAllText(
                child,
                "ACSBP 1\n" +
                "PARENT Assets/Blueprints/Parent.acsbp\n" +
                "CMP 2\n" +
                "ACS3D v2\n" +
                "PFAB3D 1 Assets/Prefabs/Missing.acsprefab\n",
                new UTF8Encoding(false));
            AssetCookPlan missingComponent =
                new AssetCookPlanner(root, assets).Build(scene);
            Check(
                missingComponent.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_REFERENCE_MISSING"),
                "missing Blueprint CMP dependency fails closed");

            File.WriteAllText(
                child,
                "ACSBP 1\nCMP 2\nACS3D v2\nBOGUS 1\n",
                new UTF8Encoding(false));
            AssetCookPlan malformed =
                new AssetCookPlanner(root, assets).Build(scene);
            Check(
                malformed.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_DEPENDENCY_SCAN_FAILED"),
                "malformed Blueprint CMP grammar cannot publish a partial Cook closure");

            File.WriteAllText(
                child,
                "ACSBP 1\nPARENT\tAssets/Blueprints/Parent.acsbp\n",
                new UTF8Encoding(false));
            AssetCookPlan hiddenParent =
                new AssetCookPlanner(root, assets).Build(scene);
            Check(
                hiddenParent.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_DEPENDENCY_SCAN_FAILED"),
                "non-canonical Blueprint PARENT cannot disappear from the Cook scanner");

            File.WriteAllText(
                child,
                "ACSBP 1\nCMP\t2\nACS3D v2\n" +
                "PFAB3D 1 Assets/Prefabs/engine.acsprefab\n",
                new UTF8Encoding(false));
            AssetCookPlan hiddenComponent =
                new AssetCookPlanner(root, assets).Build(scene);
            Check(
                hiddenComponent.Diagnostics.Any(static diagnostic =>
                    diagnostic.Code == "ASSET_DEPENDENCY_SCAN_FAILED"),
                "non-canonical Blueprint CMP cannot disappear from the Cook scanner");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static void VerifyBlueprintCookCacheVersion()
    {
        string root = FixtureRoot("blueprint-ddc-version");
        string cacheRoot = Path.Combine(root, "Temp", "DerivedDataCache");
        Directory.CreateDirectory(root);
        try
        {
            const string assetId = "11111111111111111111111111111111";
            var metadata = new AssetMetadata(
                1,
                assetId,
                "blueprint",
                "Blueprints/Child.acsbp",
                "AcsBlueprintImporter",
                1,
                Array.Empty<string>(),
                new Dictionary<string, string>(StringComparer.Ordinal));
            var record = new AssetRecord(
                assetId,
                "Blueprints/Child.acsbp",
                Path.Combine(root, "Assets", "Blueprints", "Child.acsbp"),
                "blueprint",
                64,
                0,
                "same-source-content-hash",
                metadata);
            const string graphHash = "same-asset-graph-hash";
            KeyValuePair<string, string>[] currentSettings =
                PackageCore.CreateAssetCookerSettingsForSelfTest(graphHash);
            KeyValuePair<string, string>[] legacySettings = currentSettings
                .Select(static setting =>
                    setting.Key == "referenceRewriteVersion"
                        ? new KeyValuePair<string, string>(setting.Key, "3")
                        : setting)
                .ToArray();

            var cache = new DerivedDataCache(root, cacheRoot);
            DerivedDataCacheResult legacy = cache.GetOrCreate(
                record,
                PackageCore.AssetCookerVersionForSelfTest,
                legacySettings,
                () => Encoding.UTF8.GetBytes("absolute-parent-v3"));
            int currentProducerCalls = 0;
            DerivedDataCacheResult current = cache.GetOrCreate(
                record,
                PackageCore.AssetCookerVersionForSelfTest,
                currentSettings,
                () =>
                {
                    ++currentProducerCalls;
                    return Encoding.UTF8.GetBytes("portable-parent-v4");
                });
            DerivedDataCacheResult currentHit = cache.GetOrCreate(
                record,
                PackageCore.AssetCookerVersionForSelfTest,
                currentSettings,
                () => throw new InvalidOperationException(
                    "Current Blueprint Cook cache unexpectedly missed."));

            Check(
                legacy.Status == DerivedDataCacheStatus.Miss &&
                current.Status == DerivedDataCacheStatus.Miss &&
                currentHit.Status == DerivedDataCacheStatus.Hit &&
                !string.Equals(legacy.Key, current.Key, StringComparison.Ordinal) &&
                currentProducerCalls == 1 &&
                Encoding.UTF8.GetString(current.Payload) == "portable-parent-v4" &&
                Encoding.UTF8.GetString(currentHit.Payload) == "portable-parent-v4",
                "Blueprint rewrite v4 cannot reuse a v3 DDC payload with identical source, metadata, and graph hash");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static void VerifyBlueprintParentPathPolicy()
    {
        string root = FixtureRoot("blueprint-parent-path");
        string assets = Path.Combine(root, "Assets");
        string blueprints = Path.Combine(assets, "Blueprints");
        string child = Path.Combine(blueprints, "Child.acsbp");
        string parent = Path.Combine(blueprints, "Base.acsbp");
        string grandparent = Path.Combine(blueprints, "Grandparent.acsbp");
        Directory.CreateDirectory(blueprints);
        File.WriteAllText(child, "ACSBP 1\n", new UTF8Encoding(false));
        File.WriteAllText(parent, "ACSBP 1\n", new UTF8Encoding(false));
        File.WriteAllText(grandparent, "ACSBP 1\n", new UTF8Encoding(false));

        try
        {
            bool madePortable = BlueprintParentPathPolicy.TryMakePortable(
                assets,
                child,
                parent,
                out string portable,
                out _);
            bool resolvedPortable = BlueprintParentPathPolicy.TryResolve(
                assets,
                portable,
                out string resolved,
                out _);
            Check(
                madePortable &&
                portable == "Assets/Blueprints/Base.acsbp" &&
                resolvedPortable &&
                string.Equals(resolved, parent, StringComparison.OrdinalIgnoreCase),
                "Blueprint parent nested path round-trips through portable Assets form");

            Check(
                BlueprintParentPathPolicy.TryResolve(
                    assets,
                    "assets/blueprints/base.ACSBP",
                    out string caseResolved,
                    out _) &&
                string.Equals(caseResolved, parent, StringComparison.OrdinalIgnoreCase),
                "portable Blueprint parent keeps Windows case-insensitive compatibility");

            Check(
                !BlueprintParentPathPolicy.TryResolve(
                    assets,
                    "Assets/Blueprints/../Base.acsbp",
                    out _,
                    out _) &&
                !BlueprintParentPathPolicy.TryResolve(
                    assets,
                    parent,
                    out _,
                    out _) &&
                !BlueprintParentPathPolicy.TryResolve(
                    assets,
                    "Assets/Blueprints/Missing.acsbp",
                    out _,
                    out _),
                "Blueprint parent rejects dot-segment, persisted absolute, and missing paths");

            foreach ((string directive, string label) in new[]
            {
                (
                    "PARENT\tAssets/Blueprints/Grandparent.acsbp\n",
                    "Blueprint parent selection rejects a tab-delimited PARENT"),
                (
                    "PARENT  Assets/Blueprints/Grandparent.acsbp\n",
                    "Blueprint parent selection rejects a multi-space PARENT"),
                (
                    "PARENT\n",
                    "Blueprint parent selection rejects an empty PARENT"),
            })
            {
                File.WriteAllText(
                    parent,
                    "ACSBP 1\n" + directive,
                    new UTF8Encoding(false));
                Check(
                    !BlueprintParentPathPolicy.TryMakePortable(
                        assets,
                        child,
                        parent,
                        out _,
                        out string malformedError) &&
                    malformedError.Contains(
                        "PARENT",
                        StringComparison.Ordinal),
                    label);
            }

            File.WriteAllText(
                parent,
                "ACSBP 1\nPARENT Assets/Blueprints/Grandparent.acsbp\n",
                new UTF8Encoding(false));
            Check(
                BlueprintParentPathPolicy.TryMakePortable(
                    assets,
                    child,
                    parent,
                    out string nestedPortable,
                    out _) &&
                nestedPortable == "Assets/Blueprints/Base.acsbp",
                "nested Blueprint parent chain validates without flattening subfolders");

            File.WriteAllText(
                grandparent,
                "ACSBP 1\nPARENT Assets/Blueprints/Child.acsbp\n",
                new UTF8Encoding(false));
            Check(
                !BlueprintParentPathPolicy.TryMakePortable(
                    assets,
                    child,
                    parent,
                    out _,
                    out _),
                "Blueprint parent selection rejects a nested inheritance cycle");

            File.WriteAllText(
                grandparent,
                "ACSBP 1\nPARENT Assets/Blueprints/Missing.acsbp\n",
                new UTF8Encoding(false));
            Check(
                !BlueprintParentPathPolicy.TryMakePortable(
                    assets,
                    child,
                    parent,
                    out _,
                    out _),
                "Blueprint parent selection rejects a missing nested ancestor");

            string linkedDirectory = Path.Combine(assets, "Linked");
            string externalDirectory = Path.Combine(root, "External");
            Directory.CreateDirectory(externalDirectory);
            File.WriteAllText(
                Path.Combine(externalDirectory, "LinkedParent.acsbp"),
                "ACSBP 1\n",
                new UTF8Encoding(false));
            try
            {
                Directory.CreateSymbolicLink(linkedDirectory, externalDirectory);
                Check(
                    !BlueprintParentPathPolicy.TryMakePortable(
                        assets,
                        child,
                        Path.Combine(linkedDirectory, "LinkedParent.acsbp"),
                        out _,
                        out _),
                    "Blueprint parent selection rejects reparse-point paths");
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    PlatformNotSupportedException)
            {
                Pass(
                    "reparse-point Blueprint parent test skipped by host policy: " +
                    error.GetType().Name);
            }
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static async Task VerifyLatestOnlyValidationAsync()
    {
        using var coordinator = new PackageValidationCoordinator();
        using PackageValidationOperation first = coordinator.BeginLatest();
        var firstStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        int active = 0;
        int maximumActive = 0;
        Task<int> firstTask = coordinator.RunAsync<int>(
            first,
            token =>
            {
                int current = Interlocked.Increment(ref active);
                UpdateMaximum(ref maximumActive, current);
                firstStarted.TrySetResult(true);
                try
                {
                    while (true)
                    {
                        token.ThrowIfCancellationRequested();
                        Thread.Sleep(1);
                    }
                }
                finally
                {
                    Interlocked.Decrement(ref active);
                }
            });
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));

        using PackageValidationOperation second = coordinator.BeginLatest();
        Task<int> secondTask = coordinator.RunAsync(
            second,
            token =>
            {
                int current = Interlocked.Increment(ref active);
                UpdateMaximum(ref maximumActive, current);
                try
                {
                    token.ThrowIfCancellationRequested();
                    return 42;
                }
                finally
                {
                    Interlocked.Decrement(ref active);
                }
            });

        bool firstCancelled = false;
        try
        {
            _ = await firstTask;
        }
        catch (OperationCanceledException)
        {
            firstCancelled = true;
        }
        Check(firstCancelled, "latest validation cancels prior work");
        Check(await secondTask == 42, "latest validation completes");
        Check(maximumActive == 1, "validation filesystem work is serialized");
        Check(coordinator.IsCurrent(second), "latest validation identity is current");
    }

    private static async Task VerifyConfigSnapshotAsync()
    {
        string root = FixtureRoot("config");
        string config = Path.Combine(root, "Config");
        string staged = Path.Combine(root, "Stage", "Config");
        Directory.CreateDirectory(Path.Combine(root, "Assets"));
        Directory.CreateDirectory(config);
        string settings = Path.Combine(config, "ProjectSettings.ini");
        File.WriteAllText(
            settings,
            "[Game]\nDefaultScene=Assets/main.acscene\n" +
            "[Plugin]\nMode=Baseline\n",
            new UTF8Encoding(false));
        File.WriteAllText(
            Path.Combine(config, "Input.ini"),
            "[Input]\nJump=Space\n",
            new UTF8Encoding(false));
        string projectFile = Path.Combine(root, "Game.acsproject");
        File.WriteAllText(projectFile, "{}", new UTF8Encoding(false));
        var project = new PackageProjectInfo(
            "Game",
            1,
            "test",
            projectFile,
            "Assets/main.acscene");

        try
        {
            string expectedSettingsHash = Convert.ToHexString(
                    SHA256.HashData(File.ReadAllBytes(settings)))
                .ToLowerInvariant();
            PackageCore.PackageDirectorySnapshot snapshot =
                await PackageCore.StageDirectorySnapshotAsync(
                    config,
                    staged,
                    CancellationToken.None);
            Check(snapshot.Existed && snapshot.Files.Count == 2,
                "Config snapshot captures deterministic file set");
            PackageCore.ValidateDirectorySnapshot(config, snapshot);
            PackageCore.ValidateProjectSettingsCheckpoint(
                snapshot,
                expectedSettingsHash);
            Pass("matching Package Settings checkpoint is accepted");
            Check(
                ProjectSettingsCheckpointIsRejected(snapshot, "ABC"),
                "malformed Package Settings checkpoint is rejected with a stable diagnostic");
            IReadOnlyList<PackageIssue> invalidCheckpointIssues =
                PackageCore.Validate(
                    project,
                    new PackageOptions(
                        Path.Combine(root, "Output"),
                        ExpectedProjectSettingsSha256: "ABC"),
                    Assembly.GetExecutingAssembly().Location);
            Check(
                invalidCheckpointIssues.Any(static issue =>
                    issue.Code == "CONFIG_CHECKPOINT_INVALID"),
                "Package preflight rejects malformed Settings checkpoint format");
            var missingSettingsSnapshot =
                new PackageCore.PackageDirectorySnapshot(
                    Existed: true,
                    snapshot.Files
                        .Where(static file =>
                            file.RelativePath != "ProjectSettings.ini")
                        .ToArray());
            Check(
                ProjectSettingsCheckpointIsRejected(
                    missingSettingsSnapshot,
                    expectedSettingsHash),
                "Package Settings checkpoint rejects missing canonical ProjectSettings.ini");
            Check(
                File.ReadAllText(Path.Combine(staged, "ProjectSettings.ini"))
                    .Contains("Assets/main.acscene", StringComparison.Ordinal),
                "Config snapshot stages captured bytes");
            PackageCore.ValidateStagedConfiguration(staged, project);
            Pass("staged Config DefaultScene matches cooked scene");

            File.WriteAllText(
                settings,
                "[Game]\nDefaultScene=Assets/main.acscene\n" +
                "[Plugin]\nMode=ExternalAfterGate\n",
                new UTF8Encoding(false));
            PackageCore.PackageDirectorySnapshot racedSnapshot =
                await PackageCore.StageDirectorySnapshotAsync(
                    config,
                    Path.Combine(root, "RacedStage", "Config"),
                    CancellationToken.None);
            Check(
                ProjectSettingsCheckpointIsRejected(
                    racedSnapshot,
                    expectedSettingsHash),
                "Package Settings checkpoint rejects unknown-key gate-to-Stage edit with a stable diagnostic");
            CheckThrows<InvalidDataException>(
                () => PackageCore.ValidateDirectorySnapshot(config, snapshot),
                "Config mutation is rejected before publish");

            File.WriteAllText(
                Path.Combine(staged, "ProjectSettings.ini"),
                "[Game]\nDefaultScene=Assets/other.acscene\n",
                new UTF8Encoding(false));
            CheckThrows<PackageValidationException>(
                () => PackageCore.ValidateStagedConfiguration(staged, project),
                "staged DefaultScene drift is rejected");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static bool ProjectSettingsCheckpointIsRejected(
        PackageCore.PackageDirectorySnapshot snapshot,
        string expectedSha256)
    {
        try
        {
            PackageCore.ValidateProjectSettingsCheckpoint(
                snapshot,
                expectedSha256);
            return false;
        }
        catch (PackageValidationException error)
        {
            return error.Issues.Count == 1 &&
                   error.Issues[0].Severity == PackageIssueSeverity.Error &&
                   error.Issues[0].Code == "CONFIG_CHANGED_DURING_PACKAGE" &&
                   error.Issues[0].Path == "ProjectSettings.ini";
        }
    }

    private static async Task VerifyProcessOutputBoundAsync()
    {
        ProcessStartInfo start = WorkerStart("--package-process-output-worker");
        int callbacks = 0;
        var elapsed = Stopwatch.StartNew();
        long firstCallbackTicks = long.MaxValue;
        PackageProcessResult result = await PackageProcessRunner.RunAsync(
            start,
            _ =>
            {
                Interlocked.CompareExchange(
                    ref firstCallbackTicks,
                    elapsed.ElapsedTicks,
                    long.MaxValue);
                Interlocked.Increment(ref callbacks);
            },
            CancellationToken.None);
        elapsed.Stop();
        int capturedBytes = Encoding.UTF8.GetByteCount(result.StandardOutput);
        Check(result.ExitCode == 0, "bounded-output child exits successfully");
        Check(
            capturedBytes <=
                PackageProcessRunner.CaptureLimitBytesPerStream + 128,
            "child stdout capture is bounded");
        Check(
            result.StandardOutput.Contains(
                "output truncated",
                StringComparison.Ordinal),
            "bounded stdout records truncation");
        Check(
            firstCallbackTicks != long.MaxValue &&
            Stopwatch.GetElapsedTime(0, firstCallbackTicks) <
                elapsed.Elapsed - TimeSpan.FromMilliseconds(200),
            "unterminated child output is logged before process exit");
        int callbackBudget =
            (int)Math.Ceiling(elapsed.Elapsed.TotalMilliseconds / 90.0) + 20;
        Check(callbacks <= callbackBudget,
            "large child output is batched before UI log callbacks");
    }

    private static async Task VerifyProcessCancellationAsync()
    {
        using var cancellation = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(150));
        var elapsed = Stopwatch.StartNew();
        bool cancelled = false;
        try
        {
            _ = await PackageProcessRunner.RunAsync(
                WorkerStart("--package-process-wait-worker"),
                _ => { },
                cancellation.Token);
        }
        catch (OperationCanceledException)
        {
            cancelled = true;
        }
        Check(cancelled, "package child cancellation propagates");
        Check(
            elapsed.Elapsed < TimeSpan.FromSeconds(8),
            "package child cancellation is bounded");
    }

    private static async Task VerifyOwnerCloseDrainAsync()
    {
        var coordinator = new PackageShutdownCoordinator();
        var release = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        int starts = 0;
        Task<bool> first = coordinator.RunOnceAsync(async () =>
        {
            Interlocked.Increment(ref starts);
            await release.Task;
            return true;
        });
        Task<bool> reentrant = coordinator.RunOnceAsync(() =>
        {
            Interlocked.Increment(ref starts);
            return Task.FromResult(false);
        });
        Check(
            ReferenceEquals(first, reentrant) && starts == 1,
            "reentrant editor-close requests share one build/package shutdown");
        release.TrySetResult(true);
        Check(
            await first && await reentrant,
            "coalesced package shutdown publishes one drain result");

        using var cancellation = new CancellationTokenSource();
        var started = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Task cancellable = Task.Run(async () =>
        {
            started.TrySetResult(true);
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellation.Token);
        });
        await started.Task.WaitAsync(TimeSpan.FromSeconds(3));
        bool drained =
            await PackageShutdownCoordinator.CancelAndDrainAsync(
                cancellable,
                cancellation.Cancel,
                TimeSpan.FromSeconds(3));
        Check(
            drained &&
            cancellation.IsCancellationRequested &&
            cancellable.IsCompleted,
            "owner close cancels and drains active build/package work");

        var blocked = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        int cancellationRequests = 0;
        var elapsed = Stopwatch.StartNew();
        bool bounded =
            await PackageShutdownCoordinator.CancelAndDrainAsync(
                blocked.Task,
                () => Interlocked.Increment(ref cancellationRequests),
                TimeSpan.FromMilliseconds(100));
        elapsed.Stop();
        Check(
            !bounded &&
            cancellationRequests == 1 &&
            elapsed.Elapsed < TimeSpan.FromSeconds(3),
            "non-cooperative build/package shutdown defers close at a bounded deadline");
        blocked.TrySetResult(true);
    }

    private static async Task VerifyPriorGameProcessDrainAsync()
    {
        using Process process =
            Process.Start(WorkerStart("--package-process-wait-worker"))
            ?? throw new InvalidOperationException(
                "Could not start prior-game-process fixture.");
        var elapsed = Stopwatch.StartNew();
        await MainWindow.StopGameProcessForReplacementAsync(
            process,
            CancellationToken.None);
        elapsed.Stop();
        Check(
            process.HasExited &&
            elapsed.Elapsed < TimeSpan.FromSeconds(5),
            "replacing a prior game process drains it asynchronously within a bound");
    }

    private static async Task VerifyCleanupRetryAsync()
    {
        string root = FixtureRoot("cleanup");
        string staging = Path.Combine(root, "PackageStaging");
        string transaction = Path.Combine(staging, "transaction");
        Directory.CreateDirectory(transaction);
        string heldPath = Path.Combine(transaction, "held.bin");
        File.WriteAllText(heldPath, "held", new UTF8Encoding(false));
        FileStream held = new(
            heldPath,
            FileMode.Open,
            FileAccess.ReadWrite,
            FileShare.None);
        Task release = Task.Run(async () =>
        {
            await Task.Delay(90);
            held.Dispose();
        });
        try
        {
            bool deleted = await PackageCore.TryDeleteDirectoryWithRetryAsync(
                transaction,
                staging);
            await release;
            Check(deleted && !Directory.Exists(transaction),
                "staging cleanup retries transient file locks");
        }
        finally
        {
            held.Dispose();
            TryDeleteFixture(root);
        }
    }

    private static void VerifyValidateCancellation()
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var project = new PackageProjectInfo(
            "Cancelled",
            1,
            "test",
            Path.Combine(FixtureRoot("cancelled"), "Cancelled.acsproject"),
            "Assets/main.acscene");
        var options = new PackageOptions(Path.GetTempPath());
        CheckThrows<OperationCanceledException>(
            () => PackageCore.Validate(
                project,
                options,
                "Cancelled.exe",
                cancellationToken: cancellation.Token),
            "PackageCore validation observes cancellation");
    }

    private static ProcessStartInfo WorkerStart(string argument)
    {
        string assembly = Assembly.GetExecutingAssembly().Location;
        var start = new ProcessStartInfo
        {
            FileName = "dotnet",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        start.ArgumentList.Add(assembly);
        start.ArgumentList.Add(argument);
        return start;
    }

    private static string FixtureRoot(string suffix) =>
        Path.Combine(
            Path.GetTempPath(),
            "acs-package-responsiveness-selftest",
            suffix + "-" + Guid.NewGuid().ToString("N"));

    private static void TryDeleteFixture(string path)
    {
        try
        {
            if (Directory.Exists(path))
                Directory.Delete(path, recursive: true);
        }
        catch
        {
        }
    }

    private static void UpdateMaximum(ref int target, int value)
    {
        while (true)
        {
            int current = Volatile.Read(ref target);
            if (value <= current ||
                Interlocked.CompareExchange(ref target, value, current) == current)
            {
                return;
            }
        }
    }

    private static void CheckThrows<TException>(
        Action action,
        string label)
        where TException : Exception
    {
        try
        {
            action();
            Fail(label + " (no exception)");
        }
        catch (TException)
        {
            Pass(label);
        }
        catch (Exception error)
        {
            Fail(label + $" (wrong exception: {error.GetType().Name})");
        }
    }

    private static async Task CheckThrowsAsync<TException>(
        Func<Task> action,
        string label)
        where TException : Exception
    {
        try
        {
            await action();
            Fail(label + " (no exception)");
        }
        catch (TException)
        {
            Pass(label);
        }
        catch (Exception error)
        {
            Fail(label + $" (wrong exception: {error.GetType().Name})");
        }
    }

    private static void Check(bool condition, string label)
    {
        if (condition)
            Pass(label);
        else
            Fail(label);
    }

    private static void Pass(string label) =>
        _output.WriteLine("PASS: " + label);

    private static void Fail(string label)
    {
        _failures++;
        _output.WriteLine("FAIL: " + label);
    }
}
