// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal sealed record PackageProcessResult(
    int ExitCode,
    string StandardOutput,
    string StandardError);

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
        CancellationToken cancellationToken)
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

        using var process = new Process
        {
            StartInfo = startInfo,
            EnableRaisingEvents = true,
        };
        cancellationToken.ThrowIfCancellationRequested();
        var output = new BoundedByteCapture(CaptureLimitBytesPerStream);
        var error = new BoundedByteCapture(CaptureLimitBytesPerStream);
        using var logBatcher = new BoundedLogBatcher(log);

        if (!process.Start())
            throw new InvalidOperationException("Package child process did not start.");

        // Do not use BeginOutputReadLine/BeginErrorReadLine here. StreamReader's line-oriented
        // implementation buffers an entire unterminated line before DataReceived fires, which
        // lets a malformed child allocate unbounded editor memory before our capture limit sees
        // any data. Fixed-size BaseStream reads make both capture and live logging bounded at the
        // point bytes enter this process.
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
            outputReadCancellation.Token);
        Task errorDrain = DrainStreamAsync(
            process.StandardError.BaseStream,
            errorEncoding,
            error,
            logBatcher,
            outputReadCancellation.Token);

        try
        {
            await process.WaitForExitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException cancelled)
        {
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
            capture.Append(chunk);
            lineLogger.Append(chunk);
        }
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
                process.StandardOutput.Dispose();
            }
            catch
            {
            }
            try
            {
                process.StandardError.Dispose();
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
