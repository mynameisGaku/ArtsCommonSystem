// SPDX-License-Identifier: Apache-2.0
// Verified, bounded, non-interactive launch smoke for a completed ACS package.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor.Packaging;

public sealed record PackageLaunchSmokeOptions(
    TimeSpan Timeout,
    long MaximumExtractedBytes)
{
    public static PackageLaunchSmokeOptions Default { get; } = new(
        TimeSpan.FromSeconds(45),
        16L * 1024 * 1024 * 1024);
}

public sealed record PackageLaunchSmokeDiagnostic(
    string Severity,
    string Code,
    string Message);

public sealed record PackageLaunchSmokeLimits(
    int TimeoutMilliseconds,
    long MaximumExtractedBytes,
    int CaptureLimitBytesPerStream);

public sealed record PackageLaunchSmokeChecks(
    bool ArchiveCopiedToPrivateStaging,
    bool ArchiveVerified,
    bool PayloadExtracted,
    bool ExecutableStarted,
    bool HiddenWindowRequested,
    bool RuntimeReadyHandshakeObserved,
    bool CleanExitObserved,
    bool PrivateStagingRemoved);

/// <summary>
/// Machine-readable package publication and launch-smoke result. A successful
/// report intentionally contains no wall-clock timestamp, random nonce,
/// private TEMP path, or measured duration. Re-running the same package with
/// the same limits therefore produces byte-identical successful JSON.
/// </summary>
public sealed record PackageLaunchSmokeReport(
    int SchemaVersion,
    string ReportKind,
    bool Passed,
    string Status,
    string ArchiveFileName,
    string ArchiveSha256,
    string PackageId,
    string BuildId,
    string Profile,
    string Executable,
    int FileCount,
    long UncompressedBytes,
    string AssetPackSha256,
    int CookedAssetCount,
    string StartupContract,
    int? ExitCode,
    PackageLaunchSmokeLimits Limits,
    PackageLaunchSmokeChecks Checks,
    IReadOnlyList<PackageLaunchSmokeDiagnostic> Diagnostics,
    string StandardOutputExcerpt,
    string StandardErrorExcerpt);

public sealed record PackageLaunchSmokeResult(
    PackageLaunchSmokeReport Report,
    string ReportPath);

public static class PackageLaunchSmoke
{
    public const string StartupContract = "acs.package.startup.v1";
    public const int MinimumTimeoutSeconds = 1;
    public const int MaximumTimeoutSeconds = 300;
    public const long MinimumExtractedBytes = 16L * 1024 * 1024;
    public const long MaximumExtractedBytes = 128L * 1024 * 1024 * 1024;

    private const long MaximumArchiveBytes = 129L * 1024 * 1024 * 1024;
    private const int CopyBufferBytes = 1024 * 1024;
    private const int DiagnosticExcerptCharacters = 4096;
    private const string ReportKind = "acs.package.launch-report";
    private const string SmokeTokenEnvironment = "ACS_PACKAGE_SMOKE_TOKEN";
    private const string ReadyPrefix = "ACS_PACKAGE_SMOKE_V1 READY ";
    private static readonly JsonSerializerOptions ReportJsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    private sealed class ReadyHandshakeObserver : IPackageProcessObserver
    {
        private readonly byte[] _expected;
        private readonly byte[] _line;
        private int _lineLength;
        private bool _lineOverflowed;

        internal ReadyHandshakeObserver(string token)
        {
            _expected = Encoding.ASCII.GetBytes(ReadyPrefix + token);
            _line = new byte[_expected.Length + 1];
        }

        internal bool ProcessStarted { get; private set; }
        internal int MatchCount { get; private set; }

        public void OnProcessStarted() => ProcessStarted = true;

        public void OnStandardOutput(ReadOnlySpan<byte> bytes)
        {
            foreach (byte value in bytes)
            {
                if (value == (byte)'\n')
                {
                    CompleteLine();
                    continue;
                }
                if (_lineLength < _line.Length)
                    _line[_lineLength++] = value;
                else
                    _lineOverflowed = true;
            }
        }

        public void OnStandardOutputCompleted()
        {
            if (_lineLength != 0 || _lineOverflowed)
                CompleteLine();
        }

        private void CompleteLine()
        {
            bool exact =
                !_lineOverflowed &&
                (_lineLength == _expected.Length ||
                 (_lineLength == _expected.Length + 1 &&
                  _line[_lineLength - 1] == (byte)'\r')) &&
                _line.AsSpan(0, _expected.Length)
                    .SequenceEqual(_expected);
            if (exact && MatchCount < 2)
                MatchCount++;
            _lineLength = 0;
            _lineOverflowed = false;
        }
    }

    public static string DefaultReportPath(string archivePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(archivePath);
        string archive = Path.GetFullPath(archivePath);
        return Path.Combine(
            Path.GetDirectoryName(archive) ??
                throw new IOException("Package archive parent directory is missing."),
            Path.GetFileNameWithoutExtension(archive) +
                ".package-report.json");
    }

    public static async Task<PackageLaunchSmokeResult> RunAsync(
        string archivePath,
        string? reportPath = null,
        PackageLaunchSmokeOptions? options = null,
        Action<string>? log = null,
        CancellationToken cancellationToken = default)
    {
        return await RunCoreAsync(
            archivePath,
            reportPath,
            options ?? PackageLaunchSmokeOptions.Default,
            log,
            Array.Empty<string>(),
            executableOverrideForSelfTest: null,
            cancellationToken).ConfigureAwait(false);
    }

    internal static Task<PackageLaunchSmokeResult> RunForSelfTestAsync(
        string archivePath,
        string reportPath,
        PackageLaunchSmokeOptions options,
        IReadOnlyList<string> childArguments,
        string? executableOverride,
        CancellationToken cancellationToken = default) =>
        RunCoreAsync(
            archivePath,
            reportPath,
            options,
            log: null,
            childArguments,
            executableOverride,
            cancellationToken);

    internal static async Task<bool> PinnedReadDeniesMutationForSelfTestAsync(
        string path,
        CancellationToken cancellationToken = default)
    {
        await using FileStream pin = OpenPinnedRead(Path.GetFullPath(path));
        _ = await HashPinnedStreamAsync(
            pin,
            cancellationToken).ConfigureAwait(false);
        try
        {
            await using var writer = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Write,
                FileShare.ReadWrite | FileShare.Delete);
            return false;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException)
        {
            return true;
        }
    }

    private static async Task<PackageLaunchSmokeResult> RunCoreAsync(
        string archivePath,
        string? reportPath,
        PackageLaunchSmokeOptions options,
        Action<string>? log,
        IReadOnlyList<string> childArguments,
        string? executableOverrideForSelfTest,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(archivePath);
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(childArguments);
        ValidateOptions(options);

        string archive = Path.GetFullPath(archivePath);
        string report = Path.GetFullPath(
            reportPath ?? DefaultReportPath(archive));
        if (PathsEqual(archive, report))
        {
            throw new ArgumentException(
                "Package launch report must not alias the package archive.",
                nameof(reportPath));
        }
        PrepareReportDestination(report, archive);

        var diagnostics = new List<PackageLaunchSmokeDiagnostic>();
        PackageVerificationResult? verification = null;
        string archiveHash = "";
        string stdout = "";
        string stderr = "";
        int? exitCode = null;
        bool copied = false;
        bool verified = false;
        bool extracted = false;
        bool started = false;
        bool hiddenRequested = false;
        bool ready = false;
        bool cleanExit = false;
        bool cleaned = false;
        string status = "failed";
        string temporaryParent = Path.GetFullPath(
            Path.Combine(Path.GetTempPath(), "acs-package-smoke"));
        string temporaryRoot = "";
        string smokeToken = "";
        bool externalCancellation = false;
        ReadyHandshakeObserver? processObserver = null;

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            ValidateSourceArchive(archive);
            RejectExistingReparsePoints(temporaryParent, allowMissingLeaf: true);
            Directory.CreateDirectory(temporaryParent);
            RejectExistingReparsePoints(temporaryParent, allowMissingLeaf: false);

            temporaryRoot = CreatePrivateTemporaryDirectory(temporaryParent);
            string privateArchive = Path.Combine(
                temporaryRoot,
                "package.zip");
            log?.Invoke("Copying package into private launch staging...");
            archiveHash = await CopyArchiveAndHashAsync(
                archive,
                privateArchive,
                cancellationToken).ConfigureAwait(false);
            copied = true;

            string launchRoot;
            await using (FileStream privateArchivePin = OpenPinnedRead(
                             privateArchive))
            {
                string pinnedArchiveHash = await HashPinnedStreamAsync(
                    privateArchivePin,
                    cancellationToken).ConfigureAwait(false);
                if (!string.Equals(
                        pinnedArchiveHash,
                        archiveHash,
                        StringComparison.Ordinal))
                {
                    throw new IOException(
                        "Private package copy changed before verification.");
                }

                log?.Invoke("Verifying private package copy before extraction...");
                verification = await PackageCore.VerifyPackageArchiveAsync(
                    privateArchive,
                    progress: null,
                    cancellationToken).ConfigureAwait(false);
                verified = true;
                if (verification.UncompressedBytes >
                    options.MaximumExtractedBytes)
                {
                    throw new InvalidDataException(
                        "Verified package payload exceeds the configured launch-smoke " +
                        "extraction limit.");
                }

                launchRoot = Path.Combine(
                    temporaryRoot,
                    "launch",
                    verification.PackageId);
                Directory.CreateDirectory(launchRoot);
                RejectExistingReparsePoints(launchRoot, allowMissingLeaf: false);
                log?.Invoke("Extracting verified payload into private staging...");
                await ExtractVerifiedArchiveAsync(
                    privateArchive,
                    launchRoot,
                    verification,
                    options.MaximumExtractedBytes,
                    cancellationToken).ConfigureAwait(false);
                extracted = true;
            }

            string executable = executableOverrideForSelfTest is null
                ? ResolveExtractedExecutable(
                    launchRoot,
                    verification.Executable)
                : ValidateSelfTestExecutableOverride(
                    executableOverrideForSelfTest);
            await using FileStream executablePin = OpenPinnedRead(executable);
            string executableHash = await HashPinnedStreamAsync(
                executablePin,
                cancellationToken).ConfigureAwait(false);
            string expectedExecutableHash =
                executableOverrideForSelfTest is null
                    ? verification.ExecutableSha256
                    : executableHash;
            if (!string.Equals(
                    executableHash,
                    expectedExecutableHash,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "Extracted executable no longer matches the verified manifest.");
            }
            executablePin.Position = 0;
            _ = PackageExecutableContract.Inspect(
                executablePin,
                executablePin.Length);

            smokeToken = Convert.ToHexString(
                RandomNumberGenerator.GetBytes(32)).ToLowerInvariant();
            var start = new ProcessStartInfo
            {
                FileName = executable,
                WorkingDirectory = launchRoot,
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                ErrorDialog = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            ConfigureSanitizedEnvironment(
                start,
                temporaryRoot,
                launchRoot,
                smokeToken);
            foreach (string argument in childArguments)
                start.ArgumentList.Add(argument);
            hiddenRequested = true;

            log?.Invoke(
                $"Launching verified package hidden with a " +
                $"{(int)options.Timeout.TotalSeconds}s deadline...");
            using var deadline = new CancellationTokenSource(options.Timeout);
            using var linkedCancellation =
                CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken,
                    deadline.Token);
            processObserver = new ReadyHandshakeObserver(smokeToken);
            PackageProcessResult process;
            try
            {
                process = await PackageProcessRunner.RunAsync(
                    start,
                    log,
                    linkedCancellation.Token,
                    processObserver,
                    containProcessTree: true).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (
                deadline.IsCancellationRequested &&
                !cancellationToken.IsCancellationRequested)
            {
                status = "timedOut";
                diagnostics.Add(new(
                    "error",
                    "LAUNCH_TIMEOUT",
                    "The packaged runtime did not complete its bounded startup " +
                    "contract before the configured deadline."));
                process = new(-1, "", "");
            }
            catch (OperationCanceledException)
            {
                externalCancellation = true;
                status = "cancelled";
                diagnostics.Add(new(
                    "warning",
                    "LAUNCH_CANCELLED",
                    "Package launch smoke was cancelled and its process tree was terminated."));
                process = new(-1, "", "");
            }

            stdout = process.StandardOutput;
            stderr = process.StandardError;
            started = processObserver.ProcessStarted;
            exitCode = process.ExitCode >= 0 ? process.ExitCode : null;
            ready = processObserver.MatchCount == 1;
            cleanExit = process.ExitCode == 0;

            if (status is not ("timedOut" or "cancelled"))
            {
                if (processObserver.MatchCount > 1)
                {
                    status = "startupFailed";
                    diagnostics.Add(new(
                        "error",
                        "RUNTIME_READY_DUPLICATE",
                        "The packaged runtime published the authenticated " +
                        "startup-ready handshake more than once."));
                }
                else if (!ready)
                {
                    status = "startupFailed";
                    diagnostics.Add(new(
                        "error",
                        "RUNTIME_READY_MISSING",
                        "The packaged runtime did not publish the authenticated " +
                        "startup-ready handshake after its first successful frame."));
                }
                if (!cleanExit)
                {
                    status = "startupFailed";
                    diagnostics.Add(new(
                        "error",
                        "RUNTIME_EXIT_NONZERO",
                        $"The packaged runtime exited with code {process.ExitCode}."));
                }
                if (ready && cleanExit)
                {
                    status = "passed";
                    diagnostics.Add(new(
                        "info",
                        "PACKAGE_LAUNCH_SMOKE_PASSED",
                        "Archive verification, private extraction, renderer/scene " +
                        "startup, first-frame presentation, and clean exit passed."));
                }
            }
        }
        catch (OperationCanceledException)
        {
            externalCancellation = true;
            status = "cancelled";
            diagnostics.Add(new(
                "warning",
                "LAUNCH_CANCELLED",
                "Package launch smoke was cancelled before completion."));
        }
        catch (Exception error)
        {
            status = verified ? "launchFailed" : "verificationFailed";
            diagnostics.Add(new(
                "error",
                ErrorCode(error, verified, extracted),
                BoundedMessage(
                    SanitizeSensitiveText(
                        error.Message,
                        temporaryRoot,
                        smokeToken,
                        archive,
                        report))));
        }
        finally
        {
            if (processObserver != null)
            {
                started = processObserver.ProcessStarted;
                ready = processObserver.MatchCount == 1;
            }
            if (!string.IsNullOrEmpty(temporaryRoot))
            {
                cleaned = await PackageCore.TryDeleteDirectoryWithRetryAsync(
                    temporaryRoot,
                    temporaryParent).ConfigureAwait(false);
                if (!cleaned)
                {
                    bool invalidatesSuccess = status == "passed";
                    if (invalidatesSuccess)
                        status = "cleanupFailed";
                    diagnostics.Add(new(
                        invalidatesSuccess ? "error" : "warning",
                        "PRIVATE_STAGING_CLEANUP_FAILED",
                        "Private launch staging could not be removed after bounded retries."));
                }
            }
        }

        bool passed =
            status == "passed" && copied && verified && extracted &&
            started && hiddenRequested && ready && cleanExit && cleaned;
        PackageLaunchSmokeReport launchReport = new(
            SchemaVersion: 1,
            ReportKind,
            Passed: passed,
            Status: status,
            ArchiveFileName: Path.GetFileName(archive),
            ArchiveSha256: archiveHash,
            PackageId: verification?.PackageId ?? "",
            BuildId: verification?.BuildId ?? "",
            Profile: verification?.Profile.ToString() ?? "",
            Executable: verification?.Executable ?? "",
            FileCount: verification?.FileCount ?? 0,
            UncompressedBytes: verification?.UncompressedBytes ?? 0,
            AssetPackSha256: verification?.AssetPackSha256 ?? "",
            CookedAssetCount: verification?.CookedAssetCount ?? 0,
            StartupContract,
            ExitCode: exitCode,
            Limits: new(
                checked((int)options.Timeout.TotalMilliseconds),
                options.MaximumExtractedBytes,
                PackageProcessRunner.CaptureLimitBytesPerStream),
            Checks: new(
                copied,
                verified,
                extracted,
                started,
                hiddenRequested,
                ready,
                cleanExit,
                cleaned),
            Diagnostics: diagnostics
                .OrderBy(item => item.Code, StringComparer.Ordinal)
                .ThenBy(item => item.Message, StringComparer.Ordinal)
                .ToArray(),
            StandardOutputExcerpt: passed
                ? ""
                : SanitizeExcerpt(
                    stdout,
                    temporaryRoot,
                    smokeToken,
                    archive,
                    report),
            StandardErrorExcerpt: passed
                ? ""
                : SanitizeExcerpt(
                    stderr,
                    temporaryRoot,
                    smokeToken,
                    archive,
                    report));

        await WriteReportAtomicAsync(
            report,
            archive,
            launchReport,
            CancellationToken.None).ConfigureAwait(false);

        if (externalCancellation)
            throw new OperationCanceledException(cancellationToken);
        return new(launchReport, report);
    }

    private static void ValidateOptions(PackageLaunchSmokeOptions options)
    {
        if (options.Timeout < TimeSpan.FromSeconds(MinimumTimeoutSeconds) ||
            options.Timeout > TimeSpan.FromSeconds(MaximumTimeoutSeconds))
        {
            throw new ArgumentOutOfRangeException(
                nameof(options),
                $"Launch timeout must be between {MinimumTimeoutSeconds} and " +
                $"{MaximumTimeoutSeconds} seconds.");
        }
        if (options.MaximumExtractedBytes < MinimumExtractedBytes ||
            options.MaximumExtractedBytes > MaximumExtractedBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(options),
                $"Extraction limit must be between {MinimumExtractedBytes} and " +
                $"{MaximumExtractedBytes} bytes.");
        }
    }

    private static void ValidateSourceArchive(string archive)
    {
        RejectExistingReparsePoints(archive, allowMissingLeaf: false);
        if (!File.Exists(archive))
            throw new FileNotFoundException("Package archive was not found.", archive);
        FileInfo info = new(archive);
        if (info.Length is <= 0 or > MaximumArchiveBytes)
        {
            throw new InvalidDataException(
                "Package archive exceeds the supported launch-smoke size limit.");
        }
    }

    private static FileStream OpenPinnedRead(string path) =>
        new(
            path,
            FileMode.Open,
            FileAccess.Read,
            // Existing readers (ZIP verification, extraction, and
            // CreateProcess image mapping) remain possible. Write and delete
            // sharing stay denied until the guarded operation completes.
            FileShare.Read,
            CopyBufferBytes,
            FileOptions.Asynchronous | FileOptions.SequentialScan);

    private static async Task<string> HashPinnedStreamAsync(
        FileStream stream,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanSeek || stream.Position != 0)
            throw new IOException("Pinned package stream is not at its first byte.");
        byte[] digest = await SHA256.HashDataAsync(
            stream,
            cancellationToken).ConfigureAwait(false);
        if (stream.Position != stream.Length)
            throw new IOException("Pinned package stream hash did not consume the file.");
        stream.Position = 0;
        return Convert.ToHexString(digest).ToLowerInvariant();
    }

    private static void ConfigureSanitizedEnvironment(
        ProcessStartInfo start,
        string temporaryRoot,
        string launchRoot,
        string smokeToken)
    {
        ArgumentNullException.ThrowIfNull(start);
        string runtimeRoot = Path.Combine(temporaryRoot, "runtime-environment");
        string runtimeTemp = Path.Combine(runtimeRoot, "Temp");
        string runtimeLocal = Path.Combine(runtimeRoot, "LocalAppData");
        string runtimeRoaming = Path.Combine(runtimeRoot, "AppData");
        Directory.CreateDirectory(runtimeTemp);
        Directory.CreateDirectory(runtimeLocal);
        Directory.CreateDirectory(runtimeRoaming);
        RejectExistingReparsePoints(runtimeRoot, allowMissingLeaf: false);

        // ProcessStartInfo otherwise inherits every parent variable, including
        // CI credentials, cloud tokens, signing secrets, and developer tool
        // authentication. A packaged project is executable code, so start
        // from an empty environment and add only runtime essentials.
        start.Environment.Clear();
        string systemDirectory = Path.GetFullPath(
            Environment.SystemDirectory);
        string systemRoot = Directory.GetParent(systemDirectory)?.FullName ??
            throw new InvalidOperationException(
                "Windows system root is unavailable for sanitized launch.");
        if (!Directory.Exists(systemDirectory) ||
            !Directory.Exists(systemRoot))
        {
            throw new InvalidOperationException(
                "Windows runtime directories are unavailable for sanitized launch.");
        }

        // Derive operating-system locations from the runtime/Win32 API rather
        // than forwarding caller-controlled SystemRoot, WINDIR, or PATH.
        start.Environment["SystemRoot"] = systemRoot;
        start.Environment["WINDIR"] = systemRoot;
        start.Environment["SystemDrive"] =
            Path.GetPathRoot(systemRoot) ?? "";
        start.Environment["OS"] = "Windows_NT";
        start.Environment["PROCESSOR_ARCHITECTURE"] =
            Environment.Is64BitOperatingSystem ? "AMD64" : "x86";
        start.Environment["NUMBER_OF_PROCESSORS"] =
            Environment.ProcessorCount.ToString(
                System.Globalization.CultureInfo.InvariantCulture);
        start.Environment["PATH"] = systemDirectory;
        start.Environment["TEMP"] = runtimeTemp;
        start.Environment["TMP"] = runtimeTemp;
        start.Environment["LOCALAPPDATA"] = runtimeLocal;
        start.Environment["APPDATA"] = runtimeRoaming;
        start.Environment["USERPROFILE"] = runtimeRoot;
        start.Environment["HOME"] = runtimeRoot;
        start.Environment["DOTNET_CLI_HOME"] = runtimeRoot;
        start.Environment["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1";
        start.Environment["DOTNET_NOLOGO"] = "1";
        start.Environment["DOTNET_EnableDiagnostics"] = "0";
        start.Environment[SmokeTokenEnvironment] = smokeToken;

        // Keep the verified package root explicit for diagnostics without
        // inheriting a caller-controlled current-directory variable.
        start.Environment["ACS_PACKAGE_ROOT"] = launchRoot;
    }

    private static string CreatePrivateTemporaryDirectory(string parent)
    {
        for (int attempt = 0; attempt < 8; attempt++)
        {
            string candidate = Path.Combine(
                parent,
                Guid.NewGuid().ToString("N"));
            if (File.Exists(candidate) || Directory.Exists(candidate))
                continue;
            Directory.CreateDirectory(candidate);
            RejectExistingReparsePoints(candidate, allowMissingLeaf: false);
            return candidate;
        }
        throw new IOException(
            "Could not allocate a unique private package launch directory.");
    }

    private static async Task<string> CopyArchiveAndHashAsync(
        string sourcePath,
        string destinationPath,
        CancellationToken cancellationToken)
    {
        await using var source = new FileStream(
            sourcePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            CopyBufferBytes,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (source.Length is <= 0 or > MaximumArchiveBytes)
        {
            throw new InvalidDataException(
                "Package archive exceeds the supported launch-smoke size limit.");
        }
        await using var destination = new FileStream(
            destinationPath,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            CopyBufferBytes,
            FileOptions.Asynchronous |
                FileOptions.SequentialScan |
                FileOptions.WriteThrough);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buffer = new byte[CopyBufferBytes];
        long copied = 0;
        while (true)
        {
            int read = await source.ReadAsync(
                buffer,
                cancellationToken).ConfigureAwait(false);
            if (read == 0)
                break;
            copied = checked(copied + read);
            if (copied > MaximumArchiveBytes)
                throw new InvalidDataException("Package archive copy exceeded its limit.");
            hash.AppendData(buffer, 0, read);
            await destination.WriteAsync(
                buffer.AsMemory(0, read),
                cancellationToken).ConfigureAwait(false);
        }
        if (copied != source.Length)
            throw new IOException("Package archive changed while it was copied.");
        await destination.FlushAsync(cancellationToken).ConfigureAwait(false);
        destination.Flush(flushToDisk: true);
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    private static async Task ExtractVerifiedArchiveAsync(
        string archivePath,
        string launchRoot,
        PackageVerificationResult verification,
        long maximumExtractedBytes,
        CancellationToken cancellationToken)
    {
        await using var input = new FileStream(
            archivePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            CopyBufferBytes,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        using var archive = new ZipArchive(
            input,
            ZipArchiveMode.Read,
            leaveOpen: false,
            entryNameEncoding: Encoding.UTF8);
        string prefix = verification.PackageId + "/";
        long extractedBytes = 0;
        var destinations = new HashSet<string>(
            OperatingSystem.IsWindows()
                ? StringComparer.OrdinalIgnoreCase
                : StringComparer.Ordinal);

        foreach (ZipArchiveEntry entry in archive.Entries)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal) ||
                entry.Name.Length == 0)
            {
                throw new InvalidDataException(
                    "Verified archive contains an invalid extraction path.");
            }
            string relative = entry.FullName[prefix.Length..];
            ValidateRelativePayloadPath(relative);
            string destination = Path.GetFullPath(
                Path.Combine(
                    launchRoot,
                    relative.Replace('/', Path.DirectorySeparatorChar)));
            if (!IsWithin(launchRoot, destination) ||
                !destinations.Add(destination))
            {
                throw new InvalidDataException(
                    "Verified archive extraction path escaped or collided.");
            }
            extractedBytes = checked(extractedBytes + entry.Length);
            if (entry.Length < 0 ||
                extractedBytes > maximumExtractedBytes)
            {
                throw new InvalidDataException(
                    "Verified package extraction exceeded its configured limit.");
            }

            string parent = Path.GetDirectoryName(destination) ??
                throw new InvalidDataException(
                    "Package extraction destination has no parent.");
            Directory.CreateDirectory(parent);
            RejectExistingReparsePoints(parent, allowMissingLeaf: false);
            await using Stream source = entry.Open();
            await using var output = new FileStream(
                destination,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                CopyBufferBytes,
                FileOptions.Asynchronous |
                    FileOptions.SequentialScan |
                    FileOptions.WriteThrough);
            await CopyExactAsync(
                source,
                output,
                entry.Length,
                cancellationToken).ConfigureAwait(false);
            await output.FlushAsync(cancellationToken).ConfigureAwait(false);
            output.Flush(flushToDisk: true);
        }
    }

    private static async Task CopyExactAsync(
        Stream source,
        Stream destination,
        long expectedBytes,
        CancellationToken cancellationToken)
    {
        byte[] buffer = new byte[CopyBufferBytes];
        long written = 0;
        while (true)
        {
            int read = await source.ReadAsync(
                buffer,
                cancellationToken).ConfigureAwait(false);
            if (read == 0)
                break;
            written = checked(written + read);
            if (written > expectedBytes)
                throw new InvalidDataException("ZIP entry expanded past its declared size.");
            await destination.WriteAsync(
                buffer.AsMemory(0, read),
                cancellationToken).ConfigureAwait(false);
        }
        if (written != expectedBytes)
            throw new InvalidDataException("ZIP entry ended before its declared size.");
    }

    private static string ResolveExtractedExecutable(
        string launchRoot,
        string executable)
    {
        ValidateRelativePayloadPath(executable);
        if (executable.Contains('/'))
            throw new InvalidDataException("Package executable must be at the package root.");
        string path = Path.GetFullPath(Path.Combine(launchRoot, executable));
        if (!IsWithin(launchRoot, path) || !File.Exists(path))
            throw new FileNotFoundException("Extracted package executable is missing.", path);
        RejectExistingReparsePoints(path, allowMissingLeaf: false);
        return path;
    }

    private static string ValidateSelfTestExecutableOverride(string executable)
    {
        string path = Path.GetFullPath(executable);
        RejectExistingReparsePoints(path, allowMissingLeaf: false);
        if (!File.Exists(path))
            throw new FileNotFoundException("Smoke self-test executable is missing.", path);
        return path;
    }

    private static async Task WriteReportAtomicAsync(
        string reportPath,
        string archivePath,
        PackageLaunchSmokeReport report,
        CancellationToken cancellationToken)
    {
        string destination = Path.GetFullPath(reportPath);
        PrepareReportDestination(destination, archivePath);
        string parent = Path.GetDirectoryName(destination)!;

        string temporary = Path.Combine(
            parent,
            "." + Path.GetFileName(destination) + "." +
            Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            byte[] json = JsonSerializer.SerializeToUtf8Bytes(
                report,
                ReportJsonOptions);
            await using (var output = new FileStream(
                temporary,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                16 * 1024,
                FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await output.WriteAsync(json, cancellationToken)
                    .ConfigureAwait(false);
                await output.WriteAsync(
                    new byte[] { (byte)'\n' },
                    cancellationToken).ConfigureAwait(false);
                await output.FlushAsync(cancellationToken).ConfigureAwait(false);
                output.Flush(flushToDisk: true);
            }

            RejectExistingReparsePoints(parent, allowMissingLeaf: false);
            if (File.Exists(destination) &&
                (File.GetAttributes(destination) & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException(
                    "Package report destination became a reparse point.");
            }
            File.Move(temporary, destination, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temporary) &&
                    (File.GetAttributes(temporary) &
                     FileAttributes.ReparsePoint) == 0)
                {
                    File.Delete(temporary);
                }
            }
            catch
            {
            }
        }
    }

    private static void PrepareReportDestination(
        string reportPath,
        string archivePath)
    {
        string destination = Path.GetFullPath(reportPath);
        if (PathsEqual(destination, archivePath))
            throw new IOException("Package report must not overwrite the package archive.");
        string parent = Path.GetDirectoryName(destination) ??
            throw new IOException("Package report parent directory is missing.");
        RejectExistingReparsePoints(parent, allowMissingLeaf: true);
        Directory.CreateDirectory(parent);
        RejectExistingReparsePoints(parent, allowMissingLeaf: false);
        if (!File.Exists(destination))
            return;

        FileAttributes attributes = File.GetAttributes(destination);
        if ((attributes & (FileAttributes.Directory |
                           FileAttributes.ReparsePoint)) != 0)
        {
            throw new IOException(
                "Existing package report is not an ordinary file.");
        }
    }

    private static string ErrorCode(
        Exception error,
        bool archiveVerified,
        bool payloadExtracted) =>
        error switch
        {
            FileNotFoundException => "ARCHIVE_OR_EXECUTABLE_NOT_FOUND",
            UnauthorizedAccessException => "ACCESS_DENIED",
            InvalidDataException when !archiveVerified => "ARCHIVE_INVALID",
            InvalidDataException when !payloadExtracted => "EXTRACTION_REJECTED",
            InvalidDataException => "LAUNCH_CONTRACT_INVALID",
            IOException when !archiveVerified => "ARCHIVE_IO_ERROR",
            IOException when !payloadExtracted => "EXTRACTION_IO_ERROR",
            IOException => "LAUNCH_IO_ERROR",
            _ => "LAUNCH_FAILED",
        };

    private static string BoundedMessage(string message)
    {
        if (string.IsNullOrWhiteSpace(message))
            return "Package launch smoke failed without a diagnostic message.";
        string normalized = message
            .Replace('\r', ' ')
            .Replace('\n', ' ')
            .Trim();
        return normalized.Length <= DiagnosticExcerptCharacters
            ? normalized
            : normalized[..DiagnosticExcerptCharacters] + " [truncated]";
    }

    private static string SanitizeExcerpt(
        string text,
        string temporaryRoot,
        string smokeToken,
        string archivePath,
        string reportPath)
    {
        if (string.IsNullOrEmpty(text))
            return "";
        string sanitized = SanitizeSensitiveText(
            text,
            temporaryRoot,
            smokeToken,
            archivePath,
            reportPath);
        sanitized = new string(sanitized.Select(character =>
            character == '\r' || character == '\n' || character == '\t' ||
            character >= ' '
                ? character
                : '\uFFFD').ToArray());
        if (sanitized.Length > DiagnosticExcerptCharacters)
            sanitized = "[truncated]\n" + sanitized[^DiagnosticExcerptCharacters..];
        return sanitized;
    }

    private static string SanitizeSensitiveText(
        string text,
        string temporaryRoot,
        string smokeToken,
        string archivePath,
        string reportPath)
    {
        string sanitized = text;
        sanitized = ReplaceSensitiveValue(
            sanitized,
            temporaryRoot,
            "[private-staging]",
            pathValue: true);
        sanitized = ReplaceSensitiveValue(
            sanitized,
            archivePath,
            "[package-archive]",
            pathValue: true);
        sanitized = ReplaceSensitiveValue(
            sanitized,
            reportPath,
            "[package-report]",
            pathValue: true);
        sanitized = ReplaceSensitiveValue(
            sanitized,
            smokeToken,
            "[authenticated-nonce]",
            pathValue: false);
        return sanitized;
    }

    private static string ReplaceSensitiveValue(
        string text,
        string value,
        string replacement,
        bool pathValue)
    {
        if (string.IsNullOrEmpty(value))
            return text;
        StringComparison comparison = OperatingSystem.IsWindows() ||
                                      !pathValue
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal;
        string replaced = text.Replace(value, replacement, comparison);
        if (!pathValue)
            return replaced;

        string alternate = value.Replace('\\', '/');
        if (!string.Equals(alternate, value, StringComparison.Ordinal))
            replaced = replaced.Replace(alternate, replacement, comparison);
        return replaced;
    }

    private static void ValidateRelativePayloadPath(string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) ||
            Path.IsPathRooted(relative) ||
            relative.Contains('\\') ||
            relative.Contains(':') ||
            relative.Split('/').Any(segment =>
                segment.Length == 0 || segment is "." or ".."))
        {
            throw new InvalidDataException(
                "Package payload path is not a canonical relative path.");
        }
    }

    private static void RejectExistingReparsePoints(
        string path,
        bool allowMissingLeaf)
    {
        string full = Path.GetFullPath(path);
        string? cursor = full;
        bool leaf = true;
        while (!string.IsNullOrEmpty(cursor))
        {
            bool exists = File.Exists(cursor) || Directory.Exists(cursor);
            if (!exists && leaf && !allowMissingLeaf)
                throw new FileNotFoundException("Required path does not exist.", cursor);
            if (exists &&
                (File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException(
                    $"Package launch path traverses a reparse point: {cursor}");
            }
            string? parent = Path.GetDirectoryName(cursor);
            if (string.IsNullOrEmpty(parent) ||
                PathsEqual(parent, cursor))
            {
                break;
            }
            cursor = parent;
            leaf = false;
        }
    }

    private static bool IsWithin(string root, string candidate)
    {
        string fullRoot =
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string fullCandidate =
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(candidate));
        StringComparison comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal;
        return !string.Equals(fullRoot, fullCandidate, comparison) &&
            fullCandidate.StartsWith(
                fullRoot + Path.DirectorySeparatorChar,
                comparison);
    }

    private static bool PathsEqual(string left, string right) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
            OperatingSystem.IsWindows()
                ? StringComparison.OrdinalIgnoreCase
                : StringComparison.Ordinal);
}
