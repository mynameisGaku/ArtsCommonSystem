// SPDX-License-Identifier: Apache-2.0

using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using AcsEditor.Packaging;

namespace AcsEditor;

internal static class PackageResponsivenessSelfTest
{
    private static int _failures;
    private static TextWriter _output = TextWriter.Null;

    internal static int Run(TextWriter output)
    {
        _failures = 0;
        _output = output;
        try
        {
            // OnStartup owns a WPF synchronization context even though this
            // switch creates no window. Run the async harness on a worker so a
            // synchronous CLI exit code cannot deadlock its continuations.
            Task.Run(RunAsync).GetAwaiter().GetResult();
        }
        catch (Exception error)
        {
            Fail("unhandled package responsiveness self-test error: " + error);
        }
        return _failures;
    }

    internal static int RunOutputWorker()
    {
        // Deliberately emit more than the capture limit without a newline. Line-oriented
        // Process APIs buffer this whole payload internally before raising one callback, so this
        // fixture protects the fixed-size BaseStream reader rather than only ordinary log lines.
        using Stream output = Console.OpenStandardOutput();
        byte[] payload = Encoding.ASCII.GetBytes(new string('x', 32 * 1024));
        int chunks = PackageProcessRunner.CaptureLimitBytesPerStream /
                     payload.Length + 64;
        for (int index = 0; index < chunks; index++)
            output.Write(payload);
        output.Flush();
        Thread.Sleep(TimeSpan.FromMilliseconds(500));
        return 0;
    }

    internal static int RunWaitWorker()
    {
        Thread.Sleep(TimeSpan.FromSeconds(30));
        return 0;
    }

    private static async Task RunAsync()
    {
        await VerifyLatestOnlyValidationAsync();
        await VerifyConfigSnapshotAsync();
        VerifyPrefabCookRewrite();
        await VerifyProcessOutputBoundAsync();
        await VerifyProcessCancellationAsync();
        await VerifyPriorGameProcessDrainAsync();
        await VerifyOwnerCloseDrainAsync();
        await VerifyCleanupRetryAsync();
        VerifyValidateCancellation();
    }

    private static void VerifyPrefabCookRewrite()
    {
        string root = FixtureRoot("prefab-cook");
        string assets = Path.Combine(root, "Assets");
        string mesh = Path.Combine(assets, "Models", "aircraft.glb");
        string sprite = Path.Combine(assets, "Textures", "cloud.png");
        string material = Path.Combine(assets, "Materials", "cloud.acsmat");
        string child = Path.Combine(assets, "Prefabs", "engine.acsprefab");
        Directory.CreateDirectory(Path.GetDirectoryName(mesh)!);
        Directory.CreateDirectory(Path.GetDirectoryName(sprite)!);
        Directory.CreateDirectory(Path.GetDirectoryName(material)!);
        Directory.CreateDirectory(Path.GetDirectoryName(child)!);
        File.WriteAllBytes(mesh, [0x67, 0x6c, 0x54, 0x46]);
        File.WriteAllBytes(sprite, [1, 2, 3, 4]);
        File.WriteAllText(material, "ACSMAT 1\n", new UTF8Encoding(false));
        File.WriteAllText(child, "ACS3D v2\n", new UTF8Encoding(false));

        try
        {
            string source3D =
                "ACS3D v2\n" +
                $"MSH3D 1 {mesh}\n" +
                $"SPR3D 2 {sprite}\n" +
                $"MAT3D 3 {material}\n" +
                $"PFAB3D 4 {child}\n" +
                "MAT3D 5 0.250 0.750\n";
            byte[] cooked3D = PackageCore.RewritePrefabPayloadForSelfTest(
                new UTF8Encoding(false).GetBytes(source3D),
                assets,
                root);
            string rewritten3D = Encoding.UTF8.GetString(cooked3D);
            string portableRoot = root.Replace('\\', '/');
            Check(
                rewritten3D ==
                    "ACS3D v2\n" +
                    "MSH3D 1 Assets/Models/aircraft.glb\n" +
                    "SPR3D 2 Assets/Textures/cloud.png\n" +
                    "MAT3D 3 Assets/Materials/cloud.acsmat\n" +
                    "PFAB3D 4 Assets/Prefabs/engine.acsprefab\n" +
                    "MAT3D 5 0.250 0.750\n" &&
                !rewritten3D.Contains(root, StringComparison.OrdinalIgnoreCase) &&
                !rewritten3D.Contains(
                    portableRoot,
                    StringComparison.OrdinalIgnoreCase),
                "ACS3D prefab Cook rewrites every 3D asset directive and leaks no local absolute path");

            string source2D =
                "ACSCENE v1\n" +
                $"SPRT 1 {sprite}\n" +
                $"MAT 1 {material}\n";
            string rewritten2D = Encoding.UTF8.GetString(
                PackageCore.RewritePrefabPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(source2D),
                    assets,
                    root));
            Check(
                rewritten2D ==
                    "ACSCENE v1\n" +
                    "SPRT 1 Assets/Textures/cloud.png\n" +
                    "MAT 1 Assets/Materials/cloud.acsmat\n",
                "legacy ACSCENE prefab Cook keeps the 2D reference grammar");

            CheckThrows<InvalidDataException>(
                () => PackageCore.RewritePrefabPayloadForSelfTest(
                    new UTF8Encoding(false).GetBytes(
                        "ACSPREFAB 1\nMSH3D 1 " + mesh + "\n"),
                    assets,
                    root),
                "unknown prefab payload headers fail closed");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static async Task VerifyLatestOnlyValidationAsync()
    {
        using var coordinator = new PackageValidationCoordinator();
        using PackageValidationOperation first = coordinator.BeginLatest();
        var firstStarted = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        int active = 0;
        int maximumActive = 0;
        Task<int> firstTask = coordinator.RunAsync<int>(
            first,
            token =>
            {
                int current = Interlocked.Increment(ref active);
                UpdateMaximum(ref maximumActive, current);
                firstStarted.TrySetResult(true);
                try
                {
                    while (true)
                    {
                        token.ThrowIfCancellationRequested();
                        Thread.Sleep(1);
                    }
                }
                finally
                {
                    Interlocked.Decrement(ref active);
                }
            });
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));

        using PackageValidationOperation second = coordinator.BeginLatest();
        Task<int> secondTask = coordinator.RunAsync(
            second,
            token =>
            {
                int current = Interlocked.Increment(ref active);
                UpdateMaximum(ref maximumActive, current);
                try
                {
                    token.ThrowIfCancellationRequested();
                    return 42;
                }
                finally
                {
                    Interlocked.Decrement(ref active);
                }
            });

        bool firstCancelled = false;
        try
        {
            _ = await firstTask;
        }
        catch (OperationCanceledException)
        {
            firstCancelled = true;
        }
        Check(firstCancelled, "latest validation cancels prior work");
        Check(await secondTask == 42, "latest validation completes");
        Check(maximumActive == 1, "validation filesystem work is serialized");
        Check(coordinator.IsCurrent(second), "latest validation identity is current");
    }

    private static async Task VerifyConfigSnapshotAsync()
    {
        string root = FixtureRoot("config");
        string config = Path.Combine(root, "Config");
        string staged = Path.Combine(root, "Stage", "Config");
        Directory.CreateDirectory(Path.Combine(root, "Assets"));
        Directory.CreateDirectory(config);
        string settings = Path.Combine(config, "ProjectSettings.ini");
        File.WriteAllText(
            settings,
            "[Game]\nDefaultScene=Assets/main.acscene\n",
            new UTF8Encoding(false));
        File.WriteAllText(
            Path.Combine(config, "Input.ini"),
            "[Input]\nJump=Space\n",
            new UTF8Encoding(false));
        string projectFile = Path.Combine(root, "Game.acsproject");
        File.WriteAllText(projectFile, "{}", new UTF8Encoding(false));
        var project = new PackageProjectInfo(
            "Game",
            1,
            "test",
            projectFile,
            "Assets/main.acscene");

        try
        {
            PackageCore.PackageDirectorySnapshot snapshot =
                await PackageCore.StageDirectorySnapshotAsync(
                    config,
                    staged,
                    CancellationToken.None);
            Check(snapshot.Existed && snapshot.Files.Count == 2,
                "Config snapshot captures deterministic file set");
            PackageCore.ValidateDirectorySnapshot(config, snapshot);
            Check(
                File.ReadAllText(Path.Combine(staged, "ProjectSettings.ini"))
                    .Contains("Assets/main.acscene", StringComparison.Ordinal),
                "Config snapshot stages captured bytes");
            PackageCore.ValidateStagedConfiguration(staged, project);
            Pass("staged Config DefaultScene matches cooked scene");

            File.AppendAllText(settings, "; changed\n", new UTF8Encoding(false));
            CheckThrows<InvalidDataException>(
                () => PackageCore.ValidateDirectorySnapshot(config, snapshot),
                "Config mutation is rejected before publish");

            File.WriteAllText(
                Path.Combine(staged, "ProjectSettings.ini"),
                "[Game]\nDefaultScene=Assets/other.acscene\n",
                new UTF8Encoding(false));
            CheckThrows<PackageValidationException>(
                () => PackageCore.ValidateStagedConfiguration(staged, project),
                "staged DefaultScene drift is rejected");
        }
        finally
        {
            TryDeleteFixture(root);
        }
    }

    private static async Task VerifyProcessOutputBoundAsync()
    {
        ProcessStartInfo start = WorkerStart("--package-process-output-worker");
        int callbacks = 0;
        var elapsed = Stopwatch.StartNew();
        long firstCallbackTicks = long.MaxValue;
        PackageProcessResult result = await PackageProcessRunner.RunAsync(
            start,
            _ =>
            {
                Interlocked.CompareExchange(
                    ref firstCallbackTicks,
                    elapsed.ElapsedTicks,
                    long.MaxValue);
                Interlocked.Increment(ref callbacks);
            },
            CancellationToken.None);
        elapsed.Stop();
        int capturedBytes = Encoding.UTF8.GetByteCount(result.StandardOutput);
        Check(result.ExitCode == 0, "bounded-output child exits successfully");
        Check(
            capturedBytes <=
                PackageProcessRunner.CaptureLimitBytesPerStream + 128,
            "child stdout capture is bounded");
        Check(
            result.StandardOutput.Contains(
                "output truncated",
                StringComparison.Ordinal),
            "bounded stdout records truncation");
        Check(
            firstCallbackTicks != long.MaxValue &&
            Stopwatch.GetElapsedTime(0, firstCallbackTicks) <
                elapsed.Elapsed - TimeSpan.FromMilliseconds(200),
            "unterminated child output is logged before process exit");
        int callbackBudget =
            (int)Math.Ceiling(elapsed.Elapsed.TotalMilliseconds / 90.0) + 20;
        Check(callbacks <= callbackBudget,
            "large child output is batched before UI log callbacks");
    }

    private static async Task VerifyProcessCancellationAsync()
    {
        using var cancellation = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(150));
        var elapsed = Stopwatch.StartNew();
        bool cancelled = false;
        try
        {
            _ = await PackageProcessRunner.RunAsync(
                WorkerStart("--package-process-wait-worker"),
                _ => { },
                cancellation.Token);
        }
        catch (OperationCanceledException)
        {
            cancelled = true;
        }
        Check(cancelled, "package child cancellation propagates");
        Check(
            elapsed.Elapsed < TimeSpan.FromSeconds(8),
            "package child cancellation is bounded");
    }

    private static async Task VerifyOwnerCloseDrainAsync()
    {
        var coordinator = new PackageShutdownCoordinator();
        var release = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        int starts = 0;
        Task<bool> first = coordinator.RunOnceAsync(async () =>
        {
            Interlocked.Increment(ref starts);
            await release.Task;
            return true;
        });
        Task<bool> reentrant = coordinator.RunOnceAsync(() =>
        {
            Interlocked.Increment(ref starts);
            return Task.FromResult(false);
        });
        Check(
            ReferenceEquals(first, reentrant) && starts == 1,
            "reentrant editor-close requests share one build/package shutdown");
        release.TrySetResult(true);
        Check(
            await first && await reentrant,
            "coalesced package shutdown publishes one drain result");

        using var cancellation = new CancellationTokenSource();
        var started = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Task cancellable = Task.Run(async () =>
        {
            started.TrySetResult(true);
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellation.Token);
        });
        await started.Task.WaitAsync(TimeSpan.FromSeconds(3));
        bool drained =
            await PackageShutdownCoordinator.CancelAndDrainAsync(
                cancellable,
                cancellation.Cancel,
                TimeSpan.FromSeconds(3));
        Check(
            drained &&
            cancellation.IsCancellationRequested &&
            cancellable.IsCompleted,
            "owner close cancels and drains active build/package work");

        var blocked = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        int cancellationRequests = 0;
        var elapsed = Stopwatch.StartNew();
        bool bounded =
            await PackageShutdownCoordinator.CancelAndDrainAsync(
                blocked.Task,
                () => Interlocked.Increment(ref cancellationRequests),
                TimeSpan.FromMilliseconds(100));
        elapsed.Stop();
        Check(
            !bounded &&
            cancellationRequests == 1 &&
            elapsed.Elapsed < TimeSpan.FromSeconds(3),
            "non-cooperative build/package shutdown defers close at a bounded deadline");
        blocked.TrySetResult(true);
    }

    private static async Task VerifyPriorGameProcessDrainAsync()
    {
        using Process process =
            Process.Start(WorkerStart("--package-process-wait-worker"))
            ?? throw new InvalidOperationException(
                "Could not start prior-game-process fixture.");
        var elapsed = Stopwatch.StartNew();
        await MainWindow.StopGameProcessForReplacementAsync(
            process,
            CancellationToken.None);
        elapsed.Stop();
        Check(
            process.HasExited &&
            elapsed.Elapsed < TimeSpan.FromSeconds(5),
            "replacing a prior game process drains it asynchronously within a bound");
    }

    private static async Task VerifyCleanupRetryAsync()
    {
        string root = FixtureRoot("cleanup");
        string staging = Path.Combine(root, "PackageStaging");
        string transaction = Path.Combine(staging, "transaction");
        Directory.CreateDirectory(transaction);
        string heldPath = Path.Combine(transaction, "held.bin");
        File.WriteAllText(heldPath, "held", new UTF8Encoding(false));
        FileStream held = new(
            heldPath,
            FileMode.Open,
            FileAccess.ReadWrite,
            FileShare.None);
        Task release = Task.Run(async () =>
        {
            await Task.Delay(90);
            held.Dispose();
        });
        try
        {
            bool deleted = await PackageCore.TryDeleteDirectoryWithRetryAsync(
                transaction,
                staging);
            await release;
            Check(deleted && !Directory.Exists(transaction),
                "staging cleanup retries transient file locks");
        }
        finally
        {
            held.Dispose();
            TryDeleteFixture(root);
        }
    }

    private static void VerifyValidateCancellation()
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var project = new PackageProjectInfo(
            "Cancelled",
            1,
            "test",
            Path.Combine(FixtureRoot("cancelled"), "Cancelled.acsproject"),
            "Assets/main.acscene");
        var options = new PackageOptions(Path.GetTempPath());
        CheckThrows<OperationCanceledException>(
            () => PackageCore.Validate(
                project,
                options,
                "Cancelled.exe",
                cancellationToken: cancellation.Token),
            "PackageCore validation observes cancellation");
    }

    private static ProcessStartInfo WorkerStart(string argument)
    {
        string assembly = Assembly.GetExecutingAssembly().Location;
        var start = new ProcessStartInfo
        {
            FileName = "dotnet",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        start.ArgumentList.Add(assembly);
        start.ArgumentList.Add(argument);
        return start;
    }

    private static string FixtureRoot(string suffix) =>
        Path.Combine(
            Path.GetTempPath(),
            "acs-package-responsiveness-selftest",
            suffix + "-" + Guid.NewGuid().ToString("N"));

    private static void TryDeleteFixture(string path)
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

    private static void UpdateMaximum(ref int target, int value)
    {
        while (true)
        {
            int current = Volatile.Read(ref target);
            if (value <= current ||
                Interlocked.CompareExchange(ref target, value, current) == current)
            {
                return;
            }
        }
    }

    private static void CheckThrows<TException>(
        Action action,
        string label)
        where TException : Exception
    {
        try
        {
            action();
            Fail(label + " (no exception)");
        }
        catch (TException)
        {
            Pass(label);
        }
        catch (Exception error)
        {
            Fail(label + $" (wrong exception: {error.GetType().Name})");
        }
    }

    private static void Check(bool condition, string label)
    {
        if (condition)
            Pass(label);
        else
            Fail(label);
    }

    private static void Pass(string label) =>
        _output.WriteLine("PASS: " + label);

    private static void Fail(string label)
    {
        _failures++;
        _output.WriteLine("FAIL: " + label);
    }
}
