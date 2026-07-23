// SPDX-License-Identifier: Apache-2.0

using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Self-test helper that makes a different process own the real project mutation lock.
/// This deliberately does not call <see cref="AssetMutationLock"/> in-process: the tests need to
/// exercise Windows FileShare.None contention rather than same-thread lease reentrancy.
/// </summary>
internal sealed class AssetMutationLockProcessHolder : IDisposable
{
    private const string ReadyMarker = "ACS_ASSET_MUTATION_LOCK_READY";
    private static readonly TimeSpan StartupTimeout = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(5);

    private readonly Process _process;
    private bool _disposed;

    private AssetMutationLockProcessHolder(Process process)
    {
        _process = process;
    }

    internal static AssetMutationLockProcessHolder Start(string assetsRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(assetsRoot);
        string normalizedAssets = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(assetsRoot));
        string databaseRoot = Path.Combine(
            normalizedAssets,
            AssetDatabase.InternalDirectoryName);
        Directory.CreateDirectory(databaseRoot);
        string lockPath = Path.Combine(databaseRoot, "asset-mutation.lock");

        const string script = """
            $ErrorActionPreference = 'Stop'
            $stream = [System.IO.File]::Open(
                $env:ACS_ASSET_MUTATION_LOCK_PATH,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
            try {
                [Console]::Out.WriteLine('ACS_ASSET_MUTATION_LOCK_READY')
                [Console]::Out.Flush()
                [Console]::In.ReadLine() | Out-Null
            }
            finally {
                $stream.Dispose()
            }
            """;
        string encoded = Convert.ToBase64String(Encoding.Unicode.GetBytes(script));
        string executable = Path.Combine(
            Environment.SystemDirectory,
            "WindowsPowerShell",
            "v1.0",
            "powershell.exe");
        if (!File.Exists(executable))
            executable = "powershell.exe";

        var startInfo = new ProcessStartInfo
        {
            FileName = executable,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        startInfo.ArgumentList.Add("-NoLogo");
        startInfo.ArgumentList.Add("-NoProfile");
        startInfo.ArgumentList.Add("-NonInteractive");
        startInfo.ArgumentList.Add("-EncodedCommand");
        startInfo.ArgumentList.Add(encoded);
        startInfo.Environment["ACS_ASSET_MUTATION_LOCK_PATH"] = lockPath;

        Process process = Process.Start(startInfo)
            ?? throw new IOException("Could not start the asset lock holder process.");
        try
        {
            Task<string?> readyTask = process.StandardOutput.ReadLineAsync();
            if (!readyTask.Wait(StartupTimeout))
            {
                throw new TimeoutException(
                    "The asset lock holder process did not become ready.");
            }
            string? marker = readyTask.GetAwaiter().GetResult();
            if (!string.Equals(marker, ReadyMarker, StringComparison.Ordinal))
            {
                Terminate(process);
                string diagnostics = process.StandardError.ReadToEnd();
                throw new IOException(
                    "The asset lock holder process failed before acquiring the lock. " +
                    diagnostics);
            }
            return new AssetMutationLockProcessHolder(process);
        }
        catch
        {
            Terminate(process);
            process.Dispose();
            throw;
        }
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        try
        {
            if (!_process.HasExited)
            {
                _process.StandardInput.WriteLine("release");
                _process.StandardInput.Flush();
                if (!_process.WaitForExit((int)ShutdownTimeout.TotalMilliseconds))
                    Terminate(_process);
            }
        }
        catch (Exception error) when (
            error is IOException or InvalidOperationException)
        {
            Terminate(_process);
        }
        finally
        {
            _process.Dispose();
        }
    }

    private static void Terminate(Process process)
    {
        try
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }
        catch (Exception error) when (
            error is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
        }
        try
        {
            process.WaitForExit((int)ShutdownTimeout.TotalMilliseconds);
        }
        catch (InvalidOperationException)
        {
        }
    }
}
