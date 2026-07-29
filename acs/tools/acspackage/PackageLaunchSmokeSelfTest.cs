// SPDX-License-Identifier: Apache-2.0

using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Text.Json;
using AcsEditor;
using AcsEditor.Packaging;

namespace AcsPackage;

internal static partial class Program
{
    private static async Task RunPackageLaunchSmokeContractSelfTestAsync(
        string archivePath,
        string testRoot)
    {
        string executable = Environment.ProcessPath ??
            throw new InvalidOperationException(
                "Package launch-smoke self-test process is unavailable.");
        string[] childPrefix =
            string.Equals(
                Path.GetFileName(executable),
                "dotnet.exe",
                StringComparison.OrdinalIgnoreCase)
                ?
                [
                    Assembly.GetExecutingAssembly().Location,
                ]
                : [];
        string[] ChildArguments(string mode) =>
            childPrefix
                .Concat(
                [
                    "--launch-smoke-self-test-child",
                    mode,
                ])
                .ToArray();
        SmokeCommandOptions parsed = ParseSmokeOptions(
            [
                "smoke",
                archivePath,
                "--timeout-seconds",
                "9",
                "--max-extract-mib",
                "32",
                "--quiet",
            ]);
        Assert(
            parsed.Quiet &&
            parsed.LaunchOptions.Timeout == TimeSpan.FromSeconds(9) &&
            parsed.LaunchOptions.MaximumExtractedBytes ==
                32L * 1024 * 1024 &&
            parsed.ReportPath ==
                PackageLaunchSmoke.DefaultReportPath(archivePath),
            "smoke CLI must parse bounded options and deterministic default report path");
        AssertSmokeParseRejected(
            ["smoke", archivePath, "--timeout-seconds", "0"],
            "timeout below the hard minimum");
        AssertSmokeParseRejected(
            ["smoke", archivePath, "--timeout-seconds", "301"],
            "timeout above the hard maximum");
        AssertSmokeParseRejected(
            ["smoke", archivePath, "--max-extract-mib", "999999999999"],
            "overflowing extraction limit");
        AssertSmokeParseRejected(
            ["smoke", archivePath, "--report", archivePath],
            "report/archive alias");
        AssertSmokeParseRejected(
            ["smoke", archivePath, "--quiet", "--quiet"],
            "duplicate options");
        string pinFixture = Path.Combine(
            testRoot,
            "launch-smoke-pin-fixture.bin");
        File.WriteAllBytes(pinFixture, [1, 2, 3, 4]);
        Assert(
            await PackageLaunchSmoke.PinnedReadDeniesMutationForSelfTestAsync(
                pinFixture),
            "a pinned verify/inspect/launch read must deny concurrent mutation");

        string preAssignmentMarker = Path.Combine(
            testRoot,
            "launch-smoke-pre-assignment.marker");
        var suspendedStart = new ProcessStartInfo
        {
            FileName = executable,
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (string argument in ChildArguments("assignment-marker"))
            suspendedStart.ArgumentList.Add(argument);
        suspendedStart.Environment[
            "ACS_PACKAGE_SMOKE_ASSIGNMENT_MARKER"] =
            preAssignmentMarker;
        bool markerAbsentBeforeAssignment = false;
        PackageProcessResult suspendedLaunch =
            await PackageProcessRunner.RunAsync(
                suspendedStart,
                log: null,
                CancellationToken.None,
                observer: null,
                containProcessTree: true,
                beforeContainedProcessJobAssignmentForSelfTest: () =>
                {
                    Thread.Sleep(250);
                    markerAbsentBeforeAssignment =
                        !File.Exists(preAssignmentMarker);
                });
        Assert(
            markerAbsentBeforeAssignment &&
            suspendedLaunch.ExitCode == 0 &&
            File.Exists(preAssignmentMarker),
            "contained launch must execute no package code before Job Object " +
            "assignment, then resume and complete normally");

        var options = new PackageLaunchSmokeOptions(
            TimeSpan.FromSeconds(5),
            PackageLaunchSmokeOptions.Default.MaximumExtractedBytes);

        string readyReportA = Path.Combine(
            testRoot,
            "launch-smoke-ready-a.json");
        const string inheritedSecretName =
            "ACS_PACKAGE_SMOKE_SELF_TEST_SECRET";
        string? previousSecret =
            Environment.GetEnvironmentVariable(inheritedSecretName);
        PackageLaunchSmokeResult readyA;
        try
        {
            Environment.SetEnvironmentVariable(
                inheritedSecretName,
                "must-not-reach-packaged-code");
            readyA = await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                readyReportA,
                options,
                ChildArguments("ready-sanitized"),
                executable);
        }
        finally
        {
            Environment.SetEnvironmentVariable(
                inheritedSecretName,
                previousSecret);
        }
        Assert(
            readyA.Report is
            {
                Passed: true,
                Status: "passed",
                ExitCode: 0,
                Checks:
                {
                    ArchiveCopiedToPrivateStaging: true,
                    ArchiveVerified: true,
                    PayloadExtracted: true,
                    ExecutableStarted: true,
                    HiddenWindowRequested: true,
                    RuntimeReadyHandshakeObserved: true,
                    CleanExitObserved: true,
                    PrivateStagingRemoved: true,
                },
            } &&
            readyA.Report.Diagnostics.Any(item =>
                item.Code == "PACKAGE_LAUNCH_SMOKE_PASSED") &&
            readyA.Report.StandardOutputExcerpt.Length == 0 &&
            readyA.Report.StandardErrorExcerpt.Length == 0,
            "authenticated hidden launch smoke must verify, extract, observe " +
            "readiness, exit cleanly, and clean private staging. Report: " +
            JsonSerializer.Serialize(readyA.Report));

        string readyReportB = Path.Combine(
            testRoot,
            "launch-smoke-ready-b.json");
        PackageLaunchSmokeResult readyB =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                readyReportB,
                options,
                ChildArguments("ready"),
                executable);
        Assert(
            File.ReadAllBytes(readyReportA)
                .SequenceEqual(File.ReadAllBytes(readyReportB)),
            "successful package launch reports must be byte-identical for " +
            "the same archive and limits");

        string descendantReport = Path.Combine(
            testRoot,
            "launch-smoke-descendant.json");
        var descendantTimer = Stopwatch.StartNew();
        PackageLaunchSmokeResult descendant =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                descendantReport,
                options,
                ChildArguments("ready-with-descendant"),
                executable);
        descendantTimer.Stop();
        Assert(
            descendant.Report.Passed &&
            descendant.Report.Checks.ExecutableStarted &&
            descendant.Report.Checks.PrivateStagingRemoved &&
            descendantTimer.Elapsed < TimeSpan.FromSeconds(10) &&
            File.ReadAllBytes(readyReportA)
                .SequenceEqual(File.ReadAllBytes(descendantReport)),
            "a detached helper must remain in the launch Job Object, be " +
            "terminated after root exit, close inherited pipes, and preserve " +
            "a deterministic successful report");

        string duplicateReport = Path.Combine(
            testRoot,
            "launch-smoke-duplicate.json");
        PackageLaunchSmokeResult duplicate =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                duplicateReport,
                options,
                ChildArguments("duplicate-ready"),
                executable);
        Assert(
            !duplicate.Report.Passed &&
            duplicate.Report.Status == "startupFailed" &&
            duplicate.Report.Diagnostics.Any(item =>
                item.Code == "RUNTIME_READY_DUPLICATE"),
            "duplicate authenticated readiness markers must fail closed");

        string duplicateAfterCaptureReport = Path.Combine(
            testRoot,
            "launch-smoke-duplicate-after-capture.json");
        PackageLaunchSmokeResult duplicateAfterCapture =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                duplicateAfterCaptureReport,
                options with { Timeout = TimeSpan.FromSeconds(15) },
                ChildArguments("duplicate-ready-after-capture-limit"),
                executable);
        Assert(
            !duplicateAfterCapture.Report.Passed &&
            duplicateAfterCapture.Report.Status == "startupFailed" &&
            duplicateAfterCapture.Report.Diagnostics.Any(item =>
                item.Code == "RUNTIME_READY_DUPLICATE") &&
            !ContainsHexNonce(
                duplicateAfterCapture.Report.StandardOutputExcerpt),
            "a duplicate readiness marker beyond bounded diagnostic capture " +
            "must still fail and redact the nonce");

        string missingReport = Path.Combine(
            testRoot,
            "launch-smoke-missing-ready.json");
        PackageLaunchSmokeResult missing =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                missingReport,
                options,
                ChildArguments("no-ready"),
                executable);
        Assert(
            !missing.Report.Passed &&
            missing.Report.Status == "startupFailed" &&
            missing.Report.ExitCode == 0 &&
            missing.Report.Diagnostics.Any(item =>
                item.Code == "RUNTIME_READY_MISSING"),
            "a clean child exit without authenticated readiness must fail");

        string nonzeroReport = Path.Combine(
            testRoot,
            "launch-smoke-nonzero.json");
        PackageLaunchSmokeResult nonzero =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                nonzeroReport,
                options,
                ChildArguments("ready-nonzero"),
                executable);
        Assert(
            !nonzero.Report.Passed &&
            nonzero.Report.Status == "startupFailed" &&
            nonzero.Report.ExitCode == 7 &&
            nonzero.Report.Diagnostics.Any(item =>
                item.Code == "RUNTIME_EXIT_NONZERO") &&
            !ContainsHexNonce(nonzero.Report.StandardOutputExcerpt),
            "readiness plus a non-zero exit must fail and redact the nonce");

        string sensitiveReport = Path.Combine(
            testRoot,
            "launch-smoke-sensitive-output.json");
        PackageLaunchSmokeResult sensitive =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                sensitiveReport,
                options,
                ChildArguments("ready-sensitive-nonzero"),
                executable);
        Assert(
            !sensitive.Report.Passed &&
            sensitive.Report.ExitCode == 8 &&
            !ContainsHexNonce(sensitive.Report.StandardErrorExcerpt) &&
            !sensitive.Report.StandardErrorExcerpt.Contains(
                "acs-package-smoke",
                StringComparison.OrdinalIgnoreCase) &&
            sensitive.Report.StandardErrorExcerpt.Contains(
                "[authenticated-nonce]",
                StringComparison.Ordinal) &&
            sensitive.Report.StandardErrorExcerpt.Contains(
                "[private-staging]",
                StringComparison.Ordinal),
            "failure excerpts must redact case-varied nonce and private path");

        string timeoutReport = Path.Combine(
            testRoot,
            "launch-smoke-timeout.json");
        var timeoutOptions = options with
        {
            Timeout = TimeSpan.FromSeconds(1)
        };
        var timeoutTimer = Stopwatch.StartNew();
        PackageLaunchSmokeResult timedOut =
            await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                timeoutReport,
                timeoutOptions,
                ChildArguments("hang"),
                executable);
        timeoutTimer.Stop();
        Assert(
            !timedOut.Report.Passed &&
            timedOut.Report.Status == "timedOut" &&
            timedOut.Report.Diagnostics.Any(item =>
                item.Code == "LAUNCH_TIMEOUT") &&
            timedOut.Report.Checks.PrivateStagingRemoved &&
            timeoutTimer.Elapsed < TimeSpan.FromSeconds(10),
            "hung runtime must be process-tree terminated and cleaned within " +
            "a bounded deadline");

        string cancelledReport = Path.Combine(
            testRoot,
            "launch-smoke-cancelled.json");
        using var cancellation = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(150));
        bool cancellationObserved = false;
        try
        {
            _ = await PackageLaunchSmoke.RunForSelfTestAsync(
                archivePath,
                cancelledReport,
                options,
                ChildArguments("hang"),
                executable,
                cancellation.Token);
        }
        catch (OperationCanceledException)
        {
            cancellationObserved = true;
        }
        Assert(
            cancellationObserved && File.Exists(cancelledReport),
            "external cancellation must terminate the child and still publish diagnostics");
        using (JsonDocument document = JsonDocument.Parse(
                   File.ReadAllBytes(cancelledReport)))
        {
            JsonElement root = document.RootElement;
            Assert(
                root.GetProperty("status").GetString() == "cancelled" &&
                !root.GetProperty("passed").GetBoolean() &&
                root.GetProperty("checks")
                    .GetProperty("privateStagingRemoved").GetBoolean(),
                "cancelled launch report must record bounded cleanup");
        }
    }

    private static void AssertSmokeParseRejected(
        string[] arguments,
        string scenario)
    {
        bool rejected = false;
        try
        {
            _ = ParseSmokeOptions(arguments);
        }
        catch (ArgumentException)
        {
            rejected = true;
        }
        Assert(rejected, $"smoke CLI must reject {scenario}");
    }

    private static bool ContainsHexNonce(string text)
    {
        int run = 0;
        foreach (char character in text)
        {
            if (character is >= '0' and <= '9' or
                >= 'a' and <= 'f' or >= 'A' and <= 'F')
            {
                run++;
                if (run >= 64)
                    return true;
            }
            else
            {
                run = 0;
            }
        }
        return false;
    }
}
