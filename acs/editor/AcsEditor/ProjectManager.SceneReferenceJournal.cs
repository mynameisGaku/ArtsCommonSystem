// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace AcsEditor;

internal enum ProjectSceneReferenceRecoveryStatus
{
    NoJournal,
    LiveOperation,
    Aborted,
    RolledForward,
    AlreadyComplete,
    Deferred,
}

internal sealed record ProjectSceneReferenceRecoveryResult(
    ProjectSceneReferenceRecoveryStatus Status,
    string Message);

/// <summary>
/// 永続化された一件のシーン移動 intent に対応する、共有拒否の生存確認ハンドルを所有する。
/// ハンドルを Dispose しても journal は削除せず、根拠を確認した確定処理だけが削除する。
/// </summary>
internal sealed class ProjectSceneReferenceMoveIntent : IDisposable
{
    private FileStream? _livenessHandle;

    internal ProjectSceneReferenceMoveIntent(
        Guid operationId,
        string journalPath,
        string sourceReference,
        string destinationReference,
        string assetId,
        FileStream livenessHandle)
    {
        OperationId = operationId;
        JournalPath = journalPath;
        SourceReference = sourceReference;
        DestinationReference = destinationReference;
        AssetId = assetId;
        _livenessHandle = livenessHandle;
    }

    internal Guid OperationId { get; }
    internal string JournalPath { get; }
    internal string SourceReference { get; }
    internal string DestinationReference { get; }
    internal string AssetId { get; }

    public void Dispose() =>
        System.Threading.Interlocked.Exchange(
            ref _livenessHandle,
            null)?.Dispose();
}

public static partial class ProjectManager
{
    private const int SceneReferenceJournalSchemaVersion = 1;
    private const int MaxSceneReferenceJournalBytes = 16 * 1024;
    private const int MaxAssetMetadataBytes = 1024 * 1024;
    private const string SceneReferenceJournalFileName =
        "scene-reference-follow.v1.json";
    private const string SceneReferenceJournalLeaseFileName =
        "scene-reference-follow.lease";

    private sealed record SceneReferenceJournal(
        Guid OperationId,
        string ProjectFileName,
        string SourceReference,
        string DestinationReference,
        string AssetId,
        long CreatedUtcTicks);

    /// <summary>
    /// Content Browser が物理移動を開始する前に、正確な移動元、提案された移動先、
    /// 正式なサイドカー identity を永続記録する。独立した共有拒否リースにより、
    /// 起動時復旧が実行中の非同期操作をクラッシュ済みと誤認しないようにする。
    /// </summary>
    internal static ProjectSceneReferenceMoveIntent PrepareInitialScenePathFollow(
        Project project,
        Guid operationId,
        string destinationScenePath)
    {
        ArgumentNullException.ThrowIfNull(project);
        if (operationId == Guid.Empty)
            throw new ArgumentException("Asset operation id cannot be empty.", nameof(operationId));

        using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
            project.AssetsDir,
            "Prepare initial scene path follow");

        ValidateInitialSceneReferenceFollow(project);
        string sourceReference = NormalizeCurrentInitialSceneReference(project);
        string sourcePath = SceneSourceFile.ResolveProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            sourceReference);
        EnsureOrdinarySceneAsset(
            sourcePath,
            project.AssetsDir,
            "Initial scene source");

        string assetId = ReadRequiredSidecarAssetId(sourcePath);
        if (!string.IsNullOrWhiteSpace(project.CanonicalSceneAssetId) &&
            !string.Equals(
                NormalizeAssetId(project.CanonicalSceneAssetId),
                assetId,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The initial scene sidecar identity does not match the project manifest.");
        }

        string destinationPath = SceneSourceFile.ValidateProjectScenePath(
            destinationScenePath,
            project.AssetsDir);
        if (SceneSourceFile.PathsEqual(sourcePath, destinationPath))
            throw new InvalidDataException(
                "The initial scene move does not have a distinct destination.");
        string destinationReference = SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            Path.GetRelativePath(project.RootDir, destinationPath));
        if (!IsAssetFamilyAbsent(project, destinationReference))
        {
            throw new IOException(
                "The proposed initial-scene destination already exists.");
        }

        string journalPath = GetSceneReferenceJournalPath(project);
        string leasePath = GetSceneReferenceJournalLeasePath(project);
        EnsureJournalStorage(project);
        if (File.Exists(journalPath))
        {
            throw new IOException(
                "A previous initial-scene move still requires recovery. Reopen the project " +
                "before starting another move.");
        }

        FileStream? liveness = null;
        try
        {
            liveness = OpenJournalLease(leasePath);
            if (File.Exists(journalPath))
            {
                throw new IOException(
                    "Another editor prepared an initial-scene move concurrently.");
            }

            var journal = new SceneReferenceJournal(
                operationId,
                Path.GetFileName(project.ProjectFilePath),
                sourceReference,
                destinationReference,
                assetId,
                DateTime.UtcNow.Ticks);
            WriteJournalAtomic(journalPath, journal);
            return new ProjectSceneReferenceMoveIntent(
                operationId,
                journalPath,
                sourceReference,
                destinationReference,
                assetId,
                liveness);
        }
        catch
        {
            liveness?.Dispose();
            throw;
        }
    }

    /// <summary>
    /// 非同期アセットコマンドの完了通知後に intent を確定する。失敗したコマンドは、
    /// 元の identity が移動元へ戻り、かつ移動先が存在しないことを確認できた場合だけ中止する。
    /// 成功したコマンドは、参照追従も永続的に commit されていない限り journal を保持する。
    /// </summary>
    internal static ProjectSceneReferenceRecoveryResult SettleInitialScenePathFollow(
        Project project,
        ProjectSceneReferenceMoveIntent intent,
        bool operationSucceeded,
        bool referencesCommitted)
    {
        ArgumentNullException.ThrowIfNull(project);
        ArgumentNullException.ThrowIfNull(intent);
        try
        {
            using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
                project.AssetsDir,
                "Settle initial scene path follow");
            SceneReferenceJournal journal = ReadJournal(project);
            EnsureIntentMatches(journal, intent);

            if (!operationSucceeded)
            {
                if (ReferencePointsTo(project, journal.SourceReference) &&
                    HasAssetIdentity(
                        project,
                        journal.SourceReference,
                        journal.AssetId) &&
                    IsAssetFamilyAbsent(project, journal.DestinationReference))
                {
                    DeleteJournal(intent.JournalPath);
                    return new ProjectSceneReferenceRecoveryResult(
                        ProjectSceneReferenceRecoveryStatus.Aborted,
                        "The failed asset operation was proven rolled back to its source.");
                }
                return new ProjectSceneReferenceRecoveryResult(
                    ProjectSceneReferenceRecoveryStatus.Deferred,
                    "The failed asset operation did not leave an unambiguous rollback state.");
            }

            if (referencesCommitted &&
                ReferencePointsTo(project, journal.DestinationReference) &&
                HasAssetIdentity(
                    project,
                    journal.DestinationReference,
                    journal.AssetId))
            {
                DeleteJournal(intent.JournalPath);
                return new ProjectSceneReferenceRecoveryResult(
                    ProjectSceneReferenceRecoveryStatus.AlreadyComplete,
                    "The scene move and both startup references were committed.");
            }

            return new ProjectSceneReferenceRecoveryResult(
                ProjectSceneReferenceRecoveryStatus.Deferred,
                "The asset move committed without a proven startup-reference commit.");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException or
            JsonException or KeyNotFoundException or ArgumentException or
            NotSupportedException)
        {
            return new ProjectSceneReferenceRecoveryResult(
                ProjectSceneReferenceRecoveryStatus.Deferred,
                error.Message);
        }
        finally
        {
            intent.Dispose();
        }
    }

    /// <summary>
    /// Project を開くときに、残された一件の intent を整合させる。他の Editor を待機せず、
    /// ファイル名から推測せず、metadata だけを使う全ツリー走査により、固定した Asset ID が
    /// journal 記載の移動先に一件だけ存在すると確認できた場合に限って修復する。
    /// </summary>
    internal static ProjectSceneReferenceRecoveryResult
        ReconcileInitialScenePathFollow(Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        string journalPath = GetSceneReferenceJournalPath(project);
        if (!File.Exists(journalPath))
        {
            return new ProjectSceneReferenceRecoveryResult(
                ProjectSceneReferenceRecoveryStatus.NoJournal,
                "No interrupted initial-scene move was found.");
        }

        try
        {
            using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
                project.AssetsDir,
                "Recover initial scene path follow");
            FileStream recoveryLease;
            try
            {
                recoveryLease = OpenJournalLease(
                    GetSceneReferenceJournalLeasePath(project));
            }
            catch (IOException)
            {
                return new ProjectSceneReferenceRecoveryResult(
                    ProjectSceneReferenceRecoveryStatus.LiveOperation,
                    "A live editor still owns the pending initial-scene move.");
            }
            using (recoveryLease)
            {

                SceneReferenceJournal journal = ReadJournal(project);
                Project persisted = ReadManifest(project.ProjectFilePath);
                if (!string.Equals(
                        Path.GetFileName(persisted.ProjectFilePath),
                        journal.ProjectFileName,
                        StringComparison.Ordinal) ||
                    (!string.IsNullOrWhiteSpace(persisted.CanonicalSceneAssetId) &&
                     !string.Equals(
                         NormalizeAssetId(persisted.CanonicalSceneAssetId),
                         journal.AssetId,
                         StringComparison.Ordinal)))
                {
                    return Deferred(
                        "The pending scene-move journal does not match the project identity.");
                }

                string sourcePath = ResolveJournalReference(project, journal.SourceReference);
                string destinationPath = ResolveJournalReference(
                    project,
                    journal.DestinationReference);
                if (SceneSourceFile.PathsEqual(sourcePath, destinationPath))
                    return Deferred("The pending scene-move journal has no distinct destination.");

                IReadOnlyList<string> identityPaths = FindAuthoritativeAssetIdentityPaths(
                    project.AssetsDir,
                    journal.AssetId);
                if (identityPaths.Count != 1)
                {
                    return Deferred(
                        "The pending scene identity is missing or duplicated; automatic recovery " +
                        "was refused.");
                }

                string identityPath = identityPaths[0];
                bool identityAtSource = SceneSourceFile.PathsEqual(identityPath, sourcePath);
                bool identityAtDestination =
                    SceneSourceFile.PathsEqual(identityPath, destinationPath);
                if (identityAtSource &&
                    IsAssetFamilyAbsent(project, journal.DestinationReference) &&
                    ReferencePointsTo(persisted, journal.SourceReference))
                {
                    DeleteJournal(journalPath);
                    project.InitialScene = persisted.InitialScene;
                    return new ProjectSceneReferenceRecoveryResult(
                        ProjectSceneReferenceRecoveryStatus.Aborted,
                        "The interrupted move never committed and its intent was safely aborted.");
                }
                if (!identityAtDestination ||
                    !IsAssetFamilyAbsent(project, journal.SourceReference))
                {
                    return Deferred(
                        "The pending scene identity is not in an unambiguous source/destination state.");
                }

                ProjectSceneReferenceRecoveryResult committed =
                    CommitRecoveredInitialSceneReferences(project, persisted, journal);
                if (committed.Status is
                    ProjectSceneReferenceRecoveryStatus.RolledForward or
                    ProjectSceneReferenceRecoveryStatus.AlreadyComplete)
                {
                    DeleteJournal(journalPath);
                }
                return committed;
            }
        }
        catch (IOException error) when (
            error.Message.Contains("another editor", StringComparison.OrdinalIgnoreCase) ||
            error.Message.Contains("being used by another process", StringComparison.OrdinalIgnoreCase))
        {
            return new ProjectSceneReferenceRecoveryResult(
                ProjectSceneReferenceRecoveryStatus.LiveOperation,
                "A live editor still owns the pending initial-scene move.");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException or
            JsonException or KeyNotFoundException or ArgumentException or
            NotSupportedException)
        {
            return Deferred(error.Message);
        }
    }

    internal static bool HasPendingInitialScenePathFollow(Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        return HasPendingInitialScenePathFollow(project.AssetsDir);
    }

    internal static bool HasPendingInitialScenePathFollow(string assetsRoot)
    {
        if (string.IsNullOrWhiteSpace(assetsRoot))
            return true;
        string path = Path.Combine(
            Path.GetFullPath(assetsRoot),
            AssetDatabase.InternalDirectoryName,
            SceneReferenceJournalFileName);
        return File.Exists(path) || Directory.Exists(path);
    }

    /// <summary>
    /// 永続化された起動シーン記録を再検証し、正式なサイドカー identity が参照先に
    /// 一件だけ存在することを確認する。Package 公開処理は、プロジェクトの
    /// アセット変更リースを保持した状態でこれを呼び出す。
    /// </summary>
    internal static string ValidateInitialSceneAssetIdentity(Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        ValidateInitialSceneReferenceFollow(project);

        string initialScenePath = SceneSourceFile.ResolveProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            project.InitialScene);
        EnsureOrdinarySceneAsset(initialScenePath, project.AssetsDir, "Initial scene");
        string authoritativeAssetId = ReadRequiredSidecarAssetId(initialScenePath);
        if (!string.IsNullOrWhiteSpace(project.CanonicalSceneAssetId) &&
            !string.Equals(
                NormalizeAssetId(project.CanonicalSceneAssetId),
                authoritativeAssetId,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The initial scene sidecar identity does not match the project manifest.");
        }

        IReadOnlyList<string> identityPaths = FindAuthoritativeAssetIdentityPaths(
            project.AssetsDir,
            authoritativeAssetId);
        if (identityPaths.Count != 1 ||
            !SceneSourceFile.PathsEqual(identityPaths[0], initialScenePath))
        {
            throw new InvalidDataException(
                "The initial scene identity is missing, duplicated, or no longer located at " +
                "the manifest path.");
        }
        return authoritativeAssetId;
    }

    private static ProjectSceneReferenceRecoveryResult CommitRecoveredInitialSceneReferences(
        Project project,
        Project persisted,
        SceneReferenceJournal journal)
    {
        string manifestReference = SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            persisted.InitialScene);
        if (!string.Equals(
                manifestReference,
                journal.SourceReference,
                StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(
                manifestReference,
                journal.DestinationReference,
                StringComparison.OrdinalIgnoreCase))
        {
            return Deferred(
                "The project manifest no longer contains either journaled scene reference.");
        }

        ReferenceFileSnapshot manifest = CaptureRequiredOrdinaryFile(
            project.ProjectFilePath,
            MaxProjectManifestBytes,
            ".acsproject manifest");
        Project capturedManifest;
        try
        {
            capturedManifest = ParseManifestSnapshot(
                manifest.Path,
                manifest.Bytes);
        }
        catch (InvalidDataException error)
        {
            return Deferred(
                "The captured project manifest is not valid for recovery: " +
                error.Message);
        }
        manifestReference = SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            capturedManifest.InitialScene);
        if ((!string.Equals(
                 manifestReference,
                 journal.SourceReference,
                 StringComparison.OrdinalIgnoreCase) &&
             !string.Equals(
                 manifestReference,
                 journal.DestinationReference,
                 StringComparison.OrdinalIgnoreCase)) ||
            (!string.IsNullOrWhiteSpace(
                 capturedManifest.CanonicalSceneAssetId) &&
             !string.Equals(
                 NormalizeAssetId(
                     capturedManifest.CanonicalSceneAssetId),
                 journal.AssetId,
                 StringComparison.Ordinal)))
        {
            return Deferred(
                "The captured project manifest no longer matches the journaled scene identity.");
        }
        ReferenceFileSnapshot settings = CaptureOptionalOrdinaryFile(
            GetProjectSettingsPath(project),
            MaxProjectSettingsBytes,
            "Project settings");
        byte[] updatedSettings;
        try
        {
            updatedSettings = RewriteDefaultSceneSetting(
                settings.Existed ? settings.Bytes : Array.Empty<byte>(),
                project,
                journal.SourceReference,
                journal.DestinationReference);
        }
        catch (InvalidDataException)
        {
            try
            {
                updatedSettings = RewriteDefaultSceneSetting(
                    settings.Existed ? settings.Bytes : Array.Empty<byte>(),
                    project,
                    journal.DestinationReference,
                    journal.DestinationReference);
            }
            catch (InvalidDataException)
            {
                return Deferred(
                    "Game.DefaultScene is neither the journaled source nor destination.");
            }
        }

        byte[] updatedManifest = RewriteManifestInitialScene(
            manifest.Bytes,
            journal.DestinationReference);
        if (updatedManifest.LongLength > MaxProjectManifestBytes ||
            updatedSettings.LongLength > MaxProjectSettingsBytes)
        {
            return Deferred("Recovered startup-scene state exceeds its persistence limit.");
        }

        bool alreadyComplete =
            string.Equals(
                manifestReference,
                journal.DestinationReference,
                StringComparison.OrdinalIgnoreCase) &&
            manifest.Bytes.AsSpan().SequenceEqual(updatedManifest) &&
            settings.Existed &&
            settings.Bytes.AsSpan().SequenceEqual(updatedSettings);
        if (alreadyComplete)
        {
            project.InitialScene = journal.DestinationReference;
            return new ProjectSceneReferenceRecoveryResult(
                ProjectSceneReferenceRecoveryStatus.AlreadyComplete,
                "The interrupted transaction had already committed both references.");
        }

        ValidateSettingsStoragePath(project, settings.Path, createDirectory: true);
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

            PublishTemporary(settingsTemporary, settings.Path, settings);
            settingsPublished = true;
            EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
            PublishTemporary(manifestTemporary, manifest.Path, manifest);
            manifestPublished = true;
            project.InitialScene = journal.DestinationReference;
            return new ProjectSceneReferenceRecoveryResult(
                ProjectSceneReferenceRecoveryStatus.RolledForward,
                "The interrupted initial-scene move was rolled forward.");
        }
        catch (Exception error)
        {
            bool rollbackComplete = true;
            if (manifestPublished)
                rollbackComplete &= TryRestoreSnapshot(manifest, updatedManifest);
            if (settingsPublished)
                rollbackComplete &= TryRestoreSnapshot(settings, updatedSettings);
            return Deferred(
                rollbackComplete
                    ? "Recovery publication failed and was rolled back: " + error.Message
                    : "Recovery publication and byte-for-byte rollback were incomplete: " +
                      error.Message);
        }
        finally
        {
            TryDeleteOrdinaryFile(manifestTemporary);
            TryDeleteOrdinaryFile(settingsTemporary);
        }
    }

    private static bool ReferencePointsTo(Project project, string reference)
    {
        Project persisted = ReadManifest(project.ProjectFilePath);
        string normalized = SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            persisted.InitialScene);
        if (!string.Equals(normalized, reference, StringComparison.OrdinalIgnoreCase))
            return false;
        ReferenceFileSnapshot settings = CaptureOptionalOrdinaryFile(
            GetProjectSettingsPath(project),
            MaxProjectSettingsBytes,
            "Project settings");
        try
        {
            byte[] unchanged = RewriteDefaultSceneSetting(
                settings.Existed ? settings.Bytes : Array.Empty<byte>(),
                project,
                reference,
                reference);
            return !settings.Existed ||
                   settings.Bytes.AsSpan().SequenceEqual(unchanged);
        }
        catch (InvalidDataException)
        {
            return false;
        }
    }

    private static bool HasAssetIdentity(
        Project project,
        string reference,
        string assetId)
    {
        string path = ResolveJournalReference(project, reference);
        try
        {
            EnsureOrdinarySceneAsset(path, project.AssetsDir, "Scene asset");
            return string.Equals(
                ReadRequiredSidecarAssetId(path),
                assetId,
                StringComparison.Ordinal);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return false;
        }
    }

    private static bool IsAssetFamilyAbsent(Project project, string reference)
    {
        string path = ResolveJournalReference(project, reference);
        return !File.Exists(path) &&
               !Directory.Exists(path) &&
               !File.Exists(path + AssetDatabase.MetadataSuffix) &&
               !Directory.Exists(path + AssetDatabase.MetadataSuffix);
    }

    private static IReadOnlyList<string> FindAuthoritativeAssetIdentityPaths(
        string assetsRoot,
        string assetId)
    {
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
        EnsureOrdinaryDirectory(root, "Assets root");
        var matches = new List<string>();
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.Count != 0)
        {
            string directory = pending.Pop();
            foreach (FileSystemInfo entry in new DirectoryInfo(directory)
                         .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                entry.Refresh();
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException(
                        "Asset identity recovery cannot traverse reparse points.");
                }
                if ((entry.Attributes & FileAttributes.Directory) != 0)
                {
                    if (entry.Name.Equals(
                            AssetDatabase.InternalDirectoryName,
                            StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }
                    pending.Push(entry.FullName);
                    continue;
                }
                if (!entry.Name.EndsWith(
                        AssetDatabase.MetadataSuffix,
                        StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                string candidateId = ReadMetadataAssetId(entry.FullName);
                if (!string.Equals(candidateId, assetId, StringComparison.Ordinal))
                    continue;
                string assetPath =
                    entry.FullName[..^AssetDatabase.MetadataSuffix.Length];
                EnsureOrdinarySceneAsset(
                    assetPath,
                    root,
                    "Recovered scene identity");
                matches.Add(assetPath);
                if (matches.Count > 1)
                    return matches;
            }
        }
        return matches;
    }

    private static string ReadRequiredSidecarAssetId(string assetPath)
    {
        string metadataPath = assetPath + AssetDatabase.MetadataSuffix;
        if (!File.Exists(metadataPath))
        {
            throw new InvalidDataException(
                "The initial scene requires an authoritative .acsmeta sidecar before it can move.");
        }
        return ReadMetadataAssetId(metadataPath);
    }

    private static string ReadMetadataAssetId(string metadataPath)
    {
        EnsureOrdinaryFile(metadataPath, "Asset metadata");
        var info = new FileInfo(metadataPath);
        if (info.Length > MaxAssetMetadataBytes)
            throw new InvalidDataException("Asset metadata exceeds 1 MiB.");
        byte[] bytes = File.ReadAllBytes(metadataPath);
        using JsonDocument document = JsonDocument.Parse(bytes, new JsonDocumentOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 32,
        });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("schemaVersion", out JsonElement schema) ||
            schema.ValueKind != JsonValueKind.Number ||
            !schema.TryGetInt32(out int schemaVersion) ||
            schemaVersion != AssetDatabase.CurrentSchemaVersion ||
            !root.TryGetProperty("id", out JsonElement idNode) ||
            idNode.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException("Asset metadata identity is malformed.");
        }
        return NormalizeAssetId(idNode.GetString());
    }

    private static string NormalizeAssetId(string? assetId)
    {
        string value = assetId?.Trim() ?? "";
        if (value.Length != 32 ||
            !Guid.TryParseExact(value, "N", out Guid parsed) ||
            parsed == Guid.Empty)
        {
            throw new InvalidDataException(
                "Scene asset identity must be a non-zero 32-digit GUID.");
        }
        return parsed.ToString("N");
    }

    private static void EnsureOrdinarySceneAsset(
        string path,
        string assetsRoot,
        string label)
    {
        _ = SceneSourceFile.ValidateProjectScenePath(path, assetsRoot);
        EnsureOrdinaryFile(path, label);
    }

    private static void EnsureOrdinaryFile(string path, string label)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException(label + " was not found.", path);
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException(label + " must be an ordinary file.");
        }
    }

    private static void EnsureOrdinaryDirectory(string path, string label)
    {
        if (!Directory.Exists(path))
            throw new DirectoryNotFoundException(path);
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(label + " must be an ordinary directory.");
        }
    }

    private static string ResolveJournalReference(Project project, string reference) =>
        SceneSourceFile.ResolveProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            reference);

    private static string GetSceneReferenceJournalPath(Project project) =>
        Path.Combine(
            project.AssetsDir,
            AssetDatabase.InternalDirectoryName,
            SceneReferenceJournalFileName);

    private static string GetSceneReferenceJournalLeasePath(Project project) =>
        Path.Combine(
            project.AssetsDir,
            AssetDatabase.InternalDirectoryName,
            SceneReferenceJournalLeaseFileName);

    private static void EnsureJournalStorage(Project project)
    {
        SceneSourceFile.ValidateProjectRootDirectory(project.RootDir);
        EnsureOrdinaryDirectory(project.AssetsDir, "Assets root");
        string storage = Path.Combine(
            project.AssetsDir,
            AssetDatabase.InternalDirectoryName);
        if (!Directory.Exists(storage))
            Directory.CreateDirectory(storage);
        EnsureOrdinaryDirectory(storage, "Asset database directory");
    }

    private static FileStream OpenJournalLease(string path)
    {
        string parent = Path.GetDirectoryName(path)
            ?? throw new InvalidDataException("Scene-reference lease has no parent.");
        EnsureOrdinaryDirectory(parent, "Asset database directory");
        if (File.Exists(path))
            EnsureOrdinaryFile(path, "Scene-reference lease");
        var stream = new FileStream(
            path,
            FileMode.OpenOrCreate,
            FileAccess.ReadWrite,
            FileShare.None,
            1,
            FileOptions.None);
        try
        {
            EnsureOrdinaryFile(path, "Scene-reference lease");
            return stream;
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    private static void WriteJournalAtomic(
        string journalPath,
        SceneReferenceJournal journal)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new
        {
            schemaVersion = SceneReferenceJournalSchemaVersion,
            operationId = journal.OperationId.ToString("D"),
            projectFileName = journal.ProjectFileName,
            sourceReference = journal.SourceReference,
            destinationReference = journal.DestinationReference,
            assetId = journal.AssetId,
            createdUtcTicks = journal.CreatedUtcTicks,
        }, JsonOpts) + Environment.NewLine);
        if (bytes.Length > MaxSceneReferenceJournalBytes)
            throw new InvalidDataException("Scene-reference journal exceeds 16 KiB.");
        string temporary = journalPath + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            WriteTemporaryBytes(temporary, bytes);
            if (File.Exists(journalPath))
                throw new IOException("A scene-reference journal already exists.");
            File.Move(temporary, journalPath);
        }
        finally
        {
            TryDeleteOrdinaryFile(temporary);
        }
    }

    private static SceneReferenceJournal ReadJournal(Project project)
    {
        string path = GetSceneReferenceJournalPath(project);
        EnsureOrdinaryFile(path, "Scene-reference journal");
        var info = new FileInfo(path);
        if (info.Length > MaxSceneReferenceJournalBytes)
            throw new InvalidDataException("Scene-reference journal exceeds 16 KiB.");
        byte[] bytes = File.ReadAllBytes(path);
        using JsonDocument document = JsonDocument.Parse(bytes, new JsonDocumentOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 8,
        });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            root.EnumerateObject().Count() != 7 ||
            root.GetProperty("schemaVersion").GetInt32() !=
                SceneReferenceJournalSchemaVersion ||
            !Guid.TryParseExact(
                root.GetProperty("operationId").GetString(),
                "D",
                out Guid operationId) ||
            operationId == Guid.Empty)
        {
            throw new InvalidDataException("Scene-reference journal is malformed.");
        }

        string projectFileName =
            root.GetProperty("projectFileName").GetString() ?? "";
        if (projectFileName.Length == 0 ||
            !string.Equals(
                Path.GetFileName(projectFileName),
                projectFileName,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Scene-reference journal project identity is malformed.");
        }
        string sourceReference = SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            root.GetProperty("sourceReference").GetString() ?? "");
        string destinationReference = SceneSourceFile.NormalizeProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            root.GetProperty("destinationReference").GetString() ?? "");
        string assetId = NormalizeAssetId(
            root.GetProperty("assetId").GetString());
        long createdUtcTicks = root.GetProperty("createdUtcTicks").GetInt64();
        if (createdUtcTicks <= 0)
            throw new InvalidDataException("Scene-reference journal timestamp is malformed.");
        return new SceneReferenceJournal(
            operationId,
            projectFileName,
            sourceReference,
            destinationReference,
            assetId,
            createdUtcTicks);
    }

    private static void EnsureIntentMatches(
        SceneReferenceJournal journal,
        ProjectSceneReferenceMoveIntent intent)
    {
        if (journal.OperationId != intent.OperationId ||
            !string.Equals(
                journal.SourceReference,
                intent.SourceReference,
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                journal.DestinationReference,
                intent.DestinationReference,
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(journal.AssetId, intent.AssetId, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The live scene-move intent no longer matches its durable journal.");
        }
    }

    private static void DeleteJournal(string journalPath)
    {
        EnsureOrdinaryFile(journalPath, "Scene-reference journal");
        File.Delete(journalPath);
        if (File.Exists(journalPath))
            throw new IOException("Scene-reference journal could not be removed.");
    }

    private static ProjectSceneReferenceRecoveryResult Deferred(string message) =>
        new(ProjectSceneReferenceRecoveryStatus.Deferred, message);
}
