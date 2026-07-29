// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace AcsEditor;

internal sealed record PackageProcessResult(
    int ExitCode,
    string StandardOutput,
    string StandardError);

/// <summary>
/// Synchronous, allocation-free observation hooks for security-sensitive
/// package process state that must not depend on the bounded diagnostic
/// capture. Implementations must not retain the supplied output span.
/// </summary>
internal interface IPackageProcessObserver
{
    void OnProcessStarted();
    void OnStandardOutput(ReadOnlySpan<byte> bytes);
    void OnStandardOutputCompleted();
}

/// <summary>
/// Bounded child-process lifecycle used by the packaging pipeline. Cancellation
/// kills the complete process tree, waits for exit, and drains redirected output
/// before callers begin deleting staging directories.
/// </summary>
internal static class PackageProcessRunner
{
    internal const int CaptureLimitBytesPerStream = 8 * 1024 * 1024;
    private static readonly TimeSpan TerminationGrace = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan SecondTerminationGrace = TimeSpan.FromSeconds(1);
    private static readonly TimeSpan OutputDrainGrace = TimeSpan.FromSeconds(3);

    internal static async Task<PackageProcessResult> RunAsync(
        ProcessStartInfo startInfo,
        Action<string>? log,
        CancellationToken cancellationToken,
        IPackageProcessObserver? observer = null,
        bool containProcessTree = false,
        Action? beforeContainedProcessJobAssignmentForSelfTest = null)
    {
        ArgumentNullException.ThrowIfNull(startInfo);
        if (!startInfo.RedirectStandardOutput ||
            !startInfo.RedirectStandardError ||
            startInfo.UseShellExecute)
        {
            throw new ArgumentException(
                "Package processes require redirected stdout/stderr and UseShellExecute=false.",
                nameof(startInfo));
        }

        if (containProcessTree && OperatingSystem.IsWindows())
        {
            return await RunContainedWindowsAsync(
                    startInfo,
                    log,
                    cancellationToken,
                    observer,
                    beforeContainedProcessJobAssignmentForSelfTest)
                .ConfigureAwait(false);
        }
        if (beforeContainedProcessJobAssignmentForSelfTest != null)
        {
            throw new ArgumentException(
                "The suspended-launch self-test seam requires Windows process-tree containment.",
                nameof(beforeContainedProcessJobAssignmentForSelfTest));
        }

        using var process = new Process
        {
            StartInfo = startInfo,
            EnableRaisingEvents = true,
        };
        cancellationToken.ThrowIfCancellationRequested();
        var output = new BoundedByteCapture(CaptureLimitBytesPerStream);
        var error = new BoundedByteCapture(CaptureLimitBytesPerStream);
        using var logBatcher = new BoundedLogBatcher(log);
        using WindowsProcessJob? processJob =
            WindowsProcessJob.CreateIfRequested(containProcessTree);

        if (!process.Start())
            throw new InvalidOperationException("Package child process did not start.");
        try
        {
            processJob?.Assign(process);
            observer?.OnProcessStarted();

            // Do not use BeginOutputReadLine/BeginErrorReadLine here.
            // StreamReader's line-oriented implementation buffers an entire
            // unterminated line before DataReceived fires, which lets a
            // malformed child allocate unbounded editor memory before our
            // capture limit sees any data. Fixed-size BaseStream reads make
            // capture, observation, and live logging bounded at the point
            // bytes enter this process.
            Encoding outputEncoding = SafeOutputEncoding(
                process.StandardOutput.CurrentEncoding);
            Encoding errorEncoding = SafeOutputEncoding(
                process.StandardError.CurrentEncoding);
            using var outputReadCancellation = new CancellationTokenSource();
            Task outputDrain = DrainStreamAsync(
                process.StandardOutput.BaseStream,
                outputEncoding,
                output,
                logBatcher,
                observer,
                outputReadCancellation.Token);
            Task errorDrain = DrainStreamAsync(
                process.StandardError.BaseStream,
                errorEncoding,
                error,
                logBatcher,
                observer: null,
                cancellationToken: outputReadCancellation.Token);

            try
            {
                await process.WaitForExitAsync(cancellationToken)
                    .ConfigureAwait(false);
                if (processJob != null &&
                    !processJob.TerminateRemaining())
                {
                    throw new IOException(
                        "Package process exited, but its contained descendant " +
                        "processes could not be terminated.");
                }
            }
            catch (OperationCanceledException cancelled)
            {
                _ = processJob?.TerminateRemaining();
                bool terminated = await TerminateWithinAsync(
                        process,
                        TerminationGrace)
                    .ConfigureAwait(false);
                if (!terminated)
                {
                    terminated = await TerminateWithinAsync(
                            process,
                            SecondTerminationGrace)
                        .ConfigureAwait(false);
                }
                bool cancellationDrained = await DrainOutputWithinAsync(
                        process,
                        outputDrain,
                        errorDrain,
                        outputReadCancellation,
                        OutputDrainGrace)
                    .ConfigureAwait(false);
                logBatcher.FlushAndStop();
                if (!terminated)
                {
                    throw new IOException(
                        "Package child-process cancellation was requested, but process-tree " +
                        "termination could not be confirmed within the bounded deadline.",
                        cancelled);
                }
                if (!cancellationDrained)
                {
                    throw new IOException(
                        "Package child process terminated after cancellation, but redirected " +
                        "output could not be drained within the bounded deadline.",
                        cancelled);
                }
                throw;
            }

            bool drained = await DrainOutputWithinAsync(
                    process,
                    outputDrain,
                    errorDrain,
                    outputReadCancellation,
                    OutputDrainGrace)
                .ConfigureAwait(false);
            if (!drained)
            {
                throw new IOException(
                    "Package child process exited, but redirected output did not close " +
                    "within the bounded drain deadline.");
            }

            logBatcher.FlushAndStop();
            return new(
                process.ExitCode,
                output.GetText(outputEncoding),
                error.GetText(errorEncoding));
        }
        catch
        {
            // Any observer/setup/wait/drain failure after Process.Start must
            // not detach a child merely because it was not a cancellation.
            _ = processJob?.TerminateRemaining();
            _ = await TerminateWithinAsync(
                    process,
                    TerminationGrace)
                .ConfigureAwait(false);
            throw;
        }
    }

    /// <summary>
    /// Starts a package process suspended, binds it to the kill-on-close Job
    /// Object, and only then permits its first instruction to run. Process.Start
    /// cannot provide that ordering: a hostile or simply very fast package
    /// could otherwise create a descendant between CreateProcess and
    /// AssignProcessToJobObject and escape the containment boundary.
    /// </summary>
    private static async Task<PackageProcessResult> RunContainedWindowsAsync(
        ProcessStartInfo startInfo,
        Action<string>? log,
        CancellationToken cancellationToken,
        IPackageProcessObserver? observer,
        Action? beforeJobAssignmentForSelfTest)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var output = new BoundedByteCapture(CaptureLimitBytesPerStream);
        var error = new BoundedByteCapture(CaptureLimitBytesPerStream);
        using var logBatcher = new BoundedLogBatcher(log);
        using WindowsProcessJob processJob =
            WindowsProcessJob.CreateRequired();
        using SuspendedWindowsProcess process =
            SuspendedWindowsProcess.Create(startInfo);
        using var outputReadCancellation = new CancellationTokenSource();
        Task outputDrain = DrainStreamAsync(
            process.StandardOutput,
            process.StandardOutputEncoding,
            output,
            logBatcher,
            observer,
            outputReadCancellation.Token);
        Task errorDrain = DrainStreamAsync(
            process.StandardError,
            process.StandardErrorEncoding,
            error,
            logBatcher,
            observer: null,
            cancellationToken: outputReadCancellation.Token);

        bool resumed = false;
        try
        {
            // This hook exists only for the regression test. It deliberately
            // widens the create/assign interval and proves the package cannot
            // execute while the primary thread remains suspended.
            beforeJobAssignmentForSelfTest?.Invoke();
            processJob.Assign(process.ProcessHandle);
            cancellationToken.ThrowIfCancellationRequested();
            process.Resume();
            resumed = true;
            observer?.OnProcessStarted();

            try
            {
                await process.WaitForExitAsync(cancellationToken)
                    .ConfigureAwait(false);
                if (!processJob.TerminateRemaining())
                {
                    throw new IOException(
                        "Package process exited, but its contained descendant " +
                        "processes could not be terminated.");
                }
            }
            catch (OperationCanceledException cancelled)
            {
                _ = processJob.TerminateRemaining();
                process.Terminate();
                bool terminated = await process.WaitForExitWithinAsync(
                        TerminationGrace)
                    .ConfigureAwait(false);
                if (!terminated)
                {
                    process.Terminate();
                    terminated = await process.WaitForExitWithinAsync(
                            SecondTerminationGrace)
                        .ConfigureAwait(false);
                }
                bool cancellationDrained =
                    await DrainOutputWithinAsync(
                            outputDrain,
                            errorDrain,
                            outputReadCancellation,
                            process.StandardOutput,
                            process.StandardError,
                            OutputDrainGrace)
                        .ConfigureAwait(false);
                logBatcher.FlushAndStop();
                if (!terminated)
                {
                    throw new IOException(
                        "Package child-process cancellation was requested, but process-tree " +
                        "termination could not be confirmed within the bounded deadline.",
                        cancelled);
                }
                if (!cancellationDrained)
                {
                    throw new IOException(
                        "Package child process terminated after cancellation, but redirected " +
                        "output could not be drained within the bounded deadline.",
                        cancelled);
                }
                throw;
            }

            bool drained = await DrainOutputWithinAsync(
                    outputDrain,
                    errorDrain,
                    outputReadCancellation,
                    process.StandardOutput,
                    process.StandardError,
                    OutputDrainGrace)
                .ConfigureAwait(false);
            if (!drained)
            {
                throw new IOException(
                    "Package child process exited, but redirected output did not close " +
                    "within the bounded drain deadline.");
            }

            int exitCode = process.GetExitCode();
            logBatcher.FlushAndStop();
            return new(
                exitCode,
                output.GetText(process.StandardOutputEncoding),
                error.GetText(process.StandardErrorEncoding));
        }
        catch
        {
            // Before ResumeThread, TerminateProcess closes the suspended
            // process and its inherited pipe handles. After resume, the Job
            // Object also guarantees every descendant is terminated.
            _ = processJob.TerminateRemaining();
            process.Terminate();
            _ = await process.WaitForExitWithinAsync(
                    resumed ? TerminationGrace : SecondTerminationGrace)
                .ConfigureAwait(false);
            _ = await DrainOutputWithinAsync(
                    outputDrain,
                    errorDrain,
                    outputReadCancellation,
                    process.StandardOutput,
                    process.StandardError,
                    OutputDrainGrace)
                .ConfigureAwait(false);
            throw;
        }
    }

    private sealed class SuspendedWindowsProcess : IDisposable
    {
        private const uint CreateSuspended = 0x00000004;
        private const uint CreateNoWindow = 0x08000000;
        private const uint CreateUnicodeEnvironment = 0x00000400;
        private const uint ExtendedStartupInfoPresent = 0x00080000;
        private const uint StartfUseShowWindow = 0x00000001;
        private const uint StartfUseStdHandles = 0x00000100;
        private const ushort SwHide = 0;
        private const uint HandleFlagInherit = 0x00000001;
        private const uint ProcThreadAttributeHandleList = 0x00020002;
        private const uint GenericRead = 0x80000000;
        private const uint FileShareRead = 0x00000001;
        private const uint FileShareWrite = 0x00000002;
        private const uint OpenExisting = 3;
        private const uint FileAttributeNormal = 0x00000080;
        private const uint Infinite = 0xffffffff;
        private const uint WaitObject0 = 0;

        private readonly SafeThreadHandle _primaryThread;
        private readonly Task _exitTask;
        private bool _resumed;

        private SuspendedWindowsProcess(
            SafeProcessHandle process,
            SafeThreadHandle primaryThread,
            FileStream standardOutput,
            FileStream standardError,
            Encoding standardOutputEncoding,
            Encoding standardErrorEncoding)
        {
            ProcessHandle = process;
            _primaryThread = primaryThread;
            StandardOutput = standardOutput;
            StandardError = standardError;
            StandardOutputEncoding = standardOutputEncoding;
            StandardErrorEncoding = standardErrorEncoding;
            _exitTask = Task.Run(() =>
            {
                uint result = WaitForSingleObject(ProcessHandle, Infinite);
                if (result != WaitObject0)
                {
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(),
                        "Waiting for the suspended package process failed.");
                }
            });
        }

        internal SafeProcessHandle ProcessHandle { get; }
        internal FileStream StandardOutput { get; }
        internal FileStream StandardError { get; }
        internal Encoding StandardOutputEncoding { get; }
        internal Encoding StandardErrorEncoding { get; }

        internal static SuspendedWindowsProcess Create(
            ProcessStartInfo startInfo)
        {
            if (!OperatingSystem.IsWindows())
                throw new PlatformNotSupportedException();
            if (string.IsNullOrWhiteSpace(startInfo.FileName))
            {
                throw new ArgumentException(
                    "Contained package process requires an executable path.",
                    nameof(startInfo));
            }
            if (startInfo.ArgumentList.Count != 0 &&
                !string.IsNullOrEmpty(startInfo.Arguments))
            {
                throw new ArgumentException(
                    "Contained package process cannot combine Arguments and ArgumentList.",
                    nameof(startInfo));
            }
            if (startInfo.RedirectStandardInput ||
                !string.IsNullOrEmpty(startInfo.UserName) ||
                !string.IsNullOrEmpty(startInfo.Domain) ||
                startInfo.Password != null ||
                !string.IsNullOrEmpty(startInfo.PasswordInClearText) ||
                startInfo.LoadUserProfile ||
                !string.IsNullOrEmpty(startInfo.Verb) ||
                startInfo.ErrorDialog)
            {
                throw new ArgumentException(
                    "Contained package launch does not accept credentials, shell verbs, " +
                    "interactive standard input, or process-owned error dialogs.",
                    nameof(startInfo));
            }

            string executable = Path.GetFullPath(startInfo.FileName);
            string commandLine = BuildCommandLine(startInfo, executable);
            string? workingDirectory =
                string.IsNullOrWhiteSpace(startInfo.WorkingDirectory)
                    ? null
                    : Path.GetFullPath(startInfo.WorkingDirectory);
            string environment = BuildEnvironmentBlock(startInfo);
            var security = new SecurityAttributes
            {
                Length = Marshal.SizeOf<SecurityAttributes>(),
                InheritHandle = 1,
            };

            SafeFileHandle? outputRead = null;
            SafeFileHandle? outputWrite = null;
            SafeFileHandle? errorRead = null;
            SafeFileHandle? errorWrite = null;
            SafeFileHandle? nullInput = null;
            IntPtr attributeList = IntPtr.Zero;
            bool attributeListInitialized = false;
            IntPtr handleList = IntPtr.Zero;
            IntPtr environmentBlock = IntPtr.Zero;
            SafeProcessHandle? process = null;
            SafeThreadHandle? thread = null;
            FileStream? outputStream = null;
            FileStream? errorStream = null;
            try
            {
                CreateChildPipe(
                    ref security,
                    out outputRead,
                    out outputWrite);
                CreateChildPipe(
                    ref security,
                    out errorRead,
                    out errorWrite);
                nullInput = CreateFileW(
                    "NUL",
                    GenericRead,
                    FileShareRead | FileShareWrite,
                    ref security,
                    OpenExisting,
                    FileAttributeNormal,
                    IntPtr.Zero);
                if (nullInput.IsInvalid)
                    throw new Win32Exception(Marshal.GetLastWin32Error());

                nuint attributeBytes = 0;
                _ = InitializeProcThreadAttributeList(
                    IntPtr.Zero,
                    1,
                    0,
                    ref attributeBytes);
                if (attributeBytes == 0)
                {
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(),
                        "Could not size the package process attribute list.");
                }
                attributeList = Marshal.AllocHGlobal(
                    checked((nint)attributeBytes));
                if (!InitializeProcThreadAttributeList(
                        attributeList,
                        1,
                        0,
                        ref attributeBytes))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                attributeListInitialized = true;

                IntPtr[] inheritedHandles =
                [
                    nullInput.DangerousGetHandle(),
                    outputWrite.DangerousGetHandle(),
                    errorWrite.DangerousGetHandle(),
                ];
                handleList = Marshal.AllocHGlobal(
                    checked(IntPtr.Size * inheritedHandles.Length));
                Marshal.Copy(
                    inheritedHandles,
                    0,
                    handleList,
                    inheritedHandles.Length);
                if (!UpdateProcThreadAttribute(
                        attributeList,
                        0,
                        (nuint)ProcThreadAttributeHandleList,
                        handleList,
                        checked((nuint)(
                            IntPtr.Size * inheritedHandles.Length)),
                        IntPtr.Zero,
                        IntPtr.Zero))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }

                var startup = new StartupInfoEx
                {
                    StartupInfo = new StartupInfo
                    {
                        Size = Marshal.SizeOf<StartupInfoEx>(),
                        Flags =
                            StartfUseStdHandles |
                            StartfUseShowWindow,
                        ShowWindow = SwHide,
                        StandardInput = nullInput.DangerousGetHandle(),
                        StandardOutput = outputWrite.DangerousGetHandle(),
                        StandardError = errorWrite.DangerousGetHandle(),
                    },
                    AttributeList = attributeList,
                };
                environmentBlock =
                    Marshal.StringToHGlobalUni(environment);
                uint creationFlags =
                    CreateSuspended |
                    CreateUnicodeEnvironment |
                    ExtendedStartupInfoPresent;
                if (startInfo.CreateNoWindow)
                    creationFlags |= CreateNoWindow;
                var mutableCommandLine = new StringBuilder(commandLine);
                if (!CreateProcessW(
                        executable,
                        mutableCommandLine,
                        IntPtr.Zero,
                        IntPtr.Zero,
                        inheritHandles: true,
                        creationFlags,
                        environmentBlock,
                        workingDirectory,
                        ref startup,
                        out ProcessInformation information))
                {
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(),
                        "Could not create the package process suspended.");
                }
                process = new SafeProcessHandle(
                    information.Process,
                    ownsHandle: true);
                thread = new SafeThreadHandle(
                    information.Thread,
                    ownsHandle: true);

                // The child owns the inherited write ends. Closing the parent
                // copies is required for EOF after the complete Job exits.
                outputWrite.Dispose();
                outputWrite = null;
                errorWrite.Dispose();
                errorWrite = null;
                nullInput.Dispose();
                nullInput = null;

                outputStream = new FileStream(
                    outputRead,
                    FileAccess.Read,
                    32 * 1024,
                    isAsync: false);
                outputRead = null;
                errorStream = new FileStream(
                    errorRead,
                    FileAccess.Read,
                    32 * 1024,
                    isAsync: false);
                errorRead = null;

                var result = new SuspendedWindowsProcess(
                    process,
                    thread,
                    outputStream,
                    errorStream,
                    SafeOutputEncoding(
                        startInfo.StandardOutputEncoding ??
                        Console.OutputEncoding),
                    SafeOutputEncoding(
                        startInfo.StandardErrorEncoding ??
                        Console.OutputEncoding));
                process = null;
                thread = null;
                outputStream = null;
                errorStream = null;
                return result;
            }
            finally
            {
                outputStream?.Dispose();
                errorStream?.Dispose();
                outputRead?.Dispose();
                outputWrite?.Dispose();
                errorRead?.Dispose();
                errorWrite?.Dispose();
                nullInput?.Dispose();
                thread?.Dispose();
                if (process is { IsInvalid: false })
                {
                    _ = TerminateProcess(
                        process,
                        0xC000013A);
                }
                process?.Dispose();
                if (attributeList != IntPtr.Zero)
                {
                    if (attributeListInitialized)
                        DeleteProcThreadAttributeList(attributeList);
                    Marshal.FreeHGlobal(attributeList);
                }
                if (handleList != IntPtr.Zero)
                    Marshal.FreeHGlobal(handleList);
                if (environmentBlock != IntPtr.Zero)
                    Marshal.FreeHGlobal(environmentBlock);
            }
        }

        internal void Resume()
        {
            if (_resumed)
                throw new InvalidOperationException("Package process was already resumed.");
            uint previousSuspendCount = ResumeThread(_primaryThread);
            if (previousSuspendCount == uint.MaxValue)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            if (previousSuspendCount != 1)
            {
                throw new InvalidOperationException(
                    "Package primary thread had an unexpected suspend count.");
            }
            _resumed = true;
        }

        internal async Task WaitForExitAsync(
            CancellationToken cancellationToken) =>
            await _exitTask.WaitAsync(cancellationToken)
                .ConfigureAwait(false);

        internal async Task<bool> WaitForExitWithinAsync(TimeSpan timeout)
        {
            try
            {
                await _exitTask.WaitAsync(timeout).ConfigureAwait(false);
                return true;
            }
            catch
            {
                return false;
            }
        }

        internal int GetExitCode()
        {
            if (!_exitTask.IsCompletedSuccessfully ||
                !GetExitCodeProcess(ProcessHandle, out uint exitCode))
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "Package process exit code is unavailable.");
            }
            return unchecked((int)exitCode);
        }

        internal void Terminate()
        {
            try
            {
                _ = TerminateProcess(
                    ProcessHandle,
                    0xC000013A);
            }
            catch
            {
            }
        }

        public void Dispose()
        {
            StandardOutput.Dispose();
            StandardError.Dispose();
            _primaryThread.Dispose();
            ProcessHandle.Dispose();
        }

        private static void CreateChildPipe(
            ref SecurityAttributes security,
            out SafeFileHandle read,
            out SafeFileHandle write)
        {
            if (!CreatePipe(
                    out read,
                    out write,
                    ref security,
                    0))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            if (!SetHandleInformation(
                    read,
                    HandleFlagInherit,
                    0))
            {
                int error = Marshal.GetLastWin32Error();
                read.Dispose();
                write.Dispose();
                throw new Win32Exception(error);
            }
        }

        private static string BuildCommandLine(
            ProcessStartInfo startInfo,
            string executable)
        {
            var commandLine = new StringBuilder();
            AppendWindowsArgument(commandLine, executable);
            if (startInfo.ArgumentList.Count != 0)
            {
                foreach (string argument in startInfo.ArgumentList)
                {
                    commandLine.Append(' ');
                    AppendWindowsArgument(commandLine, argument);
                }
            }
            else if (!string.IsNullOrEmpty(startInfo.Arguments))
            {
                commandLine.Append(' ').Append(startInfo.Arguments);
            }
            return commandLine.ToString();
        }

        private static void AppendWindowsArgument(
            StringBuilder output,
            string argument)
        {
            if (argument.IndexOf('\0') >= 0)
                throw new ArgumentException("Process argument contains NUL.");
            output.Append('"');
            int slashes = 0;
            foreach (char value in argument)
            {
                if (value == '\\')
                {
                    slashes++;
                    continue;
                }
                if (value == '"')
                {
                    output.Append('\\', checked(slashes * 2 + 1));
                    output.Append('"');
                    slashes = 0;
                    continue;
                }
                output.Append('\\', slashes);
                slashes = 0;
                output.Append(value);
            }
            output.Append('\\', checked(slashes * 2));
            output.Append('"');
        }

        private static string BuildEnvironmentBlock(
            ProcessStartInfo startInfo)
        {
            var block = new StringBuilder();
            foreach (KeyValuePair<string, string?> variable in
                     startInfo.Environment
                         .OrderBy(
                             static pair => pair.Key,
                             StringComparer.OrdinalIgnoreCase)
                         .ThenBy(
                             static pair => pair.Key,
                             StringComparer.Ordinal))
            {
                if (string.IsNullOrEmpty(variable.Key) ||
                    variable.Key.Contains('=') ||
                    variable.Key.Contains('\0') ||
                    variable.Value is null ||
                    variable.Value.Contains('\0'))
                {
                    throw new ArgumentException(
                        "Contained package environment contains an invalid entry.",
                        nameof(startInfo));
                }
                block.Append(variable.Key)
                    .Append('=')
                    .Append(variable.Value)
                    .Append('\0');
            }
            // Every non-empty entry already contributes its terminator and
            // Marshal.StringToHGlobalUni adds the final empty-string NUL. An
            // empty environment needs one explicit NUL before that terminator.
            if (block.Length == 0)
                block.Append('\0');
            return block.ToString();
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct SecurityAttributes
        {
            internal int Length;
            internal IntPtr SecurityDescriptor;
            internal int InheritHandle;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            internal int Size;
            internal string? Reserved;
            internal string? Desktop;
            internal string? Title;
            internal uint X;
            internal uint Y;
            internal uint XSize;
            internal uint YSize;
            internal uint XCountChars;
            internal uint YCountChars;
            internal uint FillAttribute;
            internal uint Flags;
            internal ushort ShowWindow;
            internal ushort ReservedByteCount;
            internal IntPtr ReservedBytes;
            internal IntPtr StandardInput;
            internal IntPtr StandardOutput;
            internal IntPtr StandardError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct StartupInfoEx
        {
            internal StartupInfo StartupInfo;
            internal IntPtr AttributeList;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation
        {
            internal IntPtr Process;
            internal IntPtr Thread;
            internal uint ProcessId;
            internal uint ThreadId;
        }

        private sealed class SafeThreadHandle :
            SafeHandleZeroOrMinusOneIsInvalid
        {
            internal SafeThreadHandle(IntPtr handle, bool ownsHandle)
                : base(ownsHandle)
            {
                SetHandle(handle);
            }

            protected override bool ReleaseHandle() => CloseHandle(handle);
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreatePipe(
            out SafeFileHandle readPipe,
            out SafeFileHandle writePipe,
            ref SecurityAttributes pipeAttributes,
            uint size);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetHandleInformation(
            SafeFileHandle handle,
            uint mask,
            uint flags);

        [DllImport(
            "kernel32.dll",
            CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            ref SecurityAttributes securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InitializeProcThreadAttributeList(
            IntPtr attributeList,
            int attributeCount,
            uint flags,
            ref nuint size);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool UpdateProcThreadAttribute(
            IntPtr attributeList,
            uint flags,
            nuint attribute,
            IntPtr value,
            nuint size,
            IntPtr previousValue,
            IntPtr returnSize);

        [DllImport("kernel32.dll")]
        private static extern void DeleteProcThreadAttributeList(
            IntPtr attributeList);

        [DllImport(
            "kernel32.dll",
            CharSet = CharSet.Unicode,
            SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateProcessW(
            string applicationName,
            StringBuilder commandLine,
            IntPtr processAttributes,
            IntPtr threadAttributes,
            [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
            uint creationFlags,
            IntPtr environment,
            string? currentDirectory,
            ref StartupInfoEx startupInfo,
            out ProcessInformation processInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint ResumeThread(SafeThreadHandle thread);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(
            SafeProcessHandle handle,
            uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetExitCodeProcess(
            SafeProcessHandle process,
            out uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool TerminateProcess(
            SafeProcessHandle process,
            uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);
    }

    /// <summary>
    /// Windows Job Object containment for launch-smoke processes. The
    /// kill-on-close limit protects abnormal runner teardown; explicit
    /// termination after the root exits closes inherited output handles and
    /// prevents detached helpers from surviving a successful smoke.
    /// </summary>
    private sealed class WindowsProcessJob : IDisposable
    {
        private const uint JobObjectLimitKillOnJobClose = 0x00002000;
        private const int JobObjectExtendedLimitInformationClass = 9;

        private readonly SafeJobHandle _handle;
        private bool _terminated;

        private WindowsProcessJob(SafeJobHandle handle)
        {
            _handle = handle;
        }

        internal static WindowsProcessJob? CreateIfRequested(bool requested)
        {
            if (!requested || !OperatingSystem.IsWindows())
                return null;

            return CreateRequired();
        }

        internal static WindowsProcessJob CreateRequired()
        {
            if (!OperatingSystem.IsWindows())
                throw new PlatformNotSupportedException();

            SafeJobHandle handle = CreateJobObjectW(IntPtr.Zero, null);
            if (handle.IsInvalid)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            try
            {
                var limits = new JobObjectExtendedLimitInformation
                {
                    BasicLimitInformation = new()
                    {
                        LimitFlags = JobObjectLimitKillOnJobClose,
                    },
                };
                if (!SetInformationJobObject(
                        handle,
                        JobObjectExtendedLimitInformationClass,
                        ref limits,
                        checked((uint)Marshal.SizeOf(limits))))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                return new WindowsProcessJob(handle);
            }
            catch
            {
                handle.Dispose();
                throw;
            }
        }

        internal void Assign(Process process)
        {
            ArgumentNullException.ThrowIfNull(process);
            if (!AssignProcessToJobObject(_handle, process.Handle))
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "The package process could not be assigned to its bounded job.");
            }
        }

        internal void Assign(SafeProcessHandle process)
        {
            ArgumentNullException.ThrowIfNull(process);
            if (process.IsInvalid ||
                !AssignProcessToJobObject(_handle, process))
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "The suspended package process could not be assigned to its bounded job.");
            }
        }

        internal bool TerminateRemaining()
        {
            if (_terminated)
                return true;
            if (!TerminateJobObject(_handle, 0xC000013A))
                return false;
            _terminated = true;
            return true;
        }

        public void Dispose() => _handle.Dispose();

        [StructLayout(LayoutKind.Sequential)]
        private struct JobObjectBasicLimitInformation
        {
            internal long PerProcessUserTimeLimit;
            internal long PerJobUserTimeLimit;
            internal uint LimitFlags;
            internal UIntPtr MinimumWorkingSetSize;
            internal UIntPtr MaximumWorkingSetSize;
            internal uint ActiveProcessLimit;
            internal UIntPtr Affinity;
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
            internal JobObjectBasicLimitInformation BasicLimitInformation;
            internal IoCounters IoInfo;
            internal UIntPtr ProcessMemoryLimit;
            internal UIntPtr JobMemoryLimit;
            internal UIntPtr PeakProcessMemoryUsed;
            internal UIntPtr PeakJobMemoryUsed;
        }

        private sealed class SafeJobHandle :
            SafeHandleZeroOrMinusOneIsInvalid
        {
            private SafeJobHandle()
                : base(ownsHandle: true)
            {
            }

            protected override bool ReleaseHandle() => CloseHandle(handle);
        }

        [DllImport(
            "kernel32.dll",
            CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern SafeJobHandle CreateJobObjectW(
            IntPtr jobAttributes,
            string? name);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetInformationJobObject(
            SafeJobHandle job,
            int informationClass,
            ref JobObjectExtendedLimitInformation information,
            uint informationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool AssignProcessToJobObject(
            SafeJobHandle job,
            IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool AssignProcessToJobObject(
            SafeJobHandle job,
            SafeProcessHandle process);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool TerminateJobObject(
            SafeJobHandle job,
            uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);
    }

    private static Encoding SafeOutputEncoding(Encoding source)
    {
        var encoding = (Encoding)source.Clone();
        encoding.DecoderFallback = DecoderFallback.ReplacementFallback;
        return encoding;
    }

    private static async Task DrainStreamAsync(
        Stream stream,
        Encoding encoding,
        BoundedByteCapture capture,
        BoundedLogBatcher logBatcher,
        IPackageProcessObserver? observer,
        CancellationToken cancellationToken)
    {
        var bytes = new byte[32 * 1024];
        var lineLogger = new BoundedLineDecoder(encoding, logBatcher);
        while (true)
        {
            int read = await stream.ReadAsync(bytes, cancellationToken)
                .ConfigureAwait(false);
            if (read == 0)
                break;
            ReadOnlySpan<byte> chunk = bytes.AsSpan(0, read);
            observer?.OnStandardOutput(chunk);
            capture.Append(chunk);
            lineLogger.Append(chunk);
        }
        observer?.OnStandardOutputCompleted();
        lineLogger.Complete();
    }

    private sealed class BoundedByteCapture
    {
        private const string TruncatedMarker =
            "\n[package process output truncated]\n";
        private readonly object _sync = new();
        private readonly byte[] _bytes;
        private int _count;
        private bool _truncated;

        internal BoundedByteCapture(int limitBytes)
        {
            _bytes = new byte[limitBytes];
        }

        internal void Append(ReadOnlySpan<byte> bytes)
        {
            lock (_sync)
            {
                if (_truncated)
                    return;
                int remaining = _bytes.Length - _count;
                if (remaining <= 0)
                {
                    _truncated = true;
                    return;
                }

                int copy = Math.Min(remaining, bytes.Length);
                bytes[..copy].CopyTo(_bytes.AsSpan(_count));
                _count += copy;
                if (copy != bytes.Length)
                    _truncated = true;
            }
        }

        internal string GetText(Encoding encoding)
        {
            lock (_sync)
            {
                string text = encoding.GetString(_bytes, 0, _count);
                return _truncated
                    ? text + TruncatedMarker
                    : text;
            }
        }
    }

    /// <summary>
    /// Incrementally frames log lines without ever retaining an unbounded unterminated line.
    /// Capture remains byte-exact up to its separate limit; this decoder is only for UI logging.
    /// </summary>
    private sealed class BoundedLineDecoder
    {
        private const string TruncatedSuffix = " [line truncated]";
        private static readonly int MaxBufferedLineCharacters =
            16 * 1024 - TruncatedSuffix.Length;
        private readonly Decoder _decoder;
        private readonly BoundedLogBatcher _logBatcher;
        private readonly char[] _characters = new char[4096];
        private readonly StringBuilder _line = new();
        private bool _lineTruncated;
        private bool _discardUntilNewline;

        internal BoundedLineDecoder(
            Encoding encoding,
            BoundedLogBatcher logBatcher)
        {
            _decoder = encoding.GetDecoder();
            _logBatcher = logBatcher;
        }

        internal void Append(ReadOnlySpan<byte> bytes)
        {
            while (!bytes.IsEmpty)
            {
                _decoder.Convert(
                    bytes,
                    _characters.AsSpan(),
                    flush: false,
                    out int bytesUsed,
                    out int charactersUsed,
                    out _);
                AppendCharacters(_characters.AsSpan(0, charactersUsed));
                bytes = bytes[bytesUsed..];
                if (bytesUsed == 0 && charactersUsed == 0)
                {
                    throw new InvalidDataException(
                        "Package child output decoder made no forward progress.");
                }
            }
        }

        internal void Complete()
        {
            bool completed;
            do
            {
                _decoder.Convert(
                    ReadOnlySpan<byte>.Empty,
                    _characters.AsSpan(),
                    flush: true,
                    out _,
                    out int charactersUsed,
                    out completed);
                AppendCharacters(_characters.AsSpan(0, charactersUsed));
            }
            while (!completed);

            if (!_discardUntilNewline &&
                (_line.Length != 0 || _lineTruncated))
            {
                PublishLine();
            }
        }

        private void AppendCharacters(ReadOnlySpan<char> characters)
        {
            foreach (char value in characters)
            {
                if (value == '\n')
                {
                    if (_discardUntilNewline)
                    {
                        _discardUntilNewline = false;
                    }
                    else
                    {
                        PublishLine();
                    }
                    continue;
                }
                if (_discardUntilNewline)
                    continue;
                if (_line.Length < MaxBufferedLineCharacters)
                {
                    _line.Append(value);
                }
                else
                {
                    _lineTruncated = true;
                    // Publish the bounded prefix immediately. Besides limiting retention, this
                    // gives the UI observable progress even if the child never terminates its
                    // current line; the remainder is discarded until the next newline.
                    PublishLine();
                    _discardUntilNewline = true;
                }
            }
        }

        private void PublishLine()
        {
            if (_line.Length != 0 && _line[^1] == '\r')
                _line.Length--;
            string line = _line.ToString();
            if (_lineTruncated)
                line += TruncatedSuffix;
            _logBatcher.Enqueue(line);
            _line.Clear();
            _lineTruncated = false;
        }
    }

    private sealed class BoundedLogBatcher : IDisposable
    {
        private const int MaxLinesPerBatch = 32;
        private const int MaxPendingLines = 512;
        private const int MaxLineCharacters = 16 * 1024;
        private readonly object _sync = new();
        private readonly object _deliverySync = new();
        private readonly Action<string>? _log;
        private readonly Queue<string> _pending = new();
        private readonly Timer? _timer;
        private long _dropped;
        private bool _stopped;

        internal BoundedLogBatcher(Action<string>? log)
        {
            _log = log;
            if (log != null)
            {
                _timer = new Timer(
                    static state => ((BoundedLogBatcher)state!).FlushOne(),
                    this,
                    dueTime: 100,
                    period: 100);
            }
        }

        internal void Enqueue(string line)
        {
            if (_log == null)
                return;
            string bounded = line.Length <= MaxLineCharacters
                ? line
                : line[..MaxLineCharacters] + " [line truncated]";
            lock (_sync)
            {
                if (_stopped)
                    return;
                if (_pending.Count >= MaxPendingLines)
                {
                    _dropped++;
                    return;
                }
                _pending.Enqueue(bounded);
            }
        }

        private void FlushOne()
        {
            lock (_deliverySync)
            {
                lock (_sync)
                {
                    if (_stopped)
                        return;
                }
                string? batch = TakeBatch();
                if (batch != null)
                    TryLog(_log, batch);
            }
        }

        private string? TakeBatch()
        {
            lock (_sync)
            {
                if (_pending.Count == 0)
                    return null;
                var builder = new StringBuilder();
                int count = Math.Min(MaxLinesPerBatch, _pending.Count);
                for (int index = 0; index < count; index++)
                {
                    if (index != 0)
                        builder.AppendLine();
                    builder.Append(_pending.Dequeue());
                }
                return builder.ToString();
            }
        }

        internal void FlushAndStop()
        {
            lock (_sync)
            {
                if (_stopped)
                    return;
                _stopped = true;
            }
            _timer?.Dispose();
            lock (_deliverySync)
            {
                while (TakeBatch() is { } batch)
                    TryLog(_log, batch);

                long dropped;
                lock (_sync)
                {
                    dropped = _dropped;
                    _dropped = 0;
                }
                if (dropped != 0)
                {
                    TryLog(
                        _log,
                        $"[package process log throttled: {dropped} line(s) omitted]");
                }
            }
        }

        public void Dispose() => FlushAndStop();
    }

    private static void TryLog(Action<string>? log, string message)
    {
        if (log == null)
            return;
        try
        {
            log(message);
        }
        catch
        {
            // A closing WPF dispatcher or external log sink must never tear
            // down the async stream callback or the packaging process.
        }
    }

    private static async Task<bool> TerminateWithinAsync(
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
            await process.WaitForExitAsync(timeout.Token)
                .ConfigureAwait(false);
            return true;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
        catch
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
    }

    private static async Task<bool> DrainOutputWithinAsync(
        Process process,
        Task outputDrain,
        Task errorDrain,
        CancellationTokenSource readCancellation,
        TimeSpan grace) =>
        await DrainOutputWithinAsync(
                outputDrain,
                errorDrain,
                readCancellation,
                process.StandardOutput,
                process.StandardError,
                grace)
            .ConfigureAwait(false);

    private static async Task<bool> DrainOutputWithinAsync(
        Task outputDrain,
        Task errorDrain,
        CancellationTokenSource readCancellation,
        IDisposable standardOutput,
        IDisposable standardError,
        TimeSpan grace)
    {
        Task combinedDrain = Task.WhenAll(outputDrain, errorDrain);
        try
        {
            await combinedDrain
                .WaitAsync(grace)
                .ConfigureAwait(false);
            return true;
        }
        catch (Exception error) when (
            error is TimeoutException or IOException or
                OperationCanceledException or ObjectDisposedException)
        {
            readCancellation.Cancel();
            try
            {
                standardOutput.Dispose();
            }
            catch
            {
            }
            try
            {
                standardError.Dispose();
            }
            catch
            {
            }
            try
            {
                // Observe any cancellation/disposal fault so an abandoned reader task cannot
                // surface later through the process-wide unobserved-task handler.
                await combinedDrain
                    .WaitAsync(TimeSpan.FromMilliseconds(250))
                    .ConfigureAwait(false);
            }
            catch
            {
            }
            return false;
        }
    }
}
