// SPDX-License-Identifier: Apache-2.0

using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal static class EditorReliabilitySoakRunner
{
    private static readonly UTF8Encoding Utf8NoBom = new(false);
    private static readonly TimeSpan TerminationGrace = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan OutputDrainGrace = TimeSpan.FromSeconds(3);

    internal static async Task<int> RunAsync(
        string executable,
        string projectFile,
        double soakSeconds,
        string reportPath,
        bool unattended,
        TextWriter output)
    {
        string childExecutable = Path.GetFullPath(executable);
        string project = Path.GetFullPath(projectFile);
        string report = Path.GetFullPath(reportPath);
        if (!File.Exists(childExecutable))
        {
            output.WriteLine("Reliability runner executable is missing: " + childExecutable);
            return 2;
        }
        if (!File.Exists(project))
        {
            output.WriteLine("Reliability runner project is missing: " + project);
            return 2;
        }
        if (!double.IsFinite(soakSeconds) || soakSeconds < 2 || soakSeconds > 600)
        {
            output.WriteLine("Reliability runner seconds must be between 2 and 600.");
            return 2;
        }

        try { if (File.Exists(report)) File.Delete(report); }
        catch (Exception error)
        {
            output.WriteLine("Cannot clear previous soak report: " + error.Message);
            return 2;
        }

        ReliabilityRecoveryFixture? recoveryFixture = null;
        if (!unattended)
        {
            try
            {
                recoveryFixture = ReliabilityRecoveryFixture.Create(project);
            }
            catch (Exception error)
            {
                output.WriteLine(
                    "Cannot create the recovery-path soak fixture: " +
                    error.Message);
                return 2;
            }
        }
        using ReliabilityRecoveryFixture? fixtureCleanup = recoveryFixture;

        var start = new ProcessStartInfo
        {
            FileName = childExecutable,
            WorkingDirectory = Path.GetDirectoryName(project)!,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        start.ArgumentList.Add(project);
        start.ArgumentList.Add("--interaction-soak");
        start.ArgumentList.Add(soakSeconds.ToString(
            "0.###",
            CultureInfo.InvariantCulture));
        start.ArgumentList.Add("--interaction-soak-report");
        start.ArgumentList.Add(report);
        start.ArgumentList.Add("--show-profiler");
        start.ArgumentList.Add("--no-activate");
        if (unattended)
        {
            // Input-free renderer soak: creates the real MainWindow/HwndHost,
            // but intentionally excludes recovery prompts and autosave writes.
            // Normal-start mode omits this flag while retaining no-activate.
            start.ArgumentList.Add("--unattended");
        }
        else
        {
            // Keep WS_EX_NOACTIVATE so the diagnostic never takes foreground
            // focus, while explicitly allowing the owned modeless recovery
            // fixture to be scheduled and required by the report.
            start.ArgumentList.Add("--interaction-soak-allow-recovery");
            start.ArgumentList.Add("--interaction-soak-require-recovery");
            start.Environment[SceneAutosaveStore.RootEnvironmentVariable] =
                recoveryFixture!.RootDirectory;
        }

        using var process = new Process { StartInfo = start };
        try
        {
            if (!process.Start())
            {
                output.WriteLine("Reliability soak child did not start.");
                return 2;
            }
        }
        catch (Exception error)
        {
            output.WriteLine(
                "Reliability soak child failed to start: " + error.Message);
            return 2;
        }
        Task<string> standardOutput = process.StandardOutput.ReadToEndAsync();
        Task<string> standardError = process.StandardError.ReadToEndAsync();

        // The child allows 45 seconds for renderer startup. The independent
        // parent deadline remains authoritative if its Dispatcher never ticks.
        TimeSpan wallTimeout = TimeSpan.FromSeconds(soakSeconds + 60);
        using var timeout = new CancellationTokenSource(wallTimeout);
        bool wallTimedOut = false;
        bool terminationTimedOut = false;
        try
        {
            await process.WaitForExitAsync(timeout.Token);
        }
        catch (OperationCanceledException)
        {
            wallTimedOut = true;
            terminationTimedOut = !await TryTerminateWithinAsync(
                process,
                TerminationGrace);
        }

        (bool outputDrained, string stdout, string stderr) =
            await DrainOutputWithinAsync(
                process,
                standardOutput,
                standardError,
                OutputDrainGrace);
        if (!string.IsNullOrWhiteSpace(stdout)) output.WriteLine(stdout.TrimEnd());
        if (!string.IsNullOrWhiteSpace(stderr)) output.WriteLine(stderr.TrimEnd());

        if (wallTimedOut)
        {
            WriteFallbackReport(
                report,
                terminationTimedOut
                    ? "CHILD_TERMINATION_TIMEOUT"
                    : "EXTERNAL_WALL_TIMEOUT",
                $"Child exceeded {wallTimeout.TotalSeconds:0} seconds; " +
                $"terminationCompleted={!terminationTimedOut}; " +
                $"outputDrained={outputDrained}.");
            output.WriteLine("Reliability soak failed: external wall timeout.");
            return 1;
        }

        if (!outputDrained)
        {
            WriteFallbackReport(
                report,
                "OUTPUT_DRAIN_TIMEOUT",
                $"Redirected output did not close within " +
                $"{OutputDrainGrace.TotalSeconds:0} seconds.");
            output.WriteLine(
                "Reliability soak failed: redirected output drain timed out.");
            return 1;
        }

        if (!File.Exists(report))
        {
            WriteFallbackReport(
                report,
                "REPORT_MISSING",
                $"Child exited {process.ExitCode} without a report.");
            output.WriteLine("Reliability soak failed: report was not generated.");
            return 1;
        }

        try
        {
            string reportJson = await File.ReadAllTextAsync(
                report,
                Encoding.UTF8);
            bool passed = TryValidatePassReport(
                reportJson,
                process.ExitCode,
                recoveryRequired: !unattended,
                out string validationError);
            output.WriteLine(
                $"Reliability soak {(passed ? "PASS" : "FAIL")}: {report}" +
                (passed ? "" : " (" + validationError + ")"));
            return passed ? 0 : 1;
        }
        catch (Exception error)
        {
            WriteFallbackReport(
                report + ".runner-failure.json",
                "REPORT_INVALID",
                error.Message);
            output.WriteLine("Reliability soak report is invalid: " + error.Message);
            return 1;
        }
    }

    internal static bool TryValidatePassReport(
        string json,
        int processExitCode,
        bool recoveryRequired,
        out string error)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;
            if (!root.TryGetProperty("SchemaVersion", out JsonElement schema) ||
                !schema.TryGetInt32(out int schemaVersion) ||
                schemaVersion != EditorReliabilityReportContract.CurrentSchemaVersion)
            {
                error = "unsupported or missing SchemaVersion";
                return false;
            }
            if (processExitCode != 0)
            {
                error = $"child exit code was {processExitCode}";
                return false;
            }
            if (!root.TryGetProperty("Result", out JsonElement result) ||
                !string.Equals(
                    result.GetString(),
                    "PASS",
                    StringComparison.Ordinal))
            {
                error = "Result is not PASS";
                return false;
            }
            if (!TryGetFiniteNumber(root, "RequestedSeconds", out double seconds) ||
                seconds < 2 || seconds > 600 ||
                !TryGetFiniteNumber(root, "ActualSeconds", out double actual) ||
                actual + 0.001 < seconds)
            {
                error = "invalid or incomplete duration metrics";
                return false;
            }
            int expectedByPolicy =
                EditorInteractionSoakPolicy.ExpectedDispatcherTicks(
                    TimeSpan.FromSeconds(seconds));
            int requiredDispatcherByPolicy = Math.Max(
                2,
                (int)Math.Ceiling(
                    expectedByPolicy *
                    EditorInteractionSoakPolicy.RequiredDispatcherTickDensity));
            int requiredProfilerByPolicy = Math.Max(
                1,
                (int)Math.Ceiling(
                    expectedByPolicy *
                    EditorInteractionSoakPolicy.RequiredProfilerAdvanceDensity));
            if (!TryGetInt32(root, "ExpectedDispatcherTicks", out int expected) ||
                expected != expectedByPolicy ||
                !TryGetInt32(root, "RequiredDispatcherTicks", out int requiredDispatcher) ||
                requiredDispatcher != requiredDispatcherByPolicy ||
                !TryGetInt32(root, "DispatcherTicks", out int dispatcherTicks) ||
                dispatcherTicks < requiredDispatcher ||
                !TryGetInt32(root, "RequiredProfilerAdvancedTicks", out int requiredProfiler) ||
                requiredProfiler != requiredProfilerByPolicy ||
                !TryGetInt32(root, "ProfilerAdvancedTicks", out int profilerTicks) ||
                profilerTicks < requiredProfiler)
            {
                error = "heartbeat counts do not satisfy the schema policy";
                return false;
            }
            if (!TryGetFiniteNumber(
                    root,
                    "DispatcherIntervalMilliseconds",
                    out double interval) ||
                Math.Abs(
                    interval -
                    EditorInteractionSoakPolicy.TimerIntervalMilliseconds) >
                    0.001 ||
                !TryGetFiniteNumber(
                    root,
                    "DispatcherTickDensity",
                    out double dispatcherDensity) ||
                Math.Abs(
                    dispatcherDensity -
                    dispatcherTicks / (double)expected) > 0.001 ||
                !TryGetFiniteNumber(
                    root,
                    "ProfilerAdvanceDensity",
                    out double profilerDensity) ||
                Math.Abs(
                    profilerDensity -
                    profilerTicks / (double)expected) > 0.001)
            {
                error = "heartbeat interval or density metrics are inconsistent";
                return false;
            }
            if (!TryGetFiniteNumber(
                    root,
                    "MaximumDispatcherGapMilliseconds",
                    out double dispatcherGap) ||
                dispatcherGap < 0 ||
                dispatcherGap >
                    EditorInteractionSoakPolicy.MaximumDispatcherGapMilliseconds ||
                !TryGetFiniteNumber(
                    root,
                    "MaximumProfilerGapMilliseconds",
                    out double profilerGap) ||
                profilerGap < 0 ||
                profilerGap >
                    EditorInteractionSoakPolicy.MaximumProfilerGapMilliseconds)
            {
                error = "heartbeat gap exceeded the schema policy";
                return false;
            }
            if (!root.TryGetProperty(
                    "RecoveryPromptRequired",
                    out JsonElement recoveryRequiredValue) ||
                recoveryRequiredValue.ValueKind is not
                    (JsonValueKind.True or JsonValueKind.False) ||
                recoveryRequiredValue.GetBoolean() != recoveryRequired ||
                !root.TryGetProperty(
                    "RecoveryPromptObserved",
                    out JsonElement recoveryObservedValue) ||
                recoveryObservedValue.ValueKind is not
                    (JsonValueKind.True or JsonValueKind.False) ||
                (recoveryRequired && !recoveryObservedValue.GetBoolean()))
            {
                error = "recovery prompt evidence does not match runner mode";
                return false;
            }
            if (!root.TryGetProperty("FaultCodes", out JsonElement faults) ||
                faults.ValueKind != JsonValueKind.Array ||
                faults.GetArrayLength() != 0)
            {
                error = "PASS report contains faults or omits FaultCodes";
                return false;
            }
            if (!root.TryGetProperty("StartupState", out JsonElement startup) ||
                !string.Equals(
                    startup.GetString(),
                    EditorEngineStartupState.Ready.ToString(),
                    StringComparison.Ordinal))
            {
                error = "engine startup did not reach Ready";
                return false;
            }

            error = "";
            return true;
        }
        catch (Exception exception)
        {
            error = exception.Message;
            return false;
        }
    }

    private static bool TryGetInt32(
        JsonElement root,
        string property,
        out int value)
    {
        value = 0;
        return root.TryGetProperty(property, out JsonElement element) &&
               element.TryGetInt32(out value);
    }

    private static bool TryGetFiniteNumber(
        JsonElement root,
        string property,
        out double value)
    {
        value = 0;
        return root.TryGetProperty(property, out JsonElement element) &&
               element.TryGetDouble(out value) &&
               double.IsFinite(value);
    }

    private static async Task<bool> TryTerminateWithinAsync(
        Process process,
        TimeSpan grace)
    {
        try
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }
        catch
        {
        }

        using var timeout = new CancellationTokenSource(grace);
        try
        {
            await process.WaitForExitAsync(timeout.Token);
            return true;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
        catch
        {
            try { return process.HasExited; }
            catch { return false; }
        }
    }

    private static async Task<(bool Completed, string Stdout, string Stderr)>
        DrainOutputWithinAsync(
            Process process,
            Task<string> standardOutput,
            Task<string> standardError,
            TimeSpan grace)
    {
        try
        {
            string[] output = await Task.WhenAll(
                    standardOutput,
                    standardError)
                .WaitAsync(grace);
            return (true, output[0], output[1]);
        }
        catch (TimeoutException)
        {
            try { process.StandardOutput.Dispose(); }
            catch { }
            try { process.StandardError.Dispose(); }
            catch { }
            ObserveFault(standardOutput);
            ObserveFault(standardError);
            return (false, "", "");
        }
        catch (Exception exception)
        {
            return (false, "", "Output capture failed: " + exception.Message);
        }
    }

    private static void ObserveFault(Task task)
    {
        _ = task.ContinueWith(
            static completed => _ = completed.Exception,
            CancellationToken.None,
            TaskContinuationOptions.OnlyOnFaulted |
                TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private static void WriteFallbackReport(
        string path,
        string faultCode,
        string detail)
    {
        try
        {
            string? parent = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            File.WriteAllText(
                path,
                JsonSerializer.Serialize(
                    new
                    {
                        SchemaVersion =
                            EditorReliabilityReportContract.CurrentSchemaVersion,
                        Result = "FAIL",
                        CompletedUtc = DateTimeOffset.UtcNow,
                        FaultCodes = new[] { faultCode },
                        Detail = detail,
                    },
                    new JsonSerializerOptions { WriteIndented = true }),
                Utf8NoBom);
        }
        catch
        {
        }
    }

    private sealed class ReliabilityRecoveryFixture : IDisposable
    {
        private ReliabilityRecoveryFixture(string rootDirectory) =>
            RootDirectory = rootDirectory;

        internal string RootDirectory { get; }

        internal static ReliabilityRecoveryFixture Create(string projectFile)
        {
            Project project = ProjectManager.Open(projectFile);
            string scenePath = project.InitialScenePath;
            if (!File.Exists(scenePath))
            {
                throw new FileNotFoundException(
                    "The configured initial scene is required for the " +
                    "recovery-path soak fixture.",
                    scenePath);
            }

            string root = Path.Combine(
                Path.GetTempPath(),
                "acs-editor-reliability-recovery-" +
                Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                SceneDocumentMode mode = string.Equals(
                    Path.GetExtension(scenePath),
                    ".acs3d",
                    StringComparison.OrdinalIgnoreCase)
                    ? SceneDocumentMode.ThreeD
                    : SceneDocumentMode.TwoD;
                var store = new SceneAutosaveStore(root);
                SceneAutosaveIdentity identity =
                    SceneAutosaveStore.CreateIdentity(
                        project.ProjectFilePath,
                        scenePath,
                        mode);
                store.WriteSnapshot(new SceneAutosaveCapture(
                    identity,
                    File.ReadAllText(scenePath, Encoding.UTF8),
                    DateTimeOffset.UtcNow));
                return new ReliabilityRecoveryFixture(root);
            }
            catch
            {
                TryDeleteDirectory(root);
                throw;
            }
        }

        public void Dispose() => TryDeleteDirectory(RootDirectory);

        private static void TryDeleteDirectory(string path)
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
    }
}
