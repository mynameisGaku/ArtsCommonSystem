// SPDX-License-Identifier: Apache-2.0
// Deterministic, source-preserving worker artifacts for editor asset import.

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace AcsEditor;

internal enum AssetImportDerivedDataStatus
{
    Hit,
    Miss,
    RebuiltCorruptEntry,
}

internal sealed record AssetImportDerivedDataResult(
    string Key,
    string RelativeCachePath,
    AssetImportDerivedDataStatus Status,
    string Processor,
    int ProcessorVersion,
    string Format);

/// <summary>
/// Produces one small, canonical import artifact on the asset worker before
/// source publication. The v1 processor intentionally leaves the staged source
/// payload unchanged and records validated processing intent; future native
/// texture, mesh, and audio transcoders can replace the derived artifact without
/// changing the transaction or cache boundary.
///
/// Cache identity is independent of destination path. It is derived from the
/// source content, importer recipe, processor contract, and normalized kind.
/// Entries are bounded, hash-verified, and published atomically below the
/// project-local Temp/DerivedDataCache tree.
/// </summary>
internal static class AssetImportDerivedDataPipeline
{
    internal const int ArtifactSchemaVersion = 1;
    internal const int ProcessorVersion = 1;
    private const int MaximumArtifactBytes = 256 * 1024;
    private const int MaximumRecipeSettings = 128;
    private const int MaximumRecipeUtf8Bytes = 192 * 1024;
    private const int ProbeBytes = 4096;
    private const uint FileAttributeDirectory = 0x00000010;
    private const uint FileAttributeReparsePoint = 0x00000400;
    private const string CacheRelativeRoot =
        "Temp/DerivedDataCache/AssetImports/v1";
    private static readonly byte[] CacheMagic =
        Encoding.ASCII.GetBytes("ACSIDD1\n");
    private static readonly byte[] ArtifactMagic =
        Encoding.ASCII.GetBytes("ACS-IMPORT-ARTIFACT\n");

    internal static AssetImportDerivedDataResult GetOrCreate(
        string projectRoot,
        string stagedSourcePath,
        string assetKind,
        string sourceExtension,
        string importer,
        int importerVersion,
        IEnumerable<KeyValuePair<string, string>> importSettings,
        string expectedSourceHash,
        long expectedSourceLength,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string root = NormalizeProjectRoot(projectRoot);
        string source = ValidateSourcePath(root, stagedSourcePath);
        string kind = NormalizeText(assetKind, "asset kind", 64)
            .ToLowerInvariant();
        string normalizedImporter =
            NormalizeText(importer, "importer", 64).ToLowerInvariant();
        string normalizedExtension = NormalizeExtension(sourceExtension);
        if (importerVersion <= 0)
            throw new InvalidDataException("Importer version must be positive.");
        ValidateSha256(expectedSourceHash, "source content hash");
        if (expectedSourceLength < 0)
            throw new InvalidDataException("Source length cannot be negative.");

        SortedDictionary<string, string> recipeSettings =
            NormalizeRecipeSettings(importSettings, cancellationToken);
        string recipeHash = ResolveRecipeHash(
            normalizedImporter,
            importerVersion,
            recipeSettings);
        cancellationToken.ThrowIfCancellationRequested();
        string processor = normalizedImporter + ".worker";
        string key = ComputeKey(
            kind,
            processor,
            normalizedExtension,
            normalizedImporter,
            importerVersion,
            recipeHash,
            expectedSourceHash,
            expectedSourceLength);

        byte[] probe = VerifySourceAndCaptureProbe(
            source,
            expectedSourceHash,
            expectedSourceLength,
            cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        string format = DetectFormat(
            kind,
            normalizedExtension,
            probe);
        byte[] artifact = BuildArtifact(
            kind,
            processor,
            normalizedImporter,
            importerVersion,
            recipeHash,
            expectedSourceHash,
            expectedSourceLength,
            format);

        string cacheRoot = EnsureCacheRoot(root);
        string bucket = Path.Combine(cacheRoot, key[..2]);
        EnsureSafeDirectory(root, bucket, createIfMissing: true);
        string entry = Path.Combine(bucket, key + ".acsimpddc");
        EnsureSafeFileOrMissing(root, cacheRoot, entry);

        bool corrupt = false;
        if (File.Exists(entry))
        {
            try
            {
                byte[] cached = ReadEntry(root, cacheRoot, entry, key);
                if (!cached.AsSpan().SequenceEqual(artifact))
                {
                    throw new InvalidDataException(
                        "Import DDC payload does not match its canonical request.");
                }
                cancellationToken.ThrowIfCancellationRequested();
                return Result(
                    root,
                    entry,
                    key,
                    AssetImportDerivedDataStatus.Hit,
                    processor,
                    format);
            }
            catch (InvalidDataException)
            {
                corrupt = true;
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        WriteEntryAtomic(root, cacheRoot, entry, key, artifact);
        byte[] published = ReadEntry(root, cacheRoot, entry, key);
        if (!published.AsSpan().SequenceEqual(artifact))
        {
            throw new InvalidDataException(
                "Published import DDC artifact failed canonical read-back.");
        }
        return Result(
            root,
            entry,
            key,
            corrupt
                ? AssetImportDerivedDataStatus.RebuiltCorruptEntry
                : AssetImportDerivedDataStatus.Miss,
            processor,
            format);
    }

    internal static string GetEntryPath(string projectRoot, string key)
    {
        ValidateSha256(key, "import DDC key");
        string root = NormalizeProjectRoot(projectRoot);
        string cacheRoot = Path.Combine(
            root,
            CacheRelativeRoot.Replace(
                '/',
                Path.DirectorySeparatorChar));
        return Path.Combine(cacheRoot, key[..2], key + ".acsimpddc");
    }

    internal static IReadOnlyDictionary<string, string> MetadataSettings(
        AssetImportDerivedDataResult result) =>
        new ReadOnlyDictionary<string, string>(
            new SortedDictionary<string, string>(StringComparer.Ordinal)
            {
                ["processedArtifactKey"] = result.Key,
                ["processedArtifactSchema"] =
                    ArtifactSchemaVersion.ToString(CultureInfo.InvariantCulture),
                ["processedFormat"] = result.Format,
                ["processedProcessor"] = result.Processor,
                ["processedProcessorVersion"] =
                    result.ProcessorVersion.ToString(
                        CultureInfo.InvariantCulture),
            });

    private static AssetImportDerivedDataResult Result(
        string projectRoot,
        string entry,
        string key,
        AssetImportDerivedDataStatus status,
        string processor,
        string format) =>
        new(
            key,
            Path.GetRelativePath(projectRoot, entry).Replace('\\', '/'),
            status,
            processor,
            ProcessorVersion,
            format);

    private static SortedDictionary<string, string> NormalizeRecipeSettings(
        IEnumerable<KeyValuePair<string, string>> settings,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var normalized = new SortedDictionary<string, string>(
            StringComparer.Ordinal);
        int inputCount = 0;
        int inputUtf8Bytes = 0;
        foreach (KeyValuePair<string, string> pair in settings)
        {
            cancellationToken.ThrowIfCancellationRequested();
            inputCount = checked(inputCount + 1);
            if (inputCount > MaximumRecipeSettings)
            {
                throw new InvalidDataException(
                    $"Importer recipe exceeds {MaximumRecipeSettings} settings.");
            }
            string key = NormalizeText(
                pair.Key,
                "import setting key",
                256);
            string value = NormalizeText(
                pair.Value,
                "import setting value",
                4096,
                allowEmpty: true);
            inputUtf8Bytes = checked(
                inputUtf8Bytes +
                Encoding.UTF8.GetByteCount(key) +
                Encoding.UTF8.GetByteCount(value));
            if (inputUtf8Bytes > MaximumRecipeUtf8Bytes)
            {
                throw new InvalidDataException(
                    "Importer recipe exceeds its bounded UTF-8 payload.");
            }
            if (AssetDatabase.IsDynamicImportSetting(key))
                continue;
            if (!normalized.TryAdd(key, value))
            {
                throw new InvalidDataException(
                    $"Duplicate importer recipe setting '{key}'.");
            }
        }
        return normalized;
    }

    private static string ResolveRecipeHash(
        string importer,
        int importerVersion,
        SortedDictionary<string, string> settings)
    {
        string computed = AssetImporterRecipeContract.ComputeRecipeHash(
            importer,
            importerVersion,
            settings);
        if (!settings.TryGetValue("recipeHash", out string? declared))
        {
            settings.Add("recipeHash", computed);
            return computed;
        }
        ValidateSha256(declared, "importer recipe hash");
        if (!string.Equals(declared, computed, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Importer recipe hash does not match its normalized settings.");
        }
        return declared;
    }

    private static string ComputeKey(
        string kind,
        string processor,
        string sourceExtension,
        string importer,
        int importerVersion,
        string recipeHash,
        string sourceHash,
        long sourceLength)
    {
        var canonical = new StringBuilder();
        Append(
            "artifactSchema",
            ArtifactSchemaVersion.ToString(CultureInfo.InvariantCulture));
        Append(
            "processorVersion",
            ProcessorVersion.ToString(CultureInfo.InvariantCulture));
        Append("processor", processor);
        Append("kind", kind);
        Append("sourceExtension", sourceExtension);
        Append("importer", importer);
        Append(
            "importerVersion",
            importerVersion.ToString(CultureInfo.InvariantCulture));
        Append("recipeHash", recipeHash);
        Append("sourceContentHash", sourceHash);
        Append(
            "sourceSizeBytes",
            sourceLength.ToString(CultureInfo.InvariantCulture));
        return Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(canonical.ToString())))
            .ToLowerInvariant();

        void Append(string name, string value)
        {
            canonical.Append(name.Length)
                .Append(':')
                .Append(name)
                .Append('=')
                .Append(value.Length)
                .Append(':')
                .Append(value)
                .Append('\n');
        }
    }

    private static byte[] VerifySourceAndCaptureProbe(
        string source,
        string expectedHash,
        long expectedLength,
        CancellationToken cancellationToken)
    {
        using var stream = new FileStream(
            source,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        EnsureOpenedOrdinaryFile(
            stream,
            source,
            "Staged import source");
        if (stream.Length != expectedLength)
        {
            throw new IOException(
                "Staged import source length changed before processing.");
        }

        using IncrementalHash hasher =
            IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buffer = new byte[128 * 1024];
        byte[] probe = new byte[
            checked((int)Math.Min(expectedLength, ProbeBytes))];
        int probeWritten = 0;
        long total = 0;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read == 0)
                break;
            hasher.AppendData(buffer, 0, read);
            if (probeWritten < probe.Length)
            {
                int copy = Math.Min(read, probe.Length - probeWritten);
                Buffer.BlockCopy(
                    buffer,
                    0,
                    probe,
                    probeWritten,
                    copy);
                probeWritten += copy;
            }
            total = checked(total + read);
        }
        if (total != expectedLength)
        {
            throw new IOException(
                "Staged import source changed while processing.");
        }
        string actualHash = Convert.ToHexString(hasher.GetHashAndReset())
            .ToLowerInvariant();
        if (!string.Equals(
                actualHash,
                expectedHash,
                StringComparison.Ordinal))
        {
            throw new IOException(
                "Staged import source hash changed before processing.");
        }
        return probe;
    }

    private static byte[] BuildArtifact(
        string kind,
        string processor,
        string importer,
        int importerVersion,
        string recipeHash,
        string sourceHash,
        long sourceLength,
        string format)
    {
        var canonical = new StringBuilder();
        canonical.Append(Encoding.ASCII.GetString(ArtifactMagic));
        Append(
            "schema",
            ArtifactSchemaVersion.ToString(CultureInfo.InvariantCulture));
        Append("payloadMode", "source-preserving-v1");
        Append("processor", processor);
        Append(
            "processorVersion",
            ProcessorVersion.ToString(CultureInfo.InvariantCulture));
        Append("kind", kind);
        Append("format", format);
        Append("importer", importer);
        Append(
            "importerVersion",
            importerVersion.ToString(CultureInfo.InvariantCulture));
        Append("recipeHash", recipeHash);
        Append("sourceContentHash", sourceHash);
        Append(
            "sourceSizeBytes",
            sourceLength.ToString(CultureInfo.InvariantCulture));

        byte[] artifact = Encoding.UTF8.GetBytes(canonical.ToString());
        if (artifact.Length > MaximumArtifactBytes)
        {
            throw new InvalidDataException(
                "Canonical import artifact exceeds its bounded payload.");
        }
        return artifact;

        void Append(string name, string value)
        {
            canonical.Append(name.Length)
                .Append(':')
                .Append(name)
                .Append('=')
                .Append(value.Length)
                .Append(':')
                .Append(value)
                .Append('\n');
        }
    }

    private static string DetectFormat(
        string kind,
        string extension,
        ReadOnlySpan<byte> probe)
    {
        string ext = extension.TrimStart('.');

        if (kind == "image")
        {
            ReadOnlySpan<byte> png =
                [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
            if (probe.StartsWith(png))
            {
                if (probe.Length >= 24)
                {
                    uint width = BinaryPrimitives.ReadUInt32BigEndian(
                        probe.Slice(16, 4));
                    uint height = BinaryPrimitives.ReadUInt32BigEndian(
                        probe.Slice(20, 4));
                    if (width is > 0 and <= 1_000_000 &&
                        height is > 0 and <= 1_000_000)
                    {
                        return string.Create(
                            CultureInfo.InvariantCulture,
                            $"png:{width}x{height}");
                    }
                }
                return "png";
            }
            if (probe.Length >= 3 &&
                probe[0] == 0xff &&
                probe[1] == 0xd8 &&
                probe[2] == 0xff)
            {
                return "jpeg";
            }
            if (probe.StartsWith("GIF87a"u8) ||
                probe.StartsWith("GIF89a"u8))
            {
                return "gif";
            }
            if (probe.StartsWith("BM"u8))
                return "bmp";
            if (probe.Length >= 12 &&
                probe.StartsWith("RIFF"u8) &&
                probe.Slice(8, 4).SequenceEqual("WEBP"u8))
            {
                return "webp";
            }
        }
        else if (kind == "audio")
        {
            if (probe.Length >= 28 &&
                probe.StartsWith("RIFF"u8) &&
                probe.Slice(8, 4).SequenceEqual("WAVE"u8))
            {
                ushort channels =
                    BinaryPrimitives.ReadUInt16LittleEndian(
                        probe.Slice(22, 2));
                uint sampleRate =
                    BinaryPrimitives.ReadUInt32LittleEndian(
                        probe.Slice(24, 4));
                if (channels is > 0 and <= 64 &&
                    sampleRate is >= 1000 and <= 768000)
                {
                    return string.Create(
                        CultureInfo.InvariantCulture,
                        $"wav:{channels}ch:{sampleRate}hz");
                }
                return "wav";
            }
            if (probe.StartsWith("OggS"u8))
                return "ogg";
            if (probe.StartsWith("fLaC"u8))
                return "flac";
            if (probe.StartsWith("ID3"u8))
                return "mp3";
        }
        else if (kind == "mesh")
        {
            return ext switch
            {
                "obj" or "fbx" or "gltf" or "glb" => ext,
                _ => "mesh-" + ext,
            };
        }
        return ext.Length == 0 ? "unknown" : ext;
    }

    private static string NormalizeExtension(string value)
    {
        string extension = (value ?? "").Trim().TrimStart('.').ToLowerInvariant();
        if (extension.Length == 0 ||
            extension.Length > 32 ||
            extension.Any(static character =>
                !char.IsAsciiLetterOrDigit(character)))
        {
            return "unknown";
        }
        return extension;
    }

    private static byte[] ReadEntry(
        string projectRoot,
        string cacheRoot,
        string path,
        string expectedKey)
    {
        EnsureSafeFileOrMissing(projectRoot, cacheRoot, path);
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read | FileShare.Delete,
            64 * 1024,
            FileOptions.SequentialScan);
        EnsureOpenedOrdinaryFile(
            stream,
            path,
            "Import DDC entry");
        int headerLength =
            CacheMagic.Length + sizeof(int) + 32 + 32 + sizeof(int);
        if (stream.Length < headerLength ||
            stream.Length > headerLength + MaximumArtifactBytes)
        {
            throw new InvalidDataException(
                "Import DDC entry size is invalid.");
        }
        byte[] header = new byte[headerLength];
        stream.ReadExactly(header);
        ReadOnlySpan<byte> span = header;
        if (!span[..CacheMagic.Length].SequenceEqual(CacheMagic))
            throw new InvalidDataException("Import DDC magic is invalid.");
        int cursor = CacheMagic.Length;
        int schema = BinaryPrimitives.ReadInt32LittleEndian(
            span.Slice(cursor, sizeof(int)));
        cursor += sizeof(int);
        if (schema != ArtifactSchemaVersion)
            throw new InvalidDataException("Import DDC schema is unsupported.");
        if (!span.Slice(cursor, 32).SequenceEqual(
                Convert.FromHexString(expectedKey)))
        {
            throw new InvalidDataException(
                "Import DDC key does not match its entry path.");
        }
        cursor += 32;
        byte[] payloadHash = span.Slice(cursor, 32).ToArray();
        cursor += 32;
        int payloadLength = BinaryPrimitives.ReadInt32LittleEndian(
            span.Slice(cursor, sizeof(int)));
        if (payloadLength < 0 ||
            payloadLength > MaximumArtifactBytes ||
            stream.Length != headerLength + payloadLength)
        {
            throw new InvalidDataException(
                "Import DDC payload length is invalid.");
        }
        byte[] payload = new byte[payloadLength];
        stream.ReadExactly(payload);
        if (!SHA256.HashData(payload).AsSpan().SequenceEqual(payloadHash))
            throw new InvalidDataException("Import DDC payload hash is invalid.");
        return payload;
    }

    private static void WriteEntryAtomic(
        string projectRoot,
        string cacheRoot,
        string path,
        string key,
        byte[] payload)
    {
        string parent = Path.GetDirectoryName(path)
            ?? throw new InvalidDataException(
                "Import DDC entry has no parent directory.");
        EnsureSafeDirectory(projectRoot, parent, createIfMissing: false);
        EnsureSafeFileOrMissing(projectRoot, cacheRoot, path);
        string temporary =
            path + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       64 * 1024,
                       FileOptions.WriteThrough))
            {
                EnsureOpenedOrdinaryFile(
                    stream,
                    temporary,
                    "Import DDC temporary");
                stream.Write(CacheMagic);
                Span<byte> schema = stackalloc byte[sizeof(int)];
                BinaryPrimitives.WriteInt32LittleEndian(
                    schema,
                    ArtifactSchemaVersion);
                stream.Write(schema);
                stream.Write(Convert.FromHexString(key));
                stream.Write(SHA256.HashData(payload));
                Span<byte> length = stackalloc byte[sizeof(int)];
                BinaryPrimitives.WriteInt32LittleEndian(
                    length,
                    payload.Length);
                stream.Write(length);
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }
            EnsureSafeDirectory(projectRoot, parent, createIfMissing: false);
            EnsureSafeFileOrMissing(
                projectRoot,
                cacheRoot,
                temporary);
            EnsureSafeFileOrMissing(projectRoot, cacheRoot, path);
            try
            {
                File.Move(temporary, path, overwrite: true);
            }
            catch (Exception error) when (
                (error is IOException or UnauthorizedAccessException) &&
                File.Exists(path))
            {
                // A second importer may have won publication for the same
                // content-addressed key. Accept only the exact canonical
                // artifact; otherwise preserve the failure for diagnosis.
                byte[] concurrent =
                    ReadEntry(projectRoot, cacheRoot, path, key);
                if (!concurrent.AsSpan().SequenceEqual(payload))
                    throw;
            }
        }
        finally
        {
            try
            {
                if (File.Exists(temporary) &&
                    (File.GetAttributes(temporary) &
                     FileAttributes.ReparsePoint) == 0)
                {
                    File.Delete(temporary);
                }
            }
            catch
            {
                // The cache remains reconstructible; a private temp may be
                // reclaimed by later project maintenance.
            }
        }
    }

    private static string EnsureCacheRoot(string projectRoot)
    {
        string current = projectRoot;
        foreach (string segment in CacheRelativeRoot.Split('/'))
        {
            current = Path.Combine(current, segment);
            EnsureSafeDirectory(
                projectRoot,
                current,
                createIfMissing: true);
        }
        return current;
    }

    private static string NormalizeProjectRoot(string path)
    {
        string root = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(
                string.IsNullOrWhiteSpace(path)
                    ? throw new ArgumentException(
                        "Project root is required.",
                        nameof(path))
                    : path));
        EnsureSafeDirectory(root, root, createIfMissing: false);
        return root;
    }

    private static string ValidateSourcePath(
        string projectRoot,
        string path)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnder(full, projectRoot))
        {
            throw new InvalidDataException(
                "Staged import source escapes the project root.");
        }
        string? parent = Path.GetDirectoryName(full);
        if (parent == null)
            throw new InvalidDataException("Staged source has no parent.");
        EnsureSafeDirectory(projectRoot, parent, createIfMissing: false);
        if (!File.Exists(full) || Directory.Exists(full))
            throw new FileNotFoundException("Staged import source is missing.", full);
        FileAttributes attributes = File.GetAttributes(full);
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException(
                "Staged import source must not be a reparse point.");
        return full;
    }

    private static void EnsureSafeDirectory(
        string projectRoot,
        string path,
        bool createIfMissing)
    {
        string root = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(projectRoot));
        string full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        if (!IsUnderOrEqual(full, root))
            throw new InvalidDataException("Import DDC path escapes project root.");

        string relative = Path.GetRelativePath(root, full);
        string current = root;
        CheckDirectory(current);
        foreach (string segment in relative.Split(
                     [
                         Path.DirectorySeparatorChar,
                         Path.AltDirectorySeparatorChar,
                     ],
                     StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if (!Directory.Exists(current))
            {
                if (File.Exists(current))
                {
                    throw new InvalidDataException(
                        "Import DDC directory segment is a file.");
                }
                if (!createIfMissing)
                    throw new DirectoryNotFoundException(current);
                Directory.CreateDirectory(current);
            }
            CheckDirectory(current);
        }
    }

    private static void EnsureSafeFileOrMissing(
        string projectRoot,
        string cacheRoot,
        string path)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnder(full, cacheRoot))
            throw new InvalidDataException("Import DDC entry escapes cache root.");
        string parent = Path.GetDirectoryName(full)
            ?? throw new InvalidDataException(
                "Import DDC entry has no parent directory.");
        EnsureSafeDirectory(projectRoot, parent, createIfMissing: false);
        if (!File.Exists(full) && !Directory.Exists(full))
            return;
        FileAttributes attributes = File.GetAttributes(full);
        if ((attributes & FileAttributes.Directory) != 0)
            throw new InvalidDataException("Import DDC entry is a directory.");
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException(
                "Import DDC entry is a reparse point.");
    }

    private static void CheckDirectory(string path)
    {
        if (!Directory.Exists(path))
            throw new DirectoryNotFoundException(path);
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                "Import DDC path crosses an unsafe directory.");
        }
    }

    /// <summary>
    /// Revalidates the object that was actually opened, not only the path that
    /// was inspected before open. This closes the check/open substitution
    /// window for staged sources, cache reads, and same-directory temporaries.
    /// </summary>
    private static void EnsureOpenedOrdinaryFile(
        FileStream stream,
        string expectedPath,
        string label)
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException(
                "Processed import file identity requires Windows.");
        }
        SafeFileHandle handle = stream.SafeFileHandle;
        if (handle.IsInvalid ||
            !GetFileInformationByHandle(
                handle,
                out ByHandleFileInformation information) ||
            (information.FileAttributes &
             (FileAttributeDirectory | FileAttributeReparsePoint)) != 0 ||
            !TryGetFinalPath(handle, out string finalPath) ||
            !string.Equals(
                finalPath,
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(expectedPath)),
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"{label} changed identity during validation.");
        }
    }

    private static bool TryGetFinalPath(
        SafeFileHandle handle,
        out string path)
    {
        path = "";
        var buffer = new StringBuilder(512);
        uint length = GetFinalPathNameByHandle(
            handle,
            buffer,
            (uint)buffer.Capacity,
            0);
        if (length == 0)
            return false;
        if (length >= buffer.Capacity)
        {
            buffer = new StringBuilder(checked((int)length + 1));
            length = GetFinalPathNameByHandle(
                handle,
                buffer,
                (uint)buffer.Capacity,
                0);
            if (length == 0 || length >= buffer.Capacity)
                return false;
        }

        string finalPath = buffer.ToString();
        if (finalPath.StartsWith(
                @"\\?\UNC\",
                StringComparison.OrdinalIgnoreCase))
        {
            finalPath = @"\\" + finalPath[8..];
        }
        else if (finalPath.StartsWith(
                     @"\\?\",
                     StringComparison.OrdinalIgnoreCase))
        {
            finalPath = finalPath[4..];
        }
        path = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(finalPath));
        return true;
    }

    private static string NormalizeText(
        string? value,
        string field,
        int maximumLength,
        bool allowEmpty = false)
    {
        string normalized = value?.Trim() ?? "";
        if ((!allowEmpty && normalized.Length == 0) ||
            normalized.Length > maximumLength ||
            normalized.IndexOf('\0') >= 0 ||
            normalized.Any(static character =>
                char.IsControl(character) &&
                character is not '\t'))
        {
            throw new InvalidDataException(
                $"Import derived-data {field} is invalid.");
        }
        return normalized;
    }

    private static void ValidateSha256(string value, string field)
    {
        if (value.Length != 64 ||
            value.Any(static character =>
                character is not (>= '0' and <= '9') and
                    not (>= 'a' and <= 'f')))
        {
            throw new InvalidDataException(
                $"Import derived-data {field} is not canonical SHA-256.");
        }
    }

    private static bool IsUnder(string path, string root)
    {
        string normalizedRoot = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(root));
        string normalizedPath = Path.GetFullPath(path);
        return normalizedPath.StartsWith(
            normalizedRoot + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsUnderOrEqual(string path, string root) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)),
            StringComparison.OrdinalIgnoreCase) ||
        IsUnder(path, root);

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        internal uint FileAttributes;
        internal System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        internal System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        internal System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        internal uint VolumeSerialNumber;
        internal uint FileSizeHigh;
        internal uint FileSizeLow;
        internal uint NumberOfLinks;
        internal uint FileIndexHigh;
        internal uint FileIndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle file,
        out ByHandleFileInformation information);

    [DllImport(
        "kernel32.dll",
        EntryPoint = "GetFinalPathNameByHandleW",
        SetLastError = true,
        CharSet = CharSet.Unicode)]
    private static extern uint GetFinalPathNameByHandle(
        SafeFileHandle file,
        StringBuilder filePath,
        uint filePathLength,
        uint flags);
}
