// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal readonly record struct ProjectSettingKey(string Section, string Key);

/// <summary>
/// Managed preflight for the native project-settings parser. The current ABI does not expose its
/// load result, so a source is accepted only when it satisfies the same bounded INI grammar and
/// every source entry survives the native load/serialize round trip.
/// </summary>
internal static class ProjectSettingsDocumentContract
{
    internal const int MaximumLines = 4096;
    internal const int MaximumLineUtf8Bytes = 511;
    internal const int MaximumSectionUtf8Bytes = 31;
    internal const int MaximumKeyUtf8Bytes = 63;
    internal const int MaximumValueUtf8Bytes = 191;
    internal const int MaximumEntries = 1024;

    private static readonly UTF8Encoding StrictUtf8NoBom = new(false, true);

    internal static Task<string> ReadSourceAsync(
        string projectRoot,
        string path,
        CancellationToken cancellationToken) =>
        Task.Run(
            () =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                string source = ReadSource(projectRoot, path);
                cancellationToken.ThrowIfCancellationRequested();
                return source;
            },
            cancellationToken);

    internal static string ReadSource(string projectRoot, string path)
    {
        if (string.IsNullOrWhiteSpace(projectRoot))
            throw new ArgumentException("A project root is required.", nameof(projectRoot));
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("A project-settings path is required.", nameof(path));

        string root =
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        SceneSourceFile.ValidateProjectRootDirectory(root);
        string configPath = Path.Combine(root, "Config");
        string expectedPath = Path.Combine(configPath, "ProjectSettings.ini");
        string fullPath = Path.GetFullPath(path);
        if (!PathsEqual(fullPath, expectedPath))
        {
            throw new InvalidDataException(
                "Project settings must be Config/ProjectSettings.ini directly under the project root.");
        }

        FileAttributes configAttributes;
        try
        {
            configAttributes = File.GetAttributes(configPath);
        }
        catch (FileNotFoundException)
        {
            return "";
        }
        catch (DirectoryNotFoundException)
        {
            return "";
        }
        if ((configAttributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) !=
            FileAttributes.Directory)
        {
            throw new InvalidDataException(
                "The project Config directory must be an ordinary directory, not a file or reparse point.");
        }

        FileAttributes attributes;
        try
        {
            attributes = File.GetAttributes(fullPath);
        }
        catch (FileNotFoundException)
        {
            return "";
        }
        catch (DirectoryNotFoundException)
        {
            return "";
        }
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException(
                "ProjectSettings.ini must be an ordinary file, not a directory or reparse point.");
        }

        byte[] bytes;
        using (var stream = new FileStream(
                   fullPath,
                   FileMode.Open,
                   FileAccess.Read,
                   FileShare.Read,
                   bufferSize: 64 * 1024,
                   FileOptions.SequentialScan))
        {
            // Recheck after acquiring a non-delete-sharing handle. A path swap before Open is
            // caught here; after Open, Windows cannot replace the held file until capture ends.
            attributes = File.GetAttributes(fullPath);
            if ((attributes &
                 (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            {
                throw new InvalidDataException(
                    "ProjectSettings.ini must be an ordinary file, not a directory or reparse point.");
            }
            SceneSourceFile.ValidateProjectRootDirectory(root);
            configAttributes = File.GetAttributes(configPath);
            if ((configAttributes &
                 (FileAttributes.Directory | FileAttributes.ReparsePoint)) !=
                FileAttributes.Directory)
            {
                throw new InvalidDataException(
                    "The project Config directory must be an ordinary directory, not a file or reparse point.");
            }
            long length = stream.Length;
            if (length < 0 ||
                length > ProjectSettingsSerialization.MaximumUtf8Bytes)
            {
                throw new InvalidDataException(
                    "Project settings exceed the 1 MiB persistence limit.");
            }

            bytes = new byte[(int)length];
            int total = 0;
            while (total < bytes.Length)
            {
                int read = stream.Read(bytes, total, bytes.Length - total);
                if (read == 0)
                {
                    throw new IOException(
                        "ProjectSettings.ini changed while it was being read.");
                }
                total += read;
            }
            if (stream.ReadByte() != -1 ||
                stream.Length != bytes.LongLength)
            {
                throw new IOException(
                    "ProjectSettings.ini changed while it was being read.");
            }
        }

        string source;
        try
        {
            source = StrictUtf8NoBom.GetString(bytes);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "Project settings are not valid UTF-8.",
                error);
        }

        // File.ReadAllText(..., Encoding.UTF8), used by the legacy path, consumed a leading BOM.
        // Preserve that compatibility while keeping embedded U+FEFF characters significant.
        if (source.Length > 0 && source[0] == '\uFEFF')
            source = source[1..];
        Parse(source);
        return source;
    }

    private static bool PathsEqual(string left, string right) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
            OperatingSystem.IsWindows()
                ? StringComparison.OrdinalIgnoreCase
                : StringComparison.Ordinal);

    internal static EditorDocumentState CreateState(string canonicalText)
    {
        ArgumentNullException.ThrowIfNull(canonicalText);
        Parse(canonicalText);
        return EditorDocumentState.Text(canonicalText);
    }

    internal static IReadOnlyDictionary<ProjectSettingKey, string> Parse(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        int textBytes = Utf8Length(text, "Project settings");
        if (textBytes > ProjectSettingsSerialization.MaximumUtf8Bytes)
        {
            throw new InvalidDataException(
                "Project settings exceed the 1 MiB persistence limit.");
        }
        if (text.IndexOf('\0') >= 0)
            throw new InvalidDataException("Project settings contain an embedded NUL.");

        var entries = new Dictionary<ProjectSettingKey, string>();
        string section = "";
        int offset = 0;
        int lineNumber = 0;
        while (offset < text.Length)
        {
            lineNumber++;
            if (lineNumber > MaximumLines)
                throw Error(lineNumber, "Project settings contain too many lines.");

            int newline = text.IndexOf('\n', offset);
            int end = newline < 0 ? text.Length : newline;
            string physicalLine = text[offset..end];
            offset = newline < 0 ? text.Length : newline + 1;
            if (Utf8Length(physicalLine, "Project settings line") > MaximumLineUtf8Bytes)
                throw Error(lineNumber, "Project settings contain an overlong line.");
            if (physicalLine.EndsWith('\r'))
                physicalLine = physicalLine[..^1];

            string line = TrimAsciiSpaceAndTab(physicalLine);
            if (line.Length == 0 || line[0] is ';' or '#')
                continue;

            if (line[0] == '[')
            {
                if (line.Length < 3 || line[^1] != ']')
                    throw Error(lineNumber, "Project settings contain an invalid section.");
                section = line[1..^1];
                if (Utf8Length(section, "Project settings section") >
                    MaximumSectionUtf8Bytes)
                {
                    throw Error(lineNumber, "Project settings contain an overlong section.");
                }
                continue;
            }

            if (section.Length == 0)
                throw Error(lineNumber, "A project setting appears before its section.");

            int equals = line.IndexOf('=');
            if (equals < 0)
                throw Error(lineNumber, "Project settings contain invalid INI syntax.");
            string key = TrimAsciiSpaceAndTabEnd(line[..equals]);
            if (key.Length == 0)
                throw Error(lineNumber, "A project setting has an empty key.");
            if (Utf8Length(key, "Project settings key") > MaximumKeyUtf8Bytes)
                throw Error(lineNumber, "Project settings contain an overlong key.");

            string value = TrimAsciiSpaceAndTabStart(line[(equals + 1)..]);
            if (Utf8Length(value, "Project settings value") > MaximumValueUtf8Bytes)
                throw Error(lineNumber, "Project settings contain an overlong value.");
            if (entries.Count >= MaximumEntries)
                throw Error(lineNumber, "Project settings contain too many entries.");

            var identity = new ProjectSettingKey(section, key);
            if (!entries.TryAdd(identity, value))
            {
                throw Error(
                    lineNumber,
                    $"Project settings contain duplicate key {section}.{key}.");
            }
        }
        return entries;
    }

    internal static void EnsureSourceEntriesPreserved(
        string sourceText,
        string canonicalText)
    {
        IReadOnlyDictionary<ProjectSettingKey, string> source = Parse(sourceText);
        IReadOnlyDictionary<ProjectSettingKey, string> canonical = Parse(canonicalText);
        foreach ((ProjectSettingKey key, string sourceValue) in source)
        {
            if (!canonical.TryGetValue(key, out string? canonicalValue) ||
                !string.Equals(sourceValue, canonicalValue, StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    $"The native settings parser rejected or changed {key.Section}.{key.Key}.");
            }
        }
    }

    private static int Utf8Length(string value, string description)
    {
        try
        {
            return StrictUtf8NoBom.GetByteCount(value);
        }
        catch (EncoderFallbackException error)
        {
            throw new InvalidDataException(
                description + " contain invalid Unicode.",
                error);
        }
    }

    private static string TrimAsciiSpaceAndTab(string value) =>
        TrimAsciiSpaceAndTabEnd(TrimAsciiSpaceAndTabStart(value));

    private static string TrimAsciiSpaceAndTabStart(string value)
    {
        int start = 0;
        while (start < value.Length && value[start] is ' ' or '\t')
            start++;
        return value[start..];
    }

    private static string TrimAsciiSpaceAndTabEnd(string value)
    {
        int end = value.Length;
        while (end > 0 && value[end - 1] is ' ' or '\t')
            end--;
        return value[..end];
    }

    private static InvalidDataException Error(int line, string detail) =>
        new($"{detail} (line {line})");
}
