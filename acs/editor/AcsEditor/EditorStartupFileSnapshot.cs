// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal readonly record struct EditorStartupTextSnapshot(
    string? Source,
    bool Missing,
    string? Warning);

/// <summary>
/// Reads small user-local startup state without probing storage from the WPF
/// Dispatcher. Only an ordinary, stable UTF-8 file snapshot is accepted.
/// </summary>
internal static class EditorStartupFileSnapshot
{
    private static readonly UTF8Encoding StrictUtf8 =
        new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    internal static Task<EditorStartupTextSnapshot> ReadAsync(
        string path,
        int maximumBytes,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (maximumBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumBytes));

        return Task.Run(
            () => Read(path, maximumBytes, cancellationToken),
            CancellationToken.None);
    }

    internal static bool IsOrdinaryFile(FileAttributes attributes) =>
        (attributes & (FileAttributes.Directory |
                       FileAttributes.ReparsePoint |
                       FileAttributes.Device)) == 0;

    private static EditorStartupTextSnapshot Read(
        string path,
        int maximumBytes,
        CancellationToken cancellationToken)
    {
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!File.Exists(path))
            {
                return new EditorStartupTextSnapshot(
                    Source: null,
                    Missing: true,
                    Warning: null);
            }

            FileAttributes attributes = File.GetAttributes(path);
            if (!IsOrdinaryFile(attributes))
            {
                throw new InvalidDataException(
                    "startup state is not an ordinary file");
            }

            var before = new FileInfo(path);
            if (before.Length < 0 || before.Length > maximumBytes)
            {
                throw new InvalidDataException(
                    $"startup state exceeds {maximumBytes} bytes");
            }
            long expectedLength = before.Length;
            DateTime expectedWriteUtc = before.LastWriteTimeUtc;

            byte[] bytes;
            using (var stream = new FileStream(
                       path,
                       FileMode.Open,
                       FileAccess.Read,
                       FileShare.Read | FileShare.Delete,
                       bufferSize: 16 * 1024,
                       FileOptions.SequentialScan))
            {
                int capacity = checked((int)Math.Min(
                    (long)maximumBytes + 1L,
                    expectedLength + 1L));
                bytes = new byte[capacity];
                int total = 0;
                while (true)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (total == bytes.Length)
                    {
                        int extra = stream.ReadByte();
                        if (extra >= 0)
                        {
                            throw new InvalidDataException(
                                $"startup state exceeds {maximumBytes} bytes");
                        }
                        break;
                    }

                    int read = stream.Read(bytes, total, bytes.Length - total);
                    if (read == 0)
                        break;
                    total += read;
                    if (total > maximumBytes)
                    {
                        throw new InvalidDataException(
                            $"startup state exceeds {maximumBytes} bytes");
                    }
                }
                if (total != bytes.Length)
                    Array.Resize(ref bytes, total);
            }

            var after = new FileInfo(path);
            if (!after.Exists ||
                after.Length != expectedLength ||
                after.LastWriteTimeUtc != expectedWriteUtc ||
                bytes.LongLength != expectedLength)
            {
                throw new IOException(
                    "startup state changed while it was being read");
            }

            string source = StrictUtf8.GetString(bytes);
            if (source.Length != 0 && source[0] == '\uFEFF')
                source = source[1..];
            return new EditorStartupTextSnapshot(
                source,
                Missing: false,
                Warning: null);
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception error)
        {
            return new EditorStartupTextSnapshot(
                Source: null,
                Missing: false,
                Warning: error.Message);
        }
    }
}
