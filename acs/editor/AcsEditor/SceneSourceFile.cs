// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>Atomic source-scene writes and project-path validation.</summary>
internal static class SceneSourceFile
{
    internal const int MaxSceneSourceBytes = 4 * 1024 * 1024;
    private static readonly UTF8Encoding StrictUtf8 =
        new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    internal readonly record struct ReadResult(bool Exists, string? Text);

    /// <summary>
    /// Opens one stable ordinary-file handle, bounds allocation before reading, rejects reparse
    /// points and invalid UTF-8, and verifies that the source did not change length mid-read.
    /// Missing sources are represented explicitly because a project may intentionally start empty.
    /// </summary>
    internal static async Task<ReadResult> ReadBoundedTextAsync(
        string path,
        CancellationToken cancellationToken = default)
    {
        string source = Path.GetFullPath(path);
        FileAttributes before;
        try
        {
            before = File.GetAttributes(source);
        }
        catch (FileNotFoundException)
        {
            return new ReadResult(false, null);
        }
        catch (DirectoryNotFoundException)
        {
            return new ReadResult(false, null);
        }
        if ((before & FileAttributes.Directory) != 0)
            throw new InvalidDataException($"Scene source is a directory: {source}");
        if ((before & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Scene source is a reparse point: {source}");

        FileStream stream;
        try
        {
            stream = new FileStream(
                source,
                new FileStreamOptions
                {
                    Mode = FileMode.Open,
                    Access = FileAccess.Read,
                    Share = FileShare.Read,
                    BufferSize = 64 * 1024,
                    Options = FileOptions.Asynchronous | FileOptions.SequentialScan,
                });
        }
        catch (FileNotFoundException)
        {
            return new ReadResult(false, null);
        }
        catch (DirectoryNotFoundException)
        {
            return new ReadResult(false, null);
        }

        await using (stream)
        {
            // Re-check through the now share-locked path. A replacement between the first
            // attribute read and handle open is caught here; FileShare.Read prevents later
            // writers/replacements from joining this read transaction.
            FileAttributes opened = File.GetAttributes(source);
            if ((opened & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
                throw new InvalidDataException(
                    $"Scene source must be an ordinary non-reparse file: {source}");

            long expectedLength = stream.Length;
            if (expectedLength > MaxSceneSourceBytes)
                throw new InvalidDataException(
                    $"Scene source exceeds the {MaxSceneSourceBytes} byte limit: {source}");
            if (expectedLength < 0 || expectedLength > int.MaxValue)
                throw new InvalidDataException($"Scene source length is invalid: {source}");

            var bytes = new byte[(int)expectedLength];
            int offset = 0;
            while (offset < bytes.Length)
            {
                int read = await stream.ReadAsync(
                    bytes.AsMemory(offset),
                    cancellationToken).ConfigureAwait(false);
                if (read == 0)
                    throw new EndOfStreamException(
                        $"Scene source changed or ended during read: {source}");
                offset += read;
            }

            var probe = new byte[1];
            int extra = await stream.ReadAsync(
                probe.AsMemory(),
                cancellationToken).ConfigureAwait(false);
            if (extra != 0 || stream.Length != expectedLength)
                throw new IOException($"Scene source changed during read: {source}");

            int start = bytes.Length >= 3 &&
                        bytes[0] == 0xEF &&
                        bytes[1] == 0xBB &&
                        bytes[2] == 0xBF
                ? 3
                : 0;
            try
            {
                return new ReadResult(
                    true,
                    StrictUtf8.GetString(bytes, start, bytes.Length - start));
            }
            catch (DecoderFallbackException ex)
            {
                throw new InvalidDataException(
                    $"Scene source is not valid UTF-8: {source}",
                    ex);
            }
        }
    }

    internal static void ValidateProjectRootDirectory(string projectRoot)
    {
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        if (!Directory.Exists(root))
            throw new DirectoryNotFoundException(root);
        CheckDirectory(root);
    }

    internal static string ResolveProjectSceneReference(
        string projectRoot,
        string assetsRoot,
        string reference,
        SceneDocumentMode? expectedMode = null)
    {
        if (string.IsNullOrWhiteSpace(reference))
            throw new InvalidDataException("Scene reference is empty.");

        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        string assets = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
        string expectedAssets = Path.Combine(root, "Assets");
        if (!PathsEqual(assets, expectedAssets))
            throw new InvalidDataException(
                $"Project Assets directory must be the Assets folder directly under the project root: {assets}");
        ValidateExistingSafeDirectoryTree(root, assets);

        string value = reference.Trim().Replace('/', Path.DirectorySeparatorChar);
        if (Path.IsPathRooted(value))
            throw new InvalidDataException(
                $"Project scene references must be relative paths under Assets: {reference}");

        string destination = Path.GetFullPath(Path.Combine(root, value));
        return ValidateProjectScenePath(destination, assets, expectedMode);
    }

    internal static string NormalizeProjectSceneReference(
        string projectRoot,
        string assetsRoot,
        string reference,
        SceneDocumentMode? expectedMode = null)
    {
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        string destination = ResolveProjectSceneReference(
            root,
            assetsRoot,
            reference,
            expectedMode);
        string relative = Path.GetRelativePath(root, destination);
        if (Path.IsPathRooted(relative) ||
            string.Equals(relative, "..", StringComparison.Ordinal) ||
            relative.StartsWith(
                ".." + Path.DirectorySeparatorChar,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Scene reference escapes the project root: {reference}");
        }
        return relative.Replace(Path.DirectorySeparatorChar, '/');
    }

    internal static string ValidateScenePath(
        string path,
        SceneDocumentMode expectedMode)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new InvalidDataException("Scene path is empty.");
        string destination = Path.GetFullPath(path);
        ValidateSceneExtension(destination, expectedMode);
        return destination;
    }

    internal static string ValidateProjectScenePath(
        string path,
        string assetsRoot,
        SceneDocumentMode? expectedMode = null)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new InvalidDataException("Scene path is empty.");

        string destination = Path.GetFullPath(path);
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
        if (!IsUnder(destination, root))
            throw new InvalidDataException(
                $"Scene target must be inside the project Assets directory: {destination}");

        ValidateSceneExtension(destination, expectedMode);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new InvalidDataException("Scene path has no parent directory.");
        ValidateExistingSafeDirectoryTree(root, parent);
        RejectExistingReparseFile(destination);
        return destination;
    }

    internal static string ValidateProjectScenePathForProject(
        string path,
        string projectRoot,
        string assetsRoot,
        SceneDocumentMode expectedMode)
    {
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        string assets = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
        string expectedAssets = Path.Combine(root, "Assets");
        if (!PathsEqual(assets, expectedAssets))
            throw new InvalidDataException(
                $"Project Assets directory must be directly under the project root: {assets}");
        ValidateExistingSafeDirectoryTree(root, assets);
        return ValidateProjectScenePath(path, assets, expectedMode);
    }

    /// <summary>
    /// Single durability boundary for a project scene. Project root, the direct Assets directory,
    /// every existing parent segment, mode extension, and target reparse state are revalidated
    /// immediately before the atomic writer creates its same-directory temporary file.
    /// </summary>
    internal static void WriteProjectSceneAtomicText(
        string path,
        string content,
        string projectRoot,
        string assetsRoot,
        SceneDocumentMode expectedMode)
    {
        string destination = ValidateProjectScenePathForProject(
            path,
            projectRoot,
            assetsRoot,
            expectedMode);
        using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
            assetsRoot,
            "Save scene source");
        WriteAtomicText(destination, content, assetsRoot, expectedMode);
    }

    internal static void WriteAtomicText(
        string path,
        string content,
        string? requiredRoot = null,
        SceneDocumentMode? expectedMode = null)
    {
        string destination = expectedMode is { } mode
            ? ValidateScenePath(path, mode)
            : Path.GetFullPath(path);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new InvalidDataException("Scene path has no parent directory.");

        if (requiredRoot != null)
        {
            string root = Path.GetFullPath(requiredRoot);
            destination = ValidateProjectScenePath(destination, root, expectedMode);
            parent = Path.GetDirectoryName(destination)
                ?? throw new InvalidDataException("Scene path has no parent directory.");
            EnsureSafeDirectoryTree(root, parent);
        }
        else
        {
            Directory.CreateDirectory(parent);
        }

        RejectExistingReparseFile(destination);
        string temp = Path.Combine(
            parent,
            "." + Path.GetFileName(destination) + "." + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            byte[] bytes = new UTF8Encoding(false, true).GetBytes(content ?? "");
            using (var stream = new FileStream(
                       temp,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       64 * 1024,
                       FileOptions.WriteThrough))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            RejectExistingReparseFile(destination);
            File.Move(temp, destination, overwrite: true);
        }
        finally
        {
            try { if (File.Exists(temp)) File.Delete(temp); } catch { }
        }
    }

    internal static void CopyAtomic(
        string source,
        string destination,
        string? requiredDestinationRoot = null)
    {
        string sourceFull = Path.GetFullPath(source);
        if (!File.Exists(sourceFull))
            throw new FileNotFoundException("Scene source was not found.", sourceFull);
        FileAttributes sourceAttributes = File.GetAttributes(sourceFull);
        if ((sourceAttributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            throw new InvalidDataException("Scene source must be an ordinary file.");

        string destinationFull = Path.GetFullPath(destination);
        string parent = Path.GetDirectoryName(destinationFull)
            ?? throw new InvalidDataException("Scene destination has no parent directory.");
        if (requiredDestinationRoot != null)
        {
            string root = Path.GetFullPath(requiredDestinationRoot);
            if (!IsUnder(destinationFull, root))
                throw new InvalidDataException("Scene destination escapes its required root.");
            EnsureSafeDirectoryTree(root, parent);
        }
        else
        {
            Directory.CreateDirectory(parent);
        }

        RejectExistingReparseFile(destinationFull);
        string temp = Path.Combine(
            parent,
            "." + Path.GetFileName(destinationFull) + "." + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var input = new FileStream(
                       sourceFull, FileMode.Open, FileAccess.Read, FileShare.Read, 64 * 1024,
                       FileOptions.SequentialScan))
            using (var output = new FileStream(
                       temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 64 * 1024,
                       FileOptions.WriteThrough))
            {
                input.CopyTo(output);
                output.Flush(flushToDisk: true);
            }
            RejectExistingReparseFile(destinationFull);
            File.Move(temp, destinationFull, overwrite: true);
        }
        finally
        {
            try { if (File.Exists(temp)) File.Delete(temp); } catch { }
        }
    }

    internal static bool PathsEqual(string? first, string? second)
    {
        if (string.IsNullOrWhiteSpace(first) || string.IsNullOrWhiteSpace(second))
            return false;
        return string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(first)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(second)),
            StringComparison.OrdinalIgnoreCase);
    }

    internal static bool IsUnder(string path, string root)
    {
        string fullPath = Path.GetFullPath(path);
        string fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string prefix = fullRoot + Path.DirectorySeparatorChar;
        return fullPath.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
    }

    private static void ValidateSceneExtension(
        string path,
        SceneDocumentMode? expectedMode)
    {
        string extension = Path.GetExtension(path);
        if (expectedMode is { } mode)
        {
            string expected = mode == SceneDocumentMode.ThreeD ? ".acs3d" : ".acscene";
            if (!string.Equals(extension, expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(
                    $"{(mode == SceneDocumentMode.ThreeD ? "3D" : "2D")} scenes must use the {expected} extension.");
            return;
        }

        if (!string.Equals(extension, ".acscene", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(extension, ".acs3d", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Project scene references must use the .acscene or .acs3d extension.");
        }
    }

    private static void ValidateExistingSafeDirectoryTree(
        string root,
        string destinationDirectory)
    {
        string fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string fullDestination = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(destinationDirectory));
        if (!string.Equals(fullDestination, fullRoot, StringComparison.OrdinalIgnoreCase) &&
            !IsUnder(fullDestination, fullRoot))
            throw new InvalidDataException("Directory target escapes its required root.");
        if (!Directory.Exists(fullRoot))
            throw new DirectoryNotFoundException(fullRoot);

        CheckDirectory(fullRoot);
        string relative = Path.GetRelativePath(fullRoot, fullDestination);
        string cursor = fullRoot;
        foreach (string segment in relative.Split(
                     [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                     StringSplitOptions.RemoveEmptyEntries))
        {
            cursor = Path.Combine(cursor, segment);
            if (Directory.Exists(cursor))
            {
                CheckDirectory(cursor);
                continue;
            }
            if (File.Exists(cursor))
                throw new InvalidDataException($"Expected a directory: {cursor}");
            break;
        }
    }

    private static void EnsureSafeDirectoryTree(string root, string destinationDirectory)
    {
        string fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string fullDestination = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(destinationDirectory));
        if (!string.Equals(fullDestination, fullRoot, StringComparison.OrdinalIgnoreCase) &&
            !IsUnder(fullDestination, fullRoot))
            throw new InvalidDataException("Directory target escapes its required root.");
        if (!Directory.Exists(fullRoot))
            throw new DirectoryNotFoundException(fullRoot);

        CheckDirectory(fullRoot);
        string relative = Path.GetRelativePath(fullRoot, fullDestination);
        string cursor = fullRoot;
        foreach (string segment in relative.Split(
                     [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                     StringSplitOptions.RemoveEmptyEntries))
        {
            cursor = Path.Combine(cursor, segment);
            if (!Directory.Exists(cursor))
                Directory.CreateDirectory(cursor);
            CheckDirectory(cursor);
        }
    }

    private static void CheckDirectory(string path)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0)
            throw new InvalidDataException($"Expected a directory: {path}");
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Reparse directory is not allowed: {path}");
    }

    private static void RejectExistingReparseFile(string path)
    {
        if (!File.Exists(path) && !Directory.Exists(path)) return;
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) != 0)
            throw new InvalidDataException($"Scene target is a directory: {path}");
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Scene target is a reparse point: {path}");
    }
}
