// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AcsEditor;

internal enum ProjectSceneReferenceCommitPoint
{
    BeforeSettingsPublish,
    AfterSettingsPublish,
    BeforeManifestPublish,
    AfterManifestPublish,
}

internal sealed record ProjectSceneReferenceUpdate(
    string PreviousReference,
    string CurrentReference,
    bool SettingsFileCreated,
    string DurableSettingsSource);

internal sealed record ProjectSettingsSaveCommit(
    string DurableSettingsSource,
    string AuthoritativeInitialScene);

/// <summary>
/// Captures the bounded UTF-8 result of the native project-settings serializer.
/// Native <c>SerializeText</c> reports <c>cap - 1</c> for a truncated result, so the
/// buffer includes one probe byte beyond the largest legal payload and its NUL terminator.
/// This makes every over-limit result distinguishable from a legal 1 MiB payload.
/// </summary>
internal static class ProjectSettingsSerialization
{
    internal const int MaximumUtf8Bytes = 1024 * 1024;
    private const int OverflowProbeBytes = 1;
    private static readonly UTF8Encoding StrictUtf8NoBom = new(false, true);

    internal static string Capture(Func<byte[], int> serialize)
    {
        ArgumentNullException.ThrowIfNull(serialize);

        var buffer = new byte[
            MaximumUtf8Bytes +
            1 + // NUL terminator
            OverflowProbeBytes];

        // A fresh byte array is zero-filled. Poison it so a serializer that forgets to
        // publish its promised terminator cannot accidentally pass validation.
        Array.Fill(buffer, byte.MaxValue);
        int written = serialize(buffer);

        if (written <= 0)
        {
            throw new InvalidDataException(
                "The native project-settings serializer returned no data.");
        }
        if (written >= buffer.Length)
        {
            throw new InvalidDataException(
                "The native project-settings serializer returned an invalid byte count.");
        }
        if (written > MaximumUtf8Bytes)
        {
            throw new InvalidDataException(
                "Serialized project settings exceed the persistence limit.");
        }
        if (buffer[written] != 0)
        {
            throw new InvalidDataException(
                "The native project-settings serializer did not NUL-terminate its result.");
        }
        if (Array.IndexOf(buffer, (byte)0, 0, written) >= 0)
        {
            throw new InvalidDataException(
                "Serialized project settings contain an embedded NUL.");
        }

        try
        {
            return StrictUtf8NoBom.GetString(buffer, 0, written);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "Serialized project settings are not valid UTF-8.",
                error);
        }
    }
}

public static partial class ProjectManager
{
    private const int MaxProjectManifestBytes = 1024 * 1024;
    private const int MaxProjectSettingsBytes =
        ProjectSettingsSerialization.MaximumUtf8Bytes;
    private static readonly UTF8Encoding StrictUtf8NoBom = new(false, true);

    private sealed record ReferenceFileSnapshot(
        string Path,
        bool Existed,
        byte[] Bytes,
        FileAttributes Attributes,
        long LastWriteUtcTicks);

    private sealed record IniLine(string Text, string Ending);

    /// <summary>
    /// Verifies that the two persistent startup-scene references are coherent and can participate
    /// in a path-follow transaction. This is intentionally read-only so Content Browser preflight
    /// can veto a rename/move before the asset transaction starts.
    /// </summary>
    internal static void ValidateInitialSceneReferenceFollow(Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        string previousReference = NormalizeCurrentInitialSceneReference(project);
        ReferenceFileSnapshot manifest = CaptureRequiredOrdinaryFile(
            project.ProjectFilePath,
            MaxProjectManifestBytes,
            ".acsproject manifest");
        ValidateManifestSnapshot(project, manifest, previousReference);

        string settingsPath = GetProjectSettingsPath(project);
        ValidateSettingsStoragePath(project, settingsPath, createDirectory: false);
        ReferenceFileSnapshot settings = CaptureOptionalOrdinaryFile(
            settingsPath,
            MaxProjectSettingsBytes,
            "Project settings");
        _ = RewriteDefaultSceneSetting(
            settings.Existed ? settings.Bytes : Array.Empty<byte>(),
            project,
            previousReference,
            previousReference);
    }

    /// <summary>
    /// Follows a successfully committed scene rename/move in both the project manifest and
    /// Game.DefaultScene. Both files are staged before either is published. A failure after the
    /// first publication restores the original bytes before returning an error, so callers never
    /// mutate the in-memory <see cref="Project.InitialScene"/> on a partial save.
    /// </summary>
    internal static ProjectSceneReferenceUpdate FollowInitialScenePath(
        Project project,
        string destinationScenePath) =>
        FollowInitialScenePath(project, destinationScenePath, faultInjector: null);

    internal static ProjectSceneReferenceUpdate FollowInitialScenePath(
        Project project,
        string destinationScenePath,
        Action<ProjectSceneReferenceCommitPoint>? faultInjector)
    {
        ArgumentNullException.ThrowIfNull(project);
        if (string.IsNullOrWhiteSpace(destinationScenePath))
            throw new ArgumentException(
                "Destination scene path is required.",
                nameof(destinationScenePath));

        // The manifest and ProjectSettings.ini form one persistent startup-scene record. Keep
        // their snapshot, compare-and-swap publication, and rollback inside the same project-wide
        // lease so two editor processes cannot interleave the two files. This path runs on the UI
        // dispatcher, so contention must fail before any write instead of waiting on a worker that
        // may itself need the dispatcher.
        using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
            project.AssetsDir,
            "Update initial scene references");
        string previousReference = NormalizeCurrentInitialSceneReference(project);
        string destination = SceneSourceFile.ValidateProjectScenePath(
            destinationScenePath,
            project.AssetsDir);
        if (!File.Exists(destination))
            throw new FileNotFoundException(
                "The committed destination scene does not exist.",
                destination);
        FileAttributes destinationAttributes = File.GetAttributes(destination);
        if ((destinationAttributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException(
                "The committed destination scene must be an ordinary file.");
        }

        string relativeDestination = Path.GetRelativePath(project.RootDir, destination)
            .Replace(Path.DirectorySeparatorChar, '/');
        string currentReference = SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            relativeDestination);
        if (StrictUtf8NoBom.GetByteCount(currentReference) >= 192)
        {
            throw new InvalidDataException(
                "The initial-scene reference is too long for Game.DefaultScene.");
        }
        if (string.Equals(
                previousReference,
                currentReference,
                StringComparison.OrdinalIgnoreCase))
        {
            return new ProjectSceneReferenceUpdate(
                previousReference,
                previousReference,
                SettingsFileCreated: false,
                DurableSettingsSource: "");
        }

        ReferenceFileSnapshot manifest = CaptureRequiredOrdinaryFile(
            project.ProjectFilePath,
            MaxProjectManifestBytes,
            ".acsproject manifest");
        ValidateManifestSnapshot(project, manifest, previousReference);

        string settingsPath = GetProjectSettingsPath(project);
        ValidateSettingsStoragePath(project, settingsPath, createDirectory: false);
        ReferenceFileSnapshot settings = CaptureOptionalOrdinaryFile(
            settingsPath,
            MaxProjectSettingsBytes,
            "Project settings");

        byte[] updatedManifest = RewriteManifestInitialScene(
            manifest.Bytes,
            currentReference);
        byte[] updatedSettings = RewriteDefaultSceneSetting(
            settings.Existed ? settings.Bytes : Array.Empty<byte>(),
            project,
            previousReference,
            currentReference);
        if (updatedManifest.LongLength > MaxProjectManifestBytes ||
            updatedSettings.LongLength > MaxProjectSettingsBytes)
        {
            throw new InvalidDataException(
                "Updated project startup-scene state exceeds its persistence limit.");
        }

        ValidateSettingsStoragePath(project, settingsPath, createDirectory: true);
        string manifestTemporary = CreateSiblingTemporaryPath(manifest.Path);
        string settingsTemporary = CreateSiblingTemporaryPath(settings.Path);
        bool settingsPublished = false;
        bool manifestPublished = false;
        try
        {
            WriteTemporaryBytes(manifestTemporary, updatedManifest);
            WriteTemporaryBytes(settingsTemporary, updatedSettings);
            EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
            EnsureSnapshotUnchanged(settings, "Project settings");

            faultInjector?.Invoke(ProjectSceneReferenceCommitPoint.BeforeSettingsPublish);
            PublishTemporary(settingsTemporary, settings.Path, settings);
            settingsPublished = true;
            faultInjector?.Invoke(ProjectSceneReferenceCommitPoint.AfterSettingsPublish);

            EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
            faultInjector?.Invoke(ProjectSceneReferenceCommitPoint.BeforeManifestPublish);
            PublishTemporary(manifestTemporary, manifest.Path, manifest);
            manifestPublished = true;
            faultInjector?.Invoke(ProjectSceneReferenceCommitPoint.AfterManifestPublish);

            project.InitialScene = currentReference;
            return new ProjectSceneReferenceUpdate(
                previousReference,
                currentReference,
                SettingsFileCreated: !settings.Existed,
                DurableSettingsSource:
                    StrictUtf8NoBom.GetString(updatedSettings));
        }
        catch (Exception error)
        {
            bool rollbackComplete = true;
            if (manifestPublished)
            {
                rollbackComplete &= TryRestoreSnapshot(
                    manifest,
                    updatedManifest);
            }
            if (settingsPublished)
            {
                rollbackComplete &= TryRestoreSnapshot(
                    settings,
                    updatedSettings);
            }
            if (!rollbackComplete)
            {
                throw new IOException(
                    "Startup-scene reference update failed and byte-for-byte rollback " +
                    "was incomplete. Build/Run remains fail-closed until the project " +
                    "manifest and Game.DefaultScene are repaired.",
                    error);
            }
            throw;
        }
        finally
        {
            TryDeleteOrdinaryFile(manifestTemporary);
            TryDeleteOrdinaryFile(settingsTemporary);
        }
    }

    /// <summary>
    /// Atomically publishes the engine's serialized project settings without allowing a stale
    /// editor process to overwrite the startup-scene reference committed by another editor.
    /// The manifest is authoritative for Game.DefaultScene; unrelated serialized settings remain
    /// last-writer-wins, while both project writers share the same cross-process mutation lease.
    /// </summary>
    internal static ProjectSettingsSaveCommit SaveProjectSettings(
        Project project,
        string serializedSettings)
    {
        ArgumentNullException.ThrowIfNull(project);
        ArgumentNullException.ThrowIfNull(serializedSettings);

        byte[] serializedBytes = StrictUtf8NoBom.GetBytes(serializedSettings);
        if (serializedBytes.LongLength > MaxProjectSettingsBytes)
        {
            throw new InvalidDataException(
                "Serialized project settings exceed the persistence limit.");
        }

        using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
            project.AssetsDir,
            "Save project settings");

        ReferenceFileSnapshot manifest = CaptureRequiredOrdinaryFile(
            project.ProjectFilePath,
            MaxProjectManifestBytes,
            ".acsproject manifest");
        Project persisted = ParseManifestSnapshot(
            manifest.Path,
            manifest.Bytes);
        EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
        if (!string.Equals(
                persisted.CanonicalSceneAssetId,
                project.CanonicalSceneAssetId,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The project manifest identity changed outside this editor. Reload the " +
                "project before saving settings.");
        }

        string authoritativeReference = persisted.InitialScene;
        string localReference = NormalizeCurrentInitialSceneReference(project);
        string settingsPath = GetProjectSettingsPath(project);
        ValidateSettingsStoragePath(project, settingsPath, createDirectory: false);
        ReferenceFileSnapshot settings = CaptureOptionalOrdinaryFile(
            settingsPath,
            MaxProjectSettingsBytes,
            "Project settings");

        // Reject a pre-existing split-brain record before publishing anything. The serialized
        // engine state may legitimately come from an editor opened before another editor moved
        // the startup scene, so rewrite that one value to the authoritative manifest reference.
        _ = RewriteDefaultSceneSetting(
            settings.Existed ? settings.Bytes : Array.Empty<byte>(),
            project,
            authoritativeReference,
            authoritativeReference);
        byte[] updatedSettings = RewriteDefaultSceneSetting(
            serializedBytes,
            project,
            localReference,
            authoritativeReference);
        if (updatedSettings.LongLength > MaxProjectSettingsBytes)
        {
            throw new InvalidDataException(
                "Updated project settings exceed the persistence limit.");
        }

        ValidateSettingsStoragePath(project, settingsPath, createDirectory: true);
        string temporary = CreateSiblingTemporaryPath(settings.Path);
        try
        {
            WriteTemporaryBytes(temporary, updatedSettings);
            EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
            PublishTemporary(temporary, settings.Path, settings);
            return new ProjectSettingsSaveCommit(
                StrictUtf8NoBom.GetString(updatedSettings),
                authoritativeReference);
        }
        finally
        {
            TryDeleteOrdinaryFile(temporary);
        }
    }

    private static string NormalizeCurrentInitialSceneReference(Project project) =>
        SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            project.InitialScene);

    private static string GetProjectSettingsPath(Project project) =>
        Path.Combine(project.RootDir, "Config", "ProjectSettings.ini");

    private static void ValidateManifestSnapshot(
        Project project,
        ReferenceFileSnapshot manifest,
        string expectedReference)
    {
        Project persisted = ParseManifestSnapshot(
            manifest.Path,
            manifest.Bytes);
        EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
        if (!string.Equals(
                persisted.InitialScene,
                expectedReference,
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                persisted.CanonicalSceneAssetId,
                project.CanonicalSceneAssetId,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The project manifest changed outside this editor. Reload the project " +
                "before renaming or moving its initial scene.");
        }
    }

    private static ReferenceFileSnapshot CaptureRequiredOrdinaryFile(
        string path,
        int maximumBytes,
        string label,
        bool requireWritable = true)
    {
        string full = Path.GetFullPath(path);
        if (!File.Exists(full))
            throw new FileNotFoundException($"{label} was not found.", full);
        return CaptureOrdinaryFile(
            full,
            maximumBytes,
            label,
            existed: true,
            requireWritable);
    }

    private static ReferenceFileSnapshot CaptureOptionalOrdinaryFile(
        string path,
        int maximumBytes,
        string label)
    {
        string full = Path.GetFullPath(path);
        if (!File.Exists(full))
        {
            if (Directory.Exists(full))
                throw new InvalidDataException($"{label} path is a directory: {full}");
            return new ReferenceFileSnapshot(
                full,
                Existed: false,
                Array.Empty<byte>(),
                default,
                0);
        }
        return CaptureOrdinaryFile(
            full,
            maximumBytes,
            label,
            existed: true,
            requireWritable: true);
    }

    private static ReferenceFileSnapshot CaptureOrdinaryFile(
        string full,
        int maximumBytes,
        string label,
        bool existed,
        bool requireWritable)
    {
        FileAttributes attributes = File.GetAttributes(full);
        if ((attributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException($"{label} must be an ordinary file: {full}");
        }
        if (requireWritable && (attributes & FileAttributes.ReadOnly) != 0)
            throw new IOException($"{label} is read-only: {full}");

        // Hold the opened file against writers/replacement while capturing it. Length comes from
        // the handle and the read loop is bounded, so a path swap/growth cannot turn a preflight
        // stat into an unbounded allocation.
        using var stream = new FileStream(
            full,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 64 * 1024,
            FileOptions.SequentialScan);
        attributes = File.GetAttributes(full);
        if ((attributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException($"{label} must be an ordinary file: {full}");
        }
        if (requireWritable && (attributes & FileAttributes.ReadOnly) != 0)
            throw new IOException($"{label} is read-only: {full}");
        long length = stream.Length;
        if (length < 0 || length > maximumBytes)
            throw new InvalidDataException($"{label} exceeds {maximumBytes} bytes.");
        var bytes = new byte[(int)length];
        int offset = 0;
        while (offset < bytes.Length)
        {
            int read = stream.Read(bytes, offset, bytes.Length - offset);
            if (read == 0)
                throw new IOException($"{label} changed while it was being read.");
            offset += read;
        }
        if (stream.ReadByte() != -1 || stream.Length != bytes.LongLength)
        {
            throw new IOException($"{label} changed while it was being read.");
        }
        return new ReferenceFileSnapshot(
            full,
            existed,
            bytes,
            attributes,
            File.GetLastWriteTimeUtc(full).Ticks);
    }

    private static void ValidateSettingsStoragePath(
        Project project,
        string settingsPath,
        bool createDirectory)
    {
        SceneSourceFile.ValidateProjectRootDirectory(project.RootDir);
        string config = Path.GetDirectoryName(settingsPath)
            ?? throw new InvalidDataException("Project settings path has no parent directory.");
        if (File.Exists(config))
            throw new InvalidDataException(
                "The project Config path is a file, not a directory.");
        if (!Directory.Exists(config))
        {
            if (!createDirectory)
                return;
            Directory.CreateDirectory(config);
        }
        FileAttributes configAttributes = File.GetAttributes(config);
        if ((configAttributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) !=
            FileAttributes.Directory)
        {
            throw new InvalidDataException(
                "The project Config directory cannot be a reparse point.");
        }
        if (!File.Exists(settingsPath)) return;
        FileAttributes settingsAttributes = File.GetAttributes(settingsPath);
        if ((settingsAttributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException(
                "ProjectSettings.ini must be an ordinary file.");
        }
    }

    private static byte[] RewriteManifestInitialScene(
        byte[] source,
        string currentReference)
    {
        ReadOnlySpan<byte> payload = source;
        if (payload.StartsWith(new byte[] { 0xEF, 0xBB, 0xBF }))
            payload = payload[3..];
        ValidateManifestJson(payload);

        JsonNode? node;
        try
        {
            node = JsonNode.Parse(
                payload.ToArray(),
                documentOptions: new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                    MaxDepth = 32,
                });
        }
        catch (JsonException error)
        {
            throw new InvalidDataException(
                "The .acsproject manifest is not valid JSON.",
                error);
        }
        if (node is not JsonObject root)
            throw new InvalidDataException(
                "The .acsproject manifest root must be a JSON object.");
        root["initialScene"] = currentReference;
        string json = root.ToJsonString(JsonOpts) + Environment.NewLine;
        return StrictUtf8NoBom.GetBytes(json);
    }

    private static byte[] RewriteDefaultSceneSetting(
        byte[] source,
        Project project,
        string previousReference,
        string currentReference)
    {
        string text;
        try
        {
            ReadOnlySpan<byte> payload = source;
            if (payload.StartsWith(new byte[] { 0xEF, 0xBB, 0xBF }))
                payload = payload[3..];
            text = StrictUtf8NoBom.GetString(payload);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "ProjectSettings.ini is not valid UTF-8.",
                error);
        }
        if (text.IndexOf('\0') >= 0)
            throw new InvalidDataException(
                "ProjectSettings.ini contains an embedded NUL.");

        List<IniLine> lines = SplitIniLines(text);
        string section = "";
        int firstGameSection = -1;
        int gameSectionEnd = lines.Count;
        int defaultSceneLine = -1;
        for (int index = 0; index < lines.Count; index++)
        {
            string trimmed = lines[index].Text.Trim();
            if (trimmed.Length == 0 ||
                trimmed.StartsWith(';') ||
                trimmed.StartsWith('#'))
            {
                continue;
            }
            if (trimmed.StartsWith('[') && trimmed.EndsWith(']'))
            {
                string candidate = trimmed[1..^1].Trim();
                if (firstGameSection >= 0 &&
                    string.Equals(section, "Game", StringComparison.OrdinalIgnoreCase) &&
                    !string.Equals(candidate, "Game", StringComparison.OrdinalIgnoreCase))
                {
                    gameSectionEnd = index;
                }
                section = candidate;
                if (string.Equals(section, "Game", StringComparison.OrdinalIgnoreCase))
                {
                    if (!string.Equals(section, "Game", StringComparison.Ordinal))
                    {
                        throw new InvalidDataException(
                            "ProjectSettings.ini must use the canonical [Game] section name.");
                    }
                    if (firstGameSection < 0)
                    {
                        firstGameSection = index;
                        gameSectionEnd = lines.Count;
                    }
                }
                continue;
            }
            if (!string.Equals(section, "Game", StringComparison.OrdinalIgnoreCase))
                continue;
            int equals = trimmed.IndexOf('=');
            if (equals <= 0)
                continue;
            string key = trimmed[..equals].Trim();
            if (!string.Equals(
                    key,
                    "DefaultScene",
                    StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            if (defaultSceneLine >= 0)
                throw new InvalidDataException(
                    "ProjectSettings.ini contains duplicate Game.DefaultScene entries.");
            if (!string.Equals(key, "DefaultScene", StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "ProjectSettings.ini must use the canonical DefaultScene key name.");
            }
            defaultSceneLine = index;
        }

        if (defaultSceneLine >= 0)
        {
            string line = lines[defaultSceneLine].Text;
            int equals = line.IndexOf('=');
            string configured = line[(equals + 1)..].Trim();
            string normalizedConfigured;
            try
            {
                // Project settings use the same portable, Assets-relative contract as the
                // manifest. Normalize dot segments before comparing the two persisted values.
                normalizedConfigured = SceneSourceFile.NormalizeProjectSceneReference(
                    project.RootDir,
                    project.AssetsDir,
                    configured);
            }
            catch (Exception error) when (
                error is ArgumentException or
                InvalidDataException or
                NotSupportedException)
            {
                throw new InvalidDataException(
                    "Game.DefaultScene is not a valid project scene reference.",
                    error);
            }
            if (!string.Equals(
                    normalizedConfigured,
                    previousReference,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "Game.DefaultScene does not match .acsproject InitialScene. " +
                    "Repair the existing project settings before moving the initial scene.");
            }
            int valueStart = equals + 1;
            while (valueStart < line.Length &&
                   (line[valueStart] == ' ' || line[valueStart] == '\t'))
            {
                valueStart++;
            }
            lines[defaultSceneLine] = lines[defaultSceneLine] with
            {
                Text = line[..valueStart] + currentReference,
            };
        }
        else if (firstGameSection >= 0)
        {
            InsertIniLine(lines, gameSectionEnd, "DefaultScene=" + currentReference);
        }
        else
        {
            AppendIniSection(lines, "Game", "DefaultScene=" + currentReference);
        }

        var builder = new StringBuilder();
        foreach (IniLine line in lines)
            builder.Append(line.Text).Append(line.Ending);
        return StrictUtf8NoBom.GetBytes(builder.ToString());
    }

    private static List<IniLine> SplitIniLines(string text)
    {
        var lines = new List<IniLine>();
        int offset = 0;
        while (offset < text.Length)
        {
            int newline = text.IndexOf('\n', offset);
            if (newline < 0)
            {
                lines.Add(new IniLine(text[offset..], ""));
                offset = text.Length;
                continue;
            }
            int contentEnd = newline;
            string ending = "\n";
            if (contentEnd > offset && text[contentEnd - 1] == '\r')
            {
                contentEnd--;
                ending = "\r\n";
            }
            lines.Add(new IniLine(text[offset..contentEnd], ending));
            offset = newline + 1;
        }
        return lines;
    }

    private static void InsertIniLine(
        List<IniLine> lines,
        int index,
        string text)
    {
        string newline = PreferredNewline(lines);
        if (index > 0 && lines[index - 1].Ending.Length == 0)
            lines[index - 1] = lines[index - 1] with { Ending = newline };
        lines.Insert(index, new IniLine(text, newline));
    }

    private static void AppendIniSection(
        List<IniLine> lines,
        string section,
        string setting)
    {
        string newline = PreferredNewline(lines);
        if (lines.Count != 0)
        {
            if (lines[^1].Ending.Length == 0)
                lines[^1] = lines[^1] with { Ending = newline };
            if (lines[^1].Text.Length != 0)
                lines.Add(new IniLine("", newline));
        }
        lines.Add(new IniLine("[" + section + "]", newline));
        lines.Add(new IniLine(setting, newline));
    }

    private static string PreferredNewline(IEnumerable<IniLine> lines) =>
        lines.Select(static line => line.Ending)
            .FirstOrDefault(static ending => ending.Length != 0) ??
        Environment.NewLine;

    private static void EnsureSnapshotUnchanged(
        ReferenceFileSnapshot snapshot,
        string label)
    {
        if (!snapshot.Existed)
        {
            if (File.Exists(snapshot.Path) || Directory.Exists(snapshot.Path))
                throw new IOException($"{label} was created by another process.");
            return;
        }
        if (!File.Exists(snapshot.Path))
            throw new IOException($"{label} was removed by another process.");
        FileAttributes attributes = File.GetAttributes(snapshot.Path);
        if ((attributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException($"{label} is no longer an ordinary file.");
        }
        var info = new FileInfo(snapshot.Path);
        if (info.Length != snapshot.Bytes.LongLength ||
            info.LastWriteTimeUtc.Ticks != snapshot.LastWriteUtcTicks ||
            attributes != snapshot.Attributes ||
            !File.ReadAllBytes(snapshot.Path).AsSpan().SequenceEqual(snapshot.Bytes))
        {
            throw new IOException($"{label} changed while preparing the update.");
        }
    }

    private static string CreateSiblingTemporaryPath(string destination)
    {
        string parent = Path.GetDirectoryName(destination)
            ?? throw new InvalidDataException("Persistence target has no parent directory.");
        return Path.Combine(
            parent,
            "." + Path.GetFileName(destination) + ".scene-ref-" +
            Guid.NewGuid().ToString("N") + ".tmp");
    }

    private static void WriteTemporaryBytes(string temporary, byte[] bytes)
    {
        using var stream = new FileStream(
            temporary,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            64 * 1024,
            FileOptions.WriteThrough);
        stream.Write(bytes);
        stream.Flush(flushToDisk: true);
    }

    private static void PublishTemporary(
        string temporary,
        string destination,
        ReferenceFileSnapshot snapshot)
    {
        EnsureSnapshotUnchanged(
            snapshot,
            Path.GetFileName(destination));
        // A target that did not exist at snapshot time must never be replaced: another editor
        // may have published it after the compare step. File.Move(false) keeps that final race
        // fail-closed instead of silently clobbering the competing writer.
        File.Move(
            temporary,
            destination,
            overwrite: snapshot.Existed);
    }

    private static bool TryRestoreSnapshot(
        ReferenceFileSnapshot snapshot,
        byte[] publishedBytes)
    {
        try
        {
            if (!File.Exists(snapshot.Path) ||
                (File.GetAttributes(snapshot.Path) &
                 (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0 ||
                !File.ReadAllBytes(snapshot.Path).AsSpan().SequenceEqual(publishedBytes))
            {
                return false;
            }
            if (!snapshot.Existed)
            {
                File.Delete(snapshot.Path);
                return !File.Exists(snapshot.Path);
            }
            string temporary = CreateSiblingTemporaryPath(snapshot.Path);
            try
            {
                WriteTemporaryBytes(temporary, snapshot.Bytes);
                File.Move(temporary, snapshot.Path, overwrite: true);
                File.SetAttributes(snapshot.Path, snapshot.Attributes);
                File.SetLastWriteTimeUtc(
                    snapshot.Path,
                    new DateTime(snapshot.LastWriteUtcTicks, DateTimeKind.Utc));
            }
            finally
            {
                TryDeleteOrdinaryFile(temporary);
            }
            return File.ReadAllBytes(snapshot.Path)
                .AsSpan()
                .SequenceEqual(snapshot.Bytes);
        }
        catch
        {
            return false;
        }
    }

    private static void TryDeleteOrdinaryFile(string path)
    {
        try
        {
            if (!File.Exists(path)) return;
            FileAttributes attributes = File.GetAttributes(path);
            if ((attributes &
                 (FileAttributes.Directory | FileAttributes.ReparsePoint)) == 0)
            {
                File.Delete(path);
            }
        }
        catch
        {
            // Temp cleanup is best effort; transaction publication/rollback is authoritative.
        }
    }
}
