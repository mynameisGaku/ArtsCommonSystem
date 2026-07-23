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

internal sealed record ImportedAssetFile(string SourcePath, string DestinationPath);

internal sealed record AssetImportOperationResult(
    IReadOnlyList<ImportedAssetFile> Imported,
    IReadOnlyList<string> Failures,
    IReadOnlyList<string> Warnings,
    AssetDatabaseRefreshResult IndexResult);

internal sealed record AssetImportReconciliationResult(
    int CompletedTransactions,
    int DiscardedTransactions,
    int PreservedTransactions,
    IReadOnlyList<string> Warnings);

internal enum AssetImportCheckpoint
{
    Prepared,
    AssetPublished,
    MetadataPublished,
}

/// <summary>中間公開状態を作らないインポートのセルフテストだけで使用する障害注入です。</summary>
internal sealed record AssetImportTestHooks(
    AssetImportCheckpoint? FailAfter = null,
    AssetImportCheckpoint? SimulateCrashAfter = null,
    Action<AssetImportCheckpoint>? Observe = null);

internal sealed class AssetImportSimulatedCrashException : IOException
{
    internal AssetImportSimulatedCrashException(AssetImportCheckpoint checkpoint)
        : base($"Simulated process termination after import checkpoint '{checkpoint}'.")
    {
    }
}

/// <summary>
/// クラッシュから復旧可能なコンテンツブラウザーのインポートです。
///
/// 各入力を完全な正規サイドカーおよび永続ジャーナルとともに
/// <c>Assets/.acsdb/import-staging</c> へコピーし、どちらの最終パスも公開する前に準備します。
/// ステージング先と公開先は同じアセット用ボリュームにあるため、二回の公開処理は
/// アトミックな名前変更になります。ジャーナルは、プロセス終了時に生じ得る二回の名前変更間の
/// 不可避な中間状態を解消します。起動時には、一致を確認できるペアを完成させるか、
/// 一度も公開されなかった準備内容を破棄するか、ユーザーファイルを削除せずに
/// 判断不能なデータを手動確認用として保持します。
///
/// バッチ方針として、一つの入力元に固有の失敗はその入力元だけをロールバックし、
/// 残りの入力処理は継続します。成功した入力はまとめてインデックスへコミットします。
/// 最終的なインデックス公開に失敗した場合、そのバッチで新たに公開した入力を
/// すべてロールバックします。
/// </summary>
internal static class AssetImportWorkflow
{
    private const int JournalSchemaVersion = 1;
    private const int MaxJournalBytes = 64 * 1024;
    private const int MaxGeneratedSuffix = 9999;
    internal const string StagingDirectoryName =
        AssetDatabase.ImportStagingDirectoryName;
    private const string JournalFileName = "manifest.v1.json";
    private const string PayloadFileName = "payload";
    private const string MetadataFileName = "payload.acsmeta";
    private static readonly UTF8Encoding Utf8NoBom = new(false, true);
    private static readonly StringComparison PathComparison =
        StringComparison.OrdinalIgnoreCase;

    internal static AssetImportOperationResult ImportFiles(
        AssetDatabase database,
        string destinationDirectory,
        IEnumerable<string> sourcePaths,
        CancellationToken cancellationToken = default,
        AssetImportTestHooks? testHooks = null)
    {
        ArgumentNullException.ThrowIfNull(database);
        ArgumentNullException.ThrowIfNull(sourcePaths);
        string[] sources = sourcePaths.ToArray();

        using AssetMutationLock mutationLock = AssetMutationLock.AcquireForRecovery(
            database.AssetsRoot,
            "Import assets");
        cancellationToken.ThrowIfCancellationRequested();
        if (ProjectManager.HasPendingInitialScenePathFollow(database.AssetsRoot))
        {
            throw new IOException(
                "Asset import is blocked while an initial-scene move requires recovery.");
        }

        AssetImportReconciliationResult recovery = ReconcileCore(
            database.AssetsRoot,
            cancellationToken);
        if (recovery.PreservedTransactions != 0)
        {
            throw new IOException(
                "Asset import is blocked because an earlier transaction requires manual " +
                $"inspection below Assets/{AssetDatabase.InternalDirectoryName}/" +
                $"{StagingDirectoryName}.");
        }
        AssetImportReconciliationResult reimportRecovery =
            AssetReimportWorkflow.Reconcile(database, cancellationToken);
        if (reimportRecovery.PreservedTransactions != 0)
        {
            throw new IOException(
                "Asset import is blocked because an earlier Reimport transaction requires " +
                "manual inspection below Assets/.acsdb/" +
                $"{AssetReimportWorkflow.StagingDirectoryName}.");
        }

        string targetDirectory = ValidateTargetDirectory(
            database.AssetsRoot,
            destinationDirectory);

        // 同じプロジェクトリースの下で正規の識別情報を再読み込みします。長時間動作する
        // エディターが、別のエディターによるインポート後も古いメモリ内ビューから
        // メタデータを割り当てることを防ぎます。
        _ = database.RefreshWithinAssetTransaction(
            verifyContent: false,
            cancellationToken);

        var imported = new List<ImportedAssetFile>();
        var failures = new List<string>();
        var warnings = new List<string>(recovery.Warnings);
        warnings.AddRange(reimportRecovery.Warnings);
        var published = new List<ImportTransaction>();

        try
        {
            foreach (string source in sources)
            {
                cancellationToken.ThrowIfCancellationRequested();
                try
                {
                    ImportTransaction transaction = PrepareAndPublish(
                        database,
                        targetDirectory,
                        source,
                        cancellationToken,
                        testHooks);
                    published.Add(transaction);
                    imported.Add(new ImportedAssetFile(
                        transaction.Manifest.SourcePath,
                        transaction.DestinationPath));
                }
                catch (AssetImportSimulatedCrashException)
                {
                    throw;
                }
                catch (AssetImportRollbackIncompleteException)
                {
                    throw;
                }
                catch (Exception error) when (
                    error is IOException or UnauthorizedAccessException or
                        InvalidDataException or ArgumentException)
                {
                    failures.Add($"{SafeFileName(source)}: {error.Message}");
                }
            }
        }
        catch (OperationCanceledException cancellationError)
        {
            bool rollbackComplete = RollbackPublishedBatch(
                database,
                published,
                warnings);
            if (!rollbackComplete)
            {
                throw new AssetImportRollbackIncompleteException(
                    "Import was cancelled and rollback of the already-published batch was " +
                    "incomplete. The recovery journal was retained.",
                    cancellationError);
            }
            throw;
        }

        AssetDatabaseRefreshResult indexResult;
        try
        {
            indexResult = database.RefreshWithinAssetTransaction(
                verifyContent: false,
                cancellationToken);
        }
        catch (Exception indexError) when (
            indexError is IOException or UnauthorizedAccessException or
                InvalidDataException or JsonException or OperationCanceledException)
        {
            bool rollbackComplete = RollbackPublishedBatch(
                database,
                published,
                warnings);

            if (!rollbackComplete)
            {
                throw new AssetImportRollbackIncompleteException(
                    "The asset index could not be published and batch rollback was incomplete. " +
                    "The import journal was retained for startup recovery.",
                    indexError);
            }
            if (indexError is OperationCanceledException)
                throw;
            throw new IOException(
                "The asset index could not be published; all files imported by this batch " +
                "were rolled back.",
                indexError);
        }

        foreach (ImportTransaction transaction in published)
        {
            if (!TryCleanupTransaction(transaction.StagingDirectory, out string? cleanupWarning))
                warnings.Add(cleanupWarning!);
        }

        return new AssetImportOperationResult(
            Array.AsReadOnly(imported.ToArray()),
            Array.AsReadOnly(failures.ToArray()),
            Array.AsReadOnly(warnings.ToArray()),
            indexResult);
    }

    private static bool RollbackPublishedBatch(
        AssetDatabase database,
        IReadOnlyList<ImportTransaction> published,
        List<string> warnings)
    {
        bool rollbackComplete = true;
        foreach (ImportTransaction transaction in published.Reverse())
        {
            rollbackComplete &= TryRollbackTransaction(
                transaction,
                warnings,
                CancellationToken.None);
        }
        try
        {
            _ = database.RefreshWithinAssetTransaction(
                verifyContent: false,
                CancellationToken.None);
        }
        catch (Exception error)
        {
            rollbackComplete = false;
            warnings.Add(
                "The asset index could not be repaired after import rollback: " +
                error.Message);
        }
        return rollbackComplete;
    }

    /// <summary>
    /// 終了したエディターが残したジャーナルを整合させます。最終公開先は削除せず、
    /// 判断不能または不一致の状態は保持して報告します。
    /// </summary>
    internal static AssetImportReconciliationResult Reconcile(
        AssetDatabase database,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(database);
        using AssetMutationLock mutationLock = AssetMutationLock.AcquireForRecovery(
            database.AssetsRoot,
            "Recover interrupted asset imports");
        return ReconcileCore(database.AssetsRoot, cancellationToken);
    }

    private static ImportTransaction PrepareAndPublish(
        AssetDatabase database,
        string targetDirectory,
        string sourcePath,
        CancellationToken cancellationToken,
        AssetImportTestHooks? testHooks)
    {
        string source = ValidateSourceFile(database.AssetsRoot, sourcePath);
        string destination = ReserveDestination(targetDirectory, source);
        string relativeDestination = NormalizeRelativeAssetPath(
            database.AssetsRoot,
            destination);
        string transactionId = Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        string stagingRoot = EnsureStagingRoot(database.AssetsRoot);
        string stagingDirectory = Path.Combine(stagingRoot, transactionId);
        Directory.CreateDirectory(stagingDirectory);
        EnsureOrdinaryDirectory(stagingDirectory, "Import transaction directory");

        string stagedPayload = Path.Combine(stagingDirectory, PayloadFileName);
        string stagedMetadata = Path.Combine(stagingDirectory, MetadataFileName);
        ImportTransaction? transaction = null;
        try
        {
            SourceSnapshot snapshot = CopySourceToStage(
                source,
                stagedPayload,
                cancellationToken);
            string importer = ImporterForKind(
                AssetDatabase.ClassifyExtension(Path.GetExtension(relativeDestination)));
            var importSettings = new[]
            {
                KeyValuePair.Create(
                    "sourceContentHash",
                    snapshot.ContentHash),
                KeyValuePair.Create(
                    "sourceLastWriteUtcTicks",
                    snapshot.LastWriteUtcTicks.ToString(CultureInfo.InvariantCulture)),
                KeyValuePair.Create(
                    "sourceSizeBytes",
                    snapshot.Length.ToString(CultureInfo.InvariantCulture)),
            };
            (AssetMetadata _, byte[] metadataBytes) =
                database.CreateImportMetadataPayload(
                    relativeDestination,
                    source,
                    importer,
                    importerVersion: 1,
                    importSettings);
            WriteNewDurableFile(stagedMetadata, metadataBytes);

            var manifest = new ImportManifest(
                JournalSchemaVersion,
                transactionId,
                AssetImportCheckpoint.Prepared,
                relativeDestination,
                source.Replace('\\', '/'),
                snapshot.Length,
                snapshot.ContentHash,
                ComputeSha256(metadataBytes),
                DateTime.UtcNow.Ticks);
            string journalPath = Path.Combine(stagingDirectory, JournalFileName);
            WriteJournal(journalPath, manifest);
            transaction = new ImportTransaction(
                database.AssetsRoot,
                stagingDirectory,
                journalPath,
                stagedPayload,
                stagedMetadata,
                destination,
                destination + AssetDatabase.MetadataSuffix,
                manifest);
            InvokeCheckpoint(testHooks, AssetImportCheckpoint.Prepared);

            cancellationToken.ThrowIfCancellationRequested();
            _ = ValidateSourceFile(database.AssetsRoot, source);
            ValidateTargetDirectory(database.AssetsRoot, targetDirectory);
            EnsureDestinationAvailable(transaction);
            File.Move(stagedPayload, destination, overwrite: false);
            ValidateTargetDirectory(database.AssetsRoot, targetDirectory);
            EnsureMatchingOrdinaryFile(
                destination,
                manifest.PayloadLength,
                manifest.PayloadSha256,
                "Published asset",
                cancellationToken);
            manifest = manifest with { Phase = AssetImportCheckpoint.AssetPublished };
            transaction = transaction with { Manifest = manifest };
            WriteJournal(journalPath, manifest);
            InvokeCheckpoint(testHooks, AssetImportCheckpoint.AssetPublished);

            cancellationToken.ThrowIfCancellationRequested();
            ValidateTargetDirectory(database.AssetsRoot, targetDirectory);
            EnsureMatchingOrdinaryFile(
                destination,
                manifest.PayloadLength,
                manifest.PayloadSha256,
                "Published asset");
            if (File.Exists(transaction.DestinationMetadataPath) ||
                Directory.Exists(transaction.DestinationMetadataPath))
            {
                throw new IOException(
                    $"Import metadata destination already exists: {relativeDestination}" +
                    AssetDatabase.MetadataSuffix);
            }
            File.Move(
                stagedMetadata,
                transaction.DestinationMetadataPath,
                overwrite: false);
            ValidateTargetDirectory(database.AssetsRoot, targetDirectory);
            EnsureMatchingOrdinaryFile(
                transaction.DestinationMetadataPath,
                expectedLength: null,
                manifest.MetadataSha256,
                "Published metadata",
                cancellationToken);
            manifest = manifest with { Phase = AssetImportCheckpoint.MetadataPublished };
            transaction = transaction with { Manifest = manifest };
            WriteJournal(journalPath, manifest);
            InvokeCheckpoint(testHooks, AssetImportCheckpoint.MetadataPublished);
            return transaction;
        }
        catch (AssetImportSimulatedCrashException)
        {
            throw;
        }
        catch (Exception error)
        {
            if (transaction == null)
            {
                if (!TryCleanupTransaction(stagingDirectory, out _))
                {
                    throw new AssetImportRollbackIncompleteException(
                        "Import preparation failed and its private staging directory could not " +
                        "be cleaned.",
                        error);
                }
                throw;
            }

            var rollbackWarnings = new List<string>();
            if (!TryRollbackTransaction(
                    transaction,
                    rollbackWarnings,
                    CancellationToken.None))
            {
                throw new AssetImportRollbackIncompleteException(
                    "Import failed after publication and automatic rollback was incomplete. " +
                    string.Join(" ", rollbackWarnings),
                    error);
            }
            throw;
        }
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
            .OrderBy(static item => item.Name, StringComparer.Ordinal))
        {
            cancellationToken.ThrowIfCancellationRequested();
            entry.Refresh();
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                (entry.Attributes & FileAttributes.Directory) == 0 ||
                entry is not DirectoryInfo directory)
            {
                preserved++;
                warnings.Add(
                    $"Import recovery preserved unexpected staging entry '{entry.Name}'.");
                continue;
            }

            if (!Guid.TryParseExact(directory.Name, "N", out Guid parsedId) ||
                parsedId == Guid.Empty)
            {
                preserved++;
                warnings.Add(
                    $"Import recovery preserved unknown transaction directory '{entry.Name}'.");
                continue;
            }

            string journalPath = Path.Combine(directory.FullName, JournalFileName);
            if (!File.Exists(journalPath))
            {
                string? cleanupWarning = null;
                if (ContainsOnlyPrivatePreparationFiles(directory.FullName) &&
                    TryCleanupTransaction(directory.FullName, out cleanupWarning))
                {
                    discarded++;
                }
                else
                {
                    preserved++;
                    warnings.Add(
                        cleanupWarning ??
                        $"Import recovery preserved incomplete transaction '{entry.Name}'.");
                }
                continue;
            }

            try
            {
                ImportManifest manifest = ReadJournal(journalPath);
                if (!string.Equals(
                        manifest.TransactionId,
                        directory.Name,
                        StringComparison.Ordinal))
                {
                    throw new InvalidDataException(
                        "Journal transaction id does not match its directory.");
                }

                RecoveryDisposition disposition = RecoverTransaction(
                    assetsRoot,
                    directory.FullName,
                    journalPath,
                    manifest,
                    cancellationToken);
                if (disposition == RecoveryDisposition.Completed)
                    completed++;
                else if (disposition == RecoveryDisposition.Discarded)
                    discarded++;
                else
                    preserved++;
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    InvalidDataException or JsonException or ArgumentException)
            {
                preserved++;
                warnings.Add(
                    $"Import recovery preserved transaction '{entry.Name}': {error.Message}");
            }
        }

        return new AssetImportReconciliationResult(
            completed,
            discarded,
            preserved,
            Array.AsReadOnly(warnings.ToArray()));
    }

    private static RecoveryDisposition RecoverTransaction(
        string assetsRoot,
        string stagingDirectory,
        string journalPath,
        ImportManifest manifest,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ValidateManifest(manifest);
        string destination = ResolveRelativeAssetPath(
            assetsRoot,
            manifest.DestinationRelativePath);
        string destinationParent = Path.GetDirectoryName(destination)
            ?? throw new InvalidDataException("Recovered destination has no parent.");
        ValidateTargetDirectory(assetsRoot, destinationParent);

        string stagedPayload = Path.Combine(stagingDirectory, PayloadFileName);
        string stagedMetadata = Path.Combine(stagingDirectory, MetadataFileName);
        string destinationMetadata = destination + AssetDatabase.MetadataSuffix;
        bool stagedAssetExists = File.Exists(stagedPayload);
        bool stagedMetadataExists = File.Exists(stagedMetadata);
        bool destinationExists = File.Exists(destination);
        bool destinationMetadataExists = File.Exists(destinationMetadata);

        if (Directory.Exists(destination) ||
            Directory.Exists(destinationMetadata))
        {
            throw new InvalidDataException(
                "Recovered destination is shadowed by a directory.");
        }
        if (stagedAssetExists)
        {
            EnsureMatchingOrdinaryFile(
                stagedPayload,
                manifest.PayloadLength,
                manifest.PayloadSha256,
                "Staged asset",
                cancellationToken);
        }
        if (stagedMetadataExists)
        {
            EnsureMatchingOrdinaryFile(
                stagedMetadata,
                expectedLength: null,
                manifest.MetadataSha256,
                "Staged metadata",
                cancellationToken);
        }

        // 両方の非公開ファイルが残っているため、公開は開始されていません。一致する公開先が
        // 独立して現れた場合は判断不能な状態となるため、削除も採用もしてはいけません。
        if (stagedAssetExists)
        {
            if (destinationExists || destinationMetadataExists)
            {
                throw new InvalidDataException(
                    "A destination appeared before the staged payload was published.");
            }
            if (!stagedMetadataExists ||
                manifest.Phase != AssetImportCheckpoint.Prepared)
            {
                throw new InvalidDataException(
                    "Prepared transaction has an inconsistent private payload set.");
            }
            if (!TryCleanupTransaction(stagingDirectory, out string? warning))
                throw new IOException(warning);
            return RecoveryDisposition.Discarded;
        }

        if (!destinationExists)
        {
            throw new InvalidDataException(
                "Published payload is missing and no private payload remains.");
        }
        EnsureMatchingOrdinaryFile(
            destination,
            manifest.PayloadLength,
            manifest.PayloadSha256,
            "Recovered asset",
            cancellationToken);

        if (destinationMetadataExists)
        {
            EnsureMatchingOrdinaryFile(
                destinationMetadata,
                expectedLength: null,
                manifest.MetadataSha256,
                "Recovered metadata",
                cancellationToken);
            if (stagedMetadataExists)
            {
                throw new InvalidDataException(
                    "Both staged and destination metadata exist.");
            }
        }
        else
        {
            if (!stagedMetadataExists)
            {
                throw new InvalidDataException(
                    "Published asset has no recoverable metadata.");
            }
            cancellationToken.ThrowIfCancellationRequested();
            ValidateTargetDirectory(assetsRoot, destinationParent);
            File.Move(stagedMetadata, destinationMetadata, overwrite: false);
            EnsureMatchingOrdinaryFile(
                destinationMetadata,
                expectedLength: null,
                manifest.MetadataSha256,
                "Recovered metadata",
                cancellationToken);
        }

        if (!TryCleanupTransaction(stagingDirectory, out string? cleanupWarning))
            throw new IOException(cleanupWarning);
        return RecoveryDisposition.Completed;
    }

    private static bool TryRollbackTransaction(
        ImportTransaction transaction,
        List<string> warnings,
        CancellationToken cancellationToken)
    {
        bool complete = true;
        try
        {
            ValidateTargetDirectory(
                transaction.AssetsRoot,
                Path.GetDirectoryName(transaction.DestinationPath)
                    ?? throw new InvalidDataException(
                        "Import rollback destination has no parent."));
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            warnings.Add(
                "Import rollback preserved an unsafe destination: " + error.Message);
            return false;
        }
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (File.Exists(transaction.DestinationMetadataPath))
            {
                if (FileMatches(
                        transaction.DestinationMetadataPath,
                        expectedLength: null,
                        transaction.Manifest.MetadataSha256))
                {
                    File.Delete(transaction.DestinationMetadataPath);
                }
                else
                {
                    complete = false;
                    warnings.Add(
                        $"Rollback preserved changed metadata at " +
                        $"'{transaction.Manifest.DestinationRelativePath}'.");
                }
            }
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            complete = false;
            warnings.Add("Import metadata rollback failed: " + error.Message);
        }

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (File.Exists(transaction.DestinationPath))
            {
                if (FileMatches(
                        transaction.DestinationPath,
                        transaction.Manifest.PayloadLength,
                        transaction.Manifest.PayloadSha256))
                {
                    File.Delete(transaction.DestinationPath);
                }
                else
                {
                    complete = false;
                    warnings.Add(
                        $"Rollback preserved changed asset " +
                        $"'{transaction.Manifest.DestinationRelativePath}'.");
                }
            }
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            complete = false;
            warnings.Add("Import payload rollback failed: " + error.Message);
        }

        if (complete &&
            !TryCleanupTransaction(transaction.StagingDirectory, out string? cleanupWarning))
        {
            complete = false;
            warnings.Add(cleanupWarning!);
        }
        return complete;
    }

    private static SourceSnapshot CopySourceToStage(
        string source,
        string stagedPayload,
        CancellationToken cancellationToken)
    {
        long lastWriteUtcTicks = File.GetLastWriteTimeUtc(source).Ticks;
        long length = 0;
        string contentHash;
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using (var input = new FileStream(
            source,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan))
        using (var output = new FileStream(
            stagedPayload,
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
            contentHash = Convert.ToHexString(hash.GetHashAndReset())
                .ToLowerInvariant();
        }

        _ = ValidateSourceFile("", source, validateAgainstAssetsRoot: false);
        EnsureOrdinaryFile(stagedPayload, "Staged import payload");
        return new SourceSnapshot(
            length,
            lastWriteUtcTicks,
            contentHash);
    }

    private static string ValidateSourceFile(string assetsRoot, string sourcePath) =>
        ValidateSourceFile(assetsRoot, sourcePath, validateAgainstAssetsRoot: true);

    private static string ValidateSourceFile(
        string assetsRoot,
        string sourcePath,
        bool validateAgainstAssetsRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sourcePath);
        string source = Path.GetFullPath(sourcePath);
        EnsureOrdinaryFile(source, "Import source");
        EnsureNoReparseDirectories(Path.GetDirectoryName(source)
            ?? throw new InvalidDataException("Import source has no parent directory."));
        ValidateImportFileName(Path.GetFileName(source));

        if (validateAgainstAssetsRoot &&
            IsUnderOrEqual(source, Path.Combine(
                Path.GetFullPath(assetsRoot),
                AssetDatabase.InternalDirectoryName)))
        {
            throw new InvalidDataException(
                "Asset database internals cannot be imported.");
        }
        return source;
    }

    private static string ValidateTargetDirectory(
        string assetsRoot,
        string destinationDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(assetsRoot);
        ArgumentException.ThrowIfNullOrWhiteSpace(destinationDirectory);
        string root = NormalizeDirectory(assetsRoot);
        string target = NormalizeDirectory(destinationDirectory);
        EnsureOrdinaryDirectory(root, "Assets root");
        if (!IsUnderOrEqual(target, root))
        {
            throw new InvalidDataException(
                "Import destination must stay below the project's Assets root.");
        }

        string relative = Path.GetRelativePath(root, target);
        string cursor = root;
        if (relative != ".")
        {
            foreach (string segment in SplitPath(relative))
            {
                if (string.Equals(
                        segment,
                        AssetDatabase.InternalDirectoryName,
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException(
                        "Assets/.acsdb is reserved for the asset database.");
                }
                cursor = Path.Combine(cursor, segment);
                EnsureOrdinaryDirectory(cursor, "Import destination");
            }
        }
        return target;
    }

    private static string ReserveDestination(string targetDirectory, string source)
    {
        string fileName = Path.GetFileName(source);
        ValidateImportFileName(fileName);
        string stem = Path.GetFileNameWithoutExtension(fileName);
        string extension = Path.GetExtension(fileName);
        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            string candidateName = suffix == 0
                ? fileName
                : $"{stem} ({suffix}){extension}";
            string candidate = Path.Combine(targetDirectory, candidateName);
            if (!File.Exists(candidate) &&
                !Directory.Exists(candidate) &&
                !File.Exists(candidate + AssetDatabase.MetadataSuffix) &&
                !Directory.Exists(candidate + AssetDatabase.MetadataSuffix))
            {
                return candidate;
            }
        }
        throw new IOException(
            $"No collision-free import name is available for '{fileName}'.");
    }

    private static void EnsureDestinationAvailable(ImportTransaction transaction)
    {
        if (File.Exists(transaction.DestinationPath) ||
            Directory.Exists(transaction.DestinationPath) ||
            File.Exists(transaction.DestinationMetadataPath) ||
            Directory.Exists(transaction.DestinationMetadataPath))
        {
            throw new IOException(
                $"Import destination already exists: " +
                transaction.Manifest.DestinationRelativePath);
        }
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
        EnsureOrdinaryDirectory(stagingRoot, "Import staging directory");
        return stagingRoot;
    }

    private static void WriteJournal(string journalPath, ImportManifest manifest)
    {
        ValidateManifest(manifest);
        byte[] bytes;
        using (var memory = new MemoryStream())
        {
            using (var writer = new Utf8JsonWriter(
                memory,
                new JsonWriterOptions { Indented = true }))
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", manifest.SchemaVersion);
                writer.WriteString("transactionId", manifest.TransactionId);
                writer.WriteString("phase", PhaseText(manifest.Phase));
                writer.WriteString(
                    "destinationRelativePath",
                    manifest.DestinationRelativePath);
                writer.WriteString("sourcePath", manifest.SourcePath);
                writer.WriteNumber("payloadLength", manifest.PayloadLength);
                writer.WriteString("payloadSha256", manifest.PayloadSha256);
                writer.WriteString("metadataSha256", manifest.MetadataSha256);
                writer.WriteNumber("createdUtcTicks", manifest.CreatedUtcTicks);
                writer.WriteEndObject();
            }
            bytes = AddFinalNewline(memory.ToArray());
        }
        AtomicWrite(journalPath, bytes);
    }

    private static ImportManifest ReadJournal(string journalPath)
    {
        EnsureOrdinaryFile(journalPath, "Import journal");
        var info = new FileInfo(journalPath);
        if (info.Length > MaxJournalBytes)
            throw new InvalidDataException("Import journal exceeds 64 KiB.");
        using JsonDocument document = JsonDocument.Parse(
            File.ReadAllBytes(journalPath),
            new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 16,
            });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("Import journal must be an object.");
        var manifest = new ImportManifest(
            ReadInt(root, "schemaVersion"),
            ReadString(root, "transactionId"),
            ParsePhase(ReadString(root, "phase")),
            ReadString(root, "destinationRelativePath"),
            ReadString(root, "sourcePath"),
            ReadLong(root, "payloadLength"),
            ReadString(root, "payloadSha256"),
            ReadString(root, "metadataSha256"),
            ReadLong(root, "createdUtcTicks"));
        ValidateManifest(manifest);
        return manifest;
    }

    private static void ValidateManifest(ImportManifest manifest)
    {
        if (manifest.SchemaVersion != JournalSchemaVersion)
            throw new InvalidDataException("Unsupported import journal schema.");
        if (!Guid.TryParseExact(manifest.TransactionId, "N", out Guid id) ||
            id == Guid.Empty)
        {
            throw new InvalidDataException("Invalid import transaction id.");
        }
        if (manifest.DestinationRelativePath.Length is 0 or > 4096 ||
            Path.IsPathRooted(manifest.DestinationRelativePath))
        {
            throw new InvalidDataException("Invalid import destination path.");
        }
        if (manifest.SourcePath.Length is 0 or > 4096 ||
            manifest.SourcePath.IndexOf('\0') >= 0)
        {
            throw new InvalidDataException("Invalid import source path.");
        }
        if (manifest.PayloadLength < 0 ||
            !IsSha256(manifest.PayloadSha256) ||
            !IsSha256(manifest.MetadataSha256) ||
            manifest.CreatedUtcTicks <= 0)
        {
            throw new InvalidDataException("Invalid import journal fingerprint.");
        }
    }

    private static string ResolveRelativeAssetPath(string assetsRoot, string relative)
    {
        string root = NormalizeDirectory(assetsRoot);
        string normalized = relative.Replace('/', Path.DirectorySeparatorChar);
        string destination = Path.GetFullPath(Path.Combine(root, normalized));
        _ = NormalizeRelativeAssetPath(root, destination);
        ValidateImportFileName(Path.GetFileName(destination));
        return destination;
    }

    private static string NormalizeRelativeAssetPath(string assetsRoot, string path)
    {
        string root = NormalizeDirectory(assetsRoot);
        string full = Path.GetFullPath(path);
        if (!IsUnder(full, root))
            throw new InvalidDataException("Import asset path escapes Assets.");
        string relative = Path.GetRelativePath(root, full).Replace('\\', '/');
        if (relative.Equals(
                AssetDatabase.InternalDirectoryName,
                StringComparison.OrdinalIgnoreCase) ||
            relative.StartsWith(
                AssetDatabase.InternalDirectoryName + "/",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Import asset path uses the reserved database directory.");
        }
        return relative;
    }

    private static void ValidateImportFileName(string fileName)
    {
        if (string.IsNullOrWhiteSpace(fileName) ||
            fileName is "." or ".." ||
            fileName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            fileName.EndsWith(
                AssetDatabase.MetadataSuffix,
                StringComparison.OrdinalIgnoreCase) ||
            fileName.EndsWith(
                ".acsmat.graph.json",
                StringComparison.OrdinalIgnoreCase) ||
            AssetCreationWorkflow.IsTemporaryPath(fileName))
        {
            throw new InvalidDataException(
                $"File name is reserved or invalid for import: '{fileName}'.");
        }
    }

    private static void EnsureNoReparseDirectories(string directory)
    {
        string full = NormalizeDirectory(directory);
        string root = Path.GetPathRoot(full)
            ?? throw new InvalidDataException("Directory has no filesystem root.");
        string cursor = root;
        EnsureOrdinaryDirectory(cursor, "Filesystem root");
        string relative = Path.GetRelativePath(root, full);
        if (relative == ".")
            return;
        foreach (string segment in SplitPath(relative))
        {
            cursor = Path.Combine(cursor, segment);
            EnsureOrdinaryDirectory(cursor, "Import source parent");
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

    private static void EnsureMatchingOrdinaryFile(
        string path,
        long? expectedLength,
        string expectedHash,
        string label,
        CancellationToken cancellationToken = default)
    {
        EnsureOrdinaryFile(path, label);
        if (!FileMatches(path, expectedLength, expectedHash, cancellationToken))
            throw new InvalidDataException($"{label} does not match its import journal.");
    }

    private static bool FileMatches(
        string path,
        long? expectedLength,
        string expectedHash,
        CancellationToken cancellationToken = default)
    {
        EnsureOrdinaryFile(path, "Import transaction file");
        var info = new FileInfo(path);
        return (!expectedLength.HasValue || info.Length == expectedLength.Value) &&
               string.Equals(
                   ComputeSha256(path, cancellationToken),
                   expectedHash,
                   StringComparison.OrdinalIgnoreCase);
    }

    private static bool ContainsOnlyPrivatePreparationFiles(string directory)
    {
        foreach (FileSystemInfo entry in new DirectoryInfo(directory)
            .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
        {
            entry.Refresh();
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                (entry.Attributes & FileAttributes.Directory) != 0)
            {
                return false;
            }
            if (entry.Name != PayloadFileName &&
                entry.Name != MetadataFileName &&
                !entry.Name.StartsWith(
                    JournalFileName + ".tmp-",
                    StringComparison.Ordinal))
            {
                return false;
            }
        }
        return true;
    }

    private static bool TryCleanupTransaction(
        string stagingDirectory,
        out string? warning)
    {
        warning = null;
        try
        {
            if (!Directory.Exists(stagingDirectory))
                return true;
            EnsureOrdinaryDirectory(
                stagingDirectory,
                "Import transaction directory");
            foreach (FileSystemInfo entry in new DirectoryInfo(stagingDirectory)
                .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                entry.Refresh();
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                    (entry.Attributes & FileAttributes.Directory) != 0 ||
                    (entry.Name != PayloadFileName &&
                     entry.Name != MetadataFileName &&
                     entry.Name != JournalFileName &&
                     !entry.Name.StartsWith(
                         JournalFileName + ".tmp-",
                         StringComparison.Ordinal)))
                {
                    warning =
                        $"Import cleanup preserved transaction '{Path.GetFileName(stagingDirectory)}' " +
                        $"because it contains unexpected entry '{entry.Name}'.";
                    return false;
                }
                File.Delete(entry.FullName);
            }
            Directory.Delete(stagingDirectory, recursive: false);
            return true;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            warning =
                $"Import cleanup left transaction '{Path.GetFileName(stagingDirectory)}': " +
                error.Message;
            return false;
        }
    }

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

    private static void AtomicWrite(string destination, byte[] bytes)
    {
        string temporary =
            destination + ".tmp-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        try
        {
            WriteNewDurableFile(temporary, bytes);
            File.Move(temporary, destination, overwrite: true);
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

    private static string ComputeSha256(
        string path,
        CancellationToken cancellationToken = default)
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

    private static void InvokeCheckpoint(
        AssetImportTestHooks? hooks,
        AssetImportCheckpoint checkpoint)
    {
        hooks?.Observe?.Invoke(checkpoint);
        if (hooks?.SimulateCrashAfter == checkpoint)
            throw new AssetImportSimulatedCrashException(checkpoint);
        if (hooks?.FailAfter == checkpoint)
            throw new IOException($"Injected import failure after '{checkpoint}'.");
    }

    private static string ImporterForKind(string kind) => kind switch
    {
        "image" => "texture",
        "audio" => "audio",
        "mesh" => "mesh",
        "scene" => "scene",
        "material" => "material",
        "blueprint" => "blueprint",
        "prefab" => "prefab",
        _ => "passthrough",
    };

    private static string SafeFileName(string? path)
    {
        try
        {
            string name = Path.GetFileName(path) ?? "";
            return name.Length == 0 ? "(unnamed source)" : name;
        }
        catch
        {
            return "(invalid source)";
        }
    }

    private static string PhaseText(AssetImportCheckpoint phase) => phase switch
    {
        AssetImportCheckpoint.Prepared => "prepared",
        AssetImportCheckpoint.AssetPublished => "assetPublished",
        AssetImportCheckpoint.MetadataPublished => "metadataPublished",
        _ => throw new InvalidDataException("Unknown import phase."),
    };

    private static AssetImportCheckpoint ParsePhase(string value) => value switch
    {
        "prepared" => AssetImportCheckpoint.Prepared,
        "assetPublished" => AssetImportCheckpoint.AssetPublished,
        "metadataPublished" => AssetImportCheckpoint.MetadataPublished,
        _ => throw new InvalidDataException("Unknown import journal phase."),
    };

    private static string ReadString(JsonElement parent, string name)
    {
        if (!parent.TryGetProperty(name, out JsonElement value) ||
            value.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException($"Import journal '{name}' must be a string.");
        }
        return value.GetString() ?? "";
    }

    private static int ReadInt(JsonElement parent, string name)
    {
        if (!parent.TryGetProperty(name, out JsonElement value) ||
            value.ValueKind != JsonValueKind.Number ||
            !value.TryGetInt32(out int result))
        {
            throw new InvalidDataException(
                $"Import journal '{name}' must be a 32-bit integer.");
        }
        return result;
    }

    private static long ReadLong(JsonElement parent, string name)
    {
        if (!parent.TryGetProperty(name, out JsonElement value) ||
            value.ValueKind != JsonValueKind.Number ||
            !value.TryGetInt64(out long result))
        {
            throw new InvalidDataException(
                $"Import journal '{name}' must be a 64-bit integer.");
        }
        return result;
    }

    private static byte[] AddFinalNewline(byte[] bytes)
    {
        if (bytes.Length != 0 && bytes[^1] == (byte)'\n')
            return bytes;
        var result = new byte[bytes.Length + 1];
        bytes.CopyTo(result, 0);
        result[^1] = (byte)'\n';
        return result;
    }

    private static bool IsSha256(string value) =>
        value.Length == 64 && value.All(static character =>
            character is >= '0' and <= '9' or
                >= 'a' and <= 'f' or
                >= 'A' and <= 'F');

    private static string[] SplitPath(string path) =>
        path.Split(
            new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
            StringSplitOptions.RemoveEmptyEntries);

    private static string NormalizeDirectory(string path)
    {
        string full = Path.GetFullPath(path);
        string root = Path.GetPathRoot(full) ?? "";
        return full.Length > root.Length
            ? Path.TrimEndingDirectorySeparator(full)
            : full;
    }

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

    private static bool IsUnderOrEqual(string candidate, string root) =>
        string.Equals(
            NormalizeDirectory(candidate),
            NormalizeDirectory(root),
            PathComparison) ||
        IsUnder(candidate, root);

    private sealed record SourceSnapshot(
        long Length,
        long LastWriteUtcTicks,
        string ContentHash);

    private sealed record ImportManifest(
        int SchemaVersion,
        string TransactionId,
        AssetImportCheckpoint Phase,
        string DestinationRelativePath,
        string SourcePath,
        long PayloadLength,
        string PayloadSha256,
        string MetadataSha256,
        long CreatedUtcTicks);

    private sealed record ImportTransaction(
        string AssetsRoot,
        string StagingDirectory,
        string JournalPath,
        string StagedPayloadPath,
        string StagedMetadataPath,
        string DestinationPath,
        string DestinationMetadataPath,
        ImportManifest Manifest);

    private sealed class AssetImportRollbackIncompleteException : IOException
    {
        internal AssetImportRollbackIncompleteException(
            string message,
            Exception innerException)
            : base(message, innerException)
        {
        }
    }

    private enum RecoveryDisposition
    {
        Completed,
        Discarded,
        Preserved,
    }
}
