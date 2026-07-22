// SPDX-License-Identifier: Apache-2.0

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace AcsEditor;

public enum DerivedDataCacheStatus
{
    Hit,
    Miss,
    RebuiltCorruptEntry,
}

public sealed record DerivedDataCacheResult(
    string Key,
    byte[] Payload,
    DerivedDataCacheStatus Status);

/// <summary>
/// Content-addressed local Derived Data Cache. Entries are independently hash-verified and written
/// atomically; cache corruption degrades to a deterministic rebuild, while unsafe paths fail.
/// </summary>
public sealed class DerivedDataCache
{
    public const int CurrentSchemaVersion = 1;
    private const long MaxPayloadBytes = 1024L * 1024 * 1024;
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("ACSDDC1\n");

    private readonly string _projectRoot;
    private readonly string _cacheRoot;

    public DerivedDataCache(string projectRoot, string cacheRoot)
    {
        _projectRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        _cacheRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(cacheRoot));
        if (!IsUnder(_cacheRoot, _projectRoot))
            throw new InvalidDataException("Derived Data Cache must be inside the project root.");
        EnsureSafeDirectory(_projectRoot, createIfMissing: false);
        EnsureSafeDirectory(_cacheRoot, createIfMissing: true);
    }

    public string CacheRoot => _cacheRoot;

    public DerivedDataCacheResult GetOrCreate(
        AssetRecord asset,
        string cookerVersion,
        IEnumerable<KeyValuePair<string, string>>? cookerSettings,
        Func<byte[]> producer)
    {
        ArgumentNullException.ThrowIfNull(asset);
        ArgumentNullException.ThrowIfNull(producer);
        string key = ComputeKey(asset, cookerVersion, cookerSettings);
        string directory = EntryDirectory(key);
        EnsureSafeDirectory(directory, createIfMissing: true);
        string entry = EntryPath(key);
        EnsureSafeFileOrMissing(entry);

        bool corrupt = false;
        if (File.Exists(entry))
        {
            try
            {
                return new(
                    key,
                    ReadEntry(entry, key),
                    DerivedDataCacheStatus.Hit);
            }
            catch (InvalidDataException)
            {
                corrupt = true;
            }
        }

        byte[] payload = producer()
            ?? throw new InvalidDataException("Derived data producer returned null.");
        if (payload.LongLength > MaxPayloadBytes)
            throw new InvalidDataException(
                $"Derived data payload exceeds {MaxPayloadBytes} bytes.");
        WriteEntryAtomic(entry, key, payload);
        return new(
            key,
            payload,
            corrupt
                ? DerivedDataCacheStatus.RebuiltCorruptEntry
                : DerivedDataCacheStatus.Miss);
    }

    public static string ComputeKey(
        AssetRecord asset,
        string cookerVersion,
        IEnumerable<KeyValuePair<string, string>>? cookerSettings)
    {
        if (string.IsNullOrWhiteSpace(cookerVersion))
            throw new ArgumentException("Cooker version is required.", nameof(cookerVersion));
        var canonical = new StringBuilder();
        Append("schema", CurrentSchemaVersion.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Append("cooker", cookerVersion.Trim());
        Append("path", asset.RelativePath);
        Append("kind", asset.Kind);
        Append("content", asset.ContentHash);
        Append("importer", asset.Metadata.Importer);
        Append(
            "importerVersion",
            asset.Metadata.ImporterVersion.ToString(System.Globalization.CultureInfo.InvariantCulture));
        foreach (KeyValuePair<string, string> setting in asset.Metadata.ImportSettings
                     .OrderBy(static item => item.Key, StringComparer.Ordinal))
            Append("import." + setting.Key, setting.Value);
        foreach (string dependency in asset.Metadata.Dependencies
                     .OrderBy(static item => item, StringComparer.Ordinal))
            Append("dependency", dependency);
        foreach (KeyValuePair<string, string> setting in
                 (cookerSettings ?? Array.Empty<KeyValuePair<string, string>>())
                 .OrderBy(static item => item.Key, StringComparer.Ordinal)
                 .ThenBy(static item => item.Value, StringComparer.Ordinal))
            Append("cook." + setting.Key, setting.Value);

        return Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(canonical.ToString())))
            .ToLowerInvariant();

        void Append(string name, string value)
        {
            canonical.Append(name.Length).Append(':').Append(name)
                .Append('=').Append(value.Length).Append(':').Append(value).Append('\n');
        }
    }

    private byte[] ReadEntry(string path, string expectedKey)
    {
        EnsureSafeFileOrMissing(path);
        using FileStream stream = new(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        long minimum = Magic.Length + 32 + 32 + sizeof(long);
        if (stream.Length < minimum || stream.Length > minimum + MaxPayloadBytes)
            throw new InvalidDataException("Derived data entry size is invalid.");

        Span<byte> header = stackalloc byte[Magic.Length + 32 + 32 + sizeof(long)];
        stream.ReadExactly(header);
        if (!header[..Magic.Length].SequenceEqual(Magic))
            throw new InvalidDataException("Derived data entry magic is invalid.");
        int cursor = Magic.Length;
        byte[] expectedKeyBytes = Convert.FromHexString(expectedKey);
        if (!header.Slice(cursor, 32).SequenceEqual(expectedKeyBytes))
            throw new InvalidDataException("Derived data entry key does not match its path.");
        cursor += 32;
        byte[] expectedPayloadHash = header.Slice(cursor, 32).ToArray();
        cursor += 32;
        long payloadLength = BinaryPrimitives.ReadInt64LittleEndian(header.Slice(cursor, sizeof(long)));
        if (payloadLength < 0 || payloadLength > MaxPayloadBytes ||
            stream.Length != header.Length + payloadLength)
            throw new InvalidDataException("Derived data payload length is invalid.");

        byte[] payload = new byte[payloadLength];
        stream.ReadExactly(payload);
        byte[] actualHash = SHA256.HashData(payload);
        if (!actualHash.SequenceEqual(expectedPayloadHash))
            throw new InvalidDataException("Derived data payload checksum is invalid.");
        return payload;
    }

    private void WriteEntryAtomic(string path, string key, byte[] payload)
    {
        string parent = Path.GetDirectoryName(path)
            ?? throw new InvalidDataException("Derived data entry has no parent directory.");
        EnsureSafeDirectory(parent, createIfMissing: false);
        EnsureSafeFileOrMissing(path);
        string temporary = path + ".tmp-" + Guid.NewGuid().ToString("N");
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
                stream.Write(Magic);
                stream.Write(Convert.FromHexString(key));
                stream.Write(SHA256.HashData(payload));
                Span<byte> length = stackalloc byte[sizeof(long)];
                BinaryPrimitives.WriteInt64LittleEndian(length, payload.LongLength);
                stream.Write(length);
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }
            EnsureSafeDirectory(parent, createIfMissing: false);
            EnsureSafeFileOrMissing(path);
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temporary) &&
                    (File.GetAttributes(temporary) & FileAttributes.ReparsePoint) == 0)
                    File.Delete(temporary);
            }
            catch { }
        }
    }

    private string EntryDirectory(string key)
    {
        ValidateKey(key);
        return Path.Combine(_cacheRoot, key[..2]);
    }

    private string EntryPath(string key) =>
        Path.Combine(EntryDirectory(key), key + ".ddc");

    private static void ValidateKey(string key)
    {
        if (key.Length != 64 || key.Any(static value =>
                !((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))))
            throw new InvalidDataException("Derived data cache key is not canonical SHA-256.");
    }

    private void EnsureSafeDirectory(string path, bool createIfMissing)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnderOrEqual(full, _projectRoot))
            throw new InvalidDataException("Derived data path escapes the project root.");

        string relative = Path.GetRelativePath(_projectRoot, full);
        string current = _projectRoot;
        CheckDirectory(current);
        foreach (string segment in relative.Split(
                     [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                     StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if (!Directory.Exists(current))
            {
                if (File.Exists(current))
                    throw new InvalidDataException(
                        $"Derived data directory segment is a file: {current}");
                if (!createIfMissing)
                    throw new DirectoryNotFoundException(current);
                Directory.CreateDirectory(current);
            }
            CheckDirectory(current);
        }
    }

    private void EnsureSafeFileOrMissing(string path)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnder(full, _cacheRoot))
            throw new InvalidDataException("Derived data entry escapes the cache root.");
        string parent = Path.GetDirectoryName(full)
            ?? throw new InvalidDataException("Derived data entry has no parent.");
        EnsureSafeDirectory(parent, createIfMissing: false);
        if (!File.Exists(full) && !Directory.Exists(full))
            return;
        FileAttributes attributes = File.GetAttributes(full);
        if ((attributes & FileAttributes.Directory) != 0)
            throw new InvalidDataException($"Derived data entry is a directory: {full}");
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Derived data entry is a reparse point: {full}");
    }

    private static void CheckDirectory(string path)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0)
            throw new InvalidDataException($"Expected a directory: {path}");
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Derived data path crosses a reparse point: {path}");
    }

    private static bool IsUnder(string path, string root)
    {
        string fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string fullPath = Path.GetFullPath(path);
        return fullPath.StartsWith(
            fullRoot + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsUnderOrEqual(string path, string root) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)),
            StringComparison.OrdinalIgnoreCase) ||
        IsUnder(path, root);
}
