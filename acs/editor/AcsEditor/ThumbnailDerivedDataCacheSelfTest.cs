// SPDX-License-Identifier: Apache-2.0

using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal static class ThumbnailDerivedDataCacheSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS: " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL: " + label);
            }
        }

        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-thumbnail-ddc-selftest-" +
            Guid.NewGuid().ToString("N"));
        string projectRoot = Path.Combine(root, "Project");
        Directory.CreateDirectory(projectRoot);
        try
        {
            string contentA = ContentHash("content-a");
            string contentB = ContentHash("content-b");
            string contentC = ContentHash("content-c");
            string contentD = ContentHash("content-d");
            string contentAlpha = ContentHash("content-alpha");
            BitmapSource expected = CreateImage(48, 24, 17);
            BitmapSource expectedAlpha = CreateAlphaImage();
            string cacheRoot = Path.Combine(
                projectRoot,
                "Temp",
                "DerivedDataCache",
                "AssetBrowserThumbnails");
            var cacheCounters =
                new ThumbnailDerivedDataCacheFileSystemCounters();
            var cache = new ThumbnailDerivedDataCache(
                projectRoot,
                cacheRoot,
                maximumDiskBytes: 8L * 1024L * 1024L,
                maximumEntries: 8,
                fileSystemCounters: cacheCounters);

            ThumbnailDerivedDataCacheLookup first =
                cache.TryLoad(contentA, "image", 64);
            bool stored = cache.Store(
                contentA,
                "image",
                64,
                expected);
            ThumbnailDerivedDataCacheLookup hit =
                cache.TryLoad(contentA, "image", 64);
            Check(
                first.Status == ThumbnailDerivedDataCacheStatus.Miss &&
                first.Image == null &&
                stored &&
                hit.Status == ThumbnailDerivedDataCacheStatus.Hit &&
                hit.Image is BitmapSource cached &&
                PixelsEqual(expected, cached),
                "thumbnail DDC miss publishes a lossless entry and the next request hits");

            bool alphaStored = cache.Store(
                contentAlpha,
                "image",
                64,
                expectedAlpha);
            ThumbnailDerivedDataCacheLookup alphaHit =
                cache.TryLoad(contentAlpha, "image", 64);
            Check(
                alphaStored &&
                alphaHit.Status == ThumbnailDerivedDataCacheStatus.Hit &&
                alphaHit.Image is BitmapSource alphaCached &&
                alphaCached.Format == PixelFormats.Pbgra32 &&
                PixelsEqualInFormat(
                    expectedAlpha,
                    alphaCached,
                    PixelFormats.Pbgra32),
                "raw Pbgra32 cache preserves transparent and semitransparent pixels exactly");

            string baseKey = ThumbnailDerivedDataCache.ComputeKey(
                contentA,
                "image",
                64);
            Check(
                baseKey != ThumbnailDerivedDataCache.ComputeKey(
                    contentB,
                    "image",
                    64) &&
                baseKey != ThumbnailDerivedDataCache.ComputeKey(
                    contentA,
                    "material",
                    64) &&
                baseKey != ThumbnailDerivedDataCache.ComputeKey(
                    contentA,
                    "image",
                    128) &&
                baseKey != ThumbnailDerivedDataCache.ComputeKey(
                    contentA,
                    "image",
                    64,
                    "asset-browser-thumbnail-raw-pbgra32-v3"),
                "content, kind, edge, and generator version independently invalidate thumbnail keys");

            Check(
                cache.TryLoad(contentB, "image", 64).Status ==
                    ThumbnailDerivedDataCacheStatus.Miss,
                "stale source content cannot reuse a prior thumbnail entry");

            string sourceSnapshotProbe = Path.Combine(
                projectRoot,
                "thumbnail-source-snapshot.bin");
            File.WriteAllText(
                sourceSnapshotProbe,
                "before",
                new UTF8Encoding(false));
            var sourceSnapshotInfo =
                new FileInfo(sourceSnapshotProbe);
            sourceSnapshotInfo.Refresh();
            bool unchangedSnapshotAccepted =
                AssetBrowserPanel.ThumbnailSourceSnapshotMatches(
                    sourceSnapshotProbe,
                    sourceSnapshotInfo.Length,
                    sourceSnapshotInfo.LastWriteTimeUtc.Ticks);
            File.AppendAllText(
                sourceSnapshotProbe,
                "-after",
                new UTF8Encoding(false));
            bool changedSnapshotRejected =
                !AssetBrowserPanel.ThumbnailSourceSnapshotMatches(
                    sourceSnapshotProbe,
                    sourceSnapshotInfo.Length,
                    sourceSnapshotInfo.LastWriteTimeUtc.Ticks);
            Check(
                unchangedSnapshotAccepted &&
                changedSnapshotRejected,
                "persistent hits re-stat the source snapshot before UI publication");

            bool diagnosticObserverCalled = false;
            bool diagnosticObserverIsolated = true;
            try
            {
                AssetBrowserPanel
                    .PublishPersistentThumbnailCacheDiagnostic(
                        _ =>
                        {
                            diagnosticObserverCalled = true;
                            throw new OperationCanceledException(
                                "observer fixture");
                        },
                        "cache fallback fixture");
            }
            catch
            {
                diagnosticObserverIsolated = false;
            }
            Check(
                diagnosticObserverCalled &&
                diagnosticObserverIsolated,
                "cache-disable diagnostics cannot suppress full-quality fallback");

            string corruptPath = cache.EntryPathForSelfTest(
                contentA,
                "image",
                64);
            byte[] corrupt = File.ReadAllBytes(corruptPath);
            corrupt[^1] ^= 0x5a;
            File.WriteAllBytes(corruptPath, corrupt);
            long reconcilesBeforeCorruption =
                cacheCounters.Snapshot().FullReconciles;
            ThumbnailDerivedDataCacheLookup corruptLookup =
                cache.TryLoad(contentA, "image", 64);
            bool rebuilt = cache.Store(
                contentA,
                "image",
                64,
                expected);
            Check(
                corruptLookup.Status ==
                    ThumbnailDerivedDataCacheStatus.Corrupt &&
                corruptLookup.Image == null &&
                rebuilt &&
                cache.TryLoad(contentA, "image", 64).Status ==
                    ThumbnailDerivedDataCacheStatus.Hit &&
                cacheCounters.Snapshot().FullReconciles >
                    reconcilesBeforeCorruption,
                "checksum corruption is rejected, rescanned, removed, and rebuilt deterministically");

            ThumbnailDerivedDataCacheFileSystemSnapshot
                beforeCompetingPublication = cacheCounters.Snapshot();
            var competingCache = new ThumbnailDerivedDataCache(
                projectRoot,
                cacheRoot,
                maximumDiskBytes: 8L * 1024L * 1024L,
                maximumEntries: 8);
            bool competingStored = competingCache.Store(
                contentD,
                "image",
                64,
                expected);
            ThumbnailDerivedDataCacheLookup competingLookup =
                cache.TryLoad(contentD, "image", 64);
            Check(
                competingStored &&
                competingLookup.Status ==
                    ThumbnailDerivedDataCacheStatus.Hit &&
                cacheCounters.Snapshot().FullReconciles >
                    beforeCompetingPublication.FullReconciles,
                "an unknown entry from another cache instance triggers safe accounting reconciliation");

            byte[] validRaw =
                ThumbnailDerivedDataCache.EncodePayloadForSelfTest(
                    expected,
                    64);
            string legacyAtV2Path = cache.EntryPathForSelfTest(
                contentD,
                "image",
                64);
            ReplaceEntryPayload(
                legacyAtV2Path,
                validRaw.Length,
                CreatePngHeader(width: 48, height: 24));
            ThumbnailDerivedDataCacheLookup legacyLookup =
                cache.TryLoad(contentD, "image", 64);
            Check(
                legacyLookup.Status ==
                    ThumbnailDerivedDataCacheStatus.Corrupt &&
                legacyLookup.Image == null &&
                !File.Exists(legacyAtV2Path),
                "legacy compressed payload at a v2 key is deleted without entering an in-process image codec");

            byte[] truncatedRaw = validRaw[..^1];
            byte[] extendedRaw = [.. validRaw, 0x00];
            byte[] invalidStrideRaw = (byte[])validRaw.Clone();
            BinaryPrimitives.WriteUInt32LittleEndian(
                invalidStrideRaw.AsSpan(24, sizeof(uint)),
                1);
            byte[] overflowWidthRaw = (byte[])validRaw.Clone();
            BinaryPrimitives.WriteUInt32LittleEndian(
                overflowWidthRaw.AsSpan(16, sizeof(uint)),
                uint.MaxValue);
            Check(
                Throws<InvalidDataException>(() =>
                    _ = ThumbnailDerivedDataCache.DecodePayloadForSelfTest(
                        truncatedRaw,
                        64)) &&
                Throws<InvalidDataException>(() =>
                    _ = ThumbnailDerivedDataCache.DecodePayloadForSelfTest(
                        extendedRaw,
                        64)) &&
                Throws<InvalidDataException>(() =>
                    _ = ThumbnailDerivedDataCache.DecodePayloadForSelfTest(
                        invalidStrideRaw,
                        64)) &&
                Throws<InvalidDataException>(() =>
                    _ = ThumbnailDerivedDataCache.DecodePayloadForSelfTest(
                        overflowWidthRaw,
                        64)),
                "raw payload rejects truncation, trailing bytes, noncanonical stride, and overflow-sized metadata");

            BitmapSource maximumImage = CreateImage(
                AssetImageDecoder.MaximumDecodeEdge,
                AssetImageDecoder.MaximumDecodeEdge,
                23);
            byte[] maximumRaw =
                ThumbnailDerivedDataCache.EncodePayloadForSelfTest(
                    maximumImage,
                    AssetImageDecoder.MaximumDecodeEdge);
            ImageSource maximumDecoded =
                ThumbnailDerivedDataCache.DecodePayloadForSelfTest(
                    maximumRaw,
                    AssetImageDecoder.MaximumDecodeEdge);
            Check(
                maximumRaw.LongLength <=
                    ThumbnailDerivedDataCache.MaximumPayloadBytes &&
                maximumDecoded is BitmapSource maximumBitmap &&
                maximumBitmap.PixelWidth ==
                    AssetImageDecoder.MaximumDecodeEdge &&
                maximumBitmap.PixelHeight ==
                    AssetImageDecoder.MaximumDecodeEdge &&
                maximumBitmap.Format == PixelFormats.Pbgra32 &&
                PixelsEqualInFormat(
                    maximumImage,
                    maximumBitmap,
                    PixelFormats.Pbgra32),
                "maximum-edge raw surface remains bounded and lossless");

            using (var cancellation = new CancellationTokenSource())
            {
                cancellation.Cancel();
                bool cancelled = false;
                try
                {
                    _ = cache.Store(
                        contentC,
                        "image",
                        64,
                        expected,
                        cancellation.Token);
                }
                catch (OperationCanceledException)
                {
                    cancelled = true;
                }
                Check(
                    cancelled &&
                    cache.TryLoad(contentC, "image", 64).Status ==
                        ThumbnailDerivedDataCacheStatus.Miss,
                    "cancelled/latest-wins work cannot publish a partial thumbnail entry");
            }

            _ = cache.Store(
                contentC,
                "image",
                64,
                expected);
            using (var cancellation = new CancellationTokenSource())
            {
                cancellation.Cancel();
                Check(
                    Throws<OperationCanceledException>(() =>
                        _ = cache.TryLoad(
                            contentC,
                            "image",
                            64,
                            cancellation.Token)),
                    "cancelled persistent lookup exits before raw allocation or publication");
            }

            string probeRoot = Path.Combine(
                projectRoot,
                "Temp",
                "ThumbnailBudgetProbe");
            var probe = new ThumbnailDerivedDataCache(
                projectRoot,
                probeRoot,
                maximumDiskBytes: 8L * 1024L * 1024L,
                maximumEntries: 8);
            _ = probe.Store(contentA, "image", 64, expected);
            long singleEntryBytes = new FileInfo(
                probe.EntryPathForSelfTest(contentA, "image", 64)).Length;

            string budgetRoot = Path.Combine(
                projectRoot,
                "Temp",
                "ThumbnailBudget");
            long budgetBytes = singleEntryBytes + 64;
            var budget = new ThumbnailDerivedDataCache(
                projectRoot,
                budgetRoot,
                maximumDiskBytes: budgetBytes,
                maximumEntries: 8);
            _ = budget.Store(contentA, "image", 64, expected);
            File.SetLastWriteTimeUtc(
                budget.EntryPathForSelfTest(contentA, "image", 64),
                DateTime.UtcNow.AddHours(-2));
            _ = budget.Store(contentB, "image", 64, expected);
            ThumbnailDerivedDataCacheCleanup budgetCleanup =
                budget.TrimToBudget();
            Check(
                budgetCleanup.RetainedEntries == 1 &&
                budgetCleanup.RetainedBytes <= budgetBytes &&
                budget.TryLoad(contentA, "image", 64).Status ==
                    ThumbnailDerivedDataCacheStatus.Miss &&
                budget.TryLoad(contentB, "image", 64).Status ==
                    ThumbnailDerivedDataCacheStatus.Hit,
                "deterministic oldest-first cleanup enforces the disk-byte budget");

            string countRoot = Path.Combine(
                projectRoot,
                "Temp",
                "ThumbnailCountBudget");
            var countBudget = new ThumbnailDerivedDataCache(
                projectRoot,
                countRoot,
                maximumDiskBytes: 8L * 1024L * 1024L,
                maximumEntries: 2);
            _ = countBudget.Store(contentA, "image", 64, expected);
            File.SetLastWriteTimeUtc(
                countBudget.EntryPathForSelfTest(contentA, "image", 64),
                DateTime.UtcNow.AddHours(-3));
            _ = countBudget.Store(contentB, "image", 64, expected);
            File.SetLastWriteTimeUtc(
                countBudget.EntryPathForSelfTest(contentB, "image", 64),
                DateTime.UtcNow.AddHours(-2));
            _ = countBudget.Store(contentC, "image", 64, expected);
            ThumbnailDerivedDataCacheCleanup countCleanup =
                countBudget.TrimToBudget();
            Check(
                countCleanup.RetainedEntries == 2 &&
                countBudget.TryLoad(contentA, "image", 64).Status ==
                    ThumbnailDerivedDataCacheStatus.Miss &&
                countBudget.TryLoad(contentB, "image", 64).Status ==
                    ThumbnailDerivedDataCacheStatus.Hit &&
                countBudget.TryLoad(contentC, "image", 64).Status ==
                    ThumbnailDerivedDataCacheStatus.Hit,
                "deterministic oldest-first cleanup enforces the entry-count budget");

            string currentPath = countBudget.EntryPathForSelfTest(
                contentB,
                "image",
                64);
            string abandoned =
                currentPath + ".tmp-" + new string('a', 32);
            File.WriteAllText(abandoned, "temporary");
            File.SetLastWriteTimeUtc(
                abandoned,
                DateTime.UtcNow -
                ThumbnailDerivedDataCache.StaleTemporaryAge -
                TimeSpan.FromMinutes(5));
            string activeTemporary =
                currentPath + ".tmp-" + new string('b', 32);
            File.WriteAllText(activeTemporary, "active");
            ThumbnailDerivedDataCacheCleanup temporaryCleanup =
                countBudget.TrimToBudget();
            Check(
                temporaryCleanup.RemovedTemporaryFiles == 1 &&
                !File.Exists(abandoned) &&
                File.Exists(activeTemporary),
                "cleanup removes stale private publications without deleting another editor's fresh temp");

            bool outsideRejected = Throws<InvalidDataException>(() =>
                _ = new ThumbnailDerivedDataCache(
                    projectRoot,
                    Path.Combine(root, "OutsideCache")));
            string blockedSegment = Path.Combine(
                projectRoot,
                "BlockedCacheSegment");
            File.WriteAllText(blockedSegment, "not a directory");
            bool fileSegmentRejected = Throws<InvalidDataException>(() =>
                _ = new ThumbnailDerivedDataCache(
                    projectRoot,
                    Path.Combine(blockedSegment, "Cache")));
            Check(
                outsideRejected && fileSegmentRejected,
                "thumbnail DDC rejects root escape and non-directory path segments");

            string reparseTarget = Path.Combine(
                root,
                "ThumbnailReparseTarget");
            string reparseCache = Path.Combine(
                projectRoot,
                "ThumbnailReparseCache");
            Directory.CreateDirectory(reparseTarget);
            if (TryCreateDirectoryLink(
                    reparseCache,
                    reparseTarget))
            {
                Check(
                    Throws<InvalidDataException>(() =>
                        _ = new ThumbnailDerivedDataCache(
                            projectRoot,
                            reparseCache)),
                    "thumbnail DDC rejects a reparse-point cache root");
                Directory.Delete(reparseCache);
            }
            else
            {
                output.WriteLine(
                    "SKIP: OS policy did not permit thumbnail DDC reparse-point fixture.");
            }

            const int performancePublications = 256;
            const int performanceMaximumEntries = 64;
            const long performanceMaximumBytes =
                64L * 1024L * 1024L;
            string performanceRoot = Path.Combine(
                projectRoot,
                "Temp",
                "ThumbnailPerformance");
            var counters =
                new ThumbnailDerivedDataCacheFileSystemCounters();
            DateTime fixedUtc =
                new(2035, 1, 2, 3, 4, 5, DateTimeKind.Utc);
            long performanceClockTicks = 0;
            var performanceCache = new ThumbnailDerivedDataCache(
                projectRoot,
                performanceRoot,
                maximumDiskBytes: performanceMaximumBytes,
                maximumEntries: performanceMaximumEntries,
                utcNow: () =>
                    fixedUtc.AddTicks(
                        performanceClockTicks++),
                fileSystemCounters: counters);
            ThumbnailDerivedDataCacheFileSystemSnapshot beforePublications =
                counters.Snapshot();
            bool everyPublicationSucceeded = true;
            for (int index = 0;
                 index < performancePublications;
                 index++)
            {
                everyPublicationSucceeded &=
                    performanceCache.Store(
                        ContentHash(
                            "performance-content-" +
                            index.ToString(
                                System.Globalization.CultureInfo
                                    .InvariantCulture)),
                        "image",
                        64,
                        expected);
            }
            ThumbnailDerivedDataCacheFileSystemSnapshot afterPublications =
                counters.Snapshot();
            FileInfo[] retainedPerformanceEntries =
                Directory.EnumerateFiles(
                        performanceRoot,
                        "*.thumbddc",
                        SearchOption.AllDirectories)
                    .Select(static path => new FileInfo(path))
                    .ToArray();
            long retainedPerformanceBytes =
                retainedPerformanceEntries.Sum(
                    static info => info.Length);
            output.WriteLine(
                "INFO: thumbnail DDC steady-state filesystem operations " +
                $"publications={afterPublications.AtomicPublications - beforePublications.AtomicPublications} " +
                $"reconciles={afterPublications.FullReconciles - beforePublications.FullReconciles} " +
                $"prefix-enumerations={afterPublications.PrefixDirectoryEnumerations - beforePublications.PrefixDirectoryEnumerations} " +
                $"entry-inspections={afterPublications.EntryInspections - beforePublications.EntryInspections} " +
                $"retained={retainedPerformanceEntries.Length} " +
                $"bytes={retainedPerformanceBytes}.");
            Check(
                everyPublicationSucceeded &&
                afterPublications.AtomicPublications -
                    beforePublications.AtomicPublications ==
                    performancePublications &&
                afterPublications.FullReconciles -
                    beforePublications.FullReconciles <= 1 &&
                afterPublications.PrefixDirectoryEnumerations -
                    beforePublications.PrefixDirectoryEnumerations <=
                    performanceMaximumEntries + 1 &&
                afterPublications.EntryInspections -
                    beforePublications.EntryInspections <=
                    performanceMaximumEntries &&
                retainedPerformanceEntries.Length <=
                    performanceMaximumEntries &&
                retainedPerformanceBytes <=
                    performanceMaximumBytes,
                "steady-state thumbnail eviction performs N atomic writes with amortized bounded scans and accounting");

            Check(
                !Directory.EnumerateFiles(
                        cacheRoot,
                        "*.tmp-*",
                        SearchOption.AllDirectories)
                    .Any(),
                "atomic thumbnail publication leaves no temporary files");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine(
                "FAIL: thumbnail DDC self-test threw: " + error);
        }
        finally
        {
            try
            {
                Directory.Delete(root, recursive: true);
            }
            catch
            {
            }
        }

        output.WriteLine(
            $"Thumbnail DDC self-test: {passed} passed, {failed} failed.");
        return failed;
    }

    private static string ContentHash(string value) =>
        Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(value)))
            .ToLowerInvariant();

    private static byte[] CreatePngHeader(
        uint width,
        uint height)
    {
        byte[] payload = new byte[33];
        byte[] signature =
            [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
        signature.CopyTo(payload, 0);
        BinaryPrimitives.WriteUInt32BigEndian(
            payload.AsSpan(8, sizeof(uint)),
            13);
        payload[12] = (byte)'I';
        payload[13] = (byte)'H';
        payload[14] = (byte)'D';
        payload[15] = (byte)'R';
        BinaryPrimitives.WriteUInt32BigEndian(
            payload.AsSpan(16, sizeof(uint)),
            width);
        BinaryPrimitives.WriteUInt32BigEndian(
            payload.AsSpan(20, sizeof(uint)),
            height);
        return payload;
    }

    private static void ReplaceEntryPayload(
        string path,
        int originalPayloadLength,
        byte[] replacementPayload)
    {
        byte[] original = File.ReadAllBytes(path);
        int headerLength = checked(
            original.Length - originalPayloadLength);
        if (headerLength <= 40)
        {
            throw new InvalidDataException(
                "Thumbnail DDC self-test entry header is invalid.");
        }
        byte[] replacement =
            new byte[checked(headerLength + replacementPayload.Length)];
        Buffer.BlockCopy(
            original,
            0,
            replacement,
            0,
            headerLength);
        int payloadHashOffset =
            checked(headerLength - sizeof(long) - 32);
        SHA256.HashData(replacementPayload).CopyTo(
            replacement,
            payloadHashOffset);
        BinaryPrimitives.WriteInt64LittleEndian(
            replacement.AsSpan(
                headerLength - sizeof(long),
                sizeof(long)),
            replacementPayload.LongLength);
        replacementPayload.CopyTo(
            replacement,
            headerLength);
        File.WriteAllBytes(path, replacement);
    }

    private static BitmapSource CreateImage(
        int width,
        int height,
        byte seed)
    {
        int stride = checked(width * 4);
        byte[] pixels = new byte[checked(stride * height)];
        for (int index = 0; index < pixels.Length; index += 4)
        {
            pixels[index] = (byte)(seed + index);
            pixels[index + 1] = (byte)(seed * 3 + index);
            pixels[index + 2] = (byte)(seed * 7 + index);
            pixels[index + 3] = 0xff;
        }
        BitmapSource image = BitmapSource.Create(
            width,
            height,
            96,
            96,
            PixelFormats.Bgra32,
            palette: null,
            pixels,
            stride);
        image.Freeze();
        return image;
    }

    private static BitmapSource CreateAlphaImage()
    {
        const int width = 4;
        const int height = 1;
        const int stride = width * 4;
        byte[] pixels =
        [
            0, 0, 0, 0,
            16, 32, 48, 64,
            24, 56, 96, 128,
            52, 104, 208, 255,
        ];
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
        return image;
    }

    private static bool PixelsEqual(
        BitmapSource expected,
        BitmapSource actual) =>
        PixelsEqualInFormat(
            expected,
            actual,
            PixelFormats.Bgra32);

    private static bool PixelsEqualInFormat(
        BitmapSource expected,
        BitmapSource actual,
        PixelFormat pixelFormat)
    {
        if (expected.PixelWidth != actual.PixelWidth ||
            expected.PixelHeight != actual.PixelHeight)
        {
            return false;
        }
        BitmapSource expectedPixels =
            ConvertToFormat(expected, pixelFormat);
        BitmapSource actualPixels =
            ConvertToFormat(actual, pixelFormat);
        int stride = checked(expected.PixelWidth * 4);
        byte[] expectedBytes =
            new byte[checked(stride * expected.PixelHeight)];
        byte[] actualBytes =
            new byte[expectedBytes.Length];
        expectedPixels.CopyPixels(expectedBytes, stride, 0);
        actualPixels.CopyPixels(actualBytes, stride, 0);
        return expectedBytes.SequenceEqual(actualBytes);
    }

    private static BitmapSource ConvertToFormat(
        BitmapSource source,
        PixelFormat pixelFormat)
    {
        if (source.Format == pixelFormat)
            return source;
        var converted = new FormatConvertedBitmap(
            source,
            pixelFormat,
            destinationPalette: null,
            alphaThreshold: 0);
        converted.Freeze();
        return converted;
    }

    private static bool Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
            return false;
        }
        catch (TException)
        {
            return true;
        }
    }

    private static bool TryCreateDirectoryLink(
        string link,
        string target)
    {
        try
        {
            Directory.CreateSymbolicLink(link, target);
            return true;
        }
        catch (Exception error) when (
            error is UnauthorizedAccessException or IOException or
                PlatformNotSupportedException)
        {
            if (!OperatingSystem.IsWindows())
                return false;
        }

        try
        {
            var start = new ProcessStartInfo
            {
                FileName = "cmd.exe",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            start.ArgumentList.Add("/d");
            start.ArgumentList.Add("/c");
            start.ArgumentList.Add("mklink");
            start.ArgumentList.Add("/J");
            start.ArgumentList.Add(link);
            start.ArgumentList.Add(target);
            using Process process = Process.Start(start)
                ?? throw new IOException("Could not start mklink.");
            if (!process.WaitForExit(5000))
            {
                try
                {
                    process.Kill(entireProcessTree: true);
                }
                catch
                {
                }
                return false;
            }
            return process.ExitCode == 0 &&
                   Directory.Exists(link);
        }
        catch
        {
            return false;
        }
    }
}
