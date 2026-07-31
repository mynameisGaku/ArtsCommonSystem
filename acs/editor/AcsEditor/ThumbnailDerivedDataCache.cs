// SPDX-License-Identifier: Apache-2.0

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal enum ThumbnailDerivedDataCacheStatus
{
    Hit,
    Miss,
    Corrupt,
}

internal readonly record struct ThumbnailDerivedDataCacheLookup(
    string Key,
    ThumbnailDerivedDataCacheStatus Status,
    ImageSource? Image);

internal readonly record struct ThumbnailDerivedDataCacheCleanup(
    int RetainedEntries,
    long RetainedBytes,
    int RemovedEntries,
    int RemovedTemporaryFiles);

internal readonly record struct ThumbnailDerivedDataCacheFileSystemSnapshot(
    long FullReconciles,
    long PrefixDirectoryEnumerations,
    long EntryInspections,
    long AtomicPublications);

/// <summary>
/// Optional deterministic instrumentation for the cache's filesystem-heavy paths.
/// Interlocked counters are used instead of callbacks so observation cannot re-enter cache locks.
/// </summary>
internal sealed class ThumbnailDerivedDataCacheFileSystemCounters
{
    private long _fullReconciles;
    private long _prefixDirectoryEnumerations;
    private long _entryInspections;
    private long _atomicPublications;

    internal ThumbnailDerivedDataCacheFileSystemSnapshot Snapshot() =>
        new(
            Interlocked.Read(ref _fullReconciles),
            Interlocked.Read(ref _prefixDirectoryEnumerations),
            Interlocked.Read(ref _entryInspections),
            Interlocked.Read(ref _atomicPublications));

    internal void RecordFullReconcile() =>
        Interlocked.Increment(ref _fullReconciles);

    internal void RecordPrefixDirectoryEnumeration() =>
        Interlocked.Increment(ref _prefixDirectoryEnumerations);

    internal void RecordEntryInspection() =>
        Interlocked.Increment(ref _entryInspections);

    internal void RecordAtomicPublication() =>
        Interlocked.Increment(ref _atomicPublications);
}

/// <summary>
/// Project-local, content-addressed cache for Asset Browser thumbnails.
///
/// The cache is never authoritative: every key is derived from the indexed source content hash,
/// generator contract, asset kind, and requested edge. Entries contain canonical, lossless raw
/// Pbgra32 pixels and carry their own key and payload checksum. Persistent bytes are therefore
/// never passed to an in-process compressed-image codec. Unsafe paths fail closed; malformed
/// entries are deleted and rebuilt by the existing thumbnail generator.
/// </summary>
internal sealed class ThumbnailDerivedDataCache
{
    internal const int CurrentSchemaVersion = 2;
    internal const string CurrentGeneratorVersion =
        "asset-browser-thumbnail-raw-pbgra32-v2";
    internal const long DefaultMaximumDiskBytes =
        256L * 1024L * 1024L;
    internal const int DefaultMaximumEntries = 4096;
    private const int RawPayloadHeaderBytes = 32;
    private const uint RawPayloadVersion = 1;
    private const uint RawPixelFormatPbgra32 = 1;
    internal const long MaximumPayloadBytes =
        RawPayloadHeaderBytes +
        (long)AssetImageDecoder.MaximumDecodeEdge *
        AssetImageDecoder.MaximumDecodeEdge * 4;
    internal static readonly TimeSpan StaleTemporaryAge =
        TimeSpan.FromHours(1);
    internal static readonly TimeSpan AccountingReconcileInterval =
        TimeSpan.FromMinutes(1);

    private const string EntryExtension = ".thumbddc";
    private const int HighWaterNumerator = 7;
    private const int HighWaterDenominator = 8;
    private const int LowWaterNumerator = 3;
    private const int LowWaterDenominator = 4;
    private const int MaximumPrefixDirectoryInspections = 512;
    private const int MinimumReconcileEntryInspections = 1024;
    private const int MaximumReconcileEntryInspections = 65536;
    private const int ReconcileEntryExpansionFactor = 4;
    private static readonly byte[] Magic =
        Encoding.ASCII.GetBytes("ACSTHMB2\n");
    private static readonly byte[] RawPayloadMagic =
        Encoding.ASCII.GetBytes("ACSRAW2\n");
    private static readonly int HeaderBytes =
        Magic.Length + 32 + 32 + sizeof(long);

    private readonly object _gate = new();
    private readonly string _projectRoot;
    private readonly string _cacheRoot;
    private readonly DerivedDataCachePathPool _pathPool;
    private readonly long _maximumDiskBytes;
    private readonly int _maximumEntries;
    private readonly int _maximumReconcileEntryInspections;
    private readonly Func<DateTime> _utcNow;
    private readonly ThumbnailDerivedDataCacheFileSystemCounters?
        _fileSystemCounters;
    private readonly Dictionary<string, ManagedEntry> _entriesByPath =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly SortedSet<ManagedEntry> _entriesByRecency =
        new(ManagedEntryComparer.Instance);
    private long _retainedBytes;
    private DateTime _nextAccountingReconcileUtc;
    private bool _highWaterReconciled;

    internal ThumbnailDerivedDataCache(
        string projectRoot,
        string cacheRoot,
        long maximumDiskBytes = DefaultMaximumDiskBytes,
        int maximumEntries = DefaultMaximumEntries,
        Func<DateTime>? utcNow = null,
        ThumbnailDerivedDataCacheFileSystemCounters? fileSystemCounters = null)
    {
        if (maximumDiskBytes <= HeaderBytes)
            throw new ArgumentOutOfRangeException(nameof(maximumDiskBytes));
        if (maximumEntries <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumEntries));

        _projectRoot = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(projectRoot));
        _cacheRoot = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(cacheRoot));
        _pathPool = new DerivedDataCachePathPool(
            _cacheRoot,
            EntryExtension);
        _maximumDiskBytes = maximumDiskBytes;
        _maximumEntries = maximumEntries;
        _maximumReconcileEntryInspections = (int)Math.Clamp(
            (long)maximumEntries * ReconcileEntryExpansionFactor,
            MinimumReconcileEntryInspections,
            MaximumReconcileEntryInspections);
        _utcNow = utcNow ?? (static () => DateTime.UtcNow);
        _fileSystemCounters = fileSystemCounters;

        if (!IsUnder(_cacheRoot, _projectRoot))
        {
            throw new InvalidDataException(
                "Thumbnail Derived Data Cache must be inside the project root.");
        }
        EnsureSafeDirectory(_projectRoot, createIfMissing: false);
        EnsureSafeDirectory(_cacheRoot, createIfMissing: true);
        lock (_gate)
            _ = ReconcileAndTrimCore();
    }

    internal string CacheRoot => _cacheRoot;

    internal static bool IsCanonicalContentHash(string contentHash) =>
        contentHash.Length == 64 &&
        contentHash.All(static value =>
            (value >= '0' && value <= '9') ||
            (value >= 'a' && value <= 'f') ||
            (value >= 'A' && value <= 'F'));

    internal static string ComputeKey(
        string contentHash,
        string kind,
        int requestedEdge,
        string generatorVersion = CurrentGeneratorVersion)
    {
        if (!IsCanonicalContentHash(contentHash))
        {
            throw new InvalidDataException(
                "Thumbnail source content hash is not canonical SHA-256.");
        }
        string normalizedKind = (kind ?? "").Trim().ToLowerInvariant();
        if (normalizedKind is not ("image" or "material"))
            throw new InvalidDataException("Thumbnail asset kind is unsupported.");
        if (requestedEdge <= 0 ||
            requestedEdge > AssetImageDecoder.MaximumDecodeEdge)
        {
            throw new ArgumentOutOfRangeException(nameof(requestedEdge));
        }
        if (string.IsNullOrWhiteSpace(generatorVersion))
            throw new ArgumentException(
                "Thumbnail generator version is required.",
                nameof(generatorVersion));

        var canonical = new StringBuilder();
        Append(
            "schema",
            CurrentSchemaVersion.ToString(CultureInfo.InvariantCulture));
        Append("generator", generatorVersion.Trim());
        Append("encoding", "raw-pbgra32-le-v1");
        Append("content", contentHash.ToLowerInvariant());
        Append("kind", normalizedKind);
        Append(
            "edge",
            requestedEdge.ToString(CultureInfo.InvariantCulture));
        return Convert.ToHexString(
                SHA256.HashData(
                    Encoding.UTF8.GetBytes(canonical.ToString())))
            .ToLowerInvariant();

        void Append(string name, string value)
        {
            canonical.Append(name.Length).Append(':').Append(name)
                .Append('=').Append(value.Length).Append(':').Append(value)
                .Append('\n');
        }
    }

    internal ThumbnailDerivedDataCacheLookup TryLoad(
        string contentHash,
        string kind,
        int requestedEdge,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string key = ComputeKey(contentHash, kind, requestedEdge);
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_utcNow() >=
                _nextAccountingReconcileUtc)
            {
                _ = ReconcileAndTrimCore(
                    cancellationToken);
            }
            DerivedDataCachePath path = _pathPool.Intern(key);
            EnsureSafeDirectory(
                path.Directory,
                createIfMissing: true);
            EnsureSafeFileOrMissing(path.EntryPath);
            if (!File.Exists(path.EntryPath))
            {
                if (RemoveKnownEntry(path.EntryPath))
                {
                    _ = ReconcileAndTrimCore(
                        cancellationToken);
                }
                return new(
                    key,
                    ThumbnailDerivedDataCacheStatus.Miss,
                    null);
            }

            try
            {
                ReadEntryResult entry = ReadEntry(path.EntryPath, key);
                cancellationToken.ThrowIfCancellationRequested();
                ImageSource image = DecodePayload(
                    entry.Payload,
                    requestedEdge);
                cancellationToken.ThrowIfCancellationRequested();
                bool accountingMismatch =
                    !_entriesByPath.TryGetValue(
                        path.EntryPath,
                        out ManagedEntry? known) ||
                    known.Length != entry.EntryLength;
                DateTime touchedUtc = _utcNow();
                if (!Touch(path.EntryPath, touchedUtc))
                    accountingMismatch = true;
                RegisterKnownEntry(
                    path.EntryPath,
                    entry.EntryLength,
                    touchedUtc);
                if (accountingMismatch)
                {
                    _ = ReconcileAndTrimCore(
                        cancellationToken);
                }
                return new(
                    key,
                    ThumbnailDerivedDataCacheStatus.Hit,
                    image);
            }
            catch (InvalidDataException)
            {
                DeleteManagedFile(path.EntryPath);
                RemoveKnownEntry(path.EntryPath);
                _ = ReconcileAndTrimCore(
                    cancellationToken);
                return new(
                    key,
                    ThumbnailDerivedDataCacheStatus.Corrupt,
                    null);
            }
        }
    }

    internal bool Store(
        string contentHash,
        string kind,
        int requestedEdge,
        ImageSource image,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(image);
        cancellationToken.ThrowIfCancellationRequested();
        string key = ComputeKey(contentHash, kind, requestedEdge);
        byte[] payload = EncodePayload(
            image,
            requestedEdge);
        cancellationToken.ThrowIfCancellationRequested();
        if (HeaderBytes + payload.LongLength > _maximumDiskBytes)
            return false;

        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            DerivedDataCachePath path = _pathPool.Intern(key);
            EnsureSafeDirectory(path.Directory, createIfMissing: true);
            EnsureSafeFileOrMissing(path.EntryPath);
            ReconcileForScheduledOrExternalChange(
                path.EntryPath,
                HeaderBytes + payload.LongLength,
                cancellationToken);
            WriteEntryAtomic(
                path.EntryPath,
                key,
                payload,
                cancellationToken);
            _fileSystemCounters?.RecordAtomicPublication();
            RegisterKnownEntry(
                path.EntryPath,
                HeaderBytes + payload.LongLength,
                _utcNow());
            _ = TrimKnownEntriesToBudgetCore();
            return _entriesByPath.ContainsKey(path.EntryPath) &&
                   File.Exists(path.EntryPath);
        }
    }

    internal ThumbnailDerivedDataCacheCleanup TrimToBudget()
    {
        lock (_gate)
            return ReconcileAndTrimCore();
    }

    internal DerivedDataCachePathPoolDiagnostics
        CapturePathDiagnostics()
    {
        lock (_gate)
            return _pathPool.CaptureDiagnostics();
    }

    internal string EntryPathForSelfTest(
        string contentHash,
        string kind,
        int requestedEdge)
    {
        lock (_gate)
        {
            return _pathPool.Intern(
                ComputeKey(
                    contentHash,
                    kind,
                    requestedEdge)).EntryPath;
        }
    }

    internal static ImageSource DecodePayloadForSelfTest(
        byte[] payload,
        int expectedMaximumEdge) =>
        DecodePayload(payload, expectedMaximumEdge);

    internal static byte[] EncodePayloadForSelfTest(
        ImageSource image,
        int expectedMaximumEdge) =>
        EncodePayload(image, expectedMaximumEdge);

    private ThumbnailDerivedDataCacheCleanup ReconcileAndTrimCore(
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _fileSystemCounters?.RecordFullReconcile();
        EnsureSafeDirectory(_cacheRoot, createIfMissing: false);
        var entries = new List<ManagedEntry>();
        int removedInvalidEntries = 0;
        int removedTemporaryFiles = 0;
        int prefixDirectoryInspections = 0;
        int entryInspections = 0;

        _fileSystemCounters?.RecordPrefixDirectoryEnumeration();
        foreach (string directory in Directory.EnumerateDirectories(
                     _cacheRoot,
                     "*",
                     SearchOption.TopDirectoryOnly))
        {
            cancellationToken.ThrowIfCancellationRequested();
            prefixDirectoryInspections++;
            if (prefixDirectoryInspections >
                MaximumPrefixDirectoryInspections)
            {
                throw new InvalidDataException(
                    "Thumbnail cache contains too many top-level directories.");
            }
            CheckDirectory(directory);
            string directoryName = Path.GetFileName(directory);
            if (!IsCanonicalPrefix(directoryName))
                continue;

            _fileSystemCounters?.RecordPrefixDirectoryEnumeration();
            foreach (string path in Directory.EnumerateFileSystemEntries(
                         directory,
                         "*",
                         SearchOption.TopDirectoryOnly))
            {
                cancellationToken.ThrowIfCancellationRequested();
                entryInspections++;
                if (entryInspections >
                    _maximumReconcileEntryInspections)
                {
                    throw new InvalidDataException(
                        "Thumbnail cache reconciliation exceeded its bounded entry scan.");
                }
                _fileSystemCounters?.RecordEntryInspection();
                FileAttributes attributes = File.GetAttributes(path);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException(
                        "Thumbnail cache contains a reparse point.");
                }
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    throw new InvalidDataException(
                        "Thumbnail cache entry directory is nested unexpectedly.");
                }

                string name = Path.GetFileName(path);
                if (IsManagedTemporaryName(
                        name,
                        directoryName))
                {
                    var temporaryInfo = new FileInfo(path);
                    temporaryInfo.Refresh();
                    if (temporaryInfo.Exists &&
                        temporaryInfo.LastWriteTimeUtc <=
                        _utcNow() - StaleTemporaryAge)
                    {
                        DeleteManagedFile(path);
                        removedTemporaryFiles++;
                    }
                    continue;
                }
                if (!TryGetManagedKey(name, directoryName, out _))
                    continue;

                var info = new FileInfo(path);
                info.Refresh();
                if (!info.Exists) continue;
                if (info.Length < HeaderBytes ||
                    info.Length >
                        HeaderBytes + MaximumPayloadBytes)
                {
                    DeleteManagedFile(path);
                    removedInvalidEntries++;
                    continue;
                }
                entries.Add(
                    new ManagedEntry(
                        path,
                        info.Length,
                        info.LastWriteTimeUtc));
            }
        }

        ReplaceAccounting(entries);
        ThumbnailDerivedDataCacheCleanup trimmed =
            TrimKnownEntriesToBudgetCore();
        DateTime now = _utcNow();
        _nextAccountingReconcileUtc =
            now + AccountingReconcileInterval;
        _highWaterReconciled = IsAtHighWater(
            _retainedBytes,
            _entriesByPath.Count);
        return trimmed with
        {
            RemovedEntries =
                trimmed.RemovedEntries +
                removedInvalidEntries,
            RemovedTemporaryFiles = removedTemporaryFiles,
        };
    }

    private void ReconcileForScheduledOrExternalChange(
        string path,
        long incomingLength,
        CancellationToken cancellationToken)
    {
        bool known =
            _entriesByPath.TryGetValue(
                path,
                out ManagedEntry? knownEntry);
        bool exists = File.Exists(path);
        bool externalMismatch = known != exists;
        if (known && exists)
        {
            _fileSystemCounters?.RecordEntryInspection();
            var info = new FileInfo(path);
            info.Refresh();
            externalMismatch =
                !info.Exists ||
                info.Length != knownEntry!.Length;
        }

        long projectedBytes = ProjectedBytes(
            _retainedBytes,
            knownEntry?.Length ?? 0,
            incomingLength);
        int projectedEntries =
            _entriesByPath.Count + (known ? 0 : 1);
        bool highWater =
            IsAtHighWater(projectedBytes, projectedEntries);
        DateTime now = _utcNow();
        if (externalMismatch ||
            now >= _nextAccountingReconcileUtc ||
            (highWater && !_highWaterReconciled))
        {
            _ = ReconcileAndTrimCore(
                cancellationToken);
            known = _entriesByPath.TryGetValue(
                path,
                out knownEntry);
            projectedBytes = ProjectedBytes(
                _retainedBytes,
                knownEntry?.Length ?? 0,
                incomingLength);
            projectedEntries =
                _entriesByPath.Count + (known ? 0 : 1);
            if (IsAtHighWater(
                    projectedBytes,
                    projectedEntries))
            {
                _highWaterReconciled = true;
            }
        }
    }

    private void ReplaceAccounting(
        IEnumerable<ManagedEntry> entries)
    {
        _entriesByPath.Clear();
        _entriesByRecency.Clear();
        _retainedBytes = 0;
        foreach (ManagedEntry entry in entries)
            RegisterKnownEntry(
                entry.Path,
                entry.Length,
                entry.LastWriteUtc);
    }

    private void RegisterKnownEntry(
        string path,
        long length,
        DateTime lastWriteUtc)
    {
        if (length <= 0)
        {
            throw new InvalidDataException(
                "Thumbnail cache accounting observed an invalid entry size.");
        }
        RemoveKnownEntry(path);
        var entry = new ManagedEntry(
            path,
            length,
            lastWriteUtc);
        _entriesByPath.Add(path, entry);
        if (!_entriesByRecency.Add(entry))
        {
            _entriesByPath.Remove(path);
            throw new InvalidDataException(
                "Thumbnail cache accounting observed a duplicate entry.");
        }
        _retainedBytes = checked(_retainedBytes + length);
    }

    private bool RemoveKnownEntry(string path)
    {
        if (!_entriesByPath.Remove(
                path,
                out ManagedEntry? entry))
        {
            return false;
        }
        _entriesByRecency.Remove(entry);
        _retainedBytes =
            Math.Max(0, _retainedBytes - entry.Length);
        return true;
    }

    private ThumbnailDerivedDataCacheCleanup
        TrimKnownEntriesToBudgetCore()
    {
        int removedEntries = 0;
        while (_entriesByPath.Count > _maximumEntries ||
               _retainedBytes > _maximumDiskBytes)
        {
            ManagedEntry? oldest = _entriesByRecency.Min;
            if (oldest == null)
            {
                throw new InvalidDataException(
                    "Thumbnail cache accounting lost its eviction order.");
            }
            DeleteManagedFile(oldest.Path);
            if (!RemoveKnownEntry(oldest.Path))
            {
                throw new InvalidDataException(
                    "Thumbnail cache accounting lost an evicted entry.");
            }
            removedEntries++;
        }

        if (IsBelowLowWater(
                _retainedBytes,
                _entriesByPath.Count))
        {
            _highWaterReconciled = false;
        }
        return new(
            _entriesByPath.Count,
            _retainedBytes,
            removedEntries,
            RemovedTemporaryFiles: 0);
    }

    private bool IsAtHighWater(
        long retainedBytes,
        int retainedEntries) =>
        FractionReached(
            retainedBytes,
            _maximumDiskBytes,
            HighWaterNumerator,
            HighWaterDenominator) ||
        FractionReached(
            retainedEntries,
            _maximumEntries,
            HighWaterNumerator,
            HighWaterDenominator);

    private bool IsBelowLowWater(
        long retainedBytes,
        int retainedEntries) =>
        !FractionReached(
            retainedBytes,
            _maximumDiskBytes,
            LowWaterNumerator,
            LowWaterDenominator) &&
        !FractionReached(
            retainedEntries,
            _maximumEntries,
            LowWaterNumerator,
            LowWaterDenominator);

    private static bool FractionReached(
        long value,
        long maximum,
        int numerator,
        int denominator)
    {
        long whole = maximum / denominator;
        long remainder = maximum % denominator;
        long threshold = checked(
            whole * numerator +
            (remainder * numerator + denominator - 1) /
            denominator);
        return value >= threshold;
    }

    private static long ProjectedBytes(
        long retainedBytes,
        long replacedLength,
        long incomingLength)
    {
        long withoutReplaced =
            Math.Max(0, retainedBytes - replacedLength);
        return incomingLength >
               long.MaxValue - withoutReplaced
            ? long.MaxValue
            : withoutReplaced + incomingLength;
    }

    private ReadEntryResult ReadEntry(
        string path,
        string expectedKey)
    {
        EnsureSafeFileOrMissing(path);
        using FileStream stream = new(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read | FileShare.Delete,
            64 * 1024,
            FileOptions.SequentialScan);
        if (stream.Length < HeaderBytes ||
            stream.Length > HeaderBytes + MaximumPayloadBytes)
        {
            throw new InvalidDataException(
                "Thumbnail cache entry size is invalid.");
        }

        Span<byte> header = stackalloc byte[HeaderBytes];
        stream.ReadExactly(header);
        if (!header[..Magic.Length].SequenceEqual(Magic))
            throw new InvalidDataException(
                "Thumbnail cache entry magic is invalid.");

        int cursor = Magic.Length;
        byte[] expectedKeyBytes = Convert.FromHexString(expectedKey);
        if (!header.Slice(cursor, 32).SequenceEqual(expectedKeyBytes))
        {
            throw new InvalidDataException(
                "Thumbnail cache key does not match its path.");
        }
        cursor += 32;
        byte[] expectedPayloadHash =
            header.Slice(cursor, 32).ToArray();
        cursor += 32;
        long payloadLength = BinaryPrimitives.ReadInt64LittleEndian(
            header.Slice(cursor, sizeof(long)));
        if (payloadLength <= 0 ||
            payloadLength > MaximumPayloadBytes ||
            stream.Length != HeaderBytes + payloadLength)
        {
            throw new InvalidDataException(
                "Thumbnail cache payload length is invalid.");
        }

        byte[] payload = new byte[payloadLength];
        stream.ReadExactly(payload);
        if (!SHA256.HashData(payload).SequenceEqual(expectedPayloadHash))
        {
            throw new InvalidDataException(
                "Thumbnail cache payload checksum is invalid.");
        }
        return new(payload, stream.Length);
    }

    private void WriteEntryAtomic(
        string path,
        string key,
        byte[] payload,
        CancellationToken cancellationToken)
    {
        string parent = Path.GetDirectoryName(path)
            ?? throw new InvalidDataException(
                "Thumbnail cache entry has no parent directory.");
        EnsureSafeDirectory(parent, createIfMissing: false);
        EnsureSafeFileOrMissing(path);
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
                stream.Write(Magic);
                stream.Write(Convert.FromHexString(key));
                stream.Write(SHA256.HashData(payload));
                Span<byte> length = stackalloc byte[sizeof(long)];
                BinaryPrimitives.WriteInt64LittleEndian(
                    length,
                    payload.LongLength);
                stream.Write(length);
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }

            cancellationToken.ThrowIfCancellationRequested();
            EnsureSafeDirectory(parent, createIfMissing: false);
            EnsureSafeFileOrMissing(path);
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            TryDeleteTemporary(temporary);
        }
    }

    private static byte[] EncodePayload(
        ImageSource image,
        int requestedEdge)
    {
        if (image is not BitmapSource bitmap ||
            requestedEdge <= 0 ||
            requestedEdge > AssetImageDecoder.MaximumDecodeEdge ||
            bitmap.PixelWidth <= 0 ||
            bitmap.PixelHeight <= 0 ||
            bitmap.PixelWidth > requestedEdge ||
            bitmap.PixelHeight > requestedEdge)
        {
            throw new InvalidDataException(
                "Thumbnail image dimensions are invalid.");
        }

        BitmapSource canonical = bitmap;
        if (bitmap.Format != PixelFormats.Pbgra32)
        {
            var converted = new FormatConvertedBitmap(
                bitmap,
                PixelFormats.Pbgra32,
                destinationPalette: null,
                alphaThreshold: 0);
            converted.Freeze();
            canonical = converted;
        }

        int width = canonical.PixelWidth;
        int height = canonical.PixelHeight;
        int stride = checked(width * 4);
        int pixelBytes = checked(stride * height);
        byte[] payload =
            new byte[checked(RawPayloadHeaderBytes + pixelBytes)];
        RawPayloadMagic.CopyTo(payload, 0);
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(8, sizeof(uint)),
            RawPayloadVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(12, sizeof(uint)),
            RawPixelFormatPbgra32);
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(16, sizeof(uint)),
            checked((uint)width));
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(20, sizeof(uint)),
            checked((uint)height));
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(24, sizeof(uint)),
            checked((uint)stride));
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(28, sizeof(uint)),
            checked((uint)pixelBytes));
        canonical.CopyPixels(
            payload,
            stride,
            RawPayloadHeaderBytes);
        return payload;
    }

    private static ImageSource DecodePayload(
        byte[] payload,
        int expectedMaximumEdge)
    {
        ValidateRawPayload(
            payload,
            expectedMaximumEdge,
            out int width,
            out int height,
            out int stride,
            out int pixelBytes);
        try
        {
            byte[] pixels = new byte[pixelBytes];
            Buffer.BlockCopy(
                payload,
                RawPayloadHeaderBytes,
                pixels,
                0,
                pixelBytes);
            BitmapSource image = BitmapSource.Create(
                width,
                height,
                96,
                96,
                PixelFormats.Pbgra32,
                palette: null,
                pixels,
                stride);
            image.Freeze();
            if (image.PixelWidth != width ||
                image.PixelHeight != height ||
                image.Format != PixelFormats.Pbgra32)
            {
                throw new InvalidDataException(
                    "Thumbnail cache raw surface is invalid.");
            }
            return image;
        }
        catch (InvalidDataException)
        {
            throw;
        }
        catch (Exception error) when (
            error is not OutOfMemoryException and
            not StackOverflowException and
            not AccessViolationException)
        {
            throw new InvalidDataException(
                "Thumbnail cache raw pixel payload is invalid.",
                error);
        }
    }

    private static void ValidateRawPayload(
        byte[] payload,
        int expectedMaximumEdge,
        out int width,
        out int height,
        out int stride,
        out int pixelBytes)
    {
        width = 0;
        height = 0;
        stride = 0;
        pixelBytes = 0;
        if (expectedMaximumEdge <= 0 ||
            expectedMaximumEdge >
                AssetImageDecoder.MaximumDecodeEdge)
        {
            throw new InvalidDataException(
                "Thumbnail cache decode edge is invalid.");
        }
        if (payload.Length < RawPayloadHeaderBytes ||
            payload.LongLength > MaximumPayloadBytes ||
            !payload.AsSpan(0, RawPayloadMagic.Length)
                .SequenceEqual(RawPayloadMagic))
        {
            throw new InvalidDataException(
                "Thumbnail cache raw payload signature is invalid.");
        }
        uint version = BinaryPrimitives.ReadUInt32LittleEndian(
            payload.AsSpan(8, sizeof(uint)));
        uint pixelFormat = BinaryPrimitives.ReadUInt32LittleEndian(
            payload.AsSpan(12, sizeof(uint)));
        if (version != RawPayloadVersion ||
            pixelFormat != RawPixelFormatPbgra32)
        {
            throw new InvalidDataException(
                "Thumbnail cache raw payload contract is unsupported.");
        }
        uint encodedWidth = BinaryPrimitives.ReadUInt32LittleEndian(
            payload.AsSpan(16, sizeof(uint)));
        uint encodedHeight = BinaryPrimitives.ReadUInt32LittleEndian(
            payload.AsSpan(20, sizeof(uint)));
        if (encodedWidth == 0 ||
            encodedHeight == 0 ||
            encodedWidth > (uint)expectedMaximumEdge ||
            encodedHeight > (uint)expectedMaximumEdge)
        {
            throw new InvalidDataException(
                "Thumbnail cache raw dimensions exceed the requested edge.");
        }
        width = checked((int)encodedWidth);
        height = checked((int)encodedHeight);
        stride = checked(width * 4);
        pixelBytes = checked(stride * height);
        uint encodedStride = BinaryPrimitives.ReadUInt32LittleEndian(
            payload.AsSpan(24, sizeof(uint)));
        uint encodedPixelBytes =
            BinaryPrimitives.ReadUInt32LittleEndian(
                payload.AsSpan(28, sizeof(uint)));
        if (encodedStride != (uint)stride ||
            encodedPixelBytes != (uint)pixelBytes ||
            payload.Length !=
            checked(RawPayloadHeaderBytes + pixelBytes))
        {
            throw new InvalidDataException(
                "Thumbnail cache raw layout is invalid.");
        }
    }

    private static void ValidateKey(string key)
    {
        if (key.Length != 64 ||
            key.Any(static value =>
                !((value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f'))))
        {
            throw new InvalidDataException(
                "Thumbnail cache key is not canonical SHA-256.");
        }
    }

    private static bool IsCanonicalPrefix(string value) =>
        value.Length == 2 &&
        value.All(static character =>
            (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f'));

    private static bool TryGetManagedKey(
        string fileName,
        string expectedPrefix,
        out string key)
    {
        key = "";
        if (!fileName.EndsWith(
                EntryExtension,
                StringComparison.Ordinal) ||
            fileName.Length != 64 + EntryExtension.Length)
        {
            return false;
        }
        string candidate =
            fileName[..^EntryExtension.Length];
        try
        {
            ValidateKey(candidate);
        }
        catch (InvalidDataException)
        {
            return false;
        }
        if (!candidate.StartsWith(
                expectedPrefix,
                StringComparison.Ordinal))
        {
            return false;
        }
        key = candidate;
        return true;
    }

    private static bool IsManagedTemporaryName(
        string fileName,
        string expectedPrefix)
    {
        const int guidLength = 32;
        string marker = EntryExtension + ".tmp-";
        int markerIndex = fileName.IndexOf(
            marker,
            StringComparison.Ordinal);
        if (markerIndex != 64 ||
            fileName.Length !=
                64 + marker.Length + guidLength)
        {
            return false;
        }
        string key = fileName[..markerIndex];
        try
        {
            ValidateKey(key);
        }
        catch (InvalidDataException)
        {
            return false;
        }
        if (!key.StartsWith(
                expectedPrefix,
                StringComparison.Ordinal))
        {
            return false;
        }
        return fileName[(markerIndex + marker.Length)..]
            .All(static value =>
                (value >= '0' && value <= '9') ||
                (value >= 'a' && value <= 'f'));
    }

    private void EnsureSafeDirectory(
        string path,
        bool createIfMissing)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnderOrEqual(full, _projectRoot))
        {
            throw new InvalidDataException(
                "Thumbnail cache path escapes the project root.");
        }

        string relative =
            Path.GetRelativePath(_projectRoot, full);
        string current = _projectRoot;
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
                        "Thumbnail cache directory segment is a file.");
                }
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
        {
            throw new InvalidDataException(
                "Thumbnail cache entry escapes the cache root.");
        }
        string parent = Path.GetDirectoryName(full)
            ?? throw new InvalidDataException(
                "Thumbnail cache entry has no parent directory.");
        EnsureSafeDirectory(parent, createIfMissing: false);

        FileAttributes attributes;
        try
        {
            attributes = File.GetAttributes(full);
        }
        catch (FileNotFoundException)
        {
            return;
        }
        catch (DirectoryNotFoundException)
        {
            return;
        }
        if ((attributes & FileAttributes.Directory) != 0)
        {
            throw new InvalidDataException(
                "Thumbnail cache entry is a directory.");
        }
        if ((attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                "Thumbnail cache entry is a reparse point.");
        }
    }

    private static void CheckDirectory(string path)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0)
            throw new InvalidDataException("Expected a directory.");
        if ((attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                "Thumbnail cache path crosses a reparse point.");
        }
    }

    private void DeleteManagedFile(string path)
    {
        EnsureSafeFileOrMissing(path);
        if (File.Exists(path))
            File.Delete(path);
    }

    private void TryDeleteTemporary(string path)
    {
        try
        {
            EnsureSafeFileOrMissing(path);
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
        }
    }

    private static bool Touch(
        string path,
        DateTime touchedUtc)
    {
        try
        {
            File.SetLastWriteTimeUtc(path, touchedUtc);
            return true;
        }
        catch
        {
            // Recency is only an eviction hint; a cache hit remains valid.
            return false;
        }
    }

    private static bool IsUnder(string path, string root)
    {
        string fullRoot = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(root));
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

    private sealed record ManagedEntry(
        string Path,
        long Length,
        DateTime LastWriteUtc);

    private sealed class ManagedEntryComparer :
        IComparer<ManagedEntry>
    {
        internal static readonly ManagedEntryComparer Instance =
            new();

        public int Compare(
            ManagedEntry? left,
            ManagedEntry? right)
        {
            if (ReferenceEquals(left, right))
                return 0;
            if (left == null)
                return -1;
            if (right == null)
                return 1;
            int recency =
                left.LastWriteUtc.CompareTo(right.LastWriteUtc);
            return recency != 0
                ? recency
                : StringComparer.Ordinal.Compare(
                    left.Path,
                    right.Path);
        }
    }

    private sealed record ReadEntryResult(
        byte[] Payload,
        long EntryLength);
}
