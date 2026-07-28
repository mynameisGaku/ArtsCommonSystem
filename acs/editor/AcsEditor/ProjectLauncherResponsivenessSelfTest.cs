// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal static class ProjectLauncherResponsivenessSelfTest
{
    private static int _failures;
    private static TextWriter _output = TextWriter.Null;

    internal static int Run(TextWriter output)
    {
        _failures = 0;
        _output = output;

        using (var gate = new ProjectLauncherAsyncGate())
        {
            bool firstStarted = gate.TryBeginExclusive(
                out ProjectLauncherAsyncTicket first);
            bool duplicateStarted = gate.TryBeginExclusive(out _);
            bool firstCurrent = gate.IsCurrent(first);
            bool activeCloseDeferred =
                ProjectLauncherClosePolicy.ShouldDeferClose(
                    gate.IsExclusiveActive,
                    closeApproved: false);
            bool firstCompleted = gate.TryCompleteExclusive(first);
            bool committedCloseAccepted =
                !ProjectLauncherClosePolicy.ShouldDeferClose(
                    gate.IsExclusiveActive,
                    closeApproved: true);
            bool secondStarted = gate.TryBeginExclusive(
                out ProjectLauncherAsyncTicket second);
            Check(
                firstStarted &&
                !duplicateStarted &&
                firstCurrent &&
                activeCloseDeferred &&
                firstCompleted &&
                committedCloseAccepted &&
                secondStarted &&
                !gate.IsCurrent(first) &&
                gate.IsCurrent(second),
                "exclusive generation rejects duplicate submit and stales the previous operation");
        }

        using (var concurrentGate = new ProjectLauncherAsyncGate())
        using (var startRace = new ManualResetEventSlim(false))
        {
            Task<bool>[] contenders = Enumerable.Range(0, 16)
                .Select(index => Task.Run(() =>
                {
                    _ = index;
                    startRace.Wait();
                    return concurrentGate.TryBeginExclusive(out _);
                }))
                .ToArray();
            startRace.Set();
            Task.WaitAll(contenders);
            Check(
                contenders.Count(task => task.Result) == 1,
                "concurrent double-click race admits exactly one launcher operation");
        }

        var disposedGate = new ProjectLauncherAsyncGate();
        bool disposeStarted = disposedGate.TryBeginExclusive(
            out ProjectLauncherAsyncTicket disposeTicket);
        disposedGate.Dispose();
        Check(
            disposeStarted &&
            !disposedGate.IsCurrent(disposeTicket) &&
            !disposedGate.TryCompleteExclusive(disposeTicket) &&
            !disposedGate.TryBeginExclusive(out _),
            "window lifetime disposal rejects stale completion and later submissions");

        int callerThread = Environment.CurrentManagedThreadId;
        int workerThread = callerThread;
        using (var started = new ManualResetEventSlim(false))
        using (var release = new ManualResetEventSlim(false))
        {
            var returned = Stopwatch.StartNew();
            Task<int> pending =
                ProjectLauncherBackgroundOperations.RunAsync(
                    () =>
                    {
                        workerThread = Environment.CurrentManagedThreadId;
                        started.Set();
                        if (!release.Wait(TimeSpan.FromSeconds(5)))
                            throw new TimeoutException("test release timed out");
                        return 42;
                    });
            returned.Stop();
            bool workerStarted = started.Wait(TimeSpan.FromSeconds(5));
            bool returnedWhileBlocked = !pending.IsCompleted;
            release.Set();
            int result = pending.GetAwaiter().GetResult();
            Check(
                returned.Elapsed < TimeSpan.FromSeconds(1) &&
                workerStarted &&
                returnedWhileBlocked &&
                workerThread != callerThread &&
                result == 42,
                "filesystem runner returns immediately and executes blocking work off the caller thread");
        }

        bool cancelledAfterDrain = false;
        bool cancelledWorkCompleted = false;
        using (var cancellation = new CancellationTokenSource())
        using (var cancellationStarted = new ManualResetEventSlim(false))
        using (var cancellationRelease = new ManualResetEventSlim(false))
        {
            Task<int> cancelled =
                ProjectLauncherBackgroundOperations.RunAsync(
                    () =>
                    {
                        cancellationStarted.Set();
                        if (!cancellationRelease.Wait(TimeSpan.FromSeconds(5)))
                            throw new TimeoutException("test release timed out");
                        cancelledWorkCompleted = true;
                        return 7;
                    },
                    cancellation.Token);
            bool didStart =
                cancellationStarted.Wait(TimeSpan.FromSeconds(5));
            cancellation.Cancel();
            cancellationRelease.Set();
            try
            {
                _ = cancelled.GetAwaiter().GetResult();
            }
            catch (OperationCanceledException)
            {
                cancelledAfterDrain = true;
            }
            Check(
                didStart &&
                cancelledWorkCompleted &&
                cancelledAfterDrain,
                "non-cancellable filesystem work drains but cancellation suppresses its result");
        }

        bool exactException = false;
        try
        {
            _ = ProjectLauncherBackgroundOperations.RunAsync<int>(
                    () => throw new InvalidDataException("expected"))
                .GetAwaiter()
                .GetResult();
        }
        catch (InvalidDataException error)
            when (error.Message == "expected")
        {
            exactException = true;
        }
        Check(
            exactException,
            "filesystem runner preserves the original exception for UI diagnostics");

        int loadThread = callerThread;
        int probeThread = callerThread;
        IReadOnlyList<ProjectLauncherRecentEntry> entries =
            ProjectLauncherBackgroundOperations.LoadRecentEntriesAsync(
                    () =>
                    {
                        loadThread = Environment.CurrentManagedThreadId;
                        return
                        [
                            @"C:\Projects\Sky.acsproject",
                            @"\\offline\share\Missing.acsproject",
                        ];
                    },
                    path =>
                    {
                        probeThread = Environment.CurrentManagedThreadId;
                        return !path.Contains(
                            "Missing",
                            StringComparison.Ordinal);
                    })
                .GetAwaiter()
                .GetResult();
        Check(
            loadThread != callerThread &&
            probeThread == loadThread &&
            entries.Count == 2 &&
            entries[0].Name == "Sky" &&
            entries[0].Status == "" &&
            entries[1].Name == "Missing" &&
            entries[1].Status == "MISSING",
            "recent path read, availability probe, and status snapshot all stay on one worker");

        byte[] recentUtf8 = Encoding.UTF8.GetBytes(
            "\uFEFFC:\\Projects\\One.acsproject\r\n" +
            "c:\\projects\\ONE.acsproject\n" +
            string.Join(
                "\n",
                Enumerable.Range(2, 12).Select(
                    index => $"C:\\Projects\\P{index}.acsproject")));
        IReadOnlyList<string> parsedRecentPaths =
            ProjectManager.ParseRecentPathsSnapshot(recentUtf8);
        bool invalidUtf8Rejected = false;
        try
        {
            _ = ProjectManager.ParseRecentPathsSnapshot(
                new byte[] { 0xC3, 0x28 });
        }
        catch (DecoderFallbackException)
        {
            invalidUtf8Rejected = true;
        }
        bool oversizedRejected = false;
        try
        {
            _ = ProjectManager.ParseRecentPathsSnapshot(
                new byte[ProjectManager.MaxRecentProjectListBytes + 1]);
        }
        catch (InvalidDataException)
        {
            oversizedRejected = true;
        }
        Check(
            parsedRecentPaths.Count == 10 &&
            parsedRecentPaths[0] == @"C:\Projects\One.acsproject" &&
            invalidUtf8Rejected &&
            oversizedRejected,
            "recent list parser strips BOM, deduplicates and caps entries, and rejects invalid or oversized UTF-8");

        _output.WriteLine(
            _failures == 0
                ? "Project launcher responsiveness self-test: PASS"
                : $"Project launcher responsiveness self-test: FAIL ({_failures})");
        return _failures;
    }

    private static void Check(bool condition, string description)
    {
        _output.WriteLine(
            $"{(condition ? "PASS" : "FAIL")}: {description}");
        if (!condition)
            _failures++;
    }
}
