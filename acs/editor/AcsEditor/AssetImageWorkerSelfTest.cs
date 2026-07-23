// SPDX-License-Identifier: Apache-2.0

using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal static class AssetImageWorkerSelfTest
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
            "acs-image-worker-selftest-" +
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            string widePath = Path.Combine(root, "wide.png");
            string tallPath = Path.Combine(root, "tall.png");
            string malformedPath =
                Path.Combine(root, "malformed.png");
            string oversizedPath =
                Path.Combine(root, "oversized.png");
            WritePng(widePath, 2048, 128);
            WritePng(tallPath, 128, 2048);
            File.WriteAllText(
                malformedPath,
                "this is not an image");
            using (var oversized = new FileStream(
                       oversizedPath,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None))
            {
                oversized.SetLength(
                    AssetImageDecoder.MaximumSourceBytes + 1);
            }

            var normalDecodeElapsed = Stopwatch.StartNew();
            var wide = AssetImageDecoder.TryDecode(
                widePath,
                AssetImageDecoder.MaximumDecodeEdge + 200)
                as BitmapSource;
            var tall = AssetImageDecoder.TryDecode(
                tallPath,
                96)
                as BitmapSource;
            normalDecodeElapsed.Stop();
            output.WriteLine(
                "INFO: isolated normal decode pair completed in " +
                normalDecodeElapsed.ElapsedMilliseconds +
                " ms.");
            Check(
                wide?.PixelWidth ==
                    AssetImageDecoder.MaximumDecodeEdge &&
                wide.PixelHeight == 32 &&
                tall?.PixelWidth == 6 &&
                tall.PixelHeight == 96,
                "isolated decoder preserves aspect ratio and 512-edge quality");

            Check(
                AssetImageDecoder.TryDecode(
                    malformedPath,
                    96) == null &&
                AssetImageDecoder.TryDecode(
                    oversizedPath,
                    96) == null,
                "isolated decoder rejects malformed and oversized sources without editor failure");

            var batchElapsed = Stopwatch.StartNew();
            var batch = AssetImageWorkerClient.TryDecodeBatch(
                [widePath, malformedPath, tallPath],
                96,
                CancellationToken.None);
            batchElapsed.Stop();
            output.WriteLine(
                "INFO: isolated three-item batch completed in " +
                batchElapsed.ElapsedMilliseconds +
                " ms.");
            Check(
                batch.Count == 3 &&
                batch[0] is BitmapSource batchWide &&
                batchWide.PixelWidth == 96 &&
                batchWide.PixelHeight == 6 &&
                batch[1] == null &&
                batch[2] is BitmapSource batchTall &&
                batchTall.PixelWidth == 6 &&
                 batchTall.PixelHeight == 96,
                "bounded batch uses one child and continues past a malformed image");

            var invalidPathBatch = AssetImageWorkerClient.TryDecodeBatch(
                [widePath, "\0", tallPath],
                96,
                CancellationToken.None);
            Check(
                invalidPathBatch.Count == 3 &&
                invalidPathBatch[0] is BitmapSource invalidPathWide &&
                invalidPathWide.PixelWidth == 96 &&
                invalidPathWide.PixelHeight == 6 &&
                invalidPathBatch[1] == null &&
                invalidPathBatch[2] is BitmapSource invalidPathTall &&
                invalidPathTall.PixelWidth == 6 &&
                invalidPathTall.PixelHeight == 96,
                "invalid batch path is isolated without blanking valid siblings");

            var recoveredBatch =
                AssetImageWorkerClient.TryDecodeBatchFailureForSelfTest(
                    [widePath, tallPath, widePath],
                    96,
                    faultIndex: 1);
            Check(
                recoveredBatch.Count == 3 &&
                recoveredBatch[0] is BitmapSource recoveredFirst &&
                recoveredFirst.PixelWidth == 96 &&
                recoveredBatch[1] == null &&
                recoveredBatch[2] is BitmapSource recoveredLast &&
                recoveredLast.PixelWidth == 96,
                "batch worker failure quarantines one item and preserves valid siblings");

            var timeoutElapsed = Stopwatch.StartNew();
            ImageSource? timedOut =
                AssetImageWorkerClient.TryDecodeForSelfTest(
                    widePath,
                    96,
                    TimeSpan.FromMilliseconds(200),
                    AssetImageWorkerTestBehavior.Delay,
                    out AssetImageWorkerDiagnostics timeout);
            timeoutElapsed.Stop();
            Thread.Sleep(100);
            output.WriteLine(
                "INFO: worker memory Job Object attached: " +
                timeout.MemoryJobAttached + ".");
            Check(
                timedOut == null &&
                timeoutElapsed.Elapsed < TimeSpan.FromSeconds(3) &&
                timeout.ProcessId > 0 &&
                timeout.WasTerminated &&
                timeout.TerminationConfirmed &&
                timeout.MemoryJobAttached &&
                timeout.TemporaryDirectoryRemoved &&
                !IsProcessAlive(timeout.ProcessId) &&
                (timeout.TemporaryDirectory == null ||
                 !Directory.Exists(timeout.TemporaryDirectory)),
                "deadline kills the worker tree and removes late output state");

            ImageSource? crashed =
                AssetImageWorkerClient.TryDecodeForSelfTest(
                    widePath,
                    96,
                    TimeSpan.FromSeconds(5),
                    AssetImageWorkerTestBehavior.ExitFailure,
                    out AssetImageWorkerDiagnostics crash);
            Check(
                crashed == null &&
                crash.ProcessId > 0 &&
                crash.TerminationConfirmed &&
                crash.TemporaryDirectoryRemoved &&
                !IsProcessAlive(crash.ProcessId),
                "worker crash is contained and leaves the editor and temp store usable");

            string forbiddenNewOutput =
                Path.Combine(root, "forbidden-new.png");
            string forbiddenExistingOutput =
                Path.Combine(root, "forbidden-existing.png");
            File.WriteAllText(
                forbiddenExistingOutput,
                "sentinel");
            bool rejectedNewOutput =
                AssetImageDecodeWorker.TryRun(
                    [
                        AssetImageWorkerClient.WorkerSwitch,
                        widePath,
                        "96",
                        forbiddenNewOutput,
                    ],
                    out int rejectedNewExitCode);
            bool rejectedExistingOutput =
                AssetImageDecodeWorker.TryRun(
                    [
                        AssetImageWorkerClient.WorkerSwitch,
                        widePath,
                        "96",
                        forbiddenExistingOutput,
                    ],
                    out int rejectedExistingExitCode);
            Check(
                rejectedNewOutput &&
                rejectedExistingOutput &&
                rejectedNewExitCode != 0 &&
                rejectedExistingExitCode != 0 &&
                !File.Exists(forbiddenNewOutput) &&
                File.ReadAllText(forbiddenExistingOutput) ==
                    "sentinel",
                "worker output is confined to an ordinary private temp leaf");

            string protocolRoot = Path.Combine(
                Path.GetTempPath(),
                "acs-image-decode");
            Directory.CreateDirectory(protocolRoot);
            string ungatedDirectory = Path.Combine(
                protocolRoot,
                Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(ungatedDirectory);
            try
            {
                string ungatedOutput = Path.Combine(
                    ungatedDirectory,
                    "decoded.png");
                bool ungatedWorkerClaimed =
                    AssetImageDecodeWorker.TryRun(
                        [
                            AssetImageWorkerClient.WorkerSwitch,
                            widePath,
                            "96",
                            ungatedOutput,
                        ],
                        out int ungatedExitCode);
                Check(
                    ungatedWorkerClaimed &&
                    ungatedExitCode != 0 &&
                    !File.Exists(ungatedOutput),
                    "worker refuses WIC decode until its parent installs and releases the memory Job");
            }
            finally
            {
                try
                {
                    Directory.Delete(
                        ungatedDirectory,
                        recursive: true);
                }
                catch
                {
                }
            }

            string leasedDirectory = Path.Combine(
                protocolRoot,
                Guid.NewGuid().ToString("N"));
            string movedLeasedDirectory = leasedDirectory + "-moved";
            Directory.CreateDirectory(leasedDirectory);
            string leasedOutput = Path.Combine(
                leasedDirectory,
                "decoded.png");
            File.WriteAllText(leasedOutput, "lease sentinel");
            bool directoryLeaseOpened =
                AssetImageRequestDirectoryLease.TryOpen(
                    leasedDirectory,
                    out AssetImageRequestDirectoryLease? directoryLease) &&
                directoryLease != null;
            bool directoryRenameBlocked = false;
            bool fileReplacementBlocked = false;
            bool relativeCreateSucceeded = false;
            if (directoryLease != null)
            {
                using (directoryLease)
                {
                    if (directoryLease.TryOpenFileForRead(
                            leasedOutput,
                            out FileStream? leasedRead) &&
                        leasedRead != null)
                    {
                        using (leasedRead)
                        {
                            try
                            {
                                File.Move(
                                    leasedOutput,
                                    leasedOutput + ".moved");
                            }
                            catch (Exception error) when (
                                error is IOException or
                                    UnauthorizedAccessException)
                            {
                                fileReplacementBlocked = true;
                            }
                        }
                    }
                    relativeCreateSucceeded =
                        directoryLease.TryCreateFileForWrite(
                            "relative-probe.bin",
                            bufferSize: 64,
                            out FileStream? relativeProbe);
                    relativeProbe?.Dispose();
                    try
                    {
                        Directory.Move(
                            leasedDirectory,
                            movedLeasedDirectory);
                    }
                    catch (Exception error) when (
                        error is IOException or
                            UnauthorizedAccessException)
                    {
                        directoryRenameBlocked = true;
                    }
                }
            }
            Check(
                directoryLeaseOpened &&
                directoryRenameBlocked &&
                fileReplacementBlocked &&
                relativeCreateSucceeded &&
                Directory.Exists(leasedDirectory) &&
                File.Exists(leasedOutput) &&
                File.Exists(Path.Combine(
                    leasedDirectory,
                    "relative-probe.bin")),
                "verified temp handles block request-directory and output replacement races");
            try
            {
                if (Directory.Exists(leasedDirectory))
                    Directory.Delete(leasedDirectory, recursive: true);
                if (Directory.Exists(movedLeasedDirectory))
                    Directory.Delete(
                        movedLeasedDirectory,
                        recursive: true);
            }
            catch
            {
            }

            using var cancelled = new CancellationTokenSource(
                TimeSpan.FromMilliseconds(200));
            var cancellationElapsed = Stopwatch.StartNew();
            bool cancellationObserved = false;
            AssetImageWorkerDiagnostics cancellation = default;
            try
            {
                _ = AssetImageWorkerClient.TryDecodeForSelfTest(
                    widePath,
                    96,
                    TimeSpan.FromSeconds(10),
                    AssetImageWorkerTestBehavior.Delay,
                    cancelled.Token,
                    out cancellation);
            }
            catch (OperationCanceledException)
            {
                cancellationObserved = true;
            }
            cancellationElapsed.Stop();
            Thread.Sleep(100);
            Check(
                cancellationObserved &&
                cancellationElapsed.Elapsed <
                    TimeSpan.FromSeconds(3) &&
                cancellation.ProcessId > 0 &&
                cancellation.WasTerminated &&
                cancellation.TerminationConfirmed &&
                cancellation.TemporaryDirectoryRemoved &&
                !IsProcessAlive(cancellation.ProcessId),
                "generation cancellation kills an in-flight worker before publish");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine(
                "FAIL: image worker self-test threw: " + error);
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
            $"Asset image worker self-test: {passed} passed, {failed} failed.");
        return failed;
    }

    private static bool IsProcessAlive(int processId)
    {
        try
        {
            using Process process =
                Process.GetProcessById(processId);
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch
        {
            return false;
        }
    }

    private static void WritePng(
        string path,
        int width,
        int height)
    {
        int stride = checked(width * 4);
        byte[] pixels =
            new byte[checked(stride * height)];
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int index = y * stride + x * 4;
                pixels[index] = (byte)(x & 0xff);
                pixels[index + 1] = (byte)(y & 0xff);
                pixels[index + 2] =
                    (byte)((x + y) & 0xff);
                pixels[index + 3] = 0xff;
            }
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
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(image));
        using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None);
        encoder.Save(stream);
    }
}
