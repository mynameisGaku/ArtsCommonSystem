// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;

namespace AcsEditor;

internal enum SceneDocumentMode
{
    TwoD,
    ThreeD,
}

internal sealed record SceneAutosaveIdentity(
    string ProjectId,
    string DocumentId,
    string ProjectPath,
    string? OriginalPath,
    SceneDocumentMode Mode);

internal sealed record SceneAutosaveCapture(
    SceneAutosaveIdentity Identity,
    string Content,
    DateTimeOffset CapturedUtc);

internal sealed record SceneRecoveryCandidate(
    SceneAutosaveIdentity Identity,
    string MetadataPath,
    string SnapshotPath,
    DateTimeOffset CapturedUtc,
    string ContentSha256,
    long ContentBytes);

/// <summary>
/// Immutable, project/document-scoped crash-recovery storage. Snapshot bytes are committed before
/// their metadata, so discovery never treats a partial write as recoverable. Source scenes are
/// never opened for write by this service.
/// </summary>
internal sealed class SceneAutosaveStore
{
    private const int FormatVersion = 1;
    private const long MaxSnapshotBytes = 128L * 1024 * 1024;
    private static readonly UTF8Encoding Utf8NoBom = new(false);
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = false,
    };

    private readonly string _root;
    private readonly string _rootPrefix;
    private readonly int _retention;

    private sealed class RecoveryMetadata
    {
        public int Version { get; set; }
        public string ProjectId { get; set; } = "";
        public string DocumentId { get; set; } = "";
        public string ProjectPath { get; set; } = "";
        public string? OriginalPath { get; set; }
        public string Mode { get; set; } = "";
        public DateTimeOffset CapturedUtc { get; set; }
        public string SnapshotFile { get; set; } = "";
        public string ContentSha256 { get; set; } = "";
        public long ContentBytes { get; set; }
    }

    internal SceneAutosaveStore(string? root = null, int retention = 5)
    {
        string defaultRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "ACS", "Editor", "Recovery", "v1");
        _root = Path.GetFullPath(string.IsNullOrWhiteSpace(root) ? defaultRoot : root);
        _rootPrefix = _root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                    + Path.DirectorySeparatorChar;
        _retention = Math.Clamp(retention, 1, 20);
    }

    internal static SceneAutosaveIdentity CreateIdentity(
        string projectPath,
        string? originalPath,
        SceneDocumentMode mode)
    {
        string normalizedProject = NormalizeIdentityPath(projectPath);
        string? normalizedScene = string.IsNullOrWhiteSpace(originalPath)
            ? null
            : NormalizeIdentityPath(originalPath);
        string projectId = Sha256Hex("PROJECT\n" + normalizedProject);
        string documentKey = normalizedScene == null ? "UNTITLED" : "PATH\n" + normalizedScene;
        string documentId = Sha256Hex(
            "DOCUMENT\n" + projectId + "\n" + ModeText(mode) + "\n" + documentKey);
        return new SceneAutosaveIdentity(
            projectId,
            documentId,
            Path.GetFullPath(projectPath),
            normalizedScene == null ? null : Path.GetFullPath(originalPath!),
            mode);
    }

    internal static string ComputeContentSha256(string content) =>
        Sha256Hex(Utf8NoBom.GetBytes(content ?? ""));

    internal SceneRecoveryCandidate WriteSnapshot(
        SceneAutosaveCapture capture,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(capture);
        cancellationToken.ThrowIfCancellationRequested();
        byte[] bytes = Utf8NoBom.GetBytes(capture.Content ?? "");
        if (bytes.LongLength > MaxSnapshotBytes)
            throw new InvalidDataException($"Scene recovery snapshot exceeds {MaxSnapshotBytes} bytes.");

        SceneAutosaveIdentity identity = capture.Identity;
        string entryDirectory = EntryDirectory(identity);
        EnsureSafeDirectory(entryDirectory);

        using FileStream entryLock = AcquireEntryLock(entryDirectory, cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        string token = capture.CapturedUtc.UtcTicks.ToString("D19", CultureInfo.InvariantCulture)
                     + "-" + Guid.NewGuid().ToString("N");
        string sceneExtension = identity.Mode == SceneDocumentMode.ThreeD ? ".acs3d" : ".acscene";
        string snapshotFile = "snapshot-" + token + sceneExtension;
        string metadataFile = "recovery-" + token + ".json";
        string snapshotPath = SafePath(entryDirectory, snapshotFile);
        string metadataPath = SafePath(entryDirectory, metadataFile);
        string checksum = Sha256Hex(bytes);

        WriteAtomicNew(snapshotPath, bytes, cancellationToken);
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            var metadata = new RecoveryMetadata
            {
                Version = FormatVersion,
                ProjectId = identity.ProjectId,
                DocumentId = identity.DocumentId,
                ProjectPath = identity.ProjectPath,
                OriginalPath = identity.OriginalPath,
                Mode = ModeText(identity.Mode),
                CapturedUtc = capture.CapturedUtc.ToUniversalTime(),
                SnapshotFile = snapshotFile,
                ContentSha256 = checksum,
                ContentBytes = bytes.LongLength,
            };
            WriteAtomicNew(
                metadataPath,
                Utf8NoBom.GetBytes(JsonSerializer.Serialize(metadata, JsonOptions)),
                cancellationToken);
        }
        catch
        {
            TryDeleteSafeFile(snapshotPath);
            throw;
        }

        PruneEntryLocked(entryDirectory, identity);
        return new SceneRecoveryCandidate(
            identity,
            metadataPath,
            snapshotPath,
            capture.CapturedUtc.ToUniversalTime(),
            checksum,
            bytes.LongLength);
    }

    internal SceneRecoveryCandidate? FindLatest(SceneAutosaveIdentity identity)
    {
        string entryDirectory = EntryDirectory(identity);
        if (!Directory.Exists(entryDirectory)) return null;
        EnsureExistingSafeDirectory(entryDirectory);
        using (FileStream entryLock = AcquireEntryLock(entryDirectory))
            PruneEntryLocked(entryDirectory, identity);

        return Directory.EnumerateFiles(
                entryDirectory, "recovery-*.json", SearchOption.TopDirectoryOnly)
            .Select(path => TryReadCandidate(path, identity))
            .Where(candidate => candidate != null)
            .OrderByDescending(candidate => candidate!.CapturedUtc)
            .FirstOrDefault();
    }

    internal string ReadVerifiedContent(SceneRecoveryCandidate candidate)
    {
        ArgumentNullException.ThrowIfNull(candidate);
        EnsureSafeFile(candidate.MetadataPath);
        EnsureSafeFile(candidate.SnapshotPath);
        byte[] bytes = ReadBounded(candidate.SnapshotPath, candidate.ContentBytes);
        if (!string.Equals(Sha256Hex(bytes), candidate.ContentSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Recovery snapshot checksum mismatch.");
        return Utf8NoBom.GetString(bytes);
    }

    internal void Discard(SceneAutosaveIdentity identity)
    {
        string entryDirectory = EntryDirectory(identity);
        if (!Directory.Exists(entryDirectory)) return;
        EnsureExistingSafeDirectory(entryDirectory);
        using FileStream entryLock = AcquireEntryLock(entryDirectory);

        foreach (string metadataPath in Directory.EnumerateFiles(
                     entryDirectory, "recovery-*.json", SearchOption.TopDirectoryOnly))
        {
            if (!IsSafeExistingFile(metadataPath)) continue;
            RecoveryMetadata? metadata = TryReadMetadata(metadataPath);
            if (metadata != null &&
                string.Equals(metadata.ProjectId, identity.ProjectId, StringComparison.Ordinal) &&
                string.Equals(metadata.DocumentId, identity.DocumentId, StringComparison.Ordinal) &&
                IsPlainFileName(metadata.SnapshotFile))
                TryDeleteSafeFile(SafePath(entryDirectory, metadata.SnapshotFile));
            TryDeleteSafeFile(metadataPath);
        }

        foreach (string snapshotPath in Directory.EnumerateFiles(
                     entryDirectory, "snapshot-*", SearchOption.TopDirectoryOnly))
            TryDeleteSafeFile(snapshotPath);
    }

    private SceneRecoveryCandidate? TryReadCandidate(
        string metadataPath,
        SceneAutosaveIdentity expectedIdentity)
    {
        try
        {
            EnsureSafeFile(metadataPath);
            RecoveryMetadata? metadata = TryReadMetadata(metadataPath);
            if (metadata == null ||
                metadata.Version != FormatVersion ||
                !string.Equals(metadata.ProjectId, expectedIdentity.ProjectId, StringComparison.Ordinal) ||
                !string.Equals(metadata.DocumentId, expectedIdentity.DocumentId, StringComparison.Ordinal) ||
                !string.Equals(metadata.Mode, ModeText(expectedIdentity.Mode), StringComparison.Ordinal) ||
                !PathsEqual(metadata.ProjectPath, expectedIdentity.ProjectPath) ||
                !OptionalPathsEqual(metadata.OriginalPath, expectedIdentity.OriginalPath) ||
                metadata.ContentBytes < 0 ||
                metadata.ContentBytes > MaxSnapshotBytes ||
                metadata.ContentSha256.Length != 64 ||
                !IsPlainFileName(metadata.SnapshotFile))
                return null;

            string entryDirectory = Path.GetDirectoryName(metadataPath)!;
            string snapshotPath = SafePath(entryDirectory, metadata.SnapshotFile);
            EnsureSafeFile(snapshotPath);
            byte[] bytes = ReadBounded(snapshotPath, metadata.ContentBytes);
            if (!string.Equals(
                    Sha256Hex(bytes), metadata.ContentSha256, StringComparison.OrdinalIgnoreCase))
                return null;

            return new SceneRecoveryCandidate(
                expectedIdentity,
                metadataPath,
                snapshotPath,
                metadata.CapturedUtc.ToUniversalTime(),
                metadata.ContentSha256,
                metadata.ContentBytes);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (InvalidDataException) { return null; }
        catch (JsonException) { return null; }
    }

    private static RecoveryMetadata? TryReadMetadata(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.Length <= 0 || info.Length > 128 * 1024) return null;
            return JsonSerializer.Deserialize<RecoveryMetadata>(
                File.ReadAllText(path, Utf8NoBom), JsonOptions);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (JsonException) { return null; }
    }

    private void PruneEntryLocked(string entryDirectory, SceneAutosaveIdentity identity)
    {
        var valid = new List<(string MetadataPath, RecoveryMetadata Metadata)>();
        foreach (string metadataPath in Directory.EnumerateFiles(
                     entryDirectory, "recovery-*.json", SearchOption.TopDirectoryOnly))
        {
            if (!IsSafeExistingFile(metadataPath)) continue;
            RecoveryMetadata? metadata = TryReadMetadata(metadataPath);
            if (metadata == null ||
                !string.Equals(metadata.ProjectId, identity.ProjectId, StringComparison.Ordinal) ||
                !string.Equals(metadata.DocumentId, identity.DocumentId, StringComparison.Ordinal) ||
                !IsPlainFileName(metadata.SnapshotFile))
            {
                TryDeleteSafeFile(metadataPath);
                continue;
            }
            valid.Add((metadataPath, metadata));
        }

        var keep = valid
            .OrderByDescending(item => item.Metadata.CapturedUtc)
            .Take(_retention)
            .ToList();
        var keepMetadata = new HashSet<string>(
            keep.Select(item => Path.GetFullPath(item.MetadataPath)),
            StringComparer.OrdinalIgnoreCase);
        var keepSnapshots = new HashSet<string>(
            keep.Select(item => Path.GetFullPath(SafePath(entryDirectory, item.Metadata.SnapshotFile))),
            StringComparer.OrdinalIgnoreCase);

        foreach ((string metadataPath, RecoveryMetadata metadata) in valid)
        {
            if (keepMetadata.Contains(Path.GetFullPath(metadataPath))) continue;
            TryDeleteSafeFile(SafePath(entryDirectory, metadata.SnapshotFile));
            TryDeleteSafeFile(metadataPath);
        }
        foreach (string snapshotPath in Directory.EnumerateFiles(
                     entryDirectory, "snapshot-*", SearchOption.TopDirectoryOnly))
            if (!keepSnapshots.Contains(Path.GetFullPath(snapshotPath)))
                TryDeleteSafeFile(snapshotPath);
        // A process/power loss can leave a same-directory atomic-write temporary behind.
        // Holding the entry lock guarantees it cannot belong to an active writer.
        foreach (string tempPath in Directory.EnumerateFiles(
                     entryDirectory, "*.tmp", SearchOption.TopDirectoryOnly))
            TryDeleteSafeFile(tempPath);
    }

    private FileStream AcquireEntryLock(
        string entryDirectory,
        CancellationToken cancellationToken = default)
    {
        string lockPath = SafePath(entryDirectory, ".recovery.lock");
        for (int attempt = 0; ; ++attempt)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                EnsureSafeFileOrMissing(lockPath);
                return new FileStream(
                    lockPath,
                    FileMode.OpenOrCreate,
                    FileAccess.ReadWrite,
                    FileShare.None,
                    1,
                    FileOptions.WriteThrough);
            }
            catch (IOException) when (attempt < 4)
            {
                if (cancellationToken.WaitHandle.WaitOne(25 * (attempt + 1)))
                    cancellationToken.ThrowIfCancellationRequested();
            }
        }
    }

    private void WriteAtomicNew(
        string finalPath,
        byte[] bytes,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        EnsureSafeFileOrMissing(finalPath);
        string directory = Path.GetDirectoryName(finalPath)!;
        string tempPath = SafePath(
            directory,
            "." + Path.GetFileName(finalPath) + "." + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var stream = new FileStream(
                       tempPath,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       64 * 1024,
                       FileOptions.WriteThrough))
            {
                const int chunkBytes = 64 * 1024;
                for (int offset = 0; offset < bytes.Length; offset += chunkBytes)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    int count = Math.Min(chunkBytes, bytes.Length - offset);
                    stream.Write(bytes, offset, count);
                }
                stream.Flush(flushToDisk: true);
            }
            cancellationToken.ThrowIfCancellationRequested();
            File.Move(tempPath, finalPath, overwrite: false);
        }
        finally
        {
            if (File.Exists(tempPath)) TryDeleteSafeFile(tempPath);
        }
    }

    private byte[] ReadBounded(string path, long expectedLength)
    {
        EnsureSafeFile(path);
        var info = new FileInfo(path);
        if (!info.Exists ||
            info.Length != expectedLength ||
            info.Length < 0 ||
            info.Length > MaxSnapshotBytes)
            throw new InvalidDataException("Recovery snapshot length mismatch.");
        return File.ReadAllBytes(path);
    }

    private string EntryDirectory(SceneAutosaveIdentity identity)
    {
        if (!IsLowerHex(identity.ProjectId, 64) || !IsLowerHex(identity.DocumentId, 64))
            throw new InvalidDataException("Recovery identity is malformed.");
        return SafePath(_root, identity.ProjectId, identity.DocumentId);
    }

    private string SafePath(string baseDirectory, params string[] parts)
    {
        string path = Path.GetFullPath(Path.Combine(new[] { baseDirectory }.Concat(parts).ToArray()));
        if (!IsWithinRoot(path))
            throw new InvalidDataException("Recovery path escapes its storage root.");
        return path;
    }

    private bool IsWithinRoot(string path)
    {
        string full = Path.GetFullPath(path);
        return string.Equals(
                   full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                   _root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                   StringComparison.OrdinalIgnoreCase)
               || full.StartsWith(_rootPrefix, StringComparison.OrdinalIgnoreCase);
    }

    private void EnsureSafeDirectory(string directory)
    {
        string full = Path.GetFullPath(directory);
        if (!IsWithinRoot(full))
            throw new InvalidDataException("Recovery directory escapes its storage root.");

        Directory.CreateDirectory(_root);
        RejectReparsePoint(_root);
        string relative = Path.GetRelativePath(_root, full);
        if (relative == ".") return;
        if (relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) ||
            relative == "..")
            throw new InvalidDataException("Recovery directory escapes its storage root.");

        string current = _root;
        foreach (string segment in relative.Split(
                     new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                     StringSplitOptions.RemoveEmptyEntries))
        {
            if (segment is "." or "..")
                throw new InvalidDataException("Recovery directory contains traversal.");
            current = Path.Combine(current, segment);
            Directory.CreateDirectory(current);
            RejectReparsePoint(current);
        }
    }

    private void EnsureExistingSafeDirectory(string directory)
    {
        string full = Path.GetFullPath(directory);
        if (!IsWithinRoot(full) || !Directory.Exists(full))
            throw new InvalidDataException("Recovery directory is outside its storage root.");
        string relative = Path.GetRelativePath(_root, full);
        string current = _root;
        RejectReparsePoint(current);
        if (relative == ".") return;
        foreach (string segment in relative.Split(
                     new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                     StringSplitOptions.RemoveEmptyEntries))
        {
            if (segment is "." or "..")
                throw new InvalidDataException("Recovery directory contains traversal.");
            current = Path.Combine(current, segment);
            RejectReparsePoint(current);
        }
    }

    private void EnsureSafeFile(string file)
    {
        string full = Path.GetFullPath(file);
        if (!IsWithinRoot(full))
            throw new InvalidDataException("Recovery file escapes its storage root.");
        EnsureExistingSafeDirectory(Path.GetDirectoryName(full)!);
        if (!File.Exists(full))
            throw new FileNotFoundException("Recovery file is missing.", full);
        RejectReparsePoint(full);
    }

    private void EnsureSafeFileOrMissing(string file)
    {
        string full = Path.GetFullPath(file);
        if (!IsWithinRoot(full))
            throw new InvalidDataException("Recovery file escapes its storage root.");
        EnsureExistingSafeDirectory(Path.GetDirectoryName(full)!);
        if (File.Exists(full)) RejectReparsePoint(full);
    }

    private bool IsSafeExistingFile(string file)
    {
        try
        {
            EnsureSafeFile(file);
            return true;
        }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
        catch (InvalidDataException) { return false; }
    }

    private void TryDeleteSafeFile(string file)
    {
        try
        {
            string full = Path.GetFullPath(file);
            if (!IsWithinRoot(full) || !File.Exists(full)) return;
            EnsureExistingSafeDirectory(Path.GetDirectoryName(full)!);
            RejectReparsePoint(full);
            File.Delete(full);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        catch (InvalidDataException) { }
    }

    private static void RejectReparsePoint(string path)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Recovery storage refuses reparse point: {path}");
    }

    private static bool IsPlainFileName(string value) =>
        !string.IsNullOrWhiteSpace(value) &&
        value is not "." and not ".." &&
        string.Equals(Path.GetFileName(value), value, StringComparison.Ordinal) &&
        value.IndexOfAny(new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar }) < 0 &&
        value.IndexOfAny(Path.GetInvalidFileNameChars()) < 0;

    private static bool IsLowerHex(string value, int length) =>
        value.Length == length && value.All(ch => ch is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static bool PathsEqual(string? a, string? b) =>
        a != null && b != null &&
        string.Equals(
            NormalizeIdentityPath(a),
            NormalizeIdentityPath(b),
            StringComparison.Ordinal);

    private static bool OptionalPathsEqual(string? a, string? b) =>
        a == null && b == null || PathsEqual(a, b);

    private static string NormalizeIdentityPath(string path)
    {
        string full = Path.GetFullPath(path)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        return OperatingSystem.IsWindows() ? full.ToUpperInvariant() : full;
    }

    private static string ModeText(SceneDocumentMode mode) =>
        mode == SceneDocumentMode.ThreeD ? "3D" : "2D";

    private static string Sha256Hex(string text) =>
        Sha256Hex(Utf8NoBom.GetBytes(text));

    private static string Sha256Hex(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    // Deterministic harness support; never used by production UI.
    internal string EntryDirectoryForTest(SceneAutosaveIdentity identity) => EntryDirectory(identity);
}
