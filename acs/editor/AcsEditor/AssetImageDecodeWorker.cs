// SPDX-License-Identifier: Apache-2.0

using Microsoft.Win32.SafeHandles;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal enum AssetImageWorkerTestBehavior
{
    None,
    Delay,
    ExitFailure,
}

internal readonly record struct AssetImageWorkerDiagnostics(
    int ProcessId,
    bool WasTerminated,
    bool TerminationConfirmed,
    string? TemporaryDirectory,
    bool TemporaryDirectoryRemoved,
    bool MemoryJobAttached);

/// <summary>
/// 信頼できない WIC コーデックをエディタープロセス外で実行する。各要求には
/// 使い捨ての子プロセスと、通常形式かつサイズ上限付きの PNG 結果ファイルを割り当てる。
/// キャンセル、タイムアウト、メモリ過剰消費、クラッシュした子プロセスは、
/// エディターの失敗ではなくサムネイル生成失敗として扱う。
/// </summary>
internal static class AssetImageWorkerClient
{
    internal const string WorkerSwitch = "--asset-image-decode-worker";
    internal const string BatchWorkerSwitch =
        "--asset-image-decode-worker-batch";
    internal const string StartGateSwitch =
        "--asset-image-worker-start-gate";
    internal const int MaximumBatchSize = 24;
    private const int MaximumBatchArgumentCharacters = 24 * 1024;
    private const string TestDelaySwitch = "--asset-image-worker-selftest-delay";
    private const string TestExitSwitch = "--asset-image-worker-selftest-exit";
    private const string TestBatchExitSwitch =
        "--asset-image-worker-selftest-batch-exit";
    private const long MaximumWorkerWorkingSetBytes =
        384L * 1024L * 1024L;
    private static readonly TimeSpan DefaultDeadline =
        TimeSpan.FromSeconds(8);
    private static readonly TimeSpan BatchRecoveryBudget =
        TimeSpan.FromSeconds(12);
    private static readonly TimeSpan PollInterval =
        TimeSpan.FromMilliseconds(20);
    private static readonly TimeSpan StartGateDeadline =
        TimeSpan.FromSeconds(5);

    internal static ImageSource? TryDecode(
        string path,
        int requestedEdge,
        CancellationToken cancellationToken)
    {
        return TryDecodeCore(
            path,
            requestedEdge,
            cancellationToken,
            DefaultDeadline,
            AssetImageWorkerTestBehavior.None,
            out _);
    }

    internal static IReadOnlyList<ImageSource?> TryDecodeBatch(
        IReadOnlyList<string> paths,
        int requestedEdge,
        CancellationToken cancellationToken) =>
        TryDecodeBatchCore(
            paths,
            requestedEdge,
            cancellationToken,
            faultIndex: -1);

    internal static IReadOnlyList<ImageSource?>
        TryDecodeBatchFailureForSelfTest(
            IReadOnlyList<string> paths,
            int requestedEdge,
            int faultIndex) =>
        TryDecodeBatchCore(
            paths,
            requestedEdge,
            CancellationToken.None,
            faultIndex);

    private static IReadOnlyList<ImageSource?> TryDecodeBatchCore(
        IReadOnlyList<string> paths,
        int requestedEdge,
        CancellationToken cancellationToken,
        int faultIndex)
    {
        ArgumentNullException.ThrowIfNull(paths);
        if (faultIndex < -1 || faultIndex >= paths.Count)
            throw new ArgumentOutOfRangeException(nameof(faultIndex));
        var results = new ImageSource?[paths.Count];
        int edge = Math.Clamp(
            requestedEdge,
            1,
            AssetImageDecoder.MaximumDecodeEdge);
        var normalizedPaths = new string[paths.Count];
        for (int index = 0; index < paths.Count; index++)
        {
            try
            {
                string path = paths[index];
                if (string.IsNullOrWhiteSpace(path)) continue;
                string fullPath = Path.GetFullPath(path);
                if (fullPath.Length + 3 >
                    MaximumBatchArgumentCharacters)
                {
                    continue;
                }
                normalizedPaths[index] = fullPath;
            }
            catch
            {
                // One malformed asset path must not blank valid siblings.
            }
        }

        int offset = 0;
        while (offset < normalizedPaths.Length)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (string.IsNullOrEmpty(normalizedPaths[offset]))
            {
                offset++;
                continue;
            }

            int count = 0;
            int argumentCharacters = 0;
            while (offset + count < normalizedPaths.Length &&
                   count < MaximumBatchSize)
            {
                string path = normalizedPaths[offset + count];
                if (string.IsNullOrEmpty(path)) break;
                int nextArgumentCharacters = path.Length + 3;
                if (count > 0 &&
                    argumentCharacters + nextArgumentCharacters >
                    MaximumBatchArgumentCharacters)
                {
                    break;
                }
                argumentCharacters += nextArgumentCharacters;
                count++;
            }

            TryDecodeBatchChunk(
                normalizedPaths,
                offset,
                count,
                edge,
                cancellationToken,
                results,
                faultIndex);
            offset += count;
        }
        return results;
    }

    private static void TryDecodeBatchChunk(
        IReadOnlyList<string> paths,
        int offset,
        int count,
        int requestedEdge,
        CancellationToken cancellationToken,
        ImageSource?[] results,
        int faultIndex)
    {
        var budget = Stopwatch.StartNew();
        int currentOffset = offset;
        int remainingCount = count;
        while (remainingCount > 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            TimeSpan remainingBudget =
                BatchRecoveryBudget - budget.Elapsed;
            if (remainingBudget <= TimeSpan.Zero) return;
            TimeSpan attemptDeadline =
                remainingBudget < DefaultDeadline
                    ? remainingBudget
                    : DefaultDeadline;
            BatchAttemptResult attempt = TryDecodeBatchAttempt(
                paths,
                currentOffset,
                remainingCount,
                requestedEdge,
                cancellationToken,
                results,
                attemptDeadline,
                faultIndex);
            if (attempt.CompletedNormally) return;

            // Every completed item has a durable one-byte status marker. The
            // first item without one is the codec/crash boundary; quarantine it
            // for this request and continue the valid suffix in a fresh worker.
            int advance = Math.Min(
                remainingCount,
                attempt.CompletedPrefix + 1);
            currentOffset += advance;
            remainingCount -= advance;
        }
    }

    private static BatchAttemptResult TryDecodeBatchAttempt(
        IReadOnlyList<string> paths,
        int offset,
        int count,
        int requestedEdge,
        CancellationToken cancellationToken,
        ImageSource?[] results,
        TimeSpan deadline,
        int faultIndex)
    {
        string? temporaryDirectory = null;
        AssetImageRequestDirectoryLease? directoryLease = null;
        Process? process = null;
        AssetImageWorkerJob? job = null;
        EventWaitHandle? startGate = null;
        bool cancelled = false;
        try
        {
            temporaryDirectory = CreateTemporaryDirectory(
                out directoryLease);
            if (temporaryDirectory == null) return default;
            startGate = CreateStartGate(out string startGateName);
            if (startGate == null) return default;
            ProcessStartInfo? startInfo = CreateBaseStartInfo();
            if (startInfo == null) return default;
            startInfo.ArgumentList.Add(BatchWorkerSwitch);
            startInfo.ArgumentList.Add(
                requestedEdge.ToString(
                    CultureInfo.InvariantCulture));
            startInfo.ArgumentList.Add(temporaryDirectory);
            startInfo.ArgumentList.Add(StartGateSwitch);
            startInfo.ArgumentList.Add(startGateName);
            if (faultIndex >= offset &&
                faultIndex < offset + count)
            {
                startInfo.ArgumentList.Add(TestBatchExitSwitch);
                startInfo.ArgumentList.Add(
                    (faultIndex - offset).ToString(
                        CultureInfo.InvariantCulture));
            }
            for (int index = 0; index < count; index++)
            {
                startInfo.ArgumentList.Add(paths[offset + index]);
            }

            process = new Process
            {
                StartInfo = startInfo,
                EnableRaisingEvents = false,
            };
            if (!process.Start()) return default;
            job = AssetImageWorkerJob.TryAttach(
                process,
                MaximumWorkerWorkingSetBytes);
            if (job == null)
            {
                TerminateProcessTree(process, job);
                WaitForConfirmedExit(process);
                return default;
            }
            startGate.Set();

            var elapsed = Stopwatch.StartNew();
            bool exited = false;
            bool terminate = false;
            while (!(exited = process.WaitForExit(
                       (int)PollInterval.TotalMilliseconds)))
            {
                if (cancellationToken.IsCancellationRequested)
                {
                    cancelled = true;
                    terminate = true;
                    break;
                }
                if (elapsed.Elapsed >= deadline ||
                    WorkingSetLimitExceeded(
                        process,
                        MaximumWorkerWorkingSetBytes))
                {
                    terminate = true;
                    break;
                }
            }
            if (terminate)
            {
                TerminateProcessTree(process, job);
                if (!WaitForConfirmedExit(process))
                    return default;
                return RecoverCompletedBatchPrefix(
                    temporaryDirectory,
                    offset,
                    count,
                    requestedEdge,
                    results);
            }
            if (!exited ||
                !SafeHasExited(process) ||
                process.ExitCode != 0)
            {
                return RecoverCompletedBatchPrefix(
                    temporaryDirectory,
                    offset,
                    count,
                    requestedEdge,
                    results);
            }
            cancellationToken.ThrowIfCancellationRequested();

            for (int index = 0; index < count; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (TryReadBatchStatus(
                        temporaryDirectory,
                        index,
                        out bool decoded) &&
                    decoded)
                {
                    results[offset + index] = TryLoadWorkerOutput(
                        Path.Combine(
                            temporaryDirectory,
                            $"decoded-{index:D4}.png"),
                        requestedEdge);
                }
            }
            return new BatchAttemptResult(
                CompletedNormally: true,
                CompletedPrefix: count);
        }
        catch (OperationCanceledException) when (
            cancellationToken.IsCancellationRequested)
        {
            cancelled = true;
        }
        catch
        {
        }
        finally
        {
            if (process != null && !SafeHasExited(process))
            {
                TerminateProcessTree(process, job);
                WaitForConfirmedExit(process);
            }
            job?.Dispose();
            process?.Dispose();
            startGate?.Dispose();
            directoryLease?.Dispose();
            DeleteTemporaryDirectory(temporaryDirectory);
            if (cancellationToken.IsCancellationRequested)
                cancelled = true;
            if (cancelled)
                cancellationToken.ThrowIfCancellationRequested();
        }
        return default;
    }

    private static BatchAttemptResult RecoverCompletedBatchPrefix(
        string temporaryDirectory,
        int offset,
        int count,
        int requestedEdge,
        ImageSource?[] results)
    {
        int completedPrefix = 0;
        for (int index = 0; index < count; index++)
        {
            if (!TryReadBatchStatus(
                    temporaryDirectory,
                    index,
                    out bool decoded))
            {
                break;
            }
            if (decoded)
            {
                results[offset + index] = TryLoadWorkerOutput(
                    Path.Combine(
                        temporaryDirectory,
                        $"decoded-{index:D4}.png"),
                    requestedEdge);
            }
            completedPrefix++;
        }
        return new BatchAttemptResult(
            CompletedNormally: false,
            completedPrefix);
    }

    private static bool TryReadBatchStatus(
        string temporaryDirectory,
        int index,
        out bool decoded)
    {
        decoded = false;
        try
        {
            string path = Path.Combine(
                temporaryDirectory,
                $"status-{index:D4}.bin");
            if (!AssetImageRequestDirectoryLease.TryOpen(
                    temporaryDirectory,
                    out AssetImageRequestDirectoryLease? directoryLease) ||
                directoryLease == null)
            {
                return false;
            }
            using (directoryLease)
            {
                if (!directoryLease.TryOpenFileForRead(
                        path,
                        out FileStream? stream) ||
                    stream == null)
                {
                    return false;
                }
                using (stream)
                {
                    if (stream.Length != 1) return false;
                    int value = stream.ReadByte();
                    if (value is not (0 or 1)) return false;
                    decoded = value == 1;
                    return true;
                }
            }
        }
        catch
        {
            return false;
        }
    }

    private readonly record struct BatchAttemptResult(
        bool CompletedNormally,
        int CompletedPrefix);

    internal static ImageSource? TryDecodeForSelfTest(
        string path,
        int requestedEdge,
        TimeSpan deadline,
        AssetImageWorkerTestBehavior behavior,
        out AssetImageWorkerDiagnostics diagnostics)
    {
        return TryDecodeForSelfTest(
            path,
            requestedEdge,
            deadline,
            behavior,
            CancellationToken.None,
            out diagnostics);
    }

    internal static ImageSource? TryDecodeForSelfTest(
        string path,
        int requestedEdge,
        TimeSpan deadline,
        AssetImageWorkerTestBehavior behavior,
        CancellationToken cancellationToken,
        out AssetImageWorkerDiagnostics diagnostics)
    {
        return TryDecodeCore(
            path,
            requestedEdge,
            cancellationToken,
            deadline,
            behavior,
            out diagnostics);
    }

    private static ImageSource? TryDecodeCore(
        string path,
        int requestedEdge,
        CancellationToken cancellationToken,
        TimeSpan deadline,
        AssetImageWorkerTestBehavior behavior,
        out AssetImageWorkerDiagnostics diagnostics)
    {
        diagnostics = default;
        cancellationToken.ThrowIfCancellationRequested();
        if (string.IsNullOrWhiteSpace(path) ||
            deadline <= TimeSpan.Zero)
        {
            return null;
        }

        string? temporaryDirectory = null;
        AssetImageRequestDirectoryLease? directoryLease = null;
        string? outputPath = null;
        Process? process = null;
        AssetImageWorkerJob? job = null;
        EventWaitHandle? startGate = null;
        int processId = 0;
        bool wasTerminated = false;
        bool terminationConfirmed = false;
        bool temporaryDirectoryRemoved = false;
        bool cancelled = false;
        ImageSource? result = null;

        try
        {
            temporaryDirectory = CreateTemporaryDirectory(
                out directoryLease);
            if (temporaryDirectory == null) return null;
            outputPath = Path.Combine(temporaryDirectory, "decoded.png");
            startGate = CreateStartGate(out string startGateName);
            if (startGate == null) return null;

            ProcessStartInfo? startInfo = CreateStartInfo(
                Path.GetFullPath(path),
                Math.Clamp(
                    requestedEdge,
                    1,
                    AssetImageDecoder.MaximumDecodeEdge),
                outputPath,
                startGateName,
                behavior);
            if (startInfo == null) return null;

            process = new Process
            {
                StartInfo = startInfo,
                EnableRaisingEvents = false,
            };
            if (!process.Start()) return null;
            processId = process.Id;
            job = AssetImageWorkerJob.TryAttach(
                process,
                MaximumWorkerWorkingSetBytes);
            if (job == null)
            {
                wasTerminated = true;
                TerminateProcessTree(process, job);
                terminationConfirmed = WaitForConfirmedExit(process);
                return null;
            }
            startGate.Set();

            var elapsed = Stopwatch.StartNew();
            bool exited = false;
            while (!(exited = process.WaitForExit(
                       (int)PollInterval.TotalMilliseconds)))
            {
                if (cancellationToken.IsCancellationRequested)
                {
                    cancelled = true;
                    wasTerminated = true;
                    break;
                }
                if (elapsed.Elapsed >= deadline)
                {
                    wasTerminated = true;
                    break;
                }
                if (WorkingSetLimitExceeded(
                        process,
                        MaximumWorkerWorkingSetBytes))
                {
                    wasTerminated = true;
                    break;
                }
            }

            if (wasTerminated)
            {
                TerminateProcessTree(process, job);
                terminationConfirmed = WaitForConfirmedExit(process);
                return null;
            }

            terminationConfirmed = exited || SafeHasExited(process);
            if (!terminationConfirmed || process.ExitCode != 0)
                return null;
            if (cancellationToken.IsCancellationRequested)
            {
                cancelled = true;
                return null;
            }

            result = TryLoadWorkerOutput(
                outputPath,
                Math.Clamp(
                    requestedEdge,
                    1,
                    AssetImageDecoder.MaximumDecodeEdge));
            return result;
        }
        catch (OperationCanceledException) when (
            cancellationToken.IsCancellationRequested)
        {
            cancelled = true;
            return null;
        }
        catch
        {
            return null;
        }
        finally
        {
            if (process != null && !SafeHasExited(process))
            {
                wasTerminated = true;
                TerminateProcessTree(process, job);
                terminationConfirmed = WaitForConfirmedExit(process);
            }
            job?.Dispose();
            process?.Dispose();
            startGate?.Dispose();
            directoryLease?.Dispose();
            temporaryDirectoryRemoved =
                DeleteTemporaryDirectory(temporaryDirectory);
            if (cancellationToken.IsCancellationRequested)
                cancelled = true;
            diagnostics = new AssetImageWorkerDiagnostics(
                processId,
                wasTerminated,
                terminationConfirmed,
                temporaryDirectory,
                temporaryDirectoryRemoved,
                job != null);
            if (cancelled)
                cancellationToken.ThrowIfCancellationRequested();
        }
    }

    private static ProcessStartInfo? CreateStartInfo(
        string sourcePath,
        int requestedEdge,
        string outputPath,
        string startGateName,
        AssetImageWorkerTestBehavior behavior)
    {
        ProcessStartInfo? startInfo = CreateBaseStartInfo();
        if (startInfo == null) return null;

        startInfo.ArgumentList.Add(WorkerSwitch);
        startInfo.ArgumentList.Add(sourcePath);
        startInfo.ArgumentList.Add(
            requestedEdge.ToString(CultureInfo.InvariantCulture));
        startInfo.ArgumentList.Add(outputPath);
        startInfo.ArgumentList.Add(StartGateSwitch);
        startInfo.ArgumentList.Add(startGateName);
        if (behavior == AssetImageWorkerTestBehavior.Delay)
        {
            startInfo.ArgumentList.Add(TestDelaySwitch);
            startInfo.ArgumentList.Add("30000");
        }
        else if (behavior == AssetImageWorkerTestBehavior.ExitFailure)
        {
            startInfo.ArgumentList.Add(TestExitSwitch);
        }
        return startInfo;
    }

    private static ProcessStartInfo? CreateBaseStartInfo()
    {
        string? processPath = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(processPath))
        {
            try
            {
                processPath =
                    Process.GetCurrentProcess().MainModule?.FileName;
            }
            catch
            {
                return null;
            }
        }
        if (string.IsNullOrWhiteSpace(processPath))
            return null;

        var startInfo = new ProcessStartInfo
        {
            FileName = processPath,
            UseShellExecute = false,
            CreateNoWindow = true,
            ErrorDialog = false,
            WindowStyle = ProcessWindowStyle.Hidden,
            WorkingDirectory = AppContext.BaseDirectory,
        };

        bool usesDotnetHost = string.Equals(
            Path.GetFileNameWithoutExtension(processPath),
            "dotnet",
            StringComparison.OrdinalIgnoreCase);
        if (usesDotnetHost)
        {
            string? entryAssembly =
                Assembly.GetEntryAssembly()?.Location;
            if (string.IsNullOrWhiteSpace(entryAssembly) ||
                !File.Exists(entryAssembly))
            {
                return null;
            }
            startInfo.ArgumentList.Add(entryAssembly);
        }

        return startInfo;
    }

    private static EventWaitHandle? CreateStartGate(
        out string startGateName)
    {
        startGateName = string.Empty;
        for (int attempt = 0; attempt < 4; attempt++)
        {
            string candidate =
                @"Local\acs-image-worker-" +
                Guid.NewGuid().ToString("N");
            try
            {
                var gate = new EventWaitHandle(
                    initialState: false,
                    EventResetMode.ManualReset,
                    candidate,
                    out bool createdNew);
                if (createdNew)
                {
                    startGateName = candidate;
                    return gate;
                }
                gate.Dispose();
            }
            catch
            {
            }
        }
        return null;
    }

    internal static bool WaitForStartGate(string startGateName)
    {
        const string prefix = @"Local\acs-image-worker-";
        if (!startGateName.StartsWith(
                prefix,
                StringComparison.Ordinal) ||
            !Guid.TryParseExact(
                startGateName.AsSpan(prefix.Length),
                "N",
                out _))
        {
            return false;
        }
        try
        {
            using EventWaitHandle gate =
                EventWaitHandle.OpenExisting(startGateName);
            return gate.WaitOne(StartGateDeadline);
        }
        catch
        {
            return false;
        }
    }

    private static string? CreateTemporaryDirectory(
        out AssetImageRequestDirectoryLease? directoryLease)
    {
        directoryLease = null;
        try
        {
            string root = Path.Combine(
                Path.GetTempPath(),
                "acs-image-decode");
            Directory.CreateDirectory(root);
            if ((File.GetAttributes(root) &
                 FileAttributes.ReparsePoint) != 0)
            {
                return null;
            }

            for (int attempt = 0; attempt < 8; attempt++)
            {
                string candidate = Path.Combine(
                    root,
                    Guid.NewGuid().ToString("N"));
                if (Directory.Exists(candidate)) continue;
                Directory.CreateDirectory(candidate);
                if ((File.GetAttributes(candidate) &
                     FileAttributes.ReparsePoint) != 0)
                {
                    DeleteTemporaryDirectory(candidate);
                    continue;
                }
                if (AssetImageRequestDirectoryLease.TryOpen(
                        candidate,
                        out directoryLease))
                {
                    return candidate;
                }
                DeleteTemporaryDirectory(candidate);
            }
        }
        catch
        {
        }
        return null;
    }

    private static ImageSource? TryLoadWorkerOutput(
        string path,
        int maximumEdge)
    {
        try
        {
            string? directory = Path.GetDirectoryName(
                Path.GetFullPath(path));
            if (string.IsNullOrWhiteSpace(directory) ||
                !AssetImageRequestDirectoryLease.TryOpen(
                    directory,
                    out AssetImageRequestDirectoryLease? directoryLease) ||
                directoryLease == null)
            {
                return null;
            }
            using (directoryLease)
            {
                if (!directoryLease.TryOpenFileForRead(
                        path,
                        out FileStream? stream) ||
                    stream == null)
                {
                    return null;
                }
                using (stream)
                {
                    if (stream.Length <= 0 ||
                        stream.Length >
                        AssetImageDecoder.MaximumWorkerOutputBytes)
                    {
                        return null;
                    }

                    var image = new BitmapImage();
                    image.BeginInit();
                    image.CacheOption = BitmapCacheOption.OnLoad;
                    image.StreamSource = stream;
                    image.EndInit();
                    image.Freeze();
                    if (image.PixelWidth <= 0 ||
                        image.PixelHeight <= 0 ||
                        image.PixelWidth > maximumEdge ||
                        image.PixelHeight > maximumEdge)
                    {
                        return null;
                    }
                    return image;
                }
            }
        }
        catch
        {
            return null;
        }
    }

    private static bool WorkingSetLimitExceeded(
        Process process,
        long maximumBytes)
    {
        try
        {
            process.Refresh();
            return process.WorkingSet64 > maximumBytes;
        }
        catch
        {
            return false;
        }
    }

    private static void TerminateProcessTree(
        Process process,
        AssetImageWorkerJob? job)
    {
        job?.Terminate();
        try
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }
        catch
        {
        }
    }

    private static bool WaitForConfirmedExit(Process process)
    {
        try
        {
            return process.HasExited || process.WaitForExit(2000);
        }
        catch
        {
            return SafeHasExited(process);
        }
    }

    private static bool SafeHasExited(Process process)
    {
        try
        {
            return process.HasExited;
        }
        catch
        {
            return false;
        }
    }

    private static bool DeleteTemporaryDirectory(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return true;
        for (int attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                if (!Directory.Exists(path)) return true;
                Directory.Delete(path, recursive: true);
                return !Directory.Exists(path);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException)
            {
                if (attempt < 2) Thread.Sleep(10);
            }
        }
        return !Directory.Exists(path);
    }

    internal static bool IsWorkerSwitch(string argument) =>
        string.Equals(
            argument,
            WorkerSwitch,
            StringComparison.Ordinal);

    internal static bool IsTestDelaySwitch(string argument) =>
        string.Equals(
            argument,
            TestDelaySwitch,
            StringComparison.Ordinal);

    internal static bool IsTestExitSwitch(string argument) =>
        string.Equals(
            argument,
            TestExitSwitch,
            StringComparison.Ordinal);

    internal static bool IsTestBatchExitSwitch(string argument) =>
        string.Equals(
            argument,
            TestBatchExitSwitch,
            StringComparison.Ordinal);
}

/// <summary>
/// ヘッドレス子プロセスのエントリーポイント。同じソースハンドルを開いたまま、
/// ソース長、フレーム数、ソース寸法、ピクセル数、出力寸法をすべて検証する。
/// OS が対応していれば、親はこのプロセスを終了時強制停止および
/// プロセスメモリ制限付きの Windows Job に配置する。
/// </summary>
internal static class AssetImageDecodeWorker
{
    internal static bool TryRun(
        string[] arguments,
        out int exitCode)
    {
        exitCode = 64;
        if (arguments.Length == 0)
        {
            return false;
        }
        if (string.Equals(
                arguments[0],
                AssetImageWorkerClient.BatchWorkerSwitch,
                StringComparison.Ordinal))
        {
            exitCode = RunBatch(arguments);
            return true;
        }
        if (!AssetImageWorkerClient.IsWorkerSwitch(arguments[0]))
            return false;
        if (arguments.Length < 4 ||
            arguments.Length > 8 ||
            !int.TryParse(
                arguments[2],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out int requestedEdge))
        {
            return true;
        }

        requestedEdge = Math.Clamp(
            requestedEdge,
            1,
            AssetImageDecoder.MaximumDecodeEdge);
        if (!TryGetPermittedOutputPath(arguments[3], out _) ||
            arguments.Length < 6 ||
            !string.Equals(
                arguments[4],
                AssetImageWorkerClient.StartGateSwitch,
                StringComparison.Ordinal) ||
            !AssetImageWorkerClient.WaitForStartGate(arguments[5]))
        {
            return true;
        }
        try
        {
            if (arguments.Length == 7 &&
                AssetImageWorkerClient.IsTestExitSwitch(arguments[6]))
            {
                exitCode = 73;
                return true;
            }
            if (arguments.Length == 8 &&
                AssetImageWorkerClient.IsTestDelaySwitch(arguments[6]) &&
                int.TryParse(
                    arguments[7],
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out int delayMilliseconds))
            {
                Thread.Sleep(
                    Math.Clamp(delayMilliseconds, 1, 60_000));
            }
            else if (arguments.Length != 6)
            {
                return true;
            }

            exitCode = TryDecodeToPng(
                arguments[1],
                requestedEdge,
                arguments[3])
                ? 0
                : 2;
        }
        catch (OutOfMemoryException)
        {
            exitCode = 3;
        }
        catch
        {
            exitCode = 2;
        }
        return true;
    }

    private static int RunBatch(string[] arguments)
    {
        if (arguments.Length < 6 ||
            arguments.Length >
            7 + AssetImageWorkerClient.MaximumBatchSize ||
            !int.TryParse(
                arguments[1],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out int requestedEdge) ||
            !TryGetPermittedRequestDirectory(
                arguments[2],
                out string requestDirectory) ||
            !string.Equals(
                arguments[3],
                AssetImageWorkerClient.StartGateSwitch,
                StringComparison.Ordinal) ||
            !AssetImageWorkerClient.WaitForStartGate(arguments[4]))
        {
            return 64;
        }

        requestedEdge = Math.Clamp(
            requestedEdge,
            1,
            AssetImageDecoder.MaximumDecodeEdge);
        int firstPathIndex = 5;
        int injectedFailureIndex = -1;
        if (arguments.Length >= 8 &&
            AssetImageWorkerClient.IsTestBatchExitSwitch(arguments[5]) &&
            int.TryParse(
                arguments[6],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out int parsedFailureIndex))
        {
            firstPathIndex = 7;
            injectedFailureIndex = parsedFailureIndex;
        }
        int pathCount = arguments.Length - firstPathIndex;
        if (pathCount <= 0 ||
            pathCount > AssetImageWorkerClient.MaximumBatchSize ||
            injectedFailureIndex >= pathCount)
        {
            return 64;
        }
        try
        {
            for (int index = firstPathIndex;
                 index < arguments.Length;
                 index++)
            {
                int batchIndex = index - firstPathIndex;
                if (batchIndex == injectedFailureIndex)
                    return 73;
                string outputPath = Path.Combine(
                    requestDirectory,
                    $"decoded-{batchIndex:D4}.png");
                bool decoded = TryDecodeToPng(
                    arguments[index],
                    requestedEdge,
                    outputPath);
                if (!TryWriteBatchStatus(
                        requestDirectory,
                        batchIndex,
                        decoded))
                {
                    return 2;
                }
            }
            return 0;
        }
        catch (OutOfMemoryException)
        {
            return 3;
        }
        catch
        {
            return 2;
        }
    }

    private static bool TryWriteBatchStatus(
        string requestDirectory,
        int index,
        bool decoded)
    {
        if (index < 0 ||
            index >= AssetImageWorkerClient.MaximumBatchSize)
        {
            return false;
        }
        try
        {
            if (!TryGetPermittedRequestDirectory(
                    requestDirectory,
                    out string permittedDirectory) ||
                !AssetImageRequestDirectoryLease.TryOpen(
                    permittedDirectory,
                    out AssetImageRequestDirectoryLease? directoryLease) ||
                directoryLease == null)
            {
                return false;
            }
            using (directoryLease)
            {
                string statusPath = Path.Combine(
                    permittedDirectory,
                    $"status-{index:D4}.bin");
                if (!directoryLease.TryCreateFileForWrite(
                        Path.GetFileName(statusPath),
                        bufferSize: 1,
                        out FileStream? stream) ||
                    stream == null)
                {
                    return false;
                }
                using (stream)
                {
                    stream.WriteByte(decoded ? (byte)1 : (byte)0);
                    stream.Flush(flushToDisk: true);
                    return true;
                }
            }
        }
        catch
        {
            return false;
        }
    }

    private static bool TryDecodeToPng(
        string sourcePath,
        int requestedEdge,
        string outputPath)
    {
        if (!TryGetPermittedOutputPath(
                outputPath,
                out string permittedOutputPath))
        {
            return false;
        }
        string? requestDirectory =
            Path.GetDirectoryName(permittedOutputPath);
        if (string.IsNullOrWhiteSpace(requestDirectory) ||
            !AssetImageRequestDirectoryLease.TryOpen(
                requestDirectory,
                out AssetImageRequestDirectoryLease? directoryLease) ||
            directoryLease == null)
        {
            return false;
        }
        using (directoryLease)
        {
            try
            {
                using var source = new FileStream(
                    sourcePath,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete,
                    bufferSize: 64 * 1024,
                    FileOptions.SequentialScan);
                long sourceLength = source.Length;
                if (sourceLength <= 0 ||
                    sourceLength > AssetImageDecoder.MaximumSourceBytes)
                {
                    return false;
                }
                if (!TryReadGeometry(
                        source,
                        out int sourceWidth,
                        out int sourceHeight))
                {
                    return false;
                }

                source.Position = 0;
                var image = new BitmapImage();
                image.BeginInit();
                image.CacheOption = BitmapCacheOption.OnLoad;
                image.CreateOptions =
                    BitmapCreateOptions.PreservePixelFormat;
                if (sourceWidth >= sourceHeight)
                    image.DecodePixelWidth = requestedEdge;
                else
                    image.DecodePixelHeight = requestedEdge;
                image.StreamSource = source;
                image.EndInit();
                image.Freeze();

                if (source.Length != sourceLength ||
                    image.PixelWidth <= 0 ||
                    image.PixelHeight <= 0 ||
                    image.PixelWidth > requestedEdge ||
                    image.PixelHeight > requestedEdge)
                {
                    return false;
                }
                source.Position = 0;
                if (!TryReadGeometry(
                        source,
                        out int verifiedWidth,
                        out int verifiedHeight) ||
                    verifiedWidth != sourceWidth ||
                    verifiedHeight != sourceHeight ||
                    source.Length != sourceLength)
                {
                    return false;
                }

                if (!directoryLease.TryCreateFileForWrite(
                        Path.GetFileName(permittedOutputPath),
                        bufferSize: 64 * 1024,
                        out FileStream? outputFile) ||
                    outputFile == null)
                {
                    return false;
                }
                using var output = new BoundedWriteStream(
                    outputFile,
                    AssetImageDecoder.MaximumWorkerOutputBytes);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(image));
                encoder.Save(output);
                output.Flush();
                return output.Length > 0 &&
                       output.Length <=
                       AssetImageDecoder.MaximumWorkerOutputBytes;
            }
            catch
            {
                return false;
            }
        }
    }

    private static bool TryGetPermittedOutputPath(
        string outputPath,
        out string permittedOutputPath)
    {
        permittedOutputPath = string.Empty;
        try
        {
            string fullOutput = Path.GetFullPath(outputPath);
            string fileName = Path.GetFileName(fullOutput);
            bool isSingleOutput = string.Equals(
                fileName,
                "decoded.png",
                StringComparison.Ordinal);
            bool isBatchOutput =
                fileName.Length == 16 &&
                fileName.StartsWith(
                    "decoded-",
                    StringComparison.Ordinal) &&
                fileName.EndsWith(
                    ".png",
                    StringComparison.Ordinal) &&
                int.TryParse(
                    fileName.AsSpan(8, 4),
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out int batchIndex) &&
                batchIndex >= 0 &&
                batchIndex <
                    AssetImageWorkerClient.MaximumBatchSize;
            if (!isSingleOutput && !isBatchOutput)
            {
                return false;
            }

            string? requestDirectory =
                Path.GetDirectoryName(fullOutput);
            if (string.IsNullOrWhiteSpace(requestDirectory) ||
                !TryGetPermittedRequestDirectory(
                    requestDirectory,
                    out string permittedDirectory) ||
                !string.Equals(
                    requestDirectory,
                    permittedDirectory,
                    StringComparison.OrdinalIgnoreCase) ||
                File.Exists(fullOutput) ||
                Directory.Exists(fullOutput))
            {
                return false;
            }
            permittedOutputPath = fullOutput;
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static bool TryGetPermittedRequestDirectory(
        string directory,
        out string permittedDirectory)
    {
        permittedDirectory = string.Empty;
        try
        {
            string fullDirectory =
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(directory));
            string expectedRoot =
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(
                        Path.Combine(
                            Path.GetTempPath(),
                            "acs-image-decode")));
            if (!Directory.Exists(expectedRoot) ||
                !Directory.Exists(fullDirectory) ||
                !string.Equals(
                    Path.GetDirectoryName(fullDirectory),
                    expectedRoot,
                    StringComparison.OrdinalIgnoreCase) ||
                !Guid.TryParseExact(
                    Path.GetFileName(fullDirectory),
                    "N",
                    out _) ||
                (File.GetAttributes(expectedRoot) &
                 FileAttributes.ReparsePoint) != 0 ||
                (File.GetAttributes(fullDirectory) &
                 FileAttributes.ReparsePoint) != 0)
            {
                return false;
            }
            permittedDirectory = fullDirectory;
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static bool TryReadGeometry(
        Stream source,
        out int width,
        out int height)
    {
        width = 0;
        height = 0;
        try
        {
            BitmapDecoder decoder = BitmapDecoder.Create(
                source,
                BitmapCreateOptions.PreservePixelFormat |
                BitmapCreateOptions.DelayCreation,
                BitmapCacheOption.None);
            int frameCount = decoder.Frames.Count;
            if (frameCount <= 0 ||
                frameCount > AssetImageDecoder.MaximumFrameCount)
            {
                return false;
            }
            BitmapFrame frame = decoder.Frames[0];
            width = frame.PixelWidth;
            height = frame.PixelHeight;
            if (width <= 0 ||
                height <= 0 ||
                width > AssetImageDecoder.MaximumSourceDimension ||
                height > AssetImageDecoder.MaximumSourceDimension)
            {
                return false;
            }
            long pixels = checked((long)width * height);
            return pixels <= AssetImageDecoder.MaximumSourcePixels;
        }
        catch
        {
            width = 0;
            height = 0;
            return false;
        }
    }
}

internal sealed class AssetImageRequestDirectoryLease : IDisposable
{
    private const uint FileReadAttributes = 0x00000080;
    private const uint GenericRead = 0x80000000;
    private const uint GenericWrite = 0x40000000;
    private const uint Synchronize = 0x00100000;
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint OpenExisting = 3;
    private const uint FileAttributeDirectory = 0x00000010;
    private const uint FileAttributeNormal = 0x00000080;
    private const uint FileAttributeReparsePoint = 0x00000400;
    private const uint FileFlagBackupSemantics = 0x02000000;
    private const uint FileFlagOpenReparsePoint = 0x00200000;
    private const uint FileFlagSequentialScan = 0x08000000;
    private const uint ObjectCaseInsensitive = 0x00000040;
    private const uint FileOpen = 1;
    private const uint FileCreate = 2;
    private const uint FileSequentialOnly = 0x00000004;
    private const uint FileSynchronousIoNonAlert = 0x00000020;
    private const uint FileNonDirectoryFile = 0x00000040;

    private readonly SafeFileHandle _rootHandle;
    private readonly SafeFileHandle _directoryHandle;

    private AssetImageRequestDirectoryLease(
        string directoryPath,
        SafeFileHandle rootHandle,
        SafeFileHandle directoryHandle)
    {
        DirectoryPath = directoryPath;
        _rootHandle = rootHandle;
        _directoryHandle = directoryHandle;
    }

    internal string DirectoryPath { get; }

    internal static bool TryOpen(
        string directory,
        out AssetImageRequestDirectoryLease? lease)
    {
        lease = null;
        SafeFileHandle? rootHandle = null;
        SafeFileHandle? directoryHandle = null;
        try
        {
            string fullDirectory =
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(directory));
            string expectedRoot =
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(
                        Path.Combine(
                            Path.GetTempPath(),
                            "acs-image-decode")));
            if (!string.Equals(
                    Path.GetDirectoryName(fullDirectory),
                    expectedRoot,
                    StringComparison.OrdinalIgnoreCase) ||
                !Guid.TryParseExact(
                    Path.GetFileName(fullDirectory),
                    "N",
                    out _) ||
                !TryOpenVerifiedDirectory(
                    expectedRoot,
                    out rootHandle) ||
                !TryOpenVerifiedDirectory(
                    fullDirectory,
                    out directoryHandle))
            {
                rootHandle?.Dispose();
                directoryHandle?.Dispose();
                return false;
            }

            lease = new AssetImageRequestDirectoryLease(
                fullDirectory,
                rootHandle!,
                directoryHandle!);
            rootHandle = null;
            directoryHandle = null;
            return true;
        }
        catch
        {
            rootHandle?.Dispose();
            directoryHandle?.Dispose();
            return false;
        }
    }

    internal bool TryOpenFileForRead(
        string path,
        out FileStream? stream)
    {
        stream = null;
        try
        {
            string fullPath = Path.GetFullPath(path);
            if (!string.Equals(
                    Path.GetDirectoryName(fullPath),
                    DirectoryPath,
                    StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
            return TryOpenRelativeFile(
                Path.GetFileName(fullPath),
                GenericRead | Synchronize,
                FileShareRead,
                FileOpen,
                FileAccess.Read,
                bufferSize: 64 * 1024,
                out stream);
        }
        catch
        {
            return false;
        }
    }

    internal bool TryCreateFileForWrite(
        string fileName,
        int bufferSize,
        out FileStream? stream) =>
        TryOpenRelativeFile(
            fileName,
            GenericWrite | FileReadAttributes | Synchronize,
            shareAccess: 0,
            FileCreate,
            FileAccess.Write,
            bufferSize,
            out stream);

    public void Dispose()
    {
        _directoryHandle.Dispose();
        _rootHandle.Dispose();
    }

    private static bool TryOpenVerifiedDirectory(
        string path,
        out SafeFileHandle? handle)
    {
        handle = null;
        try
        {
            SafeFileHandle candidate = CreateFile(
                path,
                GenericRead,
                FileShareRead | FileShareWrite,
                nint.Zero,
                OpenExisting,
                FileFlagBackupSemantics |
                FileFlagOpenReparsePoint,
                nint.Zero);
            if (candidate.IsInvalid ||
                !GetFileInformationByHandle(
                    candidate,
                    out ByHandleFileInformation information) ||
                (information.FileAttributes &
                 FileAttributeDirectory) == 0 ||
                (information.FileAttributes &
                 FileAttributeReparsePoint) != 0 ||
                !TryGetFinalPath(candidate, out string finalPath) ||
                !string.Equals(
                    finalPath,
                    Path.TrimEndingDirectorySeparator(
                        Path.GetFullPath(path)),
                    StringComparison.OrdinalIgnoreCase))
            {
                candidate.Dispose();
                return false;
            }
            handle = candidate;
            return true;
        }
        catch
        {
            handle?.Dispose();
            handle = null;
            return false;
        }
    }

    private bool TryOpenRelativeFile(
        string fileName,
        uint desiredAccess,
        uint shareAccess,
        uint createDisposition,
        FileAccess fileAccess,
        int bufferSize,
        out FileStream? stream)
    {
        stream = null;
        if (string.IsNullOrWhiteSpace(fileName) ||
            fileName is "." or ".." ||
            !string.Equals(
                Path.GetFileName(fileName),
                fileName,
                StringComparison.Ordinal) ||
            fileName.Contains(':') ||
            bufferSize <= 0)
        {
            return false;
        }

        nint nameBuffer = 0;
        nint unicodeBuffer = 0;
        SafeFileHandle? handle = null;
        try
        {
            nameBuffer = Marshal.StringToHGlobalUni(fileName);
            var unicodeName = new UnicodeString
            {
                Length = checked((ushort)(fileName.Length * 2)),
                MaximumLength =
                    checked((ushort)((fileName.Length + 1) * 2)),
                Buffer = nameBuffer,
            };
            unicodeBuffer = Marshal.AllocHGlobal(
                Marshal.SizeOf<UnicodeString>());
            Marshal.StructureToPtr(
                unicodeName,
                unicodeBuffer,
                fDeleteOld: false);
            var attributes = new ObjectAttributes
            {
                Length = (uint)Marshal.SizeOf<ObjectAttributes>(),
                RootDirectory = _directoryHandle.DangerousGetHandle(),
                ObjectName = unicodeBuffer,
                Attributes = ObjectCaseInsensitive,
            };
            int status = NtCreateFile(
                out handle,
                desiredAccess,
                ref attributes,
                out _,
                nint.Zero,
                fileAttributes: FileAttributeNormal,
                shareAccess,
                createDisposition,
                FileSequentialOnly |
                FileSynchronousIoNonAlert |
                FileNonDirectoryFile,
                nint.Zero,
                eaLength: 0);
            if (status < 0 ||
                handle == null ||
                handle.IsInvalid ||
                !GetFileInformationByHandle(
                    handle,
                    out ByHandleFileInformation information) ||
                (information.FileAttributes &
                 (FileAttributeDirectory |
                  FileAttributeReparsePoint)) != 0)
            {
                handle?.Dispose();
                return false;
            }

            stream = new FileStream(
                handle,
                fileAccess,
                bufferSize,
                isAsync: false);
            handle = null;
            return true;
        }
        catch
        {
            handle?.Dispose();
            return false;
        }
        finally
        {
            if (unicodeBuffer != 0)
                Marshal.FreeHGlobal(unicodeBuffer);
            if (nameBuffer != 0)
                Marshal.FreeHGlobal(nameBuffer);
        }
    }

    private static bool TryGetFinalPath(
        SafeFileHandle handle,
        out string path)
    {
        path = string.Empty;
        try
        {
            var buffer = new StringBuilder(512);
            uint length = GetFinalPathNameByHandle(
                handle,
                buffer,
                (uint)buffer.Capacity,
                0);
            if (length == 0) return false;
            if (length >= buffer.Capacity)
            {
                buffer = new StringBuilder(
                    checked((int)length + 1));
                length = GetFinalPathNameByHandle(
                    handle,
                    buffer,
                    (uint)buffer.Capacity,
                    0);
                if (length == 0 ||
                    length >= buffer.Capacity)
                {
                    return false;
                }
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
        catch
        {
            path = string.Empty;
            return false;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        internal uint FileAttributes;
        internal System.Runtime.InteropServices.ComTypes.FILETIME
            CreationTime;
        internal System.Runtime.InteropServices.ComTypes.FILETIME
            LastAccessTime;
        internal System.Runtime.InteropServices.ComTypes.FILETIME
            LastWriteTime;
        internal uint VolumeSerialNumber;
        internal uint FileSizeHigh;
        internal uint FileSizeLow;
        internal uint NumberOfLinks;
        internal uint FileIndexHigh;
        internal uint FileIndexLow;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct UnicodeString
    {
        internal ushort Length;
        internal ushort MaximumLength;
        internal nint Buffer;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ObjectAttributes
    {
        internal uint Length;
        internal nint RootDirectory;
        internal nint ObjectName;
        internal uint Attributes;
        internal nint SecurityDescriptor;
        internal nint SecurityQualityOfService;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoStatusBlock
    {
        internal nint Status;
        internal nuint Information;
    }

    [DllImport(
        "kernel32.dll",
        EntryPoint = "CreateFileW",
        SetLastError = true,
        CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFile(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        nint securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        nint templateFile);

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

    [DllImport("ntdll.dll")]
    private static extern int NtCreateFile(
        out SafeFileHandle fileHandle,
        uint desiredAccess,
        ref ObjectAttributes objectAttributes,
        out IoStatusBlock ioStatusBlock,
        nint allocationSize,
        uint fileAttributes,
        uint shareAccess,
        uint createDisposition,
        uint createOptions,
        nint eaBuffer,
        uint eaLength);
}

internal sealed class BoundedWriteStream : Stream
{
    private readonly FileStream _inner;
    private readonly long _maximumLength;

    internal BoundedWriteStream(
        FileStream inner,
        long maximumLength)
    {
        _inner = inner ??
            throw new ArgumentNullException(nameof(inner));
        if (maximumLength <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumLength));
        _maximumLength = maximumLength;
    }

    public override bool CanRead => false;
    public override bool CanSeek => _inner.CanSeek;
    public override bool CanWrite => _inner.CanWrite;
    public override long Length => _inner.Length;
    public override long Position
    {
        get => _inner.Position;
        set
        {
            EnsureBound(value);
            _inner.Position = value;
        }
    }

    public override void Flush() => _inner.Flush(flushToDisk: true);

    public override int Read(
        byte[] buffer,
        int offset,
        int count) =>
        throw new NotSupportedException();

    public override long Seek(long offset, SeekOrigin origin)
    {
        long target = origin switch
        {
            SeekOrigin.Begin => offset,
            SeekOrigin.Current => checked(Position + offset),
            SeekOrigin.End => checked(Length + offset),
            _ => throw new ArgumentOutOfRangeException(nameof(origin)),
        };
        EnsureBound(target);
        return _inner.Seek(offset, origin);
    }

    public override void SetLength(long value)
    {
        EnsureBound(value);
        _inner.SetLength(value);
    }

    public override void Write(
        byte[] buffer,
        int offset,
        int count)
    {
        EnsureBound(checked(Position + count));
        _inner.Write(buffer, offset, count);
    }

    public override void Write(
        ReadOnlySpan<byte> buffer)
    {
        EnsureBound(checked(Position + buffer.Length));
        _inner.Write(buffer);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _inner.Dispose();
        base.Dispose(disposing);
    }

    private void EnsureBound(long value)
    {
        if (value < 0 || value > _maximumLength)
        {
            throw new IOException(
                "The image decoder worker exceeded its output limit.");
        }
    }
}

internal sealed class AssetImageWorkerJob : IDisposable
{
    private const uint JobObjectLimitActiveProcess = 0x00000008;
    private const uint JobObjectLimitProcessMemory = 0x00000100;
    private const uint JobObjectLimitJobMemory = 0x00000200;
    private const uint JobObjectLimitKillOnJobClose = 0x00002000;
    private const int JobObjectExtendedLimitInformationClass = 9;

    private readonly SafeFileHandle _handle;

    private AssetImageWorkerJob(SafeFileHandle handle)
    {
        _handle = handle;
    }

    internal static AssetImageWorkerJob? TryAttach(
        Process process,
        long processMemoryLimitBytes)
    {
        SafeFileHandle? handle = null;
        nint buffer = 0;
        try
        {
            nint rawHandle = CreateJobObject(
                nint.Zero,
                null);
            if (rawHandle == 0 || rawHandle == new nint(-1))
                return null;
            handle = new SafeFileHandle(
                rawHandle,
                ownsHandle: true);

            var limits =
                new JobObjectExtendedLimitInformation
                {
                    BasicLimitInformation =
                    {
                        LimitFlags =
                            JobObjectLimitKillOnJobClose |
                            JobObjectLimitProcessMemory |
                            JobObjectLimitJobMemory |
                            JobObjectLimitActiveProcess,
                        ActiveProcessLimit = 1,
                    },
                    ProcessMemoryLimit =
                        (nuint)processMemoryLimitBytes,
                    JobMemoryLimit =
                        (nuint)processMemoryLimitBytes,
                };
            int size =
                Marshal.SizeOf<JobObjectExtendedLimitInformation>();
            buffer = Marshal.AllocHGlobal(size);
            Marshal.StructureToPtr(
                limits,
                buffer,
                fDeleteOld: false);
            if (!SetInformationJobObject(
                    handle,
                    JobObjectExtendedLimitInformationClass,
                    buffer,
                    (uint)size) ||
                !AssignProcessToJobObject(
                    handle,
                    process.SafeHandle))
            {
                handle.Dispose();
                return null;
            }

            var job = new AssetImageWorkerJob(handle);
            handle = null;
            return job;
        }
        catch
        {
            handle?.Dispose();
            return null;
        }
        finally
        {
            if (buffer != 0) Marshal.FreeHGlobal(buffer);
        }
    }

    internal void Terminate()
    {
        try
        {
            TerminateJobObject(_handle, 74);
        }
        catch
        {
        }
    }

    public void Dispose() => _handle.Dispose();

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectBasicLimitInformation
    {
        internal long PerProcessUserTimeLimit;
        internal long PerJobUserTimeLimit;
        internal uint LimitFlags;
        internal nuint MinimumWorkingSetSize;
        internal nuint MaximumWorkingSetSize;
        internal uint ActiveProcessLimit;
        internal nuint Affinity;
        internal uint PriorityClass;
        internal uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters
    {
        internal ulong ReadOperationCount;
        internal ulong WriteOperationCount;
        internal ulong OtherOperationCount;
        internal ulong ReadTransferCount;
        internal ulong WriteTransferCount;
        internal ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectExtendedLimitInformation
    {
        internal JobObjectBasicLimitInformation
            BasicLimitInformation;
        internal IoCounters IoInfo;
        internal nuint ProcessMemoryLimit;
        internal nuint JobMemoryLimit;
        internal nuint PeakProcessMemoryUsed;
        internal nuint PeakJobMemoryUsed;
    }

    [DllImport(
        "kernel32.dll",
        EntryPoint = "CreateJobObjectW",
        SetLastError = true,
        CharSet = CharSet.Unicode)]
    private static extern nint CreateJobObject(
        nint securityAttributes,
        string? name);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetInformationJobObject(
        SafeFileHandle job,
        int informationClass,
        nint information,
        uint informationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AssignProcessToJobObject(
        SafeFileHandle job,
        SafeProcessHandle process);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateJobObject(
        SafeFileHandle job,
        uint exitCode);
}
