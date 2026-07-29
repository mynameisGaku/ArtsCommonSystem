// SPDX-License-Identifier: Apache-2.0

using System.Diagnostics;
using AcsEditor;
using AcsEditor.Packaging;

namespace AcsPackage;

internal static partial class Program
{
    private sealed record SmokeCommandOptions(
        string ArchivePath,
        string ReportPath,
        PackageLaunchSmokeOptions LaunchOptions,
        bool Quiet);

    private static async Task<int> RunSmokeCommandWithDiagnosticsAsync(
        string[] args)
    {
        SmokeCommandOptions options;
        try
        {
            options = ParseSmokeOptions(args);
        }
        catch (ArgumentException error)
        {
            Console.Error.WriteLine("ERROR: " + error.Message);
            PrintSmokeUsage();
            return 2;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("ERROR: " + error.Message);
            return 1;
        }

        using var cancellation = new CancellationTokenSource();
        ConsoleCancelEventHandler cancelHandler = (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            cancellation.Cancel();
        };
        Console.CancelKeyPress += cancelHandler;
        try
        {
            PackageLaunchSmokeResult result =
                await PackageLaunchSmoke.RunAsync(
                    options.ArchivePath,
                    options.ReportPath,
                    options.LaunchOptions,
                    options.Quiet
                        ? null
                        : line => Console.WriteLine("[smoke] " + line),
                    cancellation.Token);
            if (!options.Quiet)
                PrintSmokeSummary(result);
            return result.Report.Passed ? 0 : 1;
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine(
                "ERROR: Package launch smoke was cancelled; its process tree was terminated.");
            return 130;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(
                "ERROR: Package launch smoke failed: " + error.Message);
            return 1;
        }
        finally
        {
            Console.CancelKeyPress -= cancelHandler;
        }
    }

    private static async Task<PackageLaunchSmokeResult>
        RunPublishedPackageSmokeAsync(
            string archivePath,
            bool quiet,
            CancellationToken cancellationToken = default)
    {
        PackageLaunchSmokeResult result = await PackageLaunchSmoke.RunAsync(
            archivePath,
            PackageLaunchSmoke.DefaultReportPath(archivePath),
            PackageLaunchSmokeOptions.Default,
            quiet ? null : line => Console.WriteLine("[smoke] " + line),
            cancellationToken);
        if (!quiet)
            PrintSmokeSummary(result);
        return result;
    }

    private static SmokeCommandOptions ParseSmokeOptions(string[] args)
    {
        if (args.Length < 2 ||
            !string.Equals(args[0], "smoke", StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(args[1]) ||
            args[1].StartsWith("--", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "smoke requires one package.zip path.");
        }

        string archive = Path.GetFullPath(args[1]);
        string? report = null;
        int timeoutSeconds =
            checked((int)PackageLaunchSmokeOptions.Default.Timeout.TotalSeconds);
        long maximumExtractedBytes =
            PackageLaunchSmokeOptions.Default.MaximumExtractedBytes;
        bool quiet = false;
        var seen = new HashSet<string>(StringComparer.Ordinal);

        for (int index = 2; index < args.Length; index++)
        {
            string option = args[index];
            if (!seen.Add(option))
                throw new ArgumentException($"{option} may only be specified once.");
            switch (option)
            {
                case "--report":
                    string reportValue =
                        NextValue(args, ref index, "--report");
                    if (string.IsNullOrWhiteSpace(reportValue) ||
                        reportValue.StartsWith("--", StringComparison.Ordinal))
                    {
                        throw new ArgumentException(
                            "--report requires an output file path.");
                    }
                    report = Path.GetFullPath(reportValue);
                    break;
                case "--timeout-seconds":
                    if (!int.TryParse(
                            NextValue(args, ref index, "--timeout-seconds"),
                            System.Globalization.NumberStyles.None,
                            System.Globalization.CultureInfo.InvariantCulture,
                            out timeoutSeconds) ||
                        timeoutSeconds is <
                            PackageLaunchSmoke.MinimumTimeoutSeconds or >
                            PackageLaunchSmoke.MaximumTimeoutSeconds)
                    {
                        throw new ArgumentException(
                            $"--timeout-seconds must be between " +
                            $"{PackageLaunchSmoke.MinimumTimeoutSeconds} and " +
                            $"{PackageLaunchSmoke.MaximumTimeoutSeconds}.");
                    }
                    break;
                case "--max-extract-mib":
                    if (!long.TryParse(
                            NextValue(args, ref index, "--max-extract-mib"),
                            System.Globalization.NumberStyles.None,
                            System.Globalization.CultureInfo.InvariantCulture,
                            out long maximumExtractedMiB))
                    {
                        throw new ArgumentException(
                            "--max-extract-mib requires a positive integer.");
                    }
                    try
                    {
                        maximumExtractedBytes = checked(
                            maximumExtractedMiB * 1024L * 1024L);
                    }
                    catch (OverflowException)
                    {
                        throw new ArgumentException(
                            "--max-extract-mib is outside the supported range.");
                    }
                    if (maximumExtractedBytes <
                            PackageLaunchSmoke.MinimumExtractedBytes ||
                        maximumExtractedBytes >
                            PackageLaunchSmoke.MaximumExtractedBytes)
                    {
                        throw new ArgumentException(
                            "--max-extract-mib is outside the supported range.");
                    }
                    break;
                case "--quiet":
                    quiet = true;
                    break;
                default:
                    throw new ArgumentException(
                        $"Unknown smoke argument: {option}");
            }
        }

        report ??= PackageLaunchSmoke.DefaultReportPath(archive);
        if (PathsEqual(archive, report))
            throw new ArgumentException(
                "Launch report path must not alias the package archive.");
        return new(
            archive,
            report,
            new(
                TimeSpan.FromSeconds(timeoutSeconds),
                maximumExtractedBytes),
            quiet);
    }

    private static void PrintSmokeSummary(PackageLaunchSmokeResult result)
    {
        PackageLaunchSmokeReport report = result.Report;
        Console.WriteLine(
            "Package launch smoke: " +
            (report.Passed ? "PASS" : "FAIL"));
        Console.WriteLine("Status: " + report.Status);
        if (!string.IsNullOrEmpty(report.PackageId))
            Console.WriteLine("Package ID: " + report.PackageId);
        if (!string.IsNullOrEmpty(report.BuildId))
            Console.WriteLine("Build ID: " + report.BuildId);
        foreach (PackageLaunchSmokeDiagnostic diagnostic in report.Diagnostics)
        {
            Console.WriteLine(
                $"{diagnostic.Severity.ToUpperInvariant()} " +
                $"[{diagnostic.Code}] {diagnostic.Message}");
        }
        Console.WriteLine("Report: " + result.ReportPath);
    }

    private static void PrintSmokeUsage()
    {
        Console.WriteLine(
            """
            Usage:
              acspackage smoke <package.zip>
                [--report <report.json>]
                [--timeout-seconds <1..300>]
                [--max-extract-mib <16..131072>]
                [--quiet]
            """);
    }

    private static async Task<int> RunLaunchSmokeSelfTestChildAsync(
        string[] args)
    {
        if (args.Length != 2)
            return 64;
        string token =
            Environment.GetEnvironmentVariable("ACS_PACKAGE_SMOKE_TOKEN") ?? "";
        switch (args[1])
        {
            case "assignment-marker":
                string marker =
                    Environment.GetEnvironmentVariable(
                        "ACS_PACKAGE_SMOKE_ASSIGNMENT_MARKER") ?? "";
                if (string.IsNullOrWhiteSpace(marker))
                    return 65;
                File.WriteAllText(marker, "package code ran");
                return 0;
            case "ready":
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                return 0;
            case "ready-sanitized":
                bool sanitized =
                    Environment.GetEnvironmentVariable(
                        "ACS_PACKAGE_SMOKE_SELF_TEST_SECRET") is null &&
                    !string.IsNullOrWhiteSpace(
                        Environment.GetEnvironmentVariable("SystemRoot")) &&
                    (Environment.GetEnvironmentVariable("TEMP") ?? "")
                        .Contains(
                            "runtime-environment",
                            StringComparison.OrdinalIgnoreCase) &&
                    (Environment.GetEnvironmentVariable("LOCALAPPDATA") ?? "")
                        .Contains(
                            "runtime-environment",
                            StringComparison.OrdinalIgnoreCase) &&
                    !string.IsNullOrWhiteSpace(
                        Environment.GetEnvironmentVariable("ACS_PACKAGE_ROOT")) &&
                    Environment.GetEnvironmentVariables().Count <= 32;
                if (!sanitized)
                {
                    Console.Error.WriteLine(
                        "sanitized package environment contract failed");
                    return 9;
                }
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                return 0;
            case "duplicate-ready":
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                return 0;
            case "duplicate-ready-after-capture-limit":
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                string outputChunk = new('x', 4096);
                int outputChunks =
                    PackageProcessRunner.CaptureLimitBytesPerStream /
                    outputChunk.Length + 2;
                for (int index = 0; index < outputChunks; index++)
                    Console.Write(outputChunk);
                Console.WriteLine();
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                return 0;
            case "ready-nonzero":
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                return 7;
            case "ready-sensitive-nonzero":
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                Console.Error.WriteLine(
                    token.ToUpperInvariant() + " " +
                    (Environment.GetEnvironmentVariable("ACS_PACKAGE_ROOT") ?? "")
                        .Replace('\\', '/'));
                return 8;
            case "ready-with-descendant":
                string host = Environment.ProcessPath ??
                    throw new InvalidOperationException(
                        "Launch-smoke descendant host is unavailable.");
                var descendantStart = new ProcessStartInfo
                {
                    FileName = host,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden,
                };
                if (string.Equals(
                        Path.GetFileName(host),
                        "dotnet.exe",
                        StringComparison.OrdinalIgnoreCase))
                {
                    descendantStart.ArgumentList.Add(
                        typeof(Program).Assembly.Location);
                }
                descendantStart.ArgumentList.Add(
                    "--launch-smoke-self-test-child");
                descendantStart.ArgumentList.Add("descendant-hang");
                using (Process descendant = Process.Start(descendantStart) ??
                       throw new InvalidOperationException(
                           "Launch-smoke descendant did not start."))
                {
                    // The handle is not ownership of the process. The smoke
                    // runner's Job Object must terminate this inherited
                    // descendant after the root process exits.
                }
                Console.WriteLine("ACS_PACKAGE_SMOKE_V1 READY " + token);
                return 0;
            case "descendant-hang":
                await Task.Delay(Timeout.InfiniteTimeSpan);
                return 0;
            case "no-ready":
                Console.WriteLine("child exited without readiness");
                return 0;
            case "hang":
                await Task.Delay(Timeout.InfiniteTimeSpan);
                return 0;
            default:
                return 64;
        }
    }
}
