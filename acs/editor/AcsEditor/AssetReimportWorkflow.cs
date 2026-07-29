// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;

namespace AcsEditor;

internal sealed record AssetReimportOperationResult(
    AssetRecord Asset,
    string SourcePath,
    AssetDatabaseRefreshResult IndexResult,
    IReadOnlyList<string> Warnings);

internal enum AssetReimportCheckpoint
{
    Prepared,
    OriginalsBackedUp,
    AssetPublished,
    MetadataPublished,
}

internal sealed record AssetReimportTestHooks(
    AssetReimportCheckpoint? FailAfter = null,
    AssetReimportCheckpoint? SimulateCrashAfter = null);

internal sealed class AssetReimportSimulatedCrashException : IOException
{
    internal AssetReimportSimulatedCrashException(AssetReimportCheckpoint checkpoint)
        : base($"Simulated process termination after reimport checkpoint '{checkpoint}'.")
    {
    }
}

/// <summary>
/// 正規メタデータの入力元からインポート済みアセットをトランザクションとして置換し、
/// アセット ID を維持します。置換後のペアを公開する前に、元の二ファイルを同一ボリューム上の
/// 非公開バックアップへ移動します。ジャーナルにより、新しい内容の公開が始まっていない場合は
/// 起動時に元のペアを復元し、それ以外は世代の混在を公開せずに新しいペアを完成できます。
/// </summary>
internal static class AssetReimportWorkflow
{
    private const int SchemaVersion = 1;
    private const int MaxJournalBytes = 64 * 1024;
    internal const string StagingDirectoryName =
        AssetDatabase.ReimportStagingDirectoryName;
    private const string JournalName = "manifest.v1.json";
    private const string NewAssetName = "new-payload";
    private const string NewMetadataName = "new-payload.acsmeta";
    private const string OldAssetName = "original-payload";
    private const string OldMetadataName = "original-payload.acsmeta";
    private static readonly StringComparison PathComparison =
        StringComparison.OrdinalIgnoreCase;

    /// <summary>
    /// メニュー表示用の軽量な判定です。インポート元がオフラインのネットワークパスの場合が
    /// あるため、意図的にファイルシステム I/O を行いません。通常ファイルおよび
    /// リパースポイントの完全な検証は、再インポート実行時にアセットワーカーで行います。
    /// </summary>
    internal static bool HasExternalSourceMetadata(
        AssetDatabase database,
        string assetId)
    {
        ArgumentNullException.ThrowIfNull(database);
        if (!database.TryGetByAssetId(assetId, out AssetRecord? record) ||
            record == null)
        {
            return false;
        }
        string source = record.Metadata.Source;
        if (string.IsNullOrWhiteSpace(source) ||
            source.Length > 4096 ||
            source.IndexOf('\0') >= 0)
        {
            return false;
        }
        try
        {
            return Path.IsPathFullyQualified(source.Replace(
                '/',
                Path.DirectorySeparatorChar));
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    internal static bool CanReimport(
        AssetDatabase database,
        string assetId,
        out string reason)
    {
        ArgumentNullException.ThrowIfNull(database);
        try
        {
            if (!database.TryGetByAssetId(assetId, out AssetRecord? record) ||
                record == null)
            {
                reason = "The asset is not indexed.";
                return false;
            }
            string source = ResolveSourcePath(database, record);
            ValidateOrdinarySource(source);
            if (PathEquals(source, record.FullPath))
            {
                reason = "The asset has no external import source.";
                return false;
            }
            reason = "";
            return true;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            reason = error.Message;
            return false;
        }
    }

    internal static AssetReimportOperationResult Reimport(
        AssetDatabase database,
        string assetId,
        CancellationToken cancellationToken = default,
        AssetReimportTestHooks? testHooks = null)
    {
        ArgumentNullException.ThrowIfNull(database);
        using AssetMutationLock mutationLock = AssetMutationLock.AcquireForRecovery(
            database.AssetsRoot,
            "Reimport asset");
        cancellationToken.ThrowIfCancellationRequested();
        if (ProjectManager.HasPendingInitialScenePathFollow(database.AssetsRoot))
        {
            throw new IOException(
                "Reimport is blocked while an initial-scene move requires recovery.");
        }

        AssetImportReconciliationResult importRecovery =
            AssetImportWorkflow.Reconcile(database, cancellationToken);
        if (importRecovery.PreservedTransactions != 0)
        {
            throw new IOException(
                "Reimport is blocked because an earlier Import transaction requires manual " +
                "inspection below Assets/.acsdb/" +
                $"{AssetImportWorkflow.StagingDirectoryName}.");
        }
        AssetImportReconciliationResult recovery = ReconcileCore(
            database.AssetsRoot,
            cancellationToken);
        if (recovery.PreservedTransactions != 0)
        {
            throw new IOException(
                "Reimport is blocked because an earlier replacement transaction requires " +
                "manual inspection below Assets/.acsdb/reimport-staging.");
        }

        _ = database.RefreshWithinAssetTransaction(
            verifyContent: true,
            cancellationToken);
        if (!database.TryGetByAssetId(assetId, out AssetRecord? current) ||
            current == null)
        {
            throw new KeyNotFoundException($"Unknown asset id: {assetId}");
        }
        string source = ResolveSourcePath(database, current);
        ValidateOrdinarySource(source);
        if (PathEquals(source, current.FullPath))
            throw new InvalidOperationException("The asset has no external import source.");
        ValidateDestination(database.AssetsRoot, current.FullPath);

        string stagingRoot = EnsureStagingRoot(database.AssetsRoot);
        string transactionId = Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        string stagingDirectory = Path.Combine(stagingRoot, transactionId);
        Directory.CreateDirectory(stagingDirectory);
        EnsureOrdinaryDirectory(stagingDirectory, "Reimport transaction directory");

        string newAsset = Path.Combine(stagingDirectory, NewAssetName);
        string newMetadata = Path.Combine(stagingDirectory, NewMetadataName);
        string oldAsset = Path.Combine(stagingDirectory, OldAssetName);
        string oldMetadata = Path.Combine(stagingDirectory, OldMetadataName);
        string destinationMetadata =
            current.FullPath + AssetDatabase.MetadataSuffix;
        string journalPath = Path.Combine(stagingDirectory, JournalName);
        Transaction? transaction = null;
        try
        {
            Snapshot sourceSnapshot = CopyAndHash(
                source,
                newAsset,
                cancellationToken);
            AssetImportDerivedDataResult processed =
                AssetImportDerivedDataPipeline.GetOrCreate(
                    database.ProjectRoot,
                    newAsset,
                    current.Kind,
                    Path.GetExtension(current.RelativePath),
                    current.Metadata.Importer,
                    current.Metadata.ImporterVersion,
                    current.Metadata.ImportSettings,
                    sourceSnapshot.Hash,
                    sourceSnapshot.Length,
                    cancellationToken);
            KeyValuePair<string, string>[] fingerprint =
            [
                KeyValuePair.Create(
                    "sourceContentHash",
                    sourceSnapshot.Hash),
                KeyValuePair.Create(
                    "sourceLastWriteUtcTicks",
                    sourceSnapshot.LastWriteUtcTicks.ToString(
                        CultureInfo.InvariantCulture)),
                KeyValuePair.Create(
                    "sourceSizeBytes",
                    sourceSnapshot.Length.ToString(
                        CultureInfo.InvariantCulture)),
                .. AssetImportDerivedDataPipeline.MetadataSettings(
                    processed),
            ];
            (AssetRecord authoritative, AssetMetadata _, byte[] metadataBytes) =
                database.CreateReimportMetadataPayload(
                    current.AssetId,
                    source,
                    fingerprint);
            current = authoritative;
            WriteNewDurableFile(newMetadata, metadataBytes);
            EnsureMatchingFile(
                current.FullPath,
                current.SizeBytes,
                current.ContentHash,
                cancellationToken,
                "Current asset");
            EnsureOrdinaryFile(destinationMetadata, "Current asset metadata");
            string oldMetadataHash = ComputeSha256(
                destinationMetadata,
                cancellationToken);

            var manifest = new ReimportManifest
            {
                schemaVersion = SchemaVersion,
                transactionId = transactionId,
                phase = PhaseText(AssetReimportCheckpoint.Prepared),
                assetId = current.AssetId,
                destinationRelativePath = current.RelativePath,
                sourcePath = source.Replace('\\', '/'),
                newPayloadLength = sourceSnapshot.Length,
                newPayloadSha256 = sourceSnapshot.Hash,
                newMetadataSha256 = ComputeSha256(metadataBytes),
                oldPayloadLength = current.SizeBytes,
                oldPayloadSha256 = current.ContentHash,
                oldMetadataSha256 = oldMetadataHash,
                createdUtcTicks = DateTime.UtcNow.Ticks,
            };
            WriteJournal(journalPath, manifest);
            transaction = new Transaction(
                database.AssetsRoot,
                stagingDirectory,
                journalPath,
                newAsset,
                newMetadata,
                oldAsset,
                oldMetadata,
                current.FullPath,
                destinationMetadata,
                manifest);
            InvokeCheckpoint(testHooks, AssetReimportCheckpoint.Prepared);

            cancellationToken.ThrowIfCancellationRequested();
            ValidateOrdinarySource(source);
            ValidateDestination(database.AssetsRoot, current.FullPath);
            EnsureMatchingFile(
                current.FullPath,
                manifest.oldPayloadLength,
                manifest.oldPayloadSha256,
                cancellationToken,
                "Current asset before backup");
            EnsureMatchingFile(
                destinationMetadata,
                expectedLength: null,
                manifest.oldMetadataSha256,
                cancellationToken,
                "Current metadata before backup");
            File.Move(current.FullPath, oldAsset, overwrite: false);
            File.Move(destinationMetadata, oldMetadata, overwrite: false);
            ValidateDestinationParent(database.AssetsRoot, current.FullPath);
            EnsureMatchingFile(
                oldAsset,
                manifest.oldPayloadLength,
                manifest.oldPayloadSha256,
                cancellationToken,
                "Backed-up original asset");
            EnsureMatchingFile(
                oldMetadata,
                expectedLength: null,
                manifest.oldMetadataSha256,
                cancellationToken,
                "Backed-up original metadata");
            manifest.phase = PhaseText(AssetReimportCheckpoint.OriginalsBackedUp);
            WriteJournal(journalPath, manifest);
            InvokeCheckpoint(testHooks, AssetReimportCheckpoint.OriginalsBackedUp);

            cancellationToken.ThrowIfCancellationRequested();
            ValidateDestinationParent(database.AssetsRoot, current.FullPath);
            File.Move(newAsset, current.FullPath, overwrite: false);
            ValidateDestinationParent(database.AssetsRoot, current.FullPath);
            EnsureMatchingFile(
                current.FullPath,
                manifest.newPayloadLength,
                manifest.newPayloadSha256,
                cancellationToken,
                "Published replacement asset");
            manifest.phase = PhaseText(AssetReimportCheckpoint.AssetPublished);
            WriteJournal(journalPath, manifest);
            InvokeCheckpoint(testHooks, AssetReimportCheckpoint.AssetPublished);

            cancellationToken.ThrowIfCancellationRequested();
            ValidateDestinationParent(database.AssetsRoot, current.FullPath);
            EnsureMatchingFile(
                current.FullPath,
                manifest.newPayloadLength,
                manifest.newPayloadSha256,
                cancellationToken,
                "Reimported asset");
            File.Move(newMetadata, destinationMetadata, overwrite: false);
            ValidateDestinationParent(database.AssetsRoot, current.FullPath);
            EnsureMatchingFile(
                destinationMetadata,
                expectedLength: null,
                manifest.newMetadataSha256,
                cancellationToken,
                "Published replacement metadata");
            manifest.phase = PhaseText(AssetReimportCheckpoint.MetadataPublished);
            WriteJournal(journalPath, manifest);
            InvokeCheckpoint(testHooks, AssetReimportCheckpoint.MetadataPublished);

            AssetDatabaseRefreshResult indexResult;
            try
            {
                indexResult = database.RefreshWithinAssetTransaction(
                    verifyContent: true,
                    cancellationToken);
            }
            catch (Exception error)
            {
                if (!TryRollback(transaction, new List<string>()))
                {
                    throw new IOException(
                        "Reimport indexing failed and rollback was incomplete; the recovery " +
                        "journal was retained.",
                        error);
                }
                _ = database.RefreshWithinAssetTransaction(verifyContent: true);
                throw;
            }

            if (!database.TryGetByAssetId(
                    current.AssetId,
                    out AssetRecord? replacement) ||
                replacement == null ||
                !PathEquals(replacement.FullPath, current.FullPath) ||
                !string.Equals(
                    replacement.ContentHash,
                    manifest.newPayloadSha256,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException(
                    "Reimported asset did not retain its indexed identity.");
            }

            var warnings = new List<string>(recovery.Warnings);
            warnings.InsertRange(0, importRecovery.Warnings);
            if (!TryCleanup(stagingDirectory, out string? cleanupWarning))
                warnings.Add(cleanupWarning!);
            return new AssetReimportOperationResult(
                replacement,
                source,
                indexResult,
                Array.AsReadOnly(warnings.ToArray()));
        }
        catch (AssetReimportSimulatedCrashException)
        {
            throw;
        }
        catch (Exception error)
        {
            if (transaction == null)
            {
                if (!TryCleanup(stagingDirectory, out _))
                {
                    throw new IOException(
                        "Reimport preparation failed and private staging cleanup was incomplete.",
                        error);
                }
                throw;
            }
            var warnings = new List<string>();
            if (!TryRollback(transaction, warnings))
            {
                throw new IOException(
                    "Reimport failed and automatic rollback was incomplete. " +
                    string.Join(" ", warnings),
                    error);
            }
            try
            {
                _ = database.RefreshWithinAssetTransaction(verifyContent: true);
            }
            catch (Exception refreshError)
            {
                throw new IOException(
                    "Reimport rollback restored the source files but the asset index could not " +
                    "be restored.",
                    new AggregateException(error, refreshError));
            }
            throw;
        }
    }

    internal static AssetImportReconciliationResult Reconcile(
        AssetDatabase database,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(database);
        using AssetMutationLock mutationLock = AssetMutationLock.AcquireForRecovery(
            database.AssetsRoot,
            "Recover interrupted asset reimports");
        return ReconcileCore(database.AssetsRoot, cancellationToken);
    }

    private static AssetImportReconciliationResult ReconcileCore(
        string assetsRoot,
        CancellationToken cancellationToken)
    {
        string stagingRoot = EnsureStagingRoot(assetsRoot);
        int completed = 0;
        int discarded = 0;
        int preserved = 0;
        var warnings = new List<string>();

        foreach (FileSystemInfo entry in new DirectoryInfo(stagingRoot)
            .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly)
            .OrderBy(static value => value.Name, StringComparer.Ordinal))
        {
            cancellationToken.ThrowIfCancellationRequested();
            entry.Refresh();
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                (entry.Attributes & FileAttributes.Directory) == 0 ||
                entry is not DirectoryInfo directory ||
                !Guid.TryParseExact(entry.Name, "N", out Guid id) ||
                id == Guid.Empty)
            {
                preserved++;
                warnings.Add(
                    $"Reimport recovery preserved unexpected staging entry '{entry.Name}'.");
                continue;
            }

            string journalPath = Path.Combine(directory.FullName, JournalName);
            if (!File.Exists(journalPath))
            {
                string? cleanupWarning = null;
                if (ContainsOnlyUnpublishedPreparationFiles(directory.FullName) &&
                    TryCleanup(directory.FullName, out cleanupWarning))
                {
                    discarded++;
                }
                else
                {
                    preserved++;
                    warnings.Add(
                        cleanupWarning ??
                        $"Reimport recovery preserved incomplete transaction '{entry.Name}'.");
                }
                continue;
            }

            try
            {
                ReimportManifest manifest = ReadJournal(journalPath);
                if (!string.Equals(
                        manifest.transactionId,
                        entry.Name,
                        StringComparison.Ordinal))
                {
                    throw new InvalidDataException(
                        "Reimport journal id does not match its directory.");
                }
                bool didComplete = Recover(
                    assetsRoot,
                    directory.FullName,
                    manifest,
                    cancellationToken);
                if (didComplete)
                    completed++;
                else
                    discarded++;
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    InvalidDataException or JsonException or ArgumentException)
            {
                preserved++;
                warnings.Add(
                    $"Reimport recovery preserved transaction '{entry.Name}': {error.Message}");
            }
        }

        return new AssetImportReconciliationResult(
            completed,
            discarded,
            preserved,
            Array.AsReadOnly(warnings.ToArray()));
    }

    /// <returns>置換が完了した場合は真、元のファイルを復元した場合は偽を返します。</returns>
    private static bool Recover(
        string assetsRoot,
        string stagingDirectory,
        ReimportManifest manifest,
        CancellationToken cancellationToken)
    {
        ValidateManifest(manifest);
        string destination = ResolveDestination(
            assetsRoot,
            manifest.destinationRelativePath);
        string destinationMetadata =
            destination + AssetDatabase.MetadataSuffix;
        string newAsset = Path.Combine(stagingDirectory, NewAssetName);
        string newMetadata = Path.Combine(stagingDirectory, NewMetadataName);
        string oldAsset = Path.Combine(stagingDirectory, OldAssetName);
        string oldMetadata = Path.Combine(stagingDirectory, OldMetadataName);

        bool hasNewAsset = File.Exists(newAsset);
        bool hasNewMetadata = File.Exists(newMetadata);
        bool hasOldAsset = File.Exists(oldAsset);
        bool hasOldMetadata = File.Exists(oldMetadata);
        bool hasDestination = File.Exists(destination);
        bool hasDestinationMetadata = File.Exists(destinationMetadata);

        if (hasNewAsset)
        {
            EnsureMatchingFile(
                newAsset,
                manifest.newPayloadLength,
                manifest.newPayloadSha256,
                cancellationToken,
                "Staged replacement");
            // 新しい内容の公開は開始されていません。すでに退避した元ファイルがあれば復元します。
            if (hasDestination &&
                !FileMatches(
                    destination,
                    manifest.oldPayloadLength,
                    manifest.oldPayloadSha256,
                    cancellationToken))
            {
                throw new InvalidDataException(
                    "Destination changed before replacement publication.");
            }
            if (!hasDestination)
            {
                if (!hasOldAsset)
                    throw new InvalidDataException("Original asset backup is missing.");
                EnsureMatchingFile(
                    oldAsset,
                    manifest.oldPayloadLength,
                    manifest.oldPayloadSha256,
                    cancellationToken,
                    "Original asset backup");
                File.Move(oldAsset, destination, overwrite: false);
                hasOldAsset = false;
            }
            if (hasDestinationMetadata &&
                !FileMatches(
                    destinationMetadata,
                    expectedLength: null,
                    manifest.oldMetadataSha256,
                    cancellationToken))
            {
                throw new InvalidDataException(
                    "Destination metadata changed before replacement publication.");
            }
            if (!hasDestinationMetadata)
            {
                if (!hasOldMetadata)
                    throw new InvalidDataException("Original metadata backup is missing.");
                EnsureMatchingFile(
                    oldMetadata,
                    expectedLength: null,
                    manifest.oldMetadataSha256,
                    cancellationToken,
                    "Original metadata backup");
                File.Move(oldMetadata, destinationMetadata, overwrite: false);
                hasOldMetadata = false;
            }
            if (hasOldAsset || hasOldMetadata)
            {
                throw new InvalidDataException(
                    "Duplicate original backup remains after recovery.");
            }
            if (!TryCleanup(stagingDirectory, out string? rollbackWarning))
                throw new IOException(rollbackWarning);
            return false;
        }

        // ステージ済み置換ファイルが消えた原因は、公開先へのアトミックな移動だけです。
        if (!hasDestination)
            throw new InvalidDataException("Published replacement asset is missing.");
        EnsureMatchingFile(
            destination,
            manifest.newPayloadLength,
            manifest.newPayloadSha256,
            cancellationToken,
            "Published replacement");
        if (!hasOldAsset || !hasOldMetadata)
        {
            throw new InvalidDataException(
                "Original backups are incomplete after replacement publication.");
        }
        EnsureMatchingFile(
            oldAsset,
            manifest.oldPayloadLength,
            manifest.oldPayloadSha256,
            cancellationToken,
            "Original asset backup");
        EnsureMatchingFile(
            oldMetadata,
            expectedLength: null,
            manifest.oldMetadataSha256,
            cancellationToken,
            "Original metadata backup");

        if (hasDestinationMetadata)
        {
            EnsureMatchingFile(
                destinationMetadata,
                expectedLength: null,
                manifest.newMetadataSha256,
                cancellationToken,
                "Published replacement metadata");
            if (hasNewMetadata)
            {
                throw new InvalidDataException(
                    "Both staged and published replacement metadata exist.");
            }
        }
        else
        {
            if (!hasNewMetadata)
                throw new InvalidDataException("Replacement metadata is missing.");
            EnsureMatchingFile(
                newMetadata,
                expectedLength: null,
                manifest.newMetadataSha256,
                cancellationToken,
                "Staged replacement metadata");
            ValidateDestinationParent(assetsRoot, destination);
            File.Move(newMetadata, destinationMetadata, overwrite: false);
        }

        if (!TryCleanup(stagingDirectory, out string? cleanupWarning))
            throw new IOException(cleanupWarning);
        return true;
    }

    private static bool TryRollback(Transaction transaction, List<string> warnings)
    {
        bool complete = true;
        try
        {
            ValidateDestinationParent(
                transaction.AssetsRoot,
                transaction.Destination);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            warnings.Add(
                "Reimport rollback preserved an unsafe destination: " + error.Message);
            return false;
        }
        try
        {
            if (File.Exists(transaction.DestinationMetadata) &&
                FileMatches(
                    transaction.DestinationMetadata,
                    expectedLength: null,
                    transaction.Manifest.newMetadataSha256,
                    CancellationToken.None))
            {
                File.Delete(transaction.DestinationMetadata);
            }
            if (File.Exists(transaction.Destination) &&
                FileMatches(
                    transaction.Destination,
                    transaction.Manifest.newPayloadLength,
                    transaction.Manifest.newPayloadSha256,
                    CancellationToken.None))
            {
                File.Delete(transaction.Destination);
            }
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            complete = false;
            warnings.Add("Replacement cleanup failed: " + error.Message);
        }

        try
        {
            if (File.Exists(transaction.OldAsset))
            {
                if (File.Exists(transaction.Destination))
                    complete = false;
                else
                    File.Move(transaction.OldAsset, transaction.Destination, overwrite: false);
            }
            if (File.Exists(transaction.OldMetadata))
            {
                if (File.Exists(transaction.DestinationMetadata))
                    complete = false;
                else
                    File.Move(
                        transaction.OldMetadata,
                        transaction.DestinationMetadata,
                        overwrite: false);
            }
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException)
        {
            complete = false;
            warnings.Add("Original restore failed: " + error.Message);
        }

        try
        {
            if (!FileMatches(
                    transaction.Destination,
                    transaction.Manifest.oldPayloadLength,
                    transaction.Manifest.oldPayloadSha256,
                    CancellationToken.None) ||
                !FileMatches(
                    transaction.DestinationMetadata,
                    expectedLength: null,
                    transaction.Manifest.oldMetadataSha256,
                    CancellationToken.None))
            {
                complete = false;
            }
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            complete = false;
            warnings.Add("Restored originals could not be verified: " + error.Message);
        }

        if (complete &&
            !TryCleanup(transaction.StagingDirectory, out string? cleanupWarning))
        {
            complete = false;
            warnings.Add(cleanupWarning!);
        }
        return complete;
    }

    private static Snapshot CopyAndHash(
        string source,
        string destination,
        CancellationToken cancellationToken)
    {
        long lastWrite = File.GetLastWriteTimeUtc(source).Ticks;
        long length = 0;
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using (var input = new FileStream(
            source,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan))
        using (var output = new FileStream(
            destination,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            128 * 1024,
            FileOptions.WriteThrough))
        {
            var buffer = new byte[128 * 1024];
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                int read = input.Read(buffer, 0, buffer.Length);
                if (read == 0)
                    break;
                output.Write(buffer, 0, read);
                hash.AppendData(buffer, 0, read);
                length += read;
            }
            output.Flush(flushToDisk: true);
        }
        ValidateOrdinarySource(source);
        return new Snapshot(
            length,
            lastWrite,
            Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant());
    }

    private static string ResolveSourcePath(
        AssetDatabase database,
        AssetRecord record)
    {
        string value = record.Metadata.Source.Replace(
            '/',
            Path.DirectorySeparatorChar);
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidDataException("The asset has no import source.");
        return Path.GetFullPath(
            Path.IsPathRooted(value)
                ? value
                : Path.Combine(database.ProjectRoot, value));
    }

    private static void ValidateOrdinarySource(string path)
    {
        EnsureOrdinaryFile(path, "Reimport source");
        EnsureNoReparseDirectories(Path.GetDirectoryName(path)
            ?? throw new InvalidDataException("Reimport source has no parent."));
    }

    private static void ValidateDestination(string assetsRoot, string destination)
    {
        ValidateDestinationParent(assetsRoot, destination);
        EnsureOrdinaryFile(destination, "Reimport destination");
        EnsureOrdinaryFile(
            destination + AssetDatabase.MetadataSuffix,
            "Reimport destination metadata");
    }

    private static void ValidateDestinationParent(
        string assetsRoot,
        string destination)
    {
        string root = NormalizeDirectory(assetsRoot);
        string full = Path.GetFullPath(destination);
        if (!IsUnder(full, root))
            throw new InvalidDataException("Reimport destination escapes Assets.");
        string parent = Path.GetDirectoryName(full)
            ?? throw new InvalidDataException("Reimport destination has no parent.");
        string relative = Path.GetRelativePath(root, parent);
        string cursor = root;
        EnsureOrdinaryDirectory(cursor, "Assets root");
        if (relative != ".")
        {
            foreach (string segment in SplitPath(relative))
            {
                if (string.Equals(
                        segment,
                        AssetDatabase.InternalDirectoryName,
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException("Reimport destination uses .acsdb.");
                }
                cursor = Path.Combine(cursor, segment);
                EnsureOrdinaryDirectory(cursor, "Reimport destination parent");
            }
        }
    }

    private static string ResolveDestination(string assetsRoot, string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) ||
            Path.IsPathRooted(relative) ||
            relative.Length > 4096)
        {
            throw new InvalidDataException("Invalid reimport destination.");
        }
        string destination = Path.GetFullPath(Path.Combine(
            assetsRoot,
            relative.Replace('/', Path.DirectorySeparatorChar)));
        ValidateDestinationParent(assetsRoot, destination);
        return destination;
    }

    private static string EnsureStagingRoot(string assetsRoot)
    {
        string root = NormalizeDirectory(assetsRoot);
        EnsureOrdinaryDirectory(root, "Assets root");
        string databaseDirectory = Path.Combine(
            root,
            AssetDatabase.InternalDirectoryName);
        if (!Directory.Exists(databaseDirectory))
            Directory.CreateDirectory(databaseDirectory);
        EnsureOrdinaryDirectory(databaseDirectory, "Asset database directory");
        string stagingRoot = Path.Combine(databaseDirectory, StagingDirectoryName);
        if (!Directory.Exists(stagingRoot))
            Directory.CreateDirectory(stagingRoot);
        EnsureOrdinaryDirectory(stagingRoot, "Reimport staging directory");
        return stagingRoot;
    }

    private static void WriteJournal(string path, ReimportManifest manifest)
    {
        ValidateManifest(manifest);
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(
            manifest,
            new JsonSerializerOptions { WriteIndented = true });
        string temporary =
            path + ".tmp-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        try
        {
            WriteNewDurableFile(temporary, bytes);
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temporary))
                    File.Delete(temporary);
            }
            catch
            {
            }
        }
    }

    private static ReimportManifest ReadJournal(string path)
    {
        EnsureOrdinaryFile(path, "Reimport journal");
        if (new FileInfo(path).Length > MaxJournalBytes)
            throw new InvalidDataException("Reimport journal exceeds 64 KiB.");
        ReimportManifest manifest = JsonSerializer.Deserialize<ReimportManifest>(
            File.ReadAllBytes(path),
            new JsonSerializerOptions
            {
                AllowTrailingCommas = false,
                ReadCommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 16,
            }) ?? throw new InvalidDataException("Reimport journal is empty.");
        ValidateManifest(manifest);
        return manifest;
    }

    private static void ValidateManifest(ReimportManifest manifest)
    {
        if (manifest.schemaVersion != SchemaVersion ||
            !Guid.TryParseExact(manifest.transactionId, "N", out Guid transaction) ||
            transaction == Guid.Empty ||
            !Guid.TryParseExact(manifest.assetId, "N", out Guid asset) ||
            asset == Guid.Empty ||
            manifest.destinationRelativePath.Length is 0 or > 4096 ||
            Path.IsPathRooted(manifest.destinationRelativePath) ||
            manifest.sourcePath.Length is 0 or > 4096 ||
            manifest.newPayloadLength < 0 ||
            manifest.oldPayloadLength < 0 ||
            !IsSha256(manifest.newPayloadSha256) ||
            !IsSha256(manifest.newMetadataSha256) ||
            !IsSha256(manifest.oldPayloadSha256) ||
            !IsSha256(manifest.oldMetadataSha256) ||
            manifest.createdUtcTicks <= 0)
        {
            throw new InvalidDataException("Invalid reimport journal.");
        }
        _ = ParsePhase(manifest.phase);
    }

    private static bool ContainsOnlyUnpublishedPreparationFiles(string directory)
    {
        foreach (FileSystemInfo entry in new DirectoryInfo(directory)
            .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
        {
            entry.Refresh();
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                (entry.Attributes & FileAttributes.Directory) != 0 ||
                (entry.Name != NewAssetName &&
                 entry.Name != NewMetadataName &&
                 !entry.Name.StartsWith(
                     JournalName + ".tmp-",
                     StringComparison.Ordinal)))
            {
                return false;
            }
        }
        return true;
    }

    private static bool TryCleanup(string directory, out string? warning)
    {
        warning = null;
        try
        {
            if (!Directory.Exists(directory))
                return true;
            EnsureOrdinaryDirectory(directory, "Reimport transaction directory");
            foreach (FileSystemInfo entry in new DirectoryInfo(directory)
                .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                entry.Refresh();
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                    (entry.Attributes & FileAttributes.Directory) != 0 ||
                    !IsKnownName(entry.Name))
                {
                    warning =
                        $"Reimport cleanup preserved transaction '{Path.GetFileName(directory)}' " +
                        $"because it contains unexpected entry '{entry.Name}'.";
                    return false;
                }
                File.Delete(entry.FullName);
            }
            Directory.Delete(directory, recursive: false);
            return true;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            warning =
                $"Reimport cleanup left transaction '{Path.GetFileName(directory)}': " +
                error.Message;
            return false;
        }
    }

    private static bool IsKnownName(string name) =>
        name is JournalName or NewAssetName or NewMetadataName or
            OldAssetName or OldMetadataName ||
        name.StartsWith(JournalName + ".tmp-", StringComparison.Ordinal);

    private static void EnsureMatchingFile(
        string path,
        long? expectedLength,
        string expectedHash,
        CancellationToken cancellationToken,
        string label)
    {
        if (!FileMatches(path, expectedLength, expectedHash, cancellationToken))
            throw new InvalidDataException($"{label} does not match its journal.");
    }

    private static bool FileMatches(
        string path,
        long? expectedLength,
        string expectedHash,
        CancellationToken cancellationToken)
    {
        EnsureOrdinaryFile(path, "Reimport transaction file");
        var info = new FileInfo(path);
        return (!expectedLength.HasValue || info.Length == expectedLength.Value) &&
               string.Equals(
                   ComputeSha256(path, cancellationToken),
                   expectedHash,
                   StringComparison.OrdinalIgnoreCase);
    }

    private static string ComputeSha256(
        string path,
        CancellationToken cancellationToken)
    {
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[128 * 1024];
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int read = stream.Read(buffer, 0, buffer.Length);
            if (read == 0)
                break;
            hash.AppendData(buffer, 0, read);
        }
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    private static string ComputeSha256(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static void WriteNewDurableFile(string path, byte[] bytes)
    {
        using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            16 * 1024,
            FileOptions.WriteThrough);
        stream.Write(bytes);
        stream.Flush(flushToDisk: true);
    }

    private static void EnsureNoReparseDirectories(string directory)
    {
        string full = NormalizeDirectory(directory);
        string root = Path.GetPathRoot(full)
            ?? throw new InvalidDataException("Path has no filesystem root.");
        string cursor = root;
        EnsureOrdinaryDirectory(cursor, "Filesystem root");
        string relative = Path.GetRelativePath(root, full);
        if (relative == ".")
            return;
        foreach (string segment in SplitPath(relative))
        {
            cursor = Path.Combine(cursor, segment);
            EnsureOrdinaryDirectory(cursor, "Reimport source parent");
        }
    }

    private static void EnsureOrdinaryDirectory(string path, string label)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException($"{label} must be an ordinary directory.");
        }
    }

    private static void EnsureOrdinaryFile(string path, string label)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) != 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException($"{label} must be an ordinary file.");
        }
    }

    private static void InvokeCheckpoint(
        AssetReimportTestHooks? hooks,
        AssetReimportCheckpoint checkpoint)
    {
        if (hooks?.SimulateCrashAfter == checkpoint)
            throw new AssetReimportSimulatedCrashException(checkpoint);
        if (hooks?.FailAfter == checkpoint)
            throw new IOException($"Injected reimport failure after '{checkpoint}'.");
    }

    private static string PhaseText(AssetReimportCheckpoint phase) => phase switch
    {
        AssetReimportCheckpoint.Prepared => "prepared",
        AssetReimportCheckpoint.OriginalsBackedUp => "originalsBackedUp",
        AssetReimportCheckpoint.AssetPublished => "assetPublished",
        AssetReimportCheckpoint.MetadataPublished => "metadataPublished",
        _ => throw new InvalidDataException("Unknown reimport phase."),
    };

    private static AssetReimportCheckpoint ParsePhase(string phase) => phase switch
    {
        "prepared" => AssetReimportCheckpoint.Prepared,
        "originalsBackedUp" => AssetReimportCheckpoint.OriginalsBackedUp,
        "assetPublished" => AssetReimportCheckpoint.AssetPublished,
        "metadataPublished" => AssetReimportCheckpoint.MetadataPublished,
        _ => throw new InvalidDataException("Unknown reimport journal phase."),
    };

    private static bool IsSha256(string value) =>
        value?.Length == 64 && value.All(static character =>
            character is >= '0' and <= '9' or
                >= 'a' and <= 'f' or
                >= 'A' and <= 'F');

    private static string NormalizeDirectory(string path)
    {
        string full = Path.GetFullPath(path);
        string root = Path.GetPathRoot(full) ?? "";
        return full.Length > root.Length
            ? Path.TrimEndingDirectorySeparator(full)
            : full;
    }

    private static string[] SplitPath(string path) =>
        path.Split(
            new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
            StringSplitOptions.RemoveEmptyEntries);

    private static bool IsUnder(string candidate, string root)
    {
        string relative = Path.GetRelativePath(root, candidate);
        return relative != "." &&
               !Path.IsPathRooted(relative) &&
               relative != ".." &&
               !relative.StartsWith(
                   ".." + Path.DirectorySeparatorChar,
                   StringComparison.Ordinal) &&
               !relative.StartsWith(
                   ".." + Path.AltDirectorySeparatorChar,
                   StringComparison.Ordinal);
    }

    private static bool PathEquals(string left, string right) =>
        string.Equals(
            Path.GetFullPath(left),
            Path.GetFullPath(right),
            PathComparison);

    private sealed record Snapshot(
        long Length,
        long LastWriteUtcTicks,
        string Hash);

    private sealed record Transaction(
        string AssetsRoot,
        string StagingDirectory,
        string JournalPath,
        string NewAsset,
        string NewMetadata,
        string OldAsset,
        string OldMetadata,
        string Destination,
        string DestinationMetadata,
        ReimportManifest Manifest);

    private sealed class ReimportManifest
    {
        public int schemaVersion { get; set; }
        public string transactionId { get; set; } = "";
        public string phase { get; set; } = "";
        public string assetId { get; set; } = "";
        public string destinationRelativePath { get; set; } = "";
        public string sourcePath { get; set; } = "";
        public long newPayloadLength { get; set; }
        public string newPayloadSha256 { get; set; } = "";
        public string newMetadataSha256 { get; set; } = "";
        public long oldPayloadLength { get; set; }
        public string oldPayloadSha256 { get; set; } = "";
        public string oldMetadataSha256 { get; set; } = "";
        public long createdUtcTicks { get; set; }
    }
}
