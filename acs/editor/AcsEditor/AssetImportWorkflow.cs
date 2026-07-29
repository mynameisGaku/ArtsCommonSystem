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
    IReadOnlyList<string> ImportedDirectories,
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
    RecursiveBatchCommitted,
    RecursiveDirectoryPrepared,
}

/// <summary>中間公開状態を作らないインポートのセルフテストだけで使用する障害注入です。</summary>
internal sealed record AssetImportTestHooks(
    AssetImportCheckpoint? FailAfter = null,
    AssetImportCheckpoint? SimulateCrashAfter = null,
    Action<AssetImportCheckpoint>? Observe = null);

internal sealed record AssetImportTraversalLimits(
    int MaxDepth,
    int MaxEntries,
    long MaxTotalBytes);

internal sealed class AssetImportSimulatedCrashException : IOException
{
    internal AssetImportSimulatedCrashException(AssetImportCheckpoint checkpoint)
        : base($"Simulated process termination after import checkpoint '{checkpoint}'.")
    {
    }
}

/// <summary>
/// Signals that an import deliberately preserved ambiguous filesystem state or
/// a recovery journal. Callers must not run an ordinary index refresh, because
/// doing so could adopt destination-race bytes as a new managed asset.
/// </summary>
internal class AssetImportRecoveryRequiredException : IOException
{
    internal AssetImportRecoveryRequiredException(
        string message,
        Exception? innerException = null)
        : base(message, innerException)
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
    private const int MaxExternalSourceRoots = 4096;
    private const int DefaultMaxRecursiveDepth = 64;
    private const int DefaultMaxRecursiveEntries = 16 * 1024;
    private const long DefaultMaxRecursiveBytes = 64L * 1024 * 1024 * 1024;
    private const int RecursiveBatchJournalSchemaVersion = 1;
    private const int MaxRecursiveBatchJournalBytes = 32 * 1024 * 1024;
    internal const string StagingDirectoryName =
        AssetDatabase.ImportStagingDirectoryName;
    private const string JournalFileName = "manifest.v1.json";
    private const string RecursiveBatchDirectoryPrefix = "batch-";
    private const string RecursiveBatchJournalFileName =
        "batch-manifest.v1.json";
    private const string RecursiveBatchOwnerMarkerPrefix =
        ".acs-import-owner.tmp-";
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
        AssetImportTestHooks? testHooks = null,
        AssetImporterSettings? importerSettings = null)
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
                        testHooks,
                        importerSettings: importerSettings);
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
            Array.Empty<string>(),
            Array.AsReadOnly(failures.ToArray()),
            Array.AsReadOnly(warnings.ToArray()),
            indexResult);
    }

    /// <summary>
    /// Imports an Explorer/file-manager selection. File-only selections retain
    /// the established ImportFiles behavior. A selection containing a directory
    /// is treated as one strict recursive batch: the source tree is bounded and
    /// snapshotted before mutation, its hierarchy is preserved below a
    /// collision-free root, and every published file/directory is rolled back if
    /// any source, destination, or index invariant changes.
    /// </summary>
    internal static AssetImportOperationResult ImportExternalPaths(
        AssetDatabase database,
        string destinationDirectory,
        IEnumerable<string> sourcePaths,
        CancellationToken cancellationToken = default,
        AssetImportTestHooks? testHooks = null,
        AssetImportTraversalLimits? traversalLimits = null,
        AssetImporterSettings? importerSettings = null)
    {
        ArgumentNullException.ThrowIfNull(database);
        ArgumentNullException.ThrowIfNull(sourcePaths);
        string[] sources = sourcePaths.ToArray();
        ValidateExternalSourceRoots(database.AssetsRoot, sources);

        // Preserve the existing file-drop contract, including its per-source
        // failure reporting and collision suffix behavior. Directory candidates
        // (including directory reparse points) enter the strict path below.
        if (!ContainsDirectoryCandidate(sources))
        {
            return ImportFiles(
                database,
                destinationDirectory,
                sources,
                cancellationToken,
                testHooks,
                importerSettings);
        }

        AssetImportTraversalLimits limits = traversalLimits ??
            new AssetImportTraversalLimits(
                DefaultMaxRecursiveDepth,
                DefaultMaxRecursiveEntries,
                DefaultMaxRecursiveBytes);
        ValidateTraversalLimits(limits);

        using AssetMutationLock mutationLock = AssetMutationLock.AcquireForRecovery(
            database.AssetsRoot,
            "Import folder tree");
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
        _ = database.RefreshWithinAssetTransaction(
            verifyContent: false,
            cancellationToken);

        RecursiveImportPlan plan = BuildRecursiveImportPlan(
            database.AssetsRoot,
            sources,
            limits,
            cancellationToken);
        RecursiveBatchManifest batchManifest =
            BuildRecursiveBatchManifest(
                database.AssetsRoot,
                targetDirectory,
                plan);
        string batchDirectory = CreateRecursiveBatchJournal(
            database.AssetsRoot,
            batchManifest);
        var warnings = new List<string>(recovery.Warnings);
        warnings.AddRange(reimportRecovery.Warnings);
        var published = new List<ImportTransaction>();
        var imported = new List<ImportedAssetFile>();
        var importedRoots = new List<string>();

        AssetDatabaseRefreshResult indexResult;
        try
        {
            // Directories are published first with an exclusive rename from
            // private staging. Directory.Move fails if a destination race wins.
            foreach (RecursiveBatchDirectory directoryPlan in
                     batchManifest.Directories)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string destination = ResolveRelativeDirectoryPath(
                    database.AssetsRoot,
                    directoryPlan.DestinationRelativePath);
                CreateImportedDirectory(
                    database.AssetsRoot,
                    destination,
                    Path.Combine(
                        batchDirectory,
                        directoryPlan.PrivateStagingName),
                    batchManifest.BatchId,
                    directoryPlan,
                    cancellationToken,
                    testHooks);
                if (directoryPlan.IsSelectionRoot)
                    importedRoots.Add(destination);
            }

            foreach (RecursiveBatchFile file in batchManifest.Files)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string destination = ResolveRelativeAssetPath(
                    database.AssetsRoot,
                    file.DestinationRelativePath);
                string destinationParent = Path.GetDirectoryName(destination)
                    ?? throw new InvalidDataException(
                        "Recursive import destination has no parent.");
                ValidateTargetDirectory(
                    database.AssetsRoot,
                    destinationParent);
                ImportTransaction transaction = PrepareAndPublish(
                    database,
                    destinationParent,
                    file.SourcePath,
                    cancellationToken,
                    testHooks,
                    expectedSource: new PlannedImportFile(
                        file.SourcePath,
                        file.SourceRelativePath,
                        file.PayloadLength,
                        file.SourceLastWriteUtcTicks),
                    destinationOverride: destination,
                    transactionIdOverride: file.TransactionId,
                    stagingParentOverride: batchDirectory,
                    importerSettings: importerSettings);
                published.Add(transaction);
                imported.Add(new ImportedAssetFile(
                    transaction.Manifest.SourcePath,
                    transaction.DestinationPath));
            }

            // Detect additions, removals, type changes, and file size/time
            // changes that occurred while the recursive batch was copied.
            RecursiveImportPlan finalPlan = BuildRecursiveImportPlan(
                database.AssetsRoot,
                sources,
                limits,
                cancellationToken);
            if (!string.Equals(
                    plan.SourceFingerprint,
                    finalPlan.SourceFingerprint,
                    StringComparison.Ordinal))
            {
                throw new IOException(
                    "The dropped source tree changed while it was being imported.");
            }

            indexResult = database.RefreshWithinAssetTransaction(
                verifyContent: false,
                cancellationToken);

            var publishedById = published.ToDictionary(
                transaction => transaction.Manifest.TransactionId,
                StringComparer.Ordinal);
            RecursiveBatchFile[] committedFiles = batchManifest.Files
                .Select(file =>
                {
                    if (!publishedById.TryGetValue(
                            file.TransactionId,
                            out ImportTransaction? transaction))
                    {
                        throw new InvalidDataException(
                            "Recursive import did not publish every planned child transaction.");
                    }
                    return file with
                    {
                        PayloadSha256 = transaction.Manifest.PayloadSha256,
                        MetadataSha256 = transaction.Manifest.MetadataSha256,
                    };
                })
                .ToArray();
            batchManifest = batchManifest with
            {
                Phase = RecursiveBatchPhase.Committed,
                Files = Array.AsReadOnly(committedFiles),
            };
            WriteRecursiveBatchJournal(
                Path.Combine(
                    batchDirectory,
                    RecursiveBatchJournalFileName),
                batchManifest);
            InvokeCheckpoint(
                testHooks,
                AssetImportCheckpoint.RecursiveBatchCommitted);
        }
        catch (AssetImportSimulatedCrashException)
        {
            // This exception models process termination. Preserve journals and
            // already-published evidence exactly as a real crash would.
            throw;
        }
        catch (Exception error)
        {
            // PrepareAndPublish reports this type only when its current
            // transaction retained recovery evidence. It is not yet present in
            // 'published', so carry that incompleteness into the batch result
            // and never refresh/index ambiguous destination bytes.
            bool rollbackComplete =
                error is not AssetImportRecoveryRequiredException;
            rollbackComplete &= RollbackPublishedBatch(
                database,
                published,
                warnings,
                refreshIndex: false);
            foreach (RecursiveBatchDirectory directory in
                     batchManifest.Directories.Reverse())
            {
                rollbackComplete &= TryRollbackRecursiveBatchDirectory(
                    database.AssetsRoot,
                    batchDirectory,
                    batchManifest.BatchId,
                    directory,
                    warnings);
            }
            if (rollbackComplete)
            {
                rollbackComplete = TryRefreshAfterImportRollback(
                    database,
                    warnings);
            }
            if (rollbackComplete &&
                !TryCleanupRecursiveBatchDirectory(
                    batchDirectory,
                    out string? batchCleanupWarning))
            {
                rollbackComplete = false;
                warnings.Add(batchCleanupWarning!);
            }
            if (!rollbackComplete)
            {
                throw new AssetImportRollbackIncompleteException(
                    "Recursive import failed and rollback was incomplete. " +
                    "Changed destinations and recovery evidence were preserved.",
                    error);
            }
            throw;
        }

        bool cleanupComplete = true;
        foreach (ImportTransaction transaction in published)
        {
            if (!TryCleanupTransaction(transaction.StagingDirectory, out string? warning))
            {
                warnings.Add(warning!);
                cleanupComplete = false;
            }
        }
        foreach (RecursiveBatchDirectory directory in
                 batchManifest.Directories.Reverse())
        {
            string destination = ResolveRelativeDirectoryPath(
                database.AssetsRoot,
                directory.DestinationRelativePath);
            if (!TryRemoveRecursiveDirectoryOwnerMarker(
                    destination,
                    batchManifest.BatchId,
                    directory))
            {
                warnings.Add(
                    $"Recursive import ownership marker could not be safely " +
                    $"removed from '{directory.DestinationRelativePath}'.");
                cleanupComplete = false;
            }
        }
        if (cleanupComplete &&
            !TryCleanupRecursiveBatchDirectory(
                batchDirectory,
                out string? batchWarning))
        {
            warnings.Add(batchWarning!);
        }

        return new AssetImportOperationResult(
            Array.AsReadOnly(imported.ToArray()),
            Array.AsReadOnly(importedRoots.ToArray()),
            Array.Empty<string>(),
            Array.AsReadOnly(warnings.ToArray()),
            indexResult);
    }

    private static void ValidateExternalSourceRoots(
        string assetsRoot,
        IReadOnlyList<string> sourcePaths)
    {
        if (sourcePaths.Count > MaxExternalSourceRoots)
        {
            throw new InvalidDataException(
                $"External import accepts at most {MaxExternalSourceRoots} source roots.");
        }
        string normalizedAssetsRoot = NormalizeDirectory(assetsRoot);
        foreach (string? sourcePath in sourcePaths)
        {
            if (string.IsNullOrWhiteSpace(sourcePath) ||
                !Path.IsPathFullyQualified(sourcePath))
            {
                throw new InvalidDataException(
                    "External import sources must be absolute paths.");
            }
            string source = Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(sourcePath));
            if (source.Length == 0 ||
                IsUnderOrEqual(source, normalizedAssetsRoot))
            {
                throw new InvalidDataException(
                    "Files already managed below Assets must use Asset View move/copy.");
            }
        }
    }

    private static bool ContainsDirectoryCandidate(IEnumerable<string> sourcePaths)
    {
        foreach (string? sourcePath in sourcePaths)
        {
            try
            {
                if (!string.IsNullOrWhiteSpace(sourcePath) &&
                    (File.GetAttributes(sourcePath) & FileAttributes.Directory) != 0)
                {
                    return true;
                }
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    ArgumentException or NotSupportedException)
            {
                // ImportFiles remains authoritative for a file-only/missing
                // selection and reports its established per-source failure.
            }
        }
        return false;
    }

    private static void ValidateTraversalLimits(AssetImportTraversalLimits limits)
    {
        if (limits.MaxDepth < 0 ||
            limits.MaxEntries <= 0 ||
            limits.MaxTotalBytes < 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(limits),
                "Recursive import limits must be non-negative and include at least one entry.");
        }
    }

    private static RecursiveImportPlan BuildRecursiveImportPlan(
        string assetsRoot,
        IEnumerable<string> sourcePaths,
        AssetImportTraversalLimits limits,
        CancellationToken cancellationToken)
    {
        string normalizedAssetsRoot = NormalizeDirectory(assetsRoot);
        var candidates = new Dictionary<string, ExternalSourceRoot>(
            StringComparer.OrdinalIgnoreCase);
        int suppliedRoots = 0;
        foreach (string? sourcePath in sourcePaths)
        {
            cancellationToken.ThrowIfCancellationRequested();
            suppliedRoots++;
            if (suppliedRoots > MaxExternalSourceRoots)
            {
                throw new InvalidDataException(
                    $"Recursive import accepts at most {MaxExternalSourceRoots} source roots.");
            }
            if (string.IsNullOrWhiteSpace(sourcePath) ||
                !Path.IsPathFullyQualified(sourcePath))
            {
                throw new InvalidDataException(
                    "Recursive import sources must be absolute paths.");
            }

            string source = Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(sourcePath));
            if (source.Length == 0 ||
                IsUnderOrEqual(source, normalizedAssetsRoot))
            {
                throw new InvalidDataException(
                    "Files already managed below Assets cannot be imported recursively.");
            }
            if (candidates.ContainsKey(source))
                continue;

            FileAttributes attributes = File.GetAttributes(source);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException(
                    $"Recursive import source must not be a reparse point: " +
                    $"'{Path.GetFileName(source)}'.");
            }
            bool isDirectory = (attributes & FileAttributes.Directory) != 0;
            if (isDirectory)
            {
                EnsureNoReparseDirectories(source);
                ValidateImportDirectoryName(Path.GetFileName(source));
            }
            else
            {
                _ = ValidateSourceFile(
                    normalizedAssetsRoot,
                    source,
                    validateAgainstAssetsRoot: false);
            }
            candidates.Add(source, new ExternalSourceRoot(source, isDirectory));
        }

        if (candidates.Count == 0)
            throw new InvalidDataException("The recursive import selection is empty.");

        ExternalSourceRoot[] orderedRoots = candidates.Values
            .OrderBy(static item => item.SourcePath, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        foreach (ExternalSourceRoot directory in orderedRoots.Where(
                     static item => item.IsDirectory))
        {
            foreach (ExternalSourceRoot other in orderedRoots)
            {
                if (!string.Equals(
                        directory.SourcePath,
                        other.SourcePath,
                        PathComparison) &&
                    IsUnder(other.SourcePath, directory.SourcePath))
                {
                    throw new InvalidDataException(
                        "Recursive import source roots must not overlap.");
                }
            }
        }

        var budget = new RecursiveTraversalBudget(limits);
        var directoryRoots = new List<RecursiveImportRoot>();
        var looseFiles = new List<PlannedImportFile>();
        foreach (ExternalSourceRoot sourceRoot in orderedRoots)
        {
            cancellationToken.ThrowIfCancellationRequested();
            budget.AddEntry();
            if (!sourceRoot.IsDirectory)
            {
                PlannedImportFile file = SnapshotPlannedFile(
                    sourceRoot.SourcePath,
                    Path.GetFileName(sourceRoot.SourcePath),
                    budget);
                looseFiles.Add(file);
                continue;
            }

            directoryRoots.Add(EnumerateRecursiveRoot(
                sourceRoot.SourcePath,
                budget,
                cancellationToken));
        }

        RecursiveImportRoot[] frozenRoots = directoryRoots
            .OrderBy(static item => item.SourcePath, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        PlannedImportFile[] frozenLooseFiles = looseFiles
            .OrderBy(static item => item.SourcePath, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        return new RecursiveImportPlan(
            Array.AsReadOnly(frozenRoots),
            Array.AsReadOnly(frozenLooseFiles),
            ComputeRecursiveSourceFingerprint(frozenRoots, frozenLooseFiles));
    }

    private static RecursiveImportRoot EnumerateRecursiveRoot(
        string sourceRoot,
        RecursiveTraversalBudget budget,
        CancellationToken cancellationToken)
    {
        var relativeDirectories = new List<string>();
        var files = new List<PlannedImportFile>();
        var pending = new Stack<PendingSourceDirectory>();
        pending.Push(new PendingSourceDirectory(sourceRoot, "", 0));

        while (pending.Count != 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            PendingSourceDirectory current = pending.Pop();
            if (current.Depth > budget.Limits.MaxDepth)
            {
                throw new InvalidDataException(
                    $"Recursive import exceeds the maximum depth of " +
                    $"{budget.Limits.MaxDepth}.");
            }
            EnsureOrdinaryDirectory(current.FullPath, "Recursive import directory");

            var children = new List<FileSystemInfo>();
            foreach (FileSystemInfo child in new DirectoryInfo(current.FullPath)
                         .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                cancellationToken.ThrowIfCancellationRequested();
                budget.AddEntry();
                children.Add(child);
            }
            children.Sort(static (left, right) =>
                StringComparer.OrdinalIgnoreCase.Compare(left.Name, right.Name));

            var childDirectories = new List<PendingSourceDirectory>();
            foreach (FileSystemInfo child in children)
            {
                cancellationToken.ThrowIfCancellationRequested();
                child.Refresh();
                FileAttributes attributes = child.Attributes;
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException(
                        $"Recursive import does not follow reparse point " +
                        $"'{child.Name}'.");
                }

                string relative = current.RelativePath.Length == 0
                    ? child.Name
                    : Path.Combine(current.RelativePath, child.Name);
                int depth = current.Depth + 1;
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    ValidateImportDirectoryName(child.Name);
                    if (depth > budget.Limits.MaxDepth)
                    {
                        throw new InvalidDataException(
                            $"Recursive import exceeds the maximum depth of " +
                            $"{budget.Limits.MaxDepth}.");
                    }
                    relativeDirectories.Add(relative);
                    childDirectories.Add(new PendingSourceDirectory(
                        child.FullName,
                        relative,
                        depth));
                }
                else
                {
                    if (depth > budget.Limits.MaxDepth)
                    {
                        throw new InvalidDataException(
                            $"Recursive import exceeds the maximum depth of " +
                            $"{budget.Limits.MaxDepth}.");
                    }
                    ValidateImportFileName(child.Name);
                    files.Add(SnapshotPlannedFile(
                        child.FullName,
                        relative,
                        budget));
                }
            }

            // Push in reverse so the ordinal-first directory is visited first.
            for (int index = childDirectories.Count - 1; index >= 0; index--)
                pending.Push(childDirectories[index]);
        }

        string[] frozenDirectories = relativeDirectories
            .OrderBy(static path => SplitPath(path).Length)
            .ThenBy(static path => path, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        PlannedImportFile[] frozenFiles = files
            .OrderBy(static item => item.RelativePath, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        return new RecursiveImportRoot(
            sourceRoot,
            Array.AsReadOnly(frozenDirectories),
            Array.AsReadOnly(frozenFiles));
    }

    private static PlannedImportFile SnapshotPlannedFile(
        string sourcePath,
        string relativePath,
        RecursiveTraversalBudget budget)
    {
        EnsureOrdinaryFile(sourcePath, "Recursive import file");
        var info = new FileInfo(sourcePath);
        info.Refresh();
        long length = info.Length;
        budget.AddBytes(length);
        return new PlannedImportFile(
            Path.GetFullPath(sourcePath),
            relativePath,
            length,
            info.LastWriteTimeUtc.Ticks);
    }

    private static string ComputeRecursiveSourceFingerprint(
        IReadOnlyList<RecursiveImportRoot> directoryRoots,
        IReadOnlyList<PlannedImportFile> looseFiles)
    {
        var text = new StringBuilder();
        foreach (PlannedImportFile file in looseFiles)
            AppendFingerprintEntry(text, "F", file.SourcePath, file.Length,
                file.LastWriteUtcTicks);
        foreach (RecursiveImportRoot root in directoryRoots)
        {
            AppendFingerprintEntry(text, "R", root.SourcePath, 0, 0);
            foreach (string relativeDirectory in root.RelativeDirectories)
                AppendFingerprintEntry(text, "D", relativeDirectory, 0, 0);
            foreach (PlannedImportFile file in root.Files)
                AppendFingerprintEntry(text, "F", file.RelativePath, file.Length,
                    file.LastWriteUtcTicks);
        }
        return Convert.ToHexString(
                SHA256.HashData(Utf8NoBom.GetBytes(text.ToString())))
            .ToLowerInvariant();
    }

    private static void AppendFingerprintEntry(
        StringBuilder text,
        string kind,
        string path,
        long length,
        long lastWriteUtcTicks)
    {
        text.Append(kind)
            .Append(':')
            .Append(path.Length.ToString(CultureInfo.InvariantCulture))
            .Append(':')
            .Append(path)
            .Append(':')
            .Append(length.ToString(CultureInfo.InvariantCulture))
            .Append(':')
            .Append(lastWriteUtcTicks.ToString(CultureInfo.InvariantCulture))
            .Append('\n');
    }

    private static RecursiveBatchManifest BuildRecursiveBatchManifest(
        string assetsRoot,
        string targetDirectory,
        RecursiveImportPlan sourcePlan)
    {
        string batchId =
            Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        var reservedPaths = new HashSet<string>(
            StringComparer.OrdinalIgnoreCase);
        var directories = new List<RecursiveBatchDirectory>();
        var directoryDestinations =
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        foreach (RecursiveImportRoot root in sourcePlan.DirectoryRoots)
        {
            string rootDestination = ReservePlannedDirectoryDestination(
                targetDirectory,
                Path.GetFileName(root.SourcePath),
                reservedPaths);
            AddPlannedDirectory(
                assetsRoot,
                rootDestination,
                isSelectionRoot: true,
                reservedPaths,
                directories);
            directoryDestinations.Add(root.SourcePath, rootDestination);
            foreach (string relativeDirectory in root.RelativeDirectories)
            {
                AddPlannedDirectory(
                    assetsRoot,
                    ResolveImportedRelativePath(
                        rootDestination,
                        relativeDirectory),
                    isSelectionRoot: false,
                    reservedPaths,
                    directories);
            }
        }

        var files = new List<RecursiveBatchFile>();
        foreach (PlannedImportFile file in sourcePlan.LooseFiles)
        {
            string destination = ReservePlannedFileDestination(
                targetDirectory,
                file.SourcePath,
                reservedPaths);
            AddPlannedFile(
                assetsRoot,
                file,
                destination,
                reservedPaths,
                files);
        }
        foreach (RecursiveImportRoot root in sourcePlan.DirectoryRoots)
        {
            string destinationRoot = directoryDestinations[root.SourcePath];
            foreach (PlannedImportFile file in root.Files)
            {
                AddPlannedFile(
                    assetsRoot,
                    file,
                    ResolveImportedRelativePath(
                        destinationRoot,
                        file.RelativePath),
                    reservedPaths,
                    files);
            }
        }

        var manifest = new RecursiveBatchManifest(
            RecursiveBatchJournalSchemaVersion,
            batchId,
            RecursiveBatchPhase.Prepared,
            sourcePlan.SourceFingerprint,
            DateTime.UtcNow.Ticks,
            Array.AsReadOnly(directories.ToArray()),
            Array.AsReadOnly(files.ToArray()));
        ValidateRecursiveBatchManifest(manifest);
        return manifest;
    }

    private static string ReservePlannedDirectoryDestination(
        string targetDirectory,
        string sourceName,
        HashSet<string> reservedPaths)
    {
        ValidateImportDirectoryName(sourceName);
        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            string candidateName = suffix == 0
                ? sourceName
                : $"{sourceName} ({suffix})";
            string candidate = Path.Combine(targetDirectory, candidateName);
            if (IsPlannedDestinationAvailable(candidate, reservedPaths))
                return candidate;
        }
        throw new IOException(
            $"No collision-free import folder name is available for '{sourceName}'.");
    }

    private static string ReservePlannedFileDestination(
        string targetDirectory,
        string source,
        HashSet<string> reservedPaths)
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
            if (IsPlannedDestinationAvailable(candidate, reservedPaths))
                return candidate;
        }
        throw new IOException(
            $"No collision-free import name is available for '{fileName}'.");
    }

    private static bool IsPlannedDestinationAvailable(
        string candidate,
        HashSet<string> reservedPaths)
    {
        string full = Path.GetFullPath(candidate);
        string metadata = full + AssetDatabase.MetadataSuffix;
        return !File.Exists(full) &&
               !Directory.Exists(full) &&
               !File.Exists(metadata) &&
               !Directory.Exists(metadata) &&
               !reservedPaths.Contains(full) &&
               !reservedPaths.Contains(metadata);
    }

    private static void AddPlannedDirectory(
        string assetsRoot,
        string destination,
        bool isSelectionRoot,
        HashSet<string> reservedPaths,
        List<RecursiveBatchDirectory> directories)
    {
        string full = Path.GetFullPath(destination);
        ValidateImportDirectoryName(Path.GetFileName(full));
        if (!IsPlannedDestinationAvailable(full, reservedPaths) ||
            !reservedPaths.Add(full))
        {
            throw new InvalidDataException(
                $"Recursive import plan contains a destination collision: " +
                $"'{Path.GetFileName(full)}'.");
        }
        string metadata = full + AssetDatabase.MetadataSuffix;
        if (!reservedPaths.Add(metadata))
        {
            throw new InvalidDataException(
                "Recursive import plan contains a metadata path collision.");
        }
        directories.Add(new RecursiveBatchDirectory(
            NormalizeRelativeDirectoryPath(assetsRoot, full),
            "directory-" +
            Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture),
            Convert.ToHexString(
                    RandomNumberGenerator.GetBytes(32))
                .ToLowerInvariant(),
            isSelectionRoot));
    }

    private static void AddPlannedFile(
        string assetsRoot,
        PlannedImportFile source,
        string destination,
        HashSet<string> reservedPaths,
        List<RecursiveBatchFile> files)
    {
        string full = Path.GetFullPath(destination);
        ValidateImportFileName(Path.GetFileName(full));
        if (!IsPlannedDestinationAvailable(full, reservedPaths) ||
            !reservedPaths.Add(full) ||
            !reservedPaths.Add(full + AssetDatabase.MetadataSuffix))
        {
            throw new InvalidDataException(
                $"Recursive import plan contains a destination collision: " +
                $"'{Path.GetFileName(full)}'.");
        }
        files.Add(new RecursiveBatchFile(
            Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture),
            source.SourcePath,
            source.RelativePath,
            NormalizeRelativeAssetPath(assetsRoot, full),
            source.Length,
            source.LastWriteUtcTicks,
            "",
            ""));
    }

    private static string ReserveDirectoryDestination(
        string targetDirectory,
        string sourceName)
    {
        ValidateImportDirectoryName(sourceName);
        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            string candidateName = suffix == 0
                ? sourceName
                : $"{sourceName} ({suffix})";
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
            $"No collision-free import folder name is available for '{sourceName}'.");
    }

    private static string ResolveImportedRelativePath(
        string destinationRoot,
        string relativePath)
    {
        if (string.IsNullOrWhiteSpace(relativePath) ||
            Path.IsPathRooted(relativePath))
        {
            throw new InvalidDataException(
                "Recursive import contains an invalid relative path.");
        }
        string root = NormalizeDirectory(destinationRoot);
        string destination = Path.GetFullPath(Path.Combine(root, relativePath));
        if (!IsUnder(destination, root))
        {
            throw new InvalidDataException(
                "Recursive import destination escapes its published root.");
        }
        return destination;
    }

    private static void CreateImportedDirectory(
        string assetsRoot,
        string destination,
        string privateDirectory,
        string batchId,
        RecursiveBatchDirectory directoryPlan,
        CancellationToken cancellationToken,
        AssetImportTestHooks? testHooks)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string parent = Path.GetDirectoryName(destination)
            ?? throw new InvalidDataException(
                "Recursive import directory has no parent.");
        ValidateTargetDirectory(assetsRoot, parent);
        ValidateImportDirectoryName(Path.GetFileName(destination));
        if (File.Exists(destination) ||
            Directory.Exists(destination) ||
            File.Exists(destination + AssetDatabase.MetadataSuffix) ||
            Directory.Exists(destination + AssetDatabase.MetadataSuffix))
        {
            throw new AssetImportRecoveryRequiredException(
                $"Recursive import destination already exists: " +
                $"'{Path.GetFileName(destination)}'.");
        }

        string fullPrivateDirectory = Path.GetFullPath(privateDirectory);
        string privateParent = Path.GetDirectoryName(fullPrivateDirectory)
            ?? throw new InvalidDataException(
                "Recursive import private directory has no parent.");
        EnsureNoReparseDirectories(privateParent);
        EnsureOrdinaryDirectory(
            privateParent,
            "Recursive import batch directory");
        if (!TryParsePrivateDirectoryName(
                Path.GetFileName(fullPrivateDirectory),
                out _))
        {
            throw new InvalidDataException(
                "Recursive import private directory name is invalid.");
        }
        bool published = false;
        try
        {
            if (File.Exists(fullPrivateDirectory) ||
                Directory.Exists(fullPrivateDirectory))
            {
                throw new AssetImportRecoveryRequiredException(
                    "Recursive import private directory already exists.");
            }
            Directory.CreateDirectory(fullPrivateDirectory);
            EnsureOrdinaryDirectory(
                fullPrivateDirectory,
                "Recursive import staging directory");
            string ownerMarker = Path.Combine(
                fullPrivateDirectory,
                RecursiveDirectoryOwnerMarkerName(directoryPlan));
            byte[] ownerPayload = RecursiveDirectoryOwnerPayload(
                batchId,
                directoryPlan);
            WriteNewDurableFile(ownerMarker, ownerPayload);
            InvokeCheckpoint(
                testHooks,
                AssetImportCheckpoint.RecursiveDirectoryPrepared);
            cancellationToken.ThrowIfCancellationRequested();
            ValidateTargetDirectory(assetsRoot, parent);
            if (File.Exists(destination) ||
                Directory.Exists(destination) ||
                File.Exists(destination + AssetDatabase.MetadataSuffix) ||
                Directory.Exists(destination + AssetDatabase.MetadataSuffix))
            {
                throw new AssetImportRecoveryRequiredException(
                    $"Recursive import destination changed before publication: " +
                    $"'{Path.GetFileName(destination)}'.");
            }
            Directory.Move(fullPrivateDirectory, destination);
            published = true;
            ValidateTargetDirectory(assetsRoot, destination);
            EnsureRecursiveDirectoryOwnership(
                destination,
                batchId,
                directoryPlan);
        }
        catch (AssetImportSimulatedCrashException)
        {
            // Model abrupt process termination without running in-process
            // cleanup. Startup reconciliation must decide ownership from the
            // durable batch journal and marker.
            throw;
        }
        catch (Exception error)
        {
            bool destinationChanged =
                File.Exists(destination) ||
                Directory.Exists(destination) ||
                File.Exists(destination + AssetDatabase.MetadataSuffix) ||
                Directory.Exists(destination + AssetDatabase.MetadataSuffix);
            bool cleanupComplete = true;
            if (published && Directory.Exists(destination))
            {
                cleanupComplete &= TryDeleteOwnedRecursiveDirectory(
                    destination,
                    batchId,
                    directoryPlan);
            }
            if (Directory.Exists(fullPrivateDirectory))
            {
                cleanupComplete &= TryDeleteOwnedRecursiveDirectory(
                    fullPrivateDirectory,
                    batchId,
                    directoryPlan);
            }
            if (!cleanupComplete)
            {
                throw new AssetImportRollbackIncompleteException(
                    "Recursive import directory publication failed and its private " +
                    "directory could not be safely removed.",
                    error);
            }
            if (destinationChanged &&
                error is not AssetImportRecoveryRequiredException)
            {
                throw new AssetImportRecoveryRequiredException(
                    "Recursive import preserved a destination that changed " +
                    "during directory publication.",
                    error);
            }
            throw;
        }
    }

    private static bool RollbackImportedDirectories(
        IReadOnlyList<string> createdDirectories,
        List<string> warnings)
    {
        bool complete = true;
        foreach (string directory in createdDirectories.Reverse())
        {
            try
            {
                if (!Directory.Exists(directory))
                    continue;
                EnsureOrdinaryDirectory(
                    directory,
                    "Recursive import rollback directory");
                if (new DirectoryInfo(directory)
                    .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly)
                    .Any())
                {
                    complete = false;
                    warnings.Add(
                        $"Recursive import rollback preserved changed directory " +
                        $"'{Path.GetFileName(directory)}'.");
                    continue;
                }
                Directory.Delete(directory, recursive: false);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    InvalidDataException)
            {
                complete = false;
                warnings.Add(
                    $"Recursive import directory rollback failed: {error.Message}");
            }
        }
        return complete;
    }

    private static bool TryDeleteEmptyOrdinaryDirectory(string directory)
    {
        try
        {
            EnsureOrdinaryDirectory(directory, "Recursive import directory");
            if (new DirectoryInfo(directory)
                .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly)
                .Any())
            {
                return false;
            }
            Directory.Delete(directory, recursive: false);
            return true;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException)
        {
            return false;
        }
    }

    private static string RecursiveDirectoryOwnerMarkerName(
        RecursiveBatchDirectory directory)
    {
        if (!TryParsePrivateDirectoryName(
                directory.PrivateStagingName,
                out Guid privateId))
        {
            throw new InvalidDataException(
                "Recursive import directory has an invalid private staging name.");
        }
        return RecursiveBatchOwnerMarkerPrefix +
               privateId.ToString("N", CultureInfo.InvariantCulture);
    }

    private static byte[] RecursiveDirectoryOwnerPayload(
        string batchId,
        RecursiveBatchDirectory directory)
    {
        if (!Guid.TryParseExact(batchId, "N", out Guid parsedBatchId) ||
            parsedBatchId == Guid.Empty ||
            !IsSha256(directory.OwnerToken))
        {
            throw new InvalidDataException(
                "Recursive import directory ownership metadata is invalid.");
        }
        string payload =
            "ACS recursive import directory owner v1\n" +
            "batch=" + batchId + "\n" +
            "destination=" + directory.DestinationRelativePath + "\n" +
            "staging=" + directory.PrivateStagingName + "\n" +
            "token=" + directory.OwnerToken + "\n";
        return Utf8NoBom.GetBytes(payload);
    }

    private static void EnsureRecursiveDirectoryOwnership(
        string directory,
        string batchId,
        RecursiveBatchDirectory directoryPlan)
    {
        EnsureOrdinaryDirectory(
            directory,
            "Recursive import owned directory");
        string markerPath = Path.Combine(
            directory,
            RecursiveDirectoryOwnerMarkerName(directoryPlan));
        if (Directory.Exists(markerPath))
        {
            throw new InvalidDataException(
                "Recursive import directory ownership marker is a directory.");
        }
        EnsureOrdinaryFile(
            markerPath,
            "Recursive import directory ownership marker");

        byte[] expected = RecursiveDirectoryOwnerPayload(
            batchId,
            directoryPlan);
        var markerInfo = new FileInfo(markerPath);
        if (markerInfo.Length != expected.LongLength)
        {
            throw new InvalidDataException(
                "Recursive import directory ownership marker has an invalid length.");
        }
        byte[] actual = File.ReadAllBytes(markerPath);
        EnsureOrdinaryFile(
            markerPath,
            "Recursive import directory ownership marker");
        if (actual.LongLength != expected.LongLength ||
            !CryptographicOperations.FixedTimeEquals(actual, expected))
        {
            throw new InvalidDataException(
                "Recursive import directory ownership marker does not match its batch.");
        }
    }

    private static bool IsOwnedRecursiveDirectoryWithNoOtherEntries(
        string directory,
        string batchId,
        RecursiveBatchDirectory directoryPlan)
    {
        try
        {
            EnsureRecursiveDirectoryOwnership(
                directory,
                batchId,
                directoryPlan);
            string markerName =
                RecursiveDirectoryOwnerMarkerName(directoryPlan);
            FileSystemInfo[] entries = new DirectoryInfo(directory)
                .EnumerateFileSystemInfos(
                    "*",
                    SearchOption.TopDirectoryOnly)
                .ToArray();
            if (entries.Length != 1 ||
                !string.Equals(
                    entries[0].Name,
                    markerName,
                    StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
            entries[0].Refresh();
            if ((entries[0].Attributes & FileAttributes.Directory) != 0 ||
                (entries[0].Attributes & FileAttributes.ReparsePoint) != 0)
            {
                return false;
            }
            EnsureRecursiveDirectoryOwnership(
                directory,
                batchId,
                directoryPlan);
            return true;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            return false;
        }
    }

    private static bool TryDeleteOwnedRecursiveDirectory(
        string directory,
        string batchId,
        RecursiveBatchDirectory directoryPlan)
    {
        try
        {
            if (File.Exists(directory))
                return false;
            if (!Directory.Exists(directory))
                return true;
            if (!IsOwnedRecursiveDirectoryWithNoOtherEntries(
                    directory,
                    batchId,
                    directoryPlan))
            {
                return false;
            }

            string markerPath = Path.Combine(
                directory,
                RecursiveDirectoryOwnerMarkerName(directoryPlan));
            File.Delete(markerPath);
            Directory.Delete(directory, recursive: false);
            return true;
        }
        catch (DirectoryNotFoundException)
        {
            return !File.Exists(directory);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            return false;
        }
    }

    private static bool TryRemoveRecursiveDirectoryOwnerMarker(
        string directory,
        string batchId,
        RecursiveBatchDirectory directoryPlan)
    {
        try
        {
            if (File.Exists(directory) || !Directory.Exists(directory))
                return false;
            EnsureOrdinaryDirectory(
                directory,
                "Recursive import owned directory");
            string markerPath = Path.Combine(
                directory,
                RecursiveDirectoryOwnerMarkerName(directoryPlan));
            if (Directory.Exists(markerPath))
                return false;
            if (!File.Exists(markerPath))
                return true;
            EnsureRecursiveDirectoryOwnership(
                directory,
                batchId,
                directoryPlan);
            File.Delete(markerPath);
            return !File.Exists(markerPath) &&
                   !Directory.Exists(markerPath);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            return false;
        }
    }

    private static bool RollbackPublishedBatch(
        AssetDatabase database,
        IReadOnlyList<ImportTransaction> published,
        List<string> warnings,
        bool refreshIndex = true)
    {
        bool rollbackComplete = true;
        foreach (ImportTransaction transaction in published.Reverse())
        {
            rollbackComplete &= TryRollbackTransaction(
                transaction,
                warnings,
                CancellationToken.None);
        }
        if (refreshIndex)
        {
            rollbackComplete &= TryRefreshAfterImportRollback(
                database,
                warnings);
        }
        return rollbackComplete;
    }

    private static bool TryRefreshAfterImportRollback(
        AssetDatabase database,
        List<string> warnings)
    {
        try
        {
            _ = database.RefreshWithinAssetTransaction(
                verifyContent: false,
                CancellationToken.None);
        }
        catch (Exception error)
        {
            warnings.Add(
                "The asset index could not be repaired after import rollback: " +
                error.Message);
            return false;
        }
        return true;
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
        AssetImportTestHooks? testHooks,
        bool requireExactDestinationName = false,
        PlannedImportFile? expectedSource = null,
        string? destinationOverride = null,
        string? transactionIdOverride = null,
        string? stagingParentOverride = null,
        AssetImporterSettings? importerSettings = null)
    {
        string source = ValidateSourceFile(database.AssetsRoot, sourcePath);
        if (expectedSource != null &&
            !string.Equals(
                source,
                expectedSource.SourcePath,
                PathComparison))
        {
            throw new InvalidDataException(
                "Recursive import source no longer matches its plan.");
        }
        string destination;
        if (destinationOverride is not null)
        {
            destination = Path.GetFullPath(destinationOverride);
            string destinationParent = Path.GetDirectoryName(destination)
                ?? throw new InvalidDataException(
                    "Recursive import destination has no parent.");
            if (!string.Equals(
                    NormalizeDirectory(destinationParent),
                    NormalizeDirectory(targetDirectory),
                    PathComparison))
            {
                throw new InvalidDataException(
                    "Recursive import destination does not match its planned parent.");
            }
            ValidateImportFileName(Path.GetFileName(destination));
            if (File.Exists(destination) ||
                Directory.Exists(destination) ||
                File.Exists(destination + AssetDatabase.MetadataSuffix) ||
                Directory.Exists(destination + AssetDatabase.MetadataSuffix))
            {
                throw new AssetImportRecoveryRequiredException(
                    $"Recursive import destination already exists: " +
                    $"'{Path.GetFileName(destination)}'.");
            }
        }
        else
        {
            destination = requireExactDestinationName
                ? RequireExactDestination(targetDirectory, source)
                : ReserveDestination(targetDirectory, source);
        }
        string relativeDestination = NormalizeRelativeAssetPath(
            database.AssetsRoot,
            destination);
        string transactionId = transactionIdOverride ??
            Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        if (!Guid.TryParseExact(transactionId, "N", out Guid transactionGuid) ||
            transactionGuid == Guid.Empty)
        {
            throw new InvalidDataException(
                "Recursive import transaction id is invalid.");
        }
        string stagingRoot = stagingParentOverride is null
            ? EnsureStagingRoot(database.AssetsRoot)
            : Path.GetFullPath(stagingParentOverride);
        if (stagingParentOverride is not null)
        {
            EnsureNoReparseDirectories(stagingRoot);
            EnsureOrdinaryDirectory(
                stagingRoot,
                "Recursive import batch directory");
        }
        string stagingDirectory = Path.Combine(stagingRoot, transactionId);
        if (File.Exists(stagingDirectory) ||
            Directory.Exists(stagingDirectory))
        {
            throw new AssetImportRecoveryRequiredException(
                "Recursive import child transaction directory already exists.");
        }
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
                cancellationToken,
                maximumLength: expectedSource?.Length);
            if (expectedSource != null &&
                (snapshot.Length != expectedSource.Length ||
                 snapshot.LastWriteUtcTicks != expectedSource.LastWriteUtcTicks))
            {
                throw new IOException(
                    $"Recursive import source changed after planning: " +
                    $"'{Path.GetFileName(source)}'.");
            }
            string assetKind = AssetDatabase.ClassifyExtension(
                Path.GetExtension(relativeDestination));
            AssetImporterRecipe recipe =
                AssetImporterRecipeContract.Create(
                    assetKind,
                    importerSettings);
            AssetImportDerivedDataResult processed =
                AssetImportDerivedDataPipeline.GetOrCreate(
                    database.ProjectRoot,
                    stagedPayload,
                    assetKind,
                    Path.GetExtension(relativeDestination),
                    recipe.Importer,
                    recipe.ImporterVersion,
                    recipe.Settings,
                    snapshot.ContentHash,
                    snapshot.Length,
                    cancellationToken);
            var sourceSettings = new[]
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
            KeyValuePair<string, string>[] importSettings =
                recipe.Settings
                    .Concat(sourceSettings)
                    .Concat(
                        AssetImportDerivedDataPipeline.MetadataSettings(
                            processed))
                    .OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                    .ToArray();
            (AssetMetadata _, byte[] metadataBytes) =
                database.CreateImportMetadataPayload(
                    relativeDestination,
                    source,
                    recipe.Importer,
                    recipe.ImporterVersion,
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

            if (TryParseRecursiveBatchDirectoryName(
                    directory.Name,
                    out string batchDirectoryId))
            {
                string batchJournalPath = Path.Combine(
                    directory.FullName,
                    RecursiveBatchJournalFileName);
                if (!File.Exists(batchJournalPath))
                {
                    string? cleanupWarning = null;
                    if (ContainsOnlyIncompleteRecursiveBatchFiles(
                            directory.FullName) &&
                        TryCleanupRecursiveBatchDirectory(
                            directory.FullName,
                            out cleanupWarning))
                    {
                        discarded++;
                    }
                    else
                    {
                        preserved++;
                        warnings.Add(
                            cleanupWarning ??
                            $"Import recovery preserved incomplete recursive " +
                            $"batch '{directory.Name}'.");
                    }
                    continue;
                }

                try
                {
                    RecursiveBatchManifest batch =
                        ReadRecursiveBatchJournal(batchJournalPath);
                    if (!string.Equals(
                            batch.BatchId,
                            batchDirectoryId,
                            StringComparison.Ordinal))
                    {
                        throw new InvalidDataException(
                            "Recursive batch id does not match its directory.");
                    }
                    RecoveryDisposition disposition =
                        RecoverRecursiveBatch(
                            assetsRoot,
                            directory.FullName,
                            batch,
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
                        InvalidDataException or JsonException or
                        ArgumentException)
                {
                    preserved++;
                    warnings.Add(
                        $"Import recovery preserved recursive batch " +
                        $"'{directory.Name}': {error.Message}");
                }
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

    private static RecoveryDisposition RecoverRecursiveBatch(
        string assetsRoot,
        string batchDirectory,
        RecursiveBatchManifest manifest,
        CancellationToken cancellationToken)
    {
        ValidateRecursiveBatchManifest(manifest);
        cancellationToken.ThrowIfCancellationRequested();
        if (manifest.Phase == RecursiveBatchPhase.Committed)
        {
            ValidateCommittedRecursiveBatch(
                assetsRoot,
                batchDirectory,
                manifest,
                cancellationToken);
            foreach (RecursiveBatchFile file in manifest.Files)
            {
                string childDirectory = Path.Combine(
                    batchDirectory,
                    file.TransactionId);
                if (Directory.Exists(childDirectory) &&
                    !TryCleanupTransaction(
                        childDirectory,
                        out string? warning))
                {
                    throw new IOException(warning);
                }
            }
            foreach (RecursiveBatchDirectory directory in
                     manifest.Directories.Reverse())
            {
                string destination = ResolveRelativeDirectoryPath(
                    assetsRoot,
                    directory.DestinationRelativePath);
                if (!TryRemoveRecursiveDirectoryOwnerMarker(
                        destination,
                        manifest.BatchId,
                        directory))
                {
                    throw new IOException(
                        "Committed recursive batch ownership marker could not be safely cleaned.");
                }
                string privateDirectory = Path.Combine(
                    batchDirectory,
                    directory.PrivateStagingName);
                if (Directory.Exists(privateDirectory) &&
                    !TryDeleteOwnedRecursiveDirectory(
                        privateDirectory,
                        manifest.BatchId,
                        directory))
                {
                    throw new IOException(
                        "Committed recursive batch private directory could not be cleaned.");
                }
            }
            if (!TryCleanupRecursiveBatchDirectory(
                    batchDirectory,
                    out string? batchWarning))
            {
                throw new IOException(batchWarning);
            }
            return RecoveryDisposition.Completed;
        }

        var warnings = new List<string>();
        bool complete = true;
        foreach (RecursiveBatchFile file in manifest.Files.Reverse())
        {
            complete &= TryRollbackRecursiveBatchFile(
                assetsRoot,
                batchDirectory,
                file,
                warnings,
                cancellationToken);
        }
        foreach (RecursiveBatchDirectory directory in
                 manifest.Directories.Reverse())
        {
            complete &= TryRollbackRecursiveBatchDirectory(
                assetsRoot,
                batchDirectory,
                manifest.BatchId,
                directory,
                warnings);
        }
        if (!complete)
        {
            throw new AssetImportRecoveryRequiredException(
                "Recursive import batch rollback preserved ambiguous state. " +
                string.Join(" ", warnings));
        }
        if (!TryCleanupRecursiveBatchDirectory(
                batchDirectory,
                out string? cleanupWarning))
        {
            throw new IOException(cleanupWarning);
        }
        return RecoveryDisposition.Discarded;
    }

    private static void ValidateCommittedRecursiveBatch(
        string assetsRoot,
        string batchDirectory,
        RecursiveBatchManifest manifest,
        CancellationToken cancellationToken)
    {
        foreach (RecursiveBatchDirectory directory in manifest.Directories)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string destination = ResolveRelativeDirectoryPath(
                assetsRoot,
                directory.DestinationRelativePath);
            EnsureOrdinaryDirectory(
                destination,
                "Committed recursive import directory");
            string destinationMarker = Path.Combine(
                destination,
                RecursiveDirectoryOwnerMarkerName(directory));
            if (Directory.Exists(destinationMarker))
            {
                throw new InvalidDataException(
                    "Committed recursive import ownership marker is a directory.");
            }
            if (File.Exists(destinationMarker))
            {
                EnsureRecursiveDirectoryOwnership(
                    destination,
                    manifest.BatchId,
                    directory);
            }
            string privateDirectory = Path.Combine(
                batchDirectory,
                directory.PrivateStagingName);
            if (File.Exists(privateDirectory))
            {
                throw new InvalidDataException(
                    "Committed recursive batch private path is a file.");
            }
            if (Directory.Exists(privateDirectory))
            {
                if (!IsOwnedRecursiveDirectoryWithNoOtherEntries(
                        privateDirectory,
                        manifest.BatchId,
                        directory))
                {
                    throw new InvalidDataException(
                        "Committed recursive batch private directory has no matching ownership marker or contains unexpected entries.");
                }
            }
        }

        foreach (RecursiveBatchFile file in manifest.Files)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string destination = ResolveRelativeAssetPath(
                assetsRoot,
                file.DestinationRelativePath);
            string destinationMetadata =
                destination + AssetDatabase.MetadataSuffix;
            EnsureMatchingOrdinaryFile(
                destination,
                file.PayloadLength,
                file.PayloadSha256,
                "Committed recursive import asset",
                cancellationToken);
            EnsureMatchingOrdinaryFile(
                destinationMetadata,
                expectedLength: null,
                file.MetadataSha256,
                "Committed recursive import metadata",
                cancellationToken);

            string childDirectory = Path.Combine(
                batchDirectory,
                file.TransactionId);
            if (File.Exists(childDirectory))
            {
                throw new InvalidDataException(
                    "Committed recursive child transaction is shadowed by a file.");
            }
            if (!Directory.Exists(childDirectory))
                continue;
            EnsureOrdinaryDirectory(
                childDirectory,
                "Committed recursive child transaction");
            string journalPath = Path.Combine(
                childDirectory,
                JournalFileName);
            if (!File.Exists(journalPath))
            {
                if (!ContainsOnlyCommittedChildCleanupFiles(
                        childDirectory))
                {
                    throw new InvalidDataException(
                        "Committed recursive child transaction has no journal and contains unexpected entries.");
                }
                continue;
            }
            ImportManifest child = ReadJournal(journalPath);
            ValidateRecursiveChildManifest(file, child);
            if (child.Phase != AssetImportCheckpoint.MetadataPublished ||
                !ContainsOnlyTransactionFiles(childDirectory))
            {
                throw new InvalidDataException(
                    "Committed recursive child transaction is incomplete or contains unexpected entries.");
            }
        }
    }

    private static bool TryRollbackRecursiveBatchFile(
        string assetsRoot,
        string batchDirectory,
        RecursiveBatchFile file,
        List<string> warnings,
        CancellationToken cancellationToken)
    {
        string destination = ResolveRelativeAssetPath(
            assetsRoot,
            file.DestinationRelativePath);
        string destinationMetadata =
            destination + AssetDatabase.MetadataSuffix;
        string childDirectory = Path.Combine(
            batchDirectory,
            file.TransactionId);
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (File.Exists(childDirectory))
            {
                warnings.Add(
                    $"Recursive child '{file.TransactionId}' is shadowed by a file.");
                return false;
            }
            if (!Directory.Exists(childDirectory))
            {
                if (File.Exists(destination) ||
                    Directory.Exists(destination) ||
                    File.Exists(destinationMetadata) ||
                    Directory.Exists(destinationMetadata))
                {
                    warnings.Add(
                        $"Recursive child '{file.DestinationRelativePath}' " +
                        "has a destination but no recovery journal.");
                    return false;
                }
                return true;
            }

            EnsureOrdinaryDirectory(
                childDirectory,
                "Recursive child transaction");
            string journalPath = Path.Combine(
                childDirectory,
                JournalFileName);
            if (!File.Exists(journalPath))
            {
                string? cleanupWarning = null;
                if (ContainsOnlyPrivatePreparationFiles(childDirectory) &&
                    TryCleanupTransaction(
                        childDirectory,
                        out cleanupWarning))
                {
                    return true;
                }
                warnings.Add(
                    cleanupWarning ??
                    $"Recursive child '{file.TransactionId}' has no valid journal.");
                return false;
            }

            ImportManifest child = ReadJournal(journalPath);
            ValidateRecursiveChildManifest(file, child);
            if (Directory.Exists(destination) ||
                Directory.Exists(destinationMetadata))
            {
                warnings.Add(
                    $"Recursive child '{file.DestinationRelativePath}' is shadowed by a directory.");
                return false;
            }
            if (File.Exists(destination) &&
                !FileMatches(
                    destination,
                    child.PayloadLength,
                    child.PayloadSha256,
                    cancellationToken))
            {
                warnings.Add(
                    $"Recursive child '{file.DestinationRelativePath}' changed after publication.");
                return false;
            }
            if (File.Exists(destinationMetadata) &&
                !FileMatches(
                    destinationMetadata,
                    expectedLength: null,
                    child.MetadataSha256,
                    cancellationToken))
            {
                warnings.Add(
                    $"Recursive child metadata '{file.DestinationRelativePath}' changed after publication.");
                return false;
            }

            string stagedPayload = Path.Combine(
                childDirectory,
                PayloadFileName);
            string stagedMetadata = Path.Combine(
                childDirectory,
                MetadataFileName);
            if (File.Exists(stagedPayload) &&
                !FileMatches(
                    stagedPayload,
                    child.PayloadLength,
                    child.PayloadSha256,
                    cancellationToken))
            {
                warnings.Add(
                    $"Recursive child staged payload '{file.TransactionId}' changed.");
                return false;
            }
            if (File.Exists(stagedMetadata) &&
                !FileMatches(
                    stagedMetadata,
                    expectedLength: null,
                    child.MetadataSha256,
                    cancellationToken))
            {
                warnings.Add(
                    $"Recursive child staged metadata '{file.TransactionId}' changed.");
                return false;
            }
            if (!ContainsOnlyTransactionFiles(childDirectory))
            {
                warnings.Add(
                    $"Recursive child '{file.TransactionId}' contains unexpected entries.");
                return false;
            }

            var transaction = new ImportTransaction(
                assetsRoot,
                childDirectory,
                journalPath,
                stagedPayload,
                stagedMetadata,
                destination,
                destinationMetadata,
                child);
            return TryRollbackTransaction(
                transaction,
                warnings,
                CancellationToken.None);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or JsonException or
                ArgumentException)
        {
            warnings.Add(
                $"Recursive child rollback failed: {error.Message}");
            return false;
        }
    }

    private static bool TryRollbackRecursiveBatchDirectory(
        string assetsRoot,
        string batchDirectory,
        string batchId,
        RecursiveBatchDirectory directory,
        List<string> warnings)
    {
        try
        {
            string destination = ResolveRelativeDirectoryPath(
                assetsRoot,
                directory.DestinationRelativePath);
            string privateDirectory = Path.Combine(
                batchDirectory,
                directory.PrivateStagingName);
            if (File.Exists(privateDirectory))
            {
                warnings.Add(
                    $"Recursive private directory '{directory.PrivateStagingName}' is a file.");
                return false;
            }
            if (Directory.Exists(privateDirectory) &&
                !TryDeleteOwnedRecursiveDirectory(
                    privateDirectory,
                    batchId,
                    directory))
            {
                warnings.Add(
                    $"Recursive private directory " +
                    $"'{directory.PrivateStagingName}' has no matching ownership marker or changed.");
                return false;
            }
            if (File.Exists(destination))
            {
                warnings.Add(
                    $"Recursive destination directory " +
                    $"'{directory.DestinationRelativePath}' became a file.");
                return false;
            }
            if (!Directory.Exists(destination))
                return true;
            if (TryDeleteOwnedRecursiveDirectory(
                    destination,
                    batchId,
                    directory))
            {
                return true;
            }
            warnings.Add(
                $"Recursive destination directory " +
                $"'{directory.DestinationRelativePath}' has no matching " +
                "ownership marker or contains changed entries.");
            return false;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException)
        {
            warnings.Add(
                $"Recursive directory rollback failed: {error.Message}");
            return false;
        }
    }

    private static void ValidateRecursiveChildManifest(
        RecursiveBatchFile planned,
        ImportManifest child)
    {
        ValidateManifest(child);
        if (!string.Equals(
                child.TransactionId,
                planned.TransactionId,
                StringComparison.Ordinal) ||
            !string.Equals(
                child.SourcePath,
                planned.SourcePath.Replace('\\', '/'),
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                child.DestinationRelativePath,
                planned.DestinationRelativePath,
                StringComparison.OrdinalIgnoreCase) ||
            child.PayloadLength != planned.PayloadLength)
        {
            throw new InvalidDataException(
                "Recursive child journal does not match its batch plan.");
        }
        if (planned.PayloadSha256.Length != 0 &&
            (!string.Equals(
                 child.PayloadSha256,
                 planned.PayloadSha256,
                 StringComparison.Ordinal) ||
             !string.Equals(
                 child.MetadataSha256,
                 planned.MetadataSha256,
                 StringComparison.Ordinal)))
        {
            throw new InvalidDataException(
                "Recursive child journal fingerprints do not match the committed batch.");
        }
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
        CancellationToken cancellationToken,
        long? maximumLength = null)
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
                if (maximumLength.HasValue &&
                    length > maximumLength.Value - read)
                {
                    throw new IOException(
                        "Recursive import source grew beyond its planned size.");
                }
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

    private static string RequireExactDestination(
        string targetDirectory,
        string source)
    {
        string fileName = Path.GetFileName(source);
        ValidateImportFileName(fileName);
        string destination = Path.Combine(targetDirectory, fileName);
        if (File.Exists(destination) ||
            Directory.Exists(destination) ||
            File.Exists(destination + AssetDatabase.MetadataSuffix) ||
            Directory.Exists(destination + AssetDatabase.MetadataSuffix))
        {
            throw new IOException(
                $"Recursive import destination already exists: '{fileName}'.");
        }
        return destination;
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

    private static string CreateRecursiveBatchJournal(
        string assetsRoot,
        RecursiveBatchManifest manifest)
    {
        ValidateRecursiveBatchManifest(manifest);
        string stagingRoot = EnsureStagingRoot(assetsRoot);
        string batchDirectory = Path.Combine(
            stagingRoot,
            RecursiveBatchDirectoryPrefix + manifest.BatchId);
        if (File.Exists(batchDirectory) ||
            Directory.Exists(batchDirectory))
        {
            throw new AssetImportRecoveryRequiredException(
                "Recursive import batch journal directory already exists.");
        }

        Directory.CreateDirectory(batchDirectory);
        try
        {
            EnsureOrdinaryDirectory(
                batchDirectory,
                "Recursive import batch directory");
            WriteRecursiveBatchJournal(
                Path.Combine(
                    batchDirectory,
                    RecursiveBatchJournalFileName),
                manifest);
            return batchDirectory;
        }
        catch
        {
            _ = TryCleanupRecursiveBatchDirectory(
                batchDirectory,
                out _);
            throw;
        }
    }

    private static void WriteRecursiveBatchJournal(
        string journalPath,
        RecursiveBatchManifest manifest)
    {
        ValidateRecursiveBatchManifest(manifest);
        byte[] bytes;
        using (var memory = new MemoryStream())
        {
            using (var writer = new Utf8JsonWriter(
                memory,
                new JsonWriterOptions { Indented = true }))
            {
                writer.WriteStartObject();
                writer.WriteNumber(
                    "schemaVersion",
                    manifest.SchemaVersion);
                writer.WriteString("batchId", manifest.BatchId);
                writer.WriteString(
                    "phase",
                    RecursiveBatchPhaseText(manifest.Phase));
                writer.WriteString(
                    "sourceFingerprint",
                    manifest.SourceFingerprint);
                writer.WriteNumber(
                    "createdUtcTicks",
                    manifest.CreatedUtcTicks);
                writer.WriteStartArray("directories");
                foreach (RecursiveBatchDirectory directory in
                         manifest.Directories)
                {
                    writer.WriteStartObject();
                    writer.WriteString(
                        "destinationRelativePath",
                        directory.DestinationRelativePath);
                    writer.WriteString(
                        "privateStagingName",
                        directory.PrivateStagingName);
                    writer.WriteString(
                        "ownerToken",
                        directory.OwnerToken);
                    writer.WriteBoolean(
                        "isSelectionRoot",
                        directory.IsSelectionRoot);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteStartArray("files");
                foreach (RecursiveBatchFile file in manifest.Files)
                {
                    writer.WriteStartObject();
                    writer.WriteString(
                        "transactionId",
                        file.TransactionId);
                    writer.WriteString("sourcePath", file.SourcePath);
                    writer.WriteString(
                        "sourceRelativePath",
                        file.SourceRelativePath);
                    writer.WriteString(
                        "destinationRelativePath",
                        file.DestinationRelativePath);
                    writer.WriteNumber(
                        "payloadLength",
                        file.PayloadLength);
                    writer.WriteNumber(
                        "sourceLastWriteUtcTicks",
                        file.SourceLastWriteUtcTicks);
                    writer.WriteString(
                        "payloadSha256",
                        file.PayloadSha256);
                    writer.WriteString(
                        "metadataSha256",
                        file.MetadataSha256);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteEndObject();
            }
            bytes = AddFinalNewline(memory.ToArray());
        }
        if (bytes.Length > MaxRecursiveBatchJournalBytes)
        {
            throw new InvalidDataException(
                $"Recursive import batch journal exceeds " +
                $"{MaxRecursiveBatchJournalBytes} bytes.");
        }
        AtomicWrite(journalPath, bytes);
    }

    private static RecursiveBatchManifest ReadRecursiveBatchJournal(
        string journalPath)
    {
        EnsureOrdinaryFile(
            journalPath,
            "Recursive import batch journal");
        var info = new FileInfo(journalPath);
        if (info.Length is <= 0 or > MaxRecursiveBatchJournalBytes)
        {
            throw new InvalidDataException(
                "Recursive import batch journal has an invalid size.");
        }
        using JsonDocument document = JsonDocument.Parse(
            File.ReadAllBytes(journalPath),
            new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 32,
            });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException(
                "Recursive import batch journal must be an object.");
        }
        RequireExactProperties(
            root,
            "schemaVersion",
            "batchId",
            "phase",
            "sourceFingerprint",
            "createdUtcTicks",
            "directories",
            "files");

        JsonElement directoryArray = ReadArray(
            root,
            "directories");
        if (directoryArray.GetArrayLength() > DefaultMaxRecursiveEntries)
        {
            throw new InvalidDataException(
                "Recursive import batch journal contains too many directories.");
        }
        var directories = new List<RecursiveBatchDirectory>(
            directoryArray.GetArrayLength());
        foreach (JsonElement directory in directoryArray.EnumerateArray())
        {
            RequireExactProperties(
                directory,
                "destinationRelativePath",
                "privateStagingName",
                "ownerToken",
                "isSelectionRoot");
            directories.Add(new RecursiveBatchDirectory(
                ReadString(directory, "destinationRelativePath"),
                ReadString(directory, "privateStagingName"),
                ReadString(directory, "ownerToken"),
                ReadBoolean(directory, "isSelectionRoot")));
        }

        JsonElement fileArray = ReadArray(root, "files");
        if (fileArray.GetArrayLength() > DefaultMaxRecursiveEntries)
        {
            throw new InvalidDataException(
                "Recursive import batch journal contains too many files.");
        }
        var files = new List<RecursiveBatchFile>(
            fileArray.GetArrayLength());
        foreach (JsonElement file in fileArray.EnumerateArray())
        {
            RequireExactProperties(
                file,
                "transactionId",
                "sourcePath",
                "sourceRelativePath",
                "destinationRelativePath",
                "payloadLength",
                "sourceLastWriteUtcTicks",
                "payloadSha256",
                "metadataSha256");
            files.Add(new RecursiveBatchFile(
                ReadString(file, "transactionId"),
                ReadString(file, "sourcePath"),
                ReadString(file, "sourceRelativePath"),
                ReadString(file, "destinationRelativePath"),
                ReadLong(file, "payloadLength"),
                ReadLong(file, "sourceLastWriteUtcTicks"),
                ReadString(file, "payloadSha256"),
                ReadString(file, "metadataSha256")));
        }

        var manifest = new RecursiveBatchManifest(
            ReadInt(root, "schemaVersion"),
            ReadString(root, "batchId"),
            ParseRecursiveBatchPhase(ReadString(root, "phase")),
            ReadString(root, "sourceFingerprint"),
            ReadLong(root, "createdUtcTicks"),
            Array.AsReadOnly(directories.ToArray()),
            Array.AsReadOnly(files.ToArray()));
        ValidateRecursiveBatchManifest(manifest);
        return manifest;
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

    private static void ValidateRecursiveBatchManifest(
        RecursiveBatchManifest manifest)
    {
        if (manifest.SchemaVersion != RecursiveBatchJournalSchemaVersion)
        {
            throw new InvalidDataException(
                "Unsupported recursive import batch journal schema.");
        }
        if (!Guid.TryParseExact(
                manifest.BatchId,
                "N",
                out Guid batchId) ||
            batchId == Guid.Empty)
        {
            throw new InvalidDataException(
                "Invalid recursive import batch id.");
        }
        if (manifest.Phase is not (
                RecursiveBatchPhase.Prepared or
                RecursiveBatchPhase.Committed) ||
            !IsSha256(manifest.SourceFingerprint) ||
            manifest.CreatedUtcTicks <= 0 ||
            manifest.Directories is null ||
            manifest.Files is null ||
            manifest.Directories.Count > DefaultMaxRecursiveEntries ||
            manifest.Files.Count > DefaultMaxRecursiveEntries)
        {
            throw new InvalidDataException(
                "Invalid recursive import batch metadata.");
        }

        var destinationPaths = new HashSet<string>(
            StringComparer.OrdinalIgnoreCase);
        var privateNames = new HashSet<string>(
            StringComparer.OrdinalIgnoreCase);
        int selectionRoots = 0;
        foreach (RecursiveBatchDirectory directory in
                 manifest.Directories)
        {
            ValidateRelativeDirectoryPath(
                directory.DestinationRelativePath);
            if (!destinationPaths.Add(
                    directory.DestinationRelativePath) ||
                !destinationPaths.Add(
                    directory.DestinationRelativePath +
                    AssetDatabase.MetadataSuffix) ||
                !TryParsePrivateDirectoryName(
                    directory.PrivateStagingName,
                    out _) ||
                !IsSha256(directory.OwnerToken) ||
                !privateNames.Add(directory.PrivateStagingName))
            {
                throw new InvalidDataException(
                    "Recursive import batch contains duplicate or invalid directory paths.");
            }
            if (directory.IsSelectionRoot)
                selectionRoots++;
        }
        if (selectionRoots == 0)
        {
            throw new InvalidDataException(
                "Recursive import batch has no selected directory root.");
        }

        var transactionIds = new HashSet<string>(
            StringComparer.OrdinalIgnoreCase);
        foreach (RecursiveBatchFile file in manifest.Files)
        {
            if (!Guid.TryParseExact(
                    file.TransactionId,
                    "N",
                    out Guid transactionId) ||
                transactionId == Guid.Empty ||
                !transactionIds.Add(file.TransactionId))
            {
                throw new InvalidDataException(
                    "Recursive import batch contains an invalid child transaction id.");
            }
            if (file.SourcePath.Length is 0 or > 32767 ||
                file.SourcePath.IndexOf('\0') >= 0 ||
                file.SourceRelativePath.Length is 0 or > 4096 ||
                Path.IsPathRooted(file.SourceRelativePath) ||
                file.SourceRelativePath.IndexOf('\0') >= 0 ||
                file.PayloadLength < 0 ||
                file.SourceLastWriteUtcTicks <= 0)
            {
                throw new InvalidDataException(
                    "Recursive import batch contains invalid source metadata.");
            }
            ValidateRelativeAssetFilePath(
                file.DestinationRelativePath);
            if (!destinationPaths.Add(
                    file.DestinationRelativePath) ||
                !destinationPaths.Add(
                    file.DestinationRelativePath +
                    AssetDatabase.MetadataSuffix))
            {
                throw new InvalidDataException(
                    "Recursive import batch contains duplicate destination paths.");
            }
            bool hashesValid = manifest.Phase ==
                RecursiveBatchPhase.Prepared
                ? file.PayloadSha256.Length == 0 &&
                  file.MetadataSha256.Length == 0
                : IsSha256(file.PayloadSha256) &&
                  IsSha256(file.MetadataSha256);
            if (!hashesValid)
            {
                throw new InvalidDataException(
                    "Recursive import batch contains invalid child fingerprints.");
            }
        }
    }

    private static string NormalizeRelativeDirectoryPath(
        string assetsRoot,
        string path)
    {
        string root = NormalizeDirectory(assetsRoot);
        string full = Path.GetFullPath(path);
        if (!IsUnder(full, root))
        {
            throw new InvalidDataException(
                "Recursive import directory escapes Assets.");
        }
        string relative = Path.GetRelativePath(root, full)
            .Replace('\\', '/');
        ValidateRelativeDirectoryPath(relative);
        return relative;
    }

    private static string ResolveRelativeDirectoryPath(
        string assetsRoot,
        string relative)
    {
        ValidateRelativeDirectoryPath(relative);
        string root = NormalizeDirectory(assetsRoot);
        string destination = Path.GetFullPath(Path.Combine(
            root,
            relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!IsUnder(destination, root))
        {
            throw new InvalidDataException(
                "Recovered recursive import directory escapes Assets.");
        }
        return destination;
    }

    private static void ValidateRelativeDirectoryPath(string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) ||
            relative.Length > 4096 ||
            Path.IsPathRooted(relative) ||
            relative.Contains('\\') ||
            relative.IndexOf('\0') >= 0)
        {
            throw new InvalidDataException(
                "Invalid recursive import directory path.");
        }
        foreach (string segment in relative.Split('/'))
        {
            ValidateImportDirectoryName(segment);
            if (string.Equals(
                    segment,
                    AssetDatabase.InternalDirectoryName,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "Recursive import directory uses the reserved database directory.");
            }
        }
    }

    private static void ValidateRelativeAssetFilePath(string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) ||
            relative.Length > 4096 ||
            Path.IsPathRooted(relative) ||
            relative.Contains('\\') ||
            relative.IndexOf('\0') >= 0)
        {
            throw new InvalidDataException(
                "Invalid recursive import file path.");
        }
        string[] segments = relative.Split('/');
        if (segments.Length == 0)
        {
            throw new InvalidDataException(
                "Invalid recursive import file path.");
        }
        for (int index = 0; index < segments.Length - 1; index++)
        {
            ValidateImportDirectoryName(segments[index]);
            if (string.Equals(
                    segments[index],
                    AssetDatabase.InternalDirectoryName,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "Recursive import file uses the reserved database directory.");
            }
        }
        ValidateImportFileName(segments[^1]);
    }

    private static bool TryParsePrivateDirectoryName(
        string name,
        out Guid id)
    {
        id = Guid.Empty;
        const string prefix = "directory-";
        return name.StartsWith(prefix, StringComparison.Ordinal) &&
               Guid.TryParseExact(
                   name[prefix.Length..],
                   "N",
                   out id) &&
               id != Guid.Empty;
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

    private static void ValidateImportDirectoryName(string directoryName)
    {
        if (string.IsNullOrWhiteSpace(directoryName) ||
            directoryName is "." or ".." ||
            directoryName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            string.Equals(
                directoryName,
                AssetDatabase.InternalDirectoryName,
                StringComparison.OrdinalIgnoreCase) ||
            directoryName.EndsWith(
                AssetDatabase.MetadataSuffix,
                StringComparison.OrdinalIgnoreCase) ||
            directoryName.EndsWith(
                ".acsmat.graph.json",
                StringComparison.OrdinalIgnoreCase) ||
            AssetCreationWorkflow.IsTemporaryPath(directoryName))
        {
            throw new InvalidDataException(
                $"Directory name is reserved or invalid for import: " +
                $"'{directoryName}'.");
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

    private static bool ContainsOnlyTransactionFiles(string directory)
    {
        foreach (FileSystemInfo entry in new DirectoryInfo(directory)
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
                return false;
            }
        }
        return true;
    }

    private static bool ContainsOnlyCommittedChildCleanupFiles(
        string directory)
    {
        foreach (FileSystemInfo entry in new DirectoryInfo(directory)
            .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
        {
            entry.Refresh();
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                (entry.Attributes & FileAttributes.Directory) != 0 ||
                !entry.Name.StartsWith(
                    JournalFileName + ".tmp-",
                    StringComparison.Ordinal))
            {
                return false;
            }
        }
        return true;
    }

    private static bool TryParseRecursiveBatchDirectoryName(
        string name,
        out string batchId)
    {
        batchId = "";
        if (!name.StartsWith(
                RecursiveBatchDirectoryPrefix,
                StringComparison.Ordinal))
        {
            return false;
        }
        string candidate =
            name[RecursiveBatchDirectoryPrefix.Length..];
        if (!Guid.TryParseExact(candidate, "N", out Guid id) ||
            id == Guid.Empty)
        {
            return false;
        }
        batchId = id.ToString("N");
        return string.Equals(
            candidate,
            batchId,
            StringComparison.Ordinal);
    }

    private static bool ContainsOnlyIncompleteRecursiveBatchFiles(
        string batchDirectory)
    {
        foreach (FileSystemInfo entry in new DirectoryInfo(batchDirectory)
            .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
        {
            entry.Refresh();
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                (entry.Attributes & FileAttributes.Directory) != 0 ||
                !entry.Name.StartsWith(
                    RecursiveBatchJournalFileName + ".tmp-",
                    StringComparison.Ordinal))
            {
                return false;
            }
        }
        return true;
    }

    private static bool TryCleanupRecursiveBatchDirectory(
        string batchDirectory,
        out string? warning)
    {
        warning = null;
        try
        {
            if (!Directory.Exists(batchDirectory))
                return true;
            EnsureOrdinaryDirectory(
                batchDirectory,
                "Recursive import batch directory");
            foreach (FileSystemInfo entry in new DirectoryInfo(batchDirectory)
                .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                entry.Refresh();
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0 ||
                    (entry.Attributes & FileAttributes.Directory) != 0 ||
                    (entry.Name != RecursiveBatchJournalFileName &&
                     !entry.Name.StartsWith(
                         RecursiveBatchJournalFileName + ".tmp-",
                         StringComparison.Ordinal)))
                {
                    warning =
                        $"Recursive import cleanup preserved batch " +
                        $"'{Path.GetFileName(batchDirectory)}' because it " +
                        $"contains unexpected entry '{entry.Name}'.";
                    return false;
                }
                File.Delete(entry.FullName);
            }
            Directory.Delete(batchDirectory, recursive: false);
            return true;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException)
        {
            warning =
                $"Recursive import cleanup left batch " +
                $"'{Path.GetFileName(batchDirectory)}': {error.Message}";
            return false;
        }
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

    private static string RecursiveBatchPhaseText(
        RecursiveBatchPhase phase) => phase switch
    {
        RecursiveBatchPhase.Prepared => "prepared",
        RecursiveBatchPhase.Committed => "committed",
        _ => throw new InvalidDataException(
            "Unknown recursive import batch phase."),
    };

    private static RecursiveBatchPhase ParseRecursiveBatchPhase(
        string value) => value switch
    {
        "prepared" => RecursiveBatchPhase.Prepared,
        "committed" => RecursiveBatchPhase.Committed,
        _ => throw new InvalidDataException(
            "Unknown recursive import batch journal phase."),
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

    private static bool ReadBoolean(JsonElement parent, string name)
    {
        if (!parent.TryGetProperty(name, out JsonElement value) ||
            value.ValueKind is not (
                JsonValueKind.True or JsonValueKind.False))
        {
            throw new InvalidDataException(
                $"Import journal '{name}' must be a Boolean.");
        }
        return value.GetBoolean();
    }

    private static JsonElement ReadArray(
        JsonElement parent,
        string name)
    {
        if (!parent.TryGetProperty(name, out JsonElement value) ||
            value.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidDataException(
                $"Import journal '{name}' must be an array.");
        }
        return value;
    }

    private static void RequireExactProperties(
        JsonElement element,
        params string[] expectedProperties)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException(
                "Import journal entry must be an object.");
        }
        var expected = new HashSet<string>(
            expectedProperties,
            StringComparer.Ordinal);
        int count = 0;
        foreach (JsonProperty property in element.EnumerateObject())
        {
            count++;
            if (!expected.Remove(property.Name))
            {
                throw new InvalidDataException(
                    $"Import journal contains unexpected or duplicate property " +
                    $"'{property.Name}'.");
            }
        }
        if (count != expectedProperties.Length ||
            expected.Count != 0)
        {
            throw new InvalidDataException(
                "Import journal is missing required properties.");
        }
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

    private sealed record ExternalSourceRoot(
        string SourcePath,
        bool IsDirectory);

    private sealed record PlannedImportFile(
        string SourcePath,
        string RelativePath,
        long Length,
        long LastWriteUtcTicks);

    private sealed record RecursiveImportRoot(
        string SourcePath,
        IReadOnlyList<string> RelativeDirectories,
        IReadOnlyList<PlannedImportFile> Files);

    private sealed record RecursiveImportPlan(
        IReadOnlyList<RecursiveImportRoot> DirectoryRoots,
        IReadOnlyList<PlannedImportFile> LooseFiles,
        string SourceFingerprint);

    private enum RecursiveBatchPhase
    {
        Prepared,
        Committed,
    }

    private sealed record RecursiveBatchDirectory(
        string DestinationRelativePath,
        string PrivateStagingName,
        string OwnerToken,
        bool IsSelectionRoot);

    private sealed record RecursiveBatchFile(
        string TransactionId,
        string SourcePath,
        string SourceRelativePath,
        string DestinationRelativePath,
        long PayloadLength,
        long SourceLastWriteUtcTicks,
        string PayloadSha256,
        string MetadataSha256);

    private sealed record RecursiveBatchManifest(
        int SchemaVersion,
        string BatchId,
        RecursiveBatchPhase Phase,
        string SourceFingerprint,
        long CreatedUtcTicks,
        IReadOnlyList<RecursiveBatchDirectory> Directories,
        IReadOnlyList<RecursiveBatchFile> Files);

    private sealed record PendingSourceDirectory(
        string FullPath,
        string RelativePath,
        int Depth);

    private sealed class RecursiveTraversalBudget
    {
        private int _entries;
        private long _totalBytes;

        internal RecursiveTraversalBudget(AssetImportTraversalLimits limits)
        {
            Limits = limits;
        }

        internal AssetImportTraversalLimits Limits { get; }

        internal void AddEntry()
        {
            _entries++;
            if (_entries > Limits.MaxEntries)
            {
                throw new InvalidDataException(
                    $"Recursive import exceeds the maximum entry count of " +
                    $"{Limits.MaxEntries}.");
            }
        }

        internal void AddBytes(long length)
        {
            if (length < 0 ||
                length > Limits.MaxTotalBytes - _totalBytes)
            {
                throw new InvalidDataException(
                    $"Recursive import exceeds the maximum total size of " +
                    $"{Limits.MaxTotalBytes} bytes.");
            }
            _totalBytes += length;
        }
    }

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

    private sealed class AssetImportRollbackIncompleteException :
        AssetImportRecoveryRequiredException
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
