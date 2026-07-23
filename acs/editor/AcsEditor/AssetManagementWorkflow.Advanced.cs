// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace AcsEditor;

internal sealed record AssetRedirectorEntry(
    string OriginalRelativePath,
    string AssetId,
    string CurrentRelativePath,
    long UpdatedUtcTicks);

internal enum AssetRedirectorFixupAction
{
    RemoveMissingTarget,
    RemoveShadowedPath,
    RemoveRedundantPath,
    UpdateCurrentPath,
}

internal sealed record AssetRedirectorFixupItem(
    string OriginalRelativePath,
    string AssetId,
    string CurrentRelativePath,
    AssetRedirectorFixupAction Action);

internal sealed record AssetRedirectorFixupPreview(
    IReadOnlyList<AssetRedirectorFixupItem> Items,
    string PreviewToken);

internal sealed record AssetRedirectorFixupResult(
    int RemovedCount,
    int UpdatedCount,
    int RemainingCount);

internal sealed record AssetReplaceReferenceEdit(
    string RelativePath,
    bool ContentChanged,
    bool MetadataChanged,
    int ReplacementCount);

internal sealed record AssetReplaceReferencesPreview(
    string TargetAssetId,
    IReadOnlyList<string> SourceAssetIds,
    IReadOnlyList<AssetReplaceReferenceEdit> Edits,
    string PreviewToken);

internal sealed record AssetReplaceReferencesResult(
    int ContentFileCount,
    int MetadataAssetCount,
    int ReplacementCount);

internal sealed record AssetMigrationFile(
    string RelativePath,
    long SizeBytes);

internal sealed record AssetMigrationPreview(
    string TargetProjectRoot,
    IReadOnlyList<string> AssetIds,
    IReadOnlyList<AssetMigrationFile> Files,
    long TotalBytes,
    string PreviewToken);

internal sealed record AssetMigrationResult(
    string TargetAssetsRoot,
    int AssetCount,
    int FileCount,
    long TotalBytes);

internal sealed partial class AssetManagementWorkflow
{
    private const int RedirectorSchemaVersion = 1;
    private const long MaxRedirectorBytes = 1024L * 1024L;
    private const int MaxMigrationAssets = 65536;
    private const long MaxMigrationBytes = 16L * 1024L * 1024L * 1024L;
    private static readonly object AdvancedOperationGate = new();
    private static readonly JsonDocumentOptions StrictRedirectorJson = new()
    {
        AllowTrailingCommas = false,
        CommentHandling = JsonCommentHandling.Disallow,
        MaxDepth = 16,
    };

    private string RedirectorPath => Path.Combine(
        _assetsRoot,
        AssetDatabase.InternalDirectoryName,
        "redirectors.v1.json");

    /// <summary>
    /// Renames an asset and publishes a durable old-path redirect only after the asset transaction
    /// has completed. A redirector publication failure rolls the rename back.
    /// </summary>
    internal string RenameWithRedirector(
        string fullPath,
        string assetId,
        bool isDirectory,
        string newBaseName)
    {
        lock (AdvancedOperationGate)
        {
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Rename asset with redirector");
            RefreshAuthoritativeState();
            PathTarget source = ValidateTarget(fullPath, requireTreeValidation: isDirectory);
            IReadOnlyList<AssetRecord> originals = RecordsBelow(
                source.FullPath,
                source.IsDirectory);
            string renamed = Rename(fullPath, assetId, isDirectory, newBaseName);
            if (PathComparer.Equals(renamed, source.FullPath))
                return renamed;

            try
            {
                RegisterRedirectors(CurrentPairs(originals));
                return renamed;
            }
            catch (Exception error)
            {
                bool rolledBack = false;
                try
                {
                    string originalName = isDirectory
                        ? Path.GetFileName(source.FullPath)
                        : Path.GetFileNameWithoutExtension(source.FullPath);
                    string restored = Rename(
                        renamed,
                        isDirectory ? "" : assetId,
                        isDirectory,
                        originalName);
                    rolledBack = PathComparer.Equals(restored, source.FullPath);
                }
                catch
                {
                    rolledBack = false;
                }
                if (!rolledBack)
                {
                    throw new IOException(
                        "Redirector publication failed and the rename could not be rolled back.",
                        error);
                }
                throw;
            }
        }
    }

    /// <summary>
    /// Moves assets and records redirects for every indexed child. Redirector publication and the
    /// move are one logical transaction: failure restores every top-level source.
    /// </summary>
    internal AssetMoveResult MoveWithRedirectors(
        IEnumerable<string> fullPaths,
        string destinationDirectory)
    {
        lock (AdvancedOperationGate)
        {
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Move assets with redirectors");
            RefreshAuthoritativeState();
            IReadOnlyList<PathTarget> sources = NormalizeTopLevelTargets(fullPaths);
            AssetRecord[] originals = sources
                .SelectMany(source => RecordsBelow(source.FullPath, source.IsDirectory))
                .DistinctBy(static record => record.AssetId, StringComparer.OrdinalIgnoreCase)
                .ToArray();
            AssetMoveResult moved = MoveWithMappings(
                sources.Select(static source => source.FullPath),
                destinationDirectory);
            try
            {
                RegisterRedirectors(CurrentPairs(originals));
                return moved;
            }
            catch (Exception error)
            {
                bool rolledBack = true;
                foreach (AssetMoveMapping mapping in moved.Mappings
                             .OrderByDescending(static item => item.DestinationPath.Length))
                {
                    try
                    {
                        string parent = Path.GetDirectoryName(mapping.OriginalPath)
                            ?? throw new InvalidDataException("Original asset has no parent.");
                        string restored = Move(
                            new[] { mapping.DestinationPath },
                            parent).Single();
                        rolledBack &= PathComparer.Equals(restored, mapping.OriginalPath);
                    }
                    catch
                    {
                        rolledBack = false;
                    }
                }
                if (!rolledBack)
                {
                    throw new IOException(
                        "Redirector publication failed and the move could not be rolled back.",
                        error);
                }
                throw;
            }
        }
    }

    internal bool TryResolveRedirectedPath(string path, out string resolvedFullPath)
    {
        lock (AdvancedOperationGate)
        {
            string relative = NormalizeRedirectorRelativePath(path);
            if (_database.TryGetByPath(relative, out AssetRecord? direct) && direct != null)
            {
                resolvedFullPath = direct.FullPath;
                return true;
            }

            IReadOnlyDictionary<string, AssetRedirectorEntry> redirectors = ReadRedirectors();
            if (!redirectors.TryGetValue(relative, out AssetRedirectorEntry? redirect))
            {
                resolvedFullPath = "";
                return false;
            }
            if (!_database.TryGetByAssetId(redirect.AssetId, out AssetRecord? current) ||
                current == null)
            {
                resolvedFullPath = "";
                return false;
            }
            resolvedFullPath = current.FullPath;
            return true;
        }
    }

    internal IReadOnlyList<AssetRedirectorEntry> SnapshotRedirectors()
    {
        lock (AdvancedOperationGate)
        {
            return Array.AsReadOnly(ReadRedirectors().Values
                .OrderBy(static entry => entry.OriginalRelativePath, StringComparer.Ordinal)
                .ToArray());
        }
    }

    internal AssetRedirectorFixupPreview PreviewFixUpRedirectors()
    {
        lock (AdvancedOperationGate)
            return BuildRedirectorFixupPlan().Preview;
    }

    /// <summary>
    /// Removes only orphaned, shadowed, or redundant entries and repairs stale current-path
    /// diagnostics. Valid redirects are deliberately retained until callers no longer need them.
    /// </summary>
    internal AssetRedirectorFixupResult CommitFixUpRedirectors(
        AssetRedirectorFixupPreview preview)
    {
        ArgumentNullException.ThrowIfNull(preview);
        lock (AdvancedOperationGate)
        {
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Clean redirector registry");
            RefreshAuthoritativeState();
            AdvancedRedirectorFixupPlan plan = BuildRedirectorFixupPlan();
            if (!string.Equals(
                    plan.Preview.PreviewToken,
                    preview.PreviewToken,
                    StringComparison.Ordinal))
            {
                throw new IOException(
                    "Redirectors changed after preview. Generate a new Fix Up preview.");
            }
            foreach (AssetRedirectorFixupItem item in plan.Preview.Items)
            {
                if (item.Action == AssetRedirectorFixupAction.UpdateCurrentPath)
                {
                    AssetRedirectorEntry existing = plan.Redirectors[item.OriginalRelativePath];
                    plan.Redirectors[item.OriginalRelativePath] = existing with
                    {
                        CurrentRelativePath = item.CurrentRelativePath,
                        UpdatedUtcTicks = DateTime.UtcNow.Ticks,
                    };
                }
                else
                {
                    plan.Redirectors.Remove(item.OriginalRelativePath);
                }
            }
            if (plan.Preview.Items.Count != 0)
                WriteRedirectors(plan.Redirectors.Values);
            return new AssetRedirectorFixupResult(
                plan.Preview.Items.Count(item =>
                    item.Action != AssetRedirectorFixupAction.UpdateCurrentPath),
                plan.Preview.Items.Count(item =>
                    item.Action == AssetRedirectorFixupAction.UpdateCurrentPath),
                plan.Redirectors.Count);
        }
    }

    internal AssetReplaceReferencesPreview PreviewReplaceReferences(
        IEnumerable<string> sourceAssetIds,
        string targetAssetId)
    {
        lock (AdvancedOperationGate)
            return BuildReplacePlan(sourceAssetIds, targetAssetId).Preview;
    }

    /// <summary>
    /// Commits exactly the previewed consolidation plan. Any intervening relevant file or metadata
    /// change invalidates the token; partial writes are restored byte-for-byte.
    /// </summary>
    internal AssetReplaceReferencesResult CommitReplaceReferences(
        AssetReplaceReferencesPreview preview)
    {
        ArgumentNullException.ThrowIfNull(preview);
        lock (AdvancedOperationGate)
        {
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Replace asset references");
            // Refresh while the same cross-process lease that protects the commit is held.
            // A refresh performed by the UI before entering this method leaves a lock gap in
            // which another editor can publish new sidecar metadata. Rebuilding from that stale
            // in-memory snapshot could otherwise preserve the old preview token and overwrite
            // the other editor's update.
            RefreshAuthoritativeState();
            AdvancedReplacePlan plan = BuildReplacePlan(
                preview.SourceAssetIds,
                preview.TargetAssetId);
            if (!string.Equals(
                    plan.Preview.PreviewToken,
                    preview.PreviewToken,
                    StringComparison.Ordinal))
            {
                throw new IOException(
                    "Asset references changed after preview. Generate a new preview before commit.");
            }

            var backups = new Dictionary<string, FileRewriteBackup>(PathComparer);
            long backupBytes = 0;
            foreach (AdvancedTextRewrite planItem in plan.TextRewrites)
            {
                RegisterRewriteBackup(planItem.Backup, backups, ref backupBytes);
            }
            foreach (AdvancedMetadataRewrite metadata in plan.MetadataRewrites)
            {
                RegisterRewriteBackup(
                    ReadRewriteBackup(
                        metadata.Original.FullPath + AssetDatabase.MetadataSuffix),
                    backups,
                    ref backupBytes);
            }
            foreach (FileRewriteBackup backup in backups.Values)
                EnsureRewriteBackupUnchanged(backup);

            try
            {
                foreach (AdvancedTextRewrite rewrite in plan.TextRewrites)
                {
                    WriteRewriteBytesAtomically(
                        rewrite.Backup,
                        rewrite.UpdatedBytes,
                        restoreOriginalTimestamp: false);
                }
                foreach (AdvancedMetadataRewrite rewrite in plan.MetadataRewrites)
                {
                    _database.UpdateImportMetadata(
                        rewrite.Original.AssetId,
                        rewrite.UpdatedSource,
                        rewrite.Original.Metadata.Importer,
                        rewrite.Original.Metadata.ImporterVersion,
                        rewrite.UpdatedDependencies,
                        rewrite.UpdatedSettings);
                }
                _database.Refresh(verifyContent: true);
                return new AssetReplaceReferencesResult(
                    plan.TextRewrites.Count,
                    plan.MetadataRewrites.Count,
                    plan.Preview.Edits.Sum(static edit => edit.ReplacementCount));
            }
            catch (Exception error)
            {
                bool rollbackComplete = RestoreRewriteBackups(backups.Values);
                TryRefresh();
                if (!rollbackComplete)
                {
                    throw new IOException(
                        "Replace References failed and byte-for-byte rollback was incomplete.",
                        error);
                }
                throw;
            }
        }
    }

    internal AssetMigrationPreview PreviewMigrate(
        IEnumerable<string> rootAssetIds,
        string targetProjectRoot)
    {
        lock (AdvancedOperationGate)
            return BuildMigrationPlan(rootAssetIds, targetProjectRoot).Preview;
    }

    /// <summary>
    /// Copies a metadata-authoritative dependency closure to another project's Assets directory.
    /// GUIDs and relative paths are retained. No destination is overwritten.
    /// </summary>
    internal AssetMigrationResult CommitMigrate(AssetMigrationPreview preview)
    {
        ArgumentNullException.ThrowIfNull(preview);
        lock (AdvancedOperationGate)
        {
            // Keep the established in-process -> cross-process lock order. Both roots remain
            // leased while the plan is rebuilt, copied, published, verified, and (if needed)
            // rolled back. Canonical ordering prevents opposing A->B and B->A migrations from
            // acquiring the two process-local project gates in different orders.
            string targetProject = ValidateMigrationProject(preview.TargetProjectRoot);
            string targetAssets = NormalizeDirectory(Path.Combine(targetProject, "Assets"));
            string[] lockRoots = new[] { _assetsRoot, targetAssets }
                .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
                .ThenBy(static path => path, StringComparer.Ordinal)
                .ToArray();
            using AssetMutationLock firstMutationLock = AssetMutationLock.Acquire(
                lockRoots[0],
                "Migrate assets");
            using AssetMutationLock secondMutationLock = AssetMutationLock.Acquire(
                lockRoots[1],
                "Migrate assets");
            RefreshAuthoritativeState();
            AdvancedMigrationPlan plan = BuildMigrationPlan(
                preview.AssetIds,
                targetProject,
                rootsAreClosure: true);
            if (!string.Equals(
                    plan.Preview.PreviewToken,
                    preview.PreviewToken,
                    StringComparison.Ordinal))
            {
                throw new IOException(
                    "Migration sources or destination changed after preview. Generate a new preview.");
            }
            return PublishMigration(plan);
        }
    }

    private IReadOnlyList<(AssetRecord Original, AssetRecord Copy)> CurrentPairs(
        IEnumerable<AssetRecord> originals)
    {
        var pairs = new List<(AssetRecord Original, AssetRecord Copy)>();
        foreach (AssetRecord original in originals)
        {
            if (!_database.TryGetByAssetId(original.AssetId, out AssetRecord? current) ||
                current == null)
            {
                throw new IOException(
                    $"Moved asset identity no longer resolves: {original.RelativePath}");
            }
            pairs.Add((original, current));
        }
        return pairs;
    }

    private void RegisterRedirectors(
        IReadOnlyList<(AssetRecord Original, AssetRecord Copy)> pairs)
    {
        if (pairs.Count == 0) return;
        Dictionary<string, AssetRedirectorEntry> redirectors = new(
            ReadRedirectors(),
            StringComparer.OrdinalIgnoreCase);
        long now = DateTime.UtcNow.Ticks;
        foreach ((AssetRecord original, AssetRecord current) in pairs)
        {
            if (PathComparer.Equals(original.RelativePath, current.RelativePath))
                continue;
            if (_database.TryGetByPath(original.RelativePath, out AssetRecord? occupant) &&
                occupant != null &&
                !string.Equals(
                    occupant.AssetId,
                    original.AssetId,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException(
                    $"The old asset path has already been reused: Assets/{original.RelativePath}");
            }
            redirectors[NormalizeRedirectorRelativePath(original.RelativePath)] =
                new AssetRedirectorEntry(
                    NormalizeRedirectorRelativePath(original.RelativePath),
                    current.AssetId,
                    NormalizeRedirectorRelativePath(current.RelativePath),
                    now);
        }

        foreach (string key in redirectors.Keys.ToArray())
        {
            AssetRedirectorEntry entry = redirectors[key];
            if (!_database.TryGetByAssetId(entry.AssetId, out AssetRecord? current) ||
                current == null)
            {
                redirectors.Remove(key);
                continue;
            }
            string currentRelative = NormalizeRedirectorRelativePath(current.RelativePath);
            if (PathComparer.Equals(key, currentRelative))
            {
                redirectors.Remove(key);
                continue;
            }
            redirectors[key] = entry with { CurrentRelativePath = currentRelative };
        }
        WriteRedirectors(redirectors.Values);
    }

    private AdvancedRedirectorFixupPlan BuildRedirectorFixupPlan()
    {
        var redirectors = new Dictionary<string, AssetRedirectorEntry>(
            ReadRedirectors(),
            StringComparer.OrdinalIgnoreCase);
        var items = new List<AssetRedirectorFixupItem>();
        foreach (AssetRedirectorEntry entry in redirectors.Values
                     .OrderBy(static item => item.OriginalRelativePath, StringComparer.Ordinal))
        {
            if (!_database.TryGetByAssetId(entry.AssetId, out AssetRecord? current) ||
                current == null)
            {
                items.Add(new AssetRedirectorFixupItem(
                    entry.OriginalRelativePath,
                    entry.AssetId,
                    entry.CurrentRelativePath,
                    AssetRedirectorFixupAction.RemoveMissingTarget));
                continue;
            }
            if (_database.TryGetByPath(
                    entry.OriginalRelativePath,
                    out AssetRecord? occupant) &&
                occupant != null)
            {
                items.Add(new AssetRedirectorFixupItem(
                    entry.OriginalRelativePath,
                    entry.AssetId,
                    current.RelativePath,
                    string.Equals(
                        occupant.AssetId,
                        entry.AssetId,
                        StringComparison.OrdinalIgnoreCase)
                        ? AssetRedirectorFixupAction.RemoveRedundantPath
                        : AssetRedirectorFixupAction.RemoveShadowedPath));
                continue;
            }
            if (PathComparer.Equals(entry.OriginalRelativePath, current.RelativePath))
            {
                items.Add(new AssetRedirectorFixupItem(
                    entry.OriginalRelativePath,
                    entry.AssetId,
                    current.RelativePath,
                    AssetRedirectorFixupAction.RemoveRedundantPath));
                continue;
            }
            if (!PathComparer.Equals(entry.CurrentRelativePath, current.RelativePath))
            {
                items.Add(new AssetRedirectorFixupItem(
                    entry.OriginalRelativePath,
                    entry.AssetId,
                    current.RelativePath,
                    AssetRedirectorFixupAction.UpdateCurrentPath));
            }
        }

        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        AppendHash(hash, "redirector-fixup-v1");
        foreach (AssetRedirectorEntry entry in redirectors.Values
                     .OrderBy(static item => item.OriginalRelativePath, StringComparer.Ordinal))
        {
            AppendHash(hash, entry.OriginalRelativePath);
            AppendHash(hash, entry.AssetId);
            AppendHash(hash, entry.CurrentRelativePath);
            AppendHash(hash, entry.UpdatedUtcTicks.ToString(CultureInfo.InvariantCulture));
            if (_database.TryGetByAssetId(entry.AssetId, out AssetRecord? current) &&
                current != null)
            {
                AppendHash(hash, current.RelativePath);
            }
            if (_database.TryGetByPath(entry.OriginalRelativePath, out AssetRecord? occupant) &&
                occupant != null)
            {
                AppendHash(hash, occupant.AssetId);
            }
        }
        var preview = new AssetRedirectorFixupPreview(
            Array.AsReadOnly(items.ToArray()),
            Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant());
        return new AdvancedRedirectorFixupPlan(preview, redirectors);
    }

    private IReadOnlyDictionary<string, AssetRedirectorEntry> ReadRedirectors()
    {
        var result = new Dictionary<string, AssetRedirectorEntry>(
            StringComparer.OrdinalIgnoreCase);
        string path = RedirectorPath;
        if (!File.Exists(path))
            return new ReadOnlyDictionary<string, AssetRedirectorEntry>(result);
        EnsureNoReparseParents(path);
        EnsureNotReparse(path, expectDirectory: false);
        var info = new FileInfo(path);
        if (info.Length > MaxRedirectorBytes)
            throw new InvalidDataException("Asset redirector registry exceeds 1 MiB.");
        using JsonDocument document = JsonDocument.Parse(
            File.ReadAllBytes(path),
            StrictRedirectorJson);
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("schemaVersion", out JsonElement schema) ||
            schema.ValueKind != JsonValueKind.Number ||
            schema.GetInt32() != RedirectorSchemaVersion ||
            !root.TryGetProperty("redirectors", out JsonElement items) ||
            items.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidDataException("Asset redirector registry has an invalid schema.");
        }
        foreach (JsonElement item in items.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException("Asset redirector entry must be an object.");
            string original = NormalizeRedirectorRelativePath(
                RequiredRedirectorString(item, "originalPath"));
            string assetId = NormalizeRedirectorAssetId(
                RequiredRedirectorString(item, "assetId"));
            string current = NormalizeRedirectorRelativePath(
                RequiredRedirectorString(item, "currentPath"));
            long ticks = item.TryGetProperty("updatedUtcTicks", out JsonElement updated) &&
                         updated.ValueKind == JsonValueKind.Number &&
                         updated.TryGetInt64(out long value)
                ? value
                : throw new InvalidDataException(
                    "Asset redirector updatedUtcTicks is invalid.");
            if (!result.TryAdd(
                    original,
                    new AssetRedirectorEntry(original, assetId, current, ticks)))
            {
                throw new InvalidDataException(
                    $"Duplicate asset redirector path: {original}");
            }
        }
        return new ReadOnlyDictionary<string, AssetRedirectorEntry>(result);
    }

    private void WriteRedirectors(IEnumerable<AssetRedirectorEntry> entries)
    {
        string directory = Path.GetDirectoryName(RedirectorPath)
            ?? throw new InvalidDataException("Redirector registry has no parent.");
        Directory.CreateDirectory(directory);
        EnsureNoReparseParents(directory);
        EnsureOrdinaryDirectory(directory, "Asset database directory");

        using var memory = new MemoryStream();
        using (var writer = new Utf8JsonWriter(
                   memory,
                   new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", RedirectorSchemaVersion);
            writer.WriteStartArray("redirectors");
            foreach (AssetRedirectorEntry entry in entries
                         .OrderBy(static item => item.OriginalRelativePath, StringComparer.Ordinal))
            {
                writer.WriteStartObject();
                writer.WriteString("originalPath", entry.OriginalRelativePath);
                writer.WriteString("assetId", entry.AssetId);
                writer.WriteString("currentPath", entry.CurrentRelativePath);
                writer.WriteNumber("updatedUtcTicks", entry.UpdatedUtcTicks);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        memory.WriteByte((byte)'\n');
        if (memory.Length > MaxRedirectorBytes)
            throw new IOException("Asset redirector registry exceeds 1 MiB.");
        string temporary = RedirectorPath + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       16 * 1024,
                       FileOptions.WriteThrough))
            {
                memory.Position = 0;
                memory.CopyTo(stream);
                stream.Flush(flushToDisk: true);
            }
            EnsureNotReparse(temporary, expectDirectory: false);
            if (File.Exists(RedirectorPath))
                EnsureNotReparse(RedirectorPath, expectDirectory: false);
            File.Move(temporary, RedirectorPath, overwrite: true);
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    private string NormalizeRedirectorRelativePath(string path)
    {
        string candidate = (path ?? "").Trim();
        if (candidate.StartsWith("Assets/", StringComparison.OrdinalIgnoreCase) ||
            candidate.StartsWith("Assets\\", StringComparison.OrdinalIgnoreCase))
        {
            candidate = candidate[7..];
        }
        string full = Path.IsPathRooted(candidate)
            ? Path.GetFullPath(candidate)
            : Path.GetFullPath(
                Path.Combine(_assetsRoot, candidate.Replace('/', Path.DirectorySeparatorChar)));
        if (!IsUnder(full, _assetsRoot))
            throw new InvalidDataException("Asset redirector path escapes Assets.");
        string relative = Path.GetRelativePath(_assetsRoot, full).Replace('\\', '/');
        if (relative.Length == 0 ||
            relative.Equals(AssetDatabase.InternalDirectoryName, StringComparison.OrdinalIgnoreCase) ||
            relative.StartsWith(
                AssetDatabase.InternalDirectoryName + "/",
                StringComparison.OrdinalIgnoreCase) ||
            relative.EndsWith(AssetDatabase.MetadataSuffix, StringComparison.OrdinalIgnoreCase) ||
            IsMaterialGraphCompanionPath(relative) ||
            AssetCreationWorkflow.IsTemporaryPath(relative))
        {
            throw new InvalidDataException("Asset redirector path is reserved.");
        }
        return relative;
    }

    private static string RequiredRedirectorString(JsonElement item, string name)
    {
        if (!item.TryGetProperty(name, out JsonElement property) ||
            property.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException($"Asset redirector '{name}' is invalid.");
        }
        return property.GetString() ?? "";
    }

    private static string NormalizeRedirectorAssetId(string assetId)
    {
        string value = (assetId ?? "").Trim().ToLowerInvariant();
        if (value.Length != 32 || value.Any(static character => !Uri.IsHexDigit(character)))
            throw new InvalidDataException("Asset redirector GUID is invalid.");
        return value;
    }

    private AdvancedReplacePlan BuildReplacePlan(
        IEnumerable<string> sourceAssetIds,
        string targetAssetId)
    {
        ArgumentNullException.ThrowIfNull(sourceAssetIds);
        if (!_database.TryGetByAssetId(targetAssetId, out AssetRecord? target) ||
            target == null)
        {
            throw new InvalidDataException("Replace References target asset does not exist.");
        }
        AssetRecord[] sources = sourceAssetIds
            .Where(static id => !string.IsNullOrWhiteSpace(id))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Select(id => _database.TryGetByAssetId(id, out AssetRecord? source)
                ? source
                : null)
            .Select(source => source ??
                throw new InvalidDataException(
                    "A Replace References source asset does not exist."))
            .OrderBy(static source => source.AssetId, StringComparer.Ordinal)
            .ToArray();
        if (sources.Length == 0)
            throw new InvalidDataException("At least one source asset is required.");
        if (sources.Any(source => string.Equals(
                source.AssetId,
                target.AssetId,
                StringComparison.OrdinalIgnoreCase)))
        {
            throw new InvalidDataException(
                "Replace References target cannot also be a source.");
        }
        if (sources.Any(source => !string.Equals(
                source.Kind,
                target.Kind,
                StringComparison.OrdinalIgnoreCase)))
        {
            throw new InvalidDataException(
                "Replace References requires assets of the same kind.");
        }

        IReadOnlyList<ReferenceReplacement> replacements = BuildReferenceReplacements(
            sources.Select(source => (Original: source, Copy: target)).ToArray());
        var textRewrites = new List<AdvancedTextRewrite>();
        var metadataRewrites = new List<AdvancedMetadataRewrite>();
        var edits = new Dictionary<string, MutableReplaceEdit>(PathComparer);

        foreach (string path in EnumeratePhysicalReferenceTextPaths())
        {
            var info = new FileInfo(path);
            if (info.Length > MaxReferenceScanBytes)
            {
                throw new IOException(
                    $"Text asset is too large to replace references safely: {DisplayPath(path)}");
            }
            FileRewriteBackup backup = ReadRewriteBackup(path);
            string original = DecodeUtf8Text(
                backup.OriginalBytes,
                $"Text asset is not valid UTF-8: {DisplayPath(path)}",
                out bool hasBom);
            string updated = ReplaceReferenceTokens(original, replacements);
            if (string.Equals(original, updated, StringComparison.Ordinal))
                continue;
            int count = CountReferenceReplacements(original, replacements);
            textRewrites.Add(new AdvancedTextRewrite(
                backup,
                EncodeUtf8Text(updated, hasBom),
                count));
            string relative = Path.GetRelativePath(_assetsRoot, path).Replace('\\', '/');
            MutableReplaceEdit edit = edits.GetValueOrDefault(relative) ??
                new MutableReplaceEdit(relative);
            edit.ContentChanged = true;
            edit.ReplacementCount += count;
            edits[relative] = edit;
        }

        foreach (AssetRecord record in _database.Snapshot())
        {
            string[] dependencies = record.Metadata.Dependencies
                .Select(id => sources.Any(source => string.Equals(
                        source.AssetId,
                        id,
                        StringComparison.OrdinalIgnoreCase))
                    ? target.AssetId
                    : id)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToArray();
            string sourceText = ReplaceReferenceTokens(
                record.Metadata.Source,
                replacements);
            KeyValuePair<string, string>[] settings = record.Metadata.ImportSettings
                .Select(setting => new KeyValuePair<string, string>(
                    setting.Key,
                    ReplaceReferenceTokens(setting.Value, replacements)))
                .ToArray();
            int count = CountReferenceReplacements(record.Metadata.Source, replacements) +
                        record.Metadata.ImportSettings.Values.Sum(
                            value => CountReferenceReplacements(value, replacements)) +
                        record.Metadata.Dependencies.Count(id => sources.Any(
                            source => string.Equals(
                                source.AssetId,
                                id,
                                StringComparison.OrdinalIgnoreCase)));
            bool changed = !dependencies.SequenceEqual(
                               record.Metadata.Dependencies,
                               StringComparer.OrdinalIgnoreCase) ||
                           !string.Equals(
                               sourceText,
                               record.Metadata.Source,
                               StringComparison.Ordinal) ||
                           settings.Any(setting => !string.Equals(
                               setting.Value,
                               record.Metadata.ImportSettings[setting.Key],
                               StringComparison.Ordinal));
            if (!changed) continue;
            metadataRewrites.Add(new AdvancedMetadataRewrite(
                record,
                sourceText,
                dependencies,
                settings,
                count));
            MutableReplaceEdit edit = edits.GetValueOrDefault(record.RelativePath) ??
                new MutableReplaceEdit(record.RelativePath);
            edit.MetadataChanged = true;
            edit.ReplacementCount += count;
            edits[record.RelativePath] = edit;
        }

        string token = ComputeReplacePreviewToken(
            sources,
            target,
            textRewrites,
            metadataRewrites);
        var preview = new AssetReplaceReferencesPreview(
            target.AssetId,
            Array.AsReadOnly(sources.Select(static source => source.AssetId).ToArray()),
            Array.AsReadOnly(edits.Values
                .OrderBy(static edit => edit.RelativePath, StringComparer.Ordinal)
                .Select(static edit => new AssetReplaceReferenceEdit(
                    edit.RelativePath,
                    edit.ContentChanged,
                    edit.MetadataChanged,
                    edit.ReplacementCount))
                .ToArray()),
            token);
        return new AdvancedReplacePlan(
            preview,
            Array.AsReadOnly(textRewrites.ToArray()),
            Array.AsReadOnly(metadataRewrites.ToArray()));
    }

    private IEnumerable<string> EnumeratePhysicalReferenceTextPaths()
    {
        var pending = new Stack<string>();
        pending.Push(_assetsRoot);
        while (pending.Count != 0)
        {
            string directory = pending.Pop();
            EnsureNotReparse(directory, expectDirectory: true);
            foreach (FileSystemInfo entry in new DirectoryInfo(directory)
                         .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                entry.Refresh();
                bool isDirectory = (entry.Attributes & FileAttributes.Directory) != 0;
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException(
                        "Replace References encountered an unsupported reparse point.");
                }
                if (isDirectory)
                {
                    if (!entry.Name.Equals(
                            AssetDatabase.InternalDirectoryName,
                            StringComparison.OrdinalIgnoreCase))
                    {
                        pending.Push(entry.FullName);
                    }
                    continue;
                }
                if (AssetCreationWorkflow.IsTemporaryPath(entry.FullName))
                    continue;
                if (entry.Name.EndsWith(
                        AssetDatabase.MetadataSuffix,
                        StringComparison.OrdinalIgnoreCase))
                {
                    if (IsMaterialGraphMetadataPath(entry.FullName))
                        yield return entry.FullName;
                    continue;
                }
                if (TextReferenceExtensions.Contains(Path.GetExtension(entry.FullName)))
                    yield return entry.FullName;
            }
        }
    }

    private static int CountReferenceReplacements(
        string text,
        IReadOnlyList<ReferenceReplacement> replacements)
    {
        int count = 0;
        foreach (ReferenceReplacement replacement in replacements)
        {
            int search = 0;
            while (search <= text.Length - replacement.Source.Length)
            {
                int index = text.IndexOf(
                    replacement.Source,
                    search,
                    StringComparison.OrdinalIgnoreCase);
                if (index < 0) break;
                if (HasReferenceBoundaries(
                        text,
                        index,
                        replacement.Source.Length,
                        replacement.IsAssetId))
                {
                    count++;
                    search = index + replacement.Source.Length;
                }
                else
                {
                    search = index + 1;
                }
            }
        }
        return count;
    }

    private static string ComputeReplacePreviewToken(
        IReadOnlyList<AssetRecord> sources,
        AssetRecord target,
        IReadOnlyList<AdvancedTextRewrite> textRewrites,
        IReadOnlyList<AdvancedMetadataRewrite> metadataRewrites)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        AppendHash(hash, "replace-references-v1");
        AppendHash(hash, target.AssetId);
        AppendHash(hash, target.RelativePath);
        foreach (AssetRecord source in sources)
        {
            AppendHash(hash, source.AssetId);
            AppendHash(hash, source.RelativePath);
        }
        foreach (AdvancedTextRewrite rewrite in textRewrites
                     .OrderBy(static item => item.Backup.FullPath, PathComparer))
        {
            AppendHash(hash, rewrite.Backup.FullPath);
            hash.AppendData(SHA256.HashData(rewrite.Backup.OriginalBytes));
        }
        foreach (AdvancedMetadataRewrite rewrite in metadataRewrites
                     .OrderBy(static item => item.Original.AssetId, StringComparer.Ordinal))
        {
            AppendHash(hash, rewrite.Original.AssetId);
            AppendHash(hash, rewrite.Original.RelativePath);
            AppendHash(hash, rewrite.Original.Metadata.Source);
            foreach (string dependency in rewrite.Original.Metadata.Dependencies)
                AppendHash(hash, dependency);
            foreach (KeyValuePair<string, string> setting in
                     rewrite.Original.Metadata.ImportSettings.OrderBy(
                         static item => item.Key,
                         StringComparer.Ordinal))
            {
                AppendHash(hash, setting.Key);
                AppendHash(hash, setting.Value);
            }
        }
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    private AdvancedMigrationPlan BuildMigrationPlan(
        IEnumerable<string> rootAssetIds,
        string targetProjectRoot,
        bool rootsAreClosure = false)
    {
        ArgumentNullException.ThrowIfNull(rootAssetIds);
        string targetProject = ValidateMigrationProject(targetProjectRoot);
        string targetAssets = NormalizeDirectory(Path.Combine(targetProject, "Assets"));
        if (IsUnderOrEqual(targetAssets, _assetsRoot) ||
            IsUnderOrEqual(_assetsRoot, targetAssets))
        {
            throw new InvalidDataException(
                "Source and destination Assets directories must not overlap.");
        }

        Dictionary<string, AssetRecord> byId = _database.Snapshot().ToDictionary(
            static record => record.AssetId,
            StringComparer.OrdinalIgnoreCase);
        string[] roots = rootAssetIds
            .Where(static id => !string.IsNullOrWhiteSpace(id))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(static id => id, StringComparer.Ordinal)
            .ToArray();
        if (roots.Length == 0)
            throw new InvalidDataException("At least one migration root asset is required.");

        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pending = new Queue<string>(roots);
        while (pending.Count != 0)
        {
            string id = pending.Dequeue();
            if (!visited.Add(id)) continue;
            if (visited.Count > MaxMigrationAssets)
                throw new InvalidDataException("Migration dependency closure exceeds 65536 assets.");
            if (!byId.TryGetValue(id, out AssetRecord? record))
            {
                throw new InvalidDataException(
                    $"Migration dependency GUID does not resolve: {id}");
            }
            foreach (string dependency in record.Metadata.Dependencies)
            {
                if (!byId.ContainsKey(dependency))
                {
                    throw new InvalidDataException(
                        $"'{record.RelativePath}' has a missing dependency GUID: {dependency}");
                }
                if (!visited.Contains(dependency))
                    pending.Enqueue(dependency);
            }
        }
        if (rootsAreClosure &&
            !visited.SetEquals(roots))
        {
            throw new InvalidDataException(
                "Migration preview no longer contains the complete dependency closure.");
        }

        AssetRecord[] records = visited
            .Select(id => byId[id])
            .OrderBy(static record => record.RelativePath, StringComparer.Ordinal)
            .ToArray();
        ValidateTargetIdentityCollisions(targetProject, targetAssets, records);

        var files = new List<AdvancedMigrationFile>();
        var destinations = new HashSet<string>(PathComparer);
        long totalBytes = 0;
        foreach (AssetRecord record in records)
        {
            string[] family = EnumerateExistingAssetFamilyFiles(record.FullPath).ToArray();
            if (!family.Contains(record.FullPath, PathComparer) ||
                !family.Contains(
                    record.FullPath + AssetDatabase.MetadataSuffix,
                    PathComparer))
            {
                throw new InvalidDataException(
                    $"Asset family is incomplete: {record.RelativePath}");
            }
            foreach (string source in family)
            {
                EnsureNoReparseParents(source);
                EnsureNotReparse(source, expectDirectory: false);
                string relative = Path.GetRelativePath(_assetsRoot, source).Replace('\\', '/');
                string destination = Path.GetFullPath(Path.Combine(
                    targetAssets,
                    relative.Replace('/', Path.DirectorySeparatorChar)));
                if (!IsUnder(destination, targetAssets) ||
                    !destinations.Add(destination))
                {
                    throw new InvalidDataException(
                        $"Migration destination is ambiguous or escapes Assets: {relative}");
                }
                if (File.Exists(destination) || Directory.Exists(destination))
                    throw new IOException($"Migration never overwrites: Assets/{relative}");
                var info = new FileInfo(source);
                totalBytes = SaturatingAdd(totalBytes, info.Length);
                if (totalBytes > MaxMigrationBytes)
                    throw new IOException("Migration exceeds the 16 GiB safety limit.");
                files.Add(new AdvancedMigrationFile(
                    source,
                    destination,
                    relative,
                    info.Length,
                    info.LastWriteTimeUtc.Ticks,
                    ComputeFileSha256(source)));
            }
        }

        string token = ComputeMigrationPreviewToken(
            targetProject,
            records,
            files);
        var preview = new AssetMigrationPreview(
            targetProject,
            Array.AsReadOnly(records.Select(static record => record.AssetId).ToArray()),
            Array.AsReadOnly(files
                .Select(static file => new AssetMigrationFile(
                    file.RelativePath,
                    file.Length))
                .ToArray()),
            totalBytes,
            token);
        return new AdvancedMigrationPlan(
            preview,
            targetAssets,
            Array.AsReadOnly(records),
            Array.AsReadOnly(files.ToArray()));
    }

    private string ValidateMigrationProject(string targetProjectRoot)
    {
        if (string.IsNullOrWhiteSpace(targetProjectRoot))
            throw new ArgumentException("Target project root is required.", nameof(targetProjectRoot));
        string project = NormalizeDirectory(targetProjectRoot);
        if (!Directory.Exists(project))
            throw new DirectoryNotFoundException("Target project root does not exist.");
        EnsureExternalOrdinaryDirectory(project, "Target project root");
        string assets = Path.Combine(project, "Assets");
        if (!Directory.Exists(assets))
            throw new DirectoryNotFoundException("Target project Assets directory does not exist.");
        EnsureExternalOrdinaryDirectory(assets, "Target Assets root");
        return project;
    }

    private static void EnsureExternalOrdinaryDirectory(string path, string label)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException($"{label} must be an ordinary directory.");
        }
    }

    private static void ValidateTargetIdentityCollisions(
        string targetProject,
        string targetAssets,
        IReadOnlyList<AssetRecord> records)
    {
        var targetDatabase = new AssetDatabase(targetProject, targetAssets);
        targetDatabase.RefreshForCook();
        HashSet<string> migrating = records
            .Select(static record => record.AssetId)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        AssetRecord? collision = targetDatabase.Snapshot()
            .FirstOrDefault(record => migrating.Contains(record.AssetId));
        if (collision != null)
        {
            throw new IOException(
                $"Target project already contains migrating GUID '{collision.AssetId}' " +
                $"at Assets/{collision.RelativePath}.");
        }
    }

    private AssetMigrationResult PublishMigration(AdvancedMigrationPlan plan)
    {
        string databaseRoot = Path.Combine(
            plan.TargetAssetsRoot,
            AssetDatabase.InternalDirectoryName);
        Directory.CreateDirectory(databaseRoot);
        EnsureExternalOrdinaryDirectory(databaseRoot, "Target asset database directory");
        string operationsRoot = Path.Combine(databaseRoot, "operations");
        Directory.CreateDirectory(operationsRoot);
        EnsureExternalOrdinaryDirectory(operationsRoot, "Target operation directory");
        string transaction = Path.Combine(
            operationsRoot,
            "migrate-" + Guid.NewGuid().ToString("N"));
        string payload = Path.Combine(transaction, "payload");
        Directory.CreateDirectory(payload);
        EnsureExternalOrdinaryDirectory(transaction, "Migration transaction directory");
        EnsureExternalOrdinaryDirectory(payload, "Migration payload directory");

        var published = new List<PublishedMigrationFile>();
        var createdDirectories = new HashSet<string>(PathComparer);
        bool preserveTransaction = false;
        try
        {
            IReadOnlyList<ReferenceReplacement> replacements =
                BuildReferenceReplacements(plan.Records
                    .Select(record => (
                        Original: record,
                        Copy: record with
                        {
                            FullPath = Path.Combine(
                                plan.TargetAssetsRoot,
                                record.RelativePath.Replace(
                                    '/',
                                    Path.DirectorySeparatorChar)),
                        }))
                    .ToArray());

            foreach (AdvancedMigrationFile file in plan.Files)
            {
                RevalidateMigrationSource(file);
                string staged = Path.Combine(
                    payload,
                    file.RelativePath.Replace('/', Path.DirectorySeparatorChar));
                string parent = Path.GetDirectoryName(staged)
                    ?? throw new InvalidDataException("Migration payload has no parent.");
                CreateOrdinaryDirectories(payload, parent, createdDirectories: null);
                File.Copy(file.SourcePath, staged, overwrite: false);
                EnsureNotReparse(staged, expectDirectory: false);
                if (new FileInfo(staged).Length != file.Length ||
                    !string.Equals(
                        ComputeFileSha256(staged),
                        file.Sha256,
                        StringComparison.Ordinal))
                {
                    throw new IOException(
                        $"Migration source changed while copying: {file.RelativePath}");
                }
                RewriteMigratedStagedText(staged, file.RelativePath, replacements);
            }

            foreach (AdvancedMigrationFile file in plan.Files
                         .OrderBy(static item => item.RelativePath, StringComparer.Ordinal))
            {
                string staged = Path.Combine(
                    payload,
                    file.RelativePath.Replace('/', Path.DirectorySeparatorChar));
                string destination = file.DestinationPath;
                if (File.Exists(destination) || Directory.Exists(destination))
                    throw new IOException($"Migration never overwrites: Assets/{file.RelativePath}");
                string parent = Path.GetDirectoryName(destination)
                    ?? throw new InvalidDataException("Migration destination has no parent.");
                CreateOrdinaryDirectories(
                    plan.TargetAssetsRoot,
                    parent,
                    createdDirectories);
                File.Move(staged, destination);
                published.Add(new PublishedMigrationFile(staged, destination));
            }

            var targetDatabase = new AssetDatabase(
                plan.Preview.TargetProjectRoot,
                plan.TargetAssetsRoot);
            AssetDatabaseRefreshResult refresh = targetDatabase.RefreshForCook();
            foreach (AssetRecord source in plan.Records)
            {
                if (!targetDatabase.TryGetByAssetId(source.AssetId, out AssetRecord? migrated) ||
                    migrated == null ||
                    !PathComparer.Equals(migrated.RelativePath, source.RelativePath) ||
                    !migrated.Metadata.Dependencies.SequenceEqual(
                        source.Metadata.Dependencies,
                        StringComparer.OrdinalIgnoreCase))
                {
                    throw new IOException(
                        $"Migrated asset failed identity verification: {source.RelativePath}. " +
                        string.Join(" ", refresh.Warnings));
                }
            }
            return new AssetMigrationResult(
                plan.TargetAssetsRoot,
                plan.Records.Count,
                plan.Files.Count,
                plan.Preview.TotalBytes);
        }
        catch (Exception error)
        {
            bool rollbackComplete = RollbackMigration(published, createdDirectories);
            if (!rollbackComplete)
            {
                preserveTransaction = true;
                throw new IOException(
                    "Migration failed and rollback was incomplete. Recoverable files remain at " +
                    $"'{transaction}'.",
                    error);
            }
            throw;
        }
        finally
        {
            if (!preserveTransaction)
                TryDeleteOrdinaryDirectoryTree(transaction);
        }
    }

    private void RewriteMigratedStagedText(
        string stagedPath,
        string relativePath,
        IReadOnlyList<ReferenceReplacement> replacements)
    {
        bool isMetadata = relativePath.EndsWith(
            AssetDatabase.MetadataSuffix,
            StringComparison.OrdinalIgnoreCase);
        string assetRelative = isMetadata
            ? relativePath[..^AssetDatabase.MetadataSuffix.Length]
            : relativePath;
        bool isText = isMetadata ||
                      TextReferenceExtensions.Contains(Path.GetExtension(assetRelative));
        if (!isText) return;
        byte[] originalBytes = File.ReadAllBytes(stagedPath);
        string original = DecodeUtf8Text(
            originalBytes,
            $"Migrated text asset is not valid UTF-8: Assets/{relativePath}",
            out bool hasBom);
        string updated = ReplaceReferenceTokens(original, replacements);
        if (ContainsSourceAbsolutePath(updated))
        {
            throw new InvalidDataException(
                $"Migration closure leaves a source-project absolute reference in " +
                $"Assets/{relativePath}. Add the referenced asset as a dependency.");
        }
        if (string.Equals(original, updated, StringComparison.Ordinal))
            return;
        File.WriteAllBytes(stagedPath, EncodeUtf8Text(updated, hasBom));
    }

    private bool ContainsSourceAbsolutePath(string text)
    {
        string native = Path.GetFullPath(_assetsRoot);
        string slash = native.Replace('\\', '/');
        return text.Contains(native, StringComparison.OrdinalIgnoreCase) ||
               text.Contains(slash, StringComparison.OrdinalIgnoreCase) ||
               text.Contains(EscapeBackslashes(native), StringComparison.OrdinalIgnoreCase);
    }

    private void RevalidateMigrationSource(AdvancedMigrationFile file)
    {
        EnsureNoReparseParents(file.SourcePath);
        EnsureNotReparse(file.SourcePath, expectDirectory: false);
        var info = new FileInfo(file.SourcePath);
        if (info.Length != file.Length ||
            info.LastWriteTimeUtc.Ticks != file.LastWriteUtcTicks ||
            !string.Equals(
                ComputeFileSha256(file.SourcePath),
                file.Sha256,
                StringComparison.Ordinal))
        {
            throw new IOException(
                $"Migration source changed after preview: {file.RelativePath}");
        }
    }

    private static void CreateOrdinaryDirectories(
        string root,
        string destination,
        ISet<string>? createdDirectories)
    {
        string normalizedRoot = NormalizeDirectory(root);
        string normalizedDestination = NormalizeDirectory(destination);
        if (!IsUnderOrEqual(normalizedDestination, normalizedRoot))
            throw new InvalidDataException("Migration directory escapes its root.");
        EnsureExternalOrdinaryDirectory(normalizedRoot, "Migration root");
        string relative = Path.GetRelativePath(normalizedRoot, normalizedDestination);
        string cursor = normalizedRoot;
        if (relative == ".") return;
        foreach (string segment in relative.Split(
                     new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                     StringSplitOptions.RemoveEmptyEntries))
        {
            cursor = Path.Combine(cursor, segment);
            if (!Directory.Exists(cursor))
            {
                Directory.CreateDirectory(cursor);
                createdDirectories?.Add(cursor);
            }
            EnsureExternalOrdinaryDirectory(cursor, "Migration directory");
        }
    }

    private static bool RollbackMigration(
        IReadOnlyList<PublishedMigrationFile> published,
        ISet<string> createdDirectories)
    {
        bool complete = true;
        foreach (PublishedMigrationFile file in published.Reverse())
        {
            try
            {
                string parent = Path.GetDirectoryName(file.StagedPath)!;
                Directory.CreateDirectory(parent);
                if (File.Exists(file.DestinationPath) && !File.Exists(file.StagedPath))
                    File.Move(file.DestinationPath, file.StagedPath);
                complete &= !File.Exists(file.DestinationPath) && File.Exists(file.StagedPath);
            }
            catch
            {
                complete = false;
            }
        }
        foreach (string directory in createdDirectories.OrderByDescending(
                     static path => path.Length))
        {
            try
            {
                if (Directory.Exists(directory) &&
                    !Directory.EnumerateFileSystemEntries(directory).Any())
                {
                    Directory.Delete(directory, recursive: false);
                }
            }
            catch
            {
                complete = false;
            }
        }
        return complete;
    }

    private static string ComputeMigrationPreviewToken(
        string targetProject,
        IReadOnlyList<AssetRecord> records,
        IReadOnlyList<AdvancedMigrationFile> files)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        AppendHash(hash, "asset-migrate-v1");
        AppendHash(hash, targetProject);
        foreach (AssetRecord record in records)
        {
            AppendHash(hash, record.AssetId);
            AppendHash(hash, record.RelativePath);
            foreach (string dependency in record.Metadata.Dependencies)
                AppendHash(hash, dependency);
        }
        foreach (AdvancedMigrationFile file in files)
        {
            AppendHash(hash, file.RelativePath);
            AppendHash(hash, file.Length.ToString(CultureInfo.InvariantCulture));
            AppendHash(hash, file.LastWriteUtcTicks.ToString(CultureInfo.InvariantCulture));
            AppendHash(hash, file.Sha256);
        }
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    private static string ComputeFileSha256(string path)
    {
        using FileStream stream = new(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static void AppendHash(IncrementalHash hash, string value)
    {
        byte[] bytes = StrictUtf8.GetBytes(value ?? "");
        Span<byte> length = stackalloc byte[4];
        System.Buffers.Binary.BinaryPrimitives.WriteInt32LittleEndian(length, bytes.Length);
        hash.AppendData(length);
        hash.AppendData(bytes);
    }

    private sealed class MutableReplaceEdit
    {
        internal MutableReplaceEdit(string relativePath) => RelativePath = relativePath;

        internal string RelativePath { get; }
        internal bool ContentChanged { get; set; }
        internal bool MetadataChanged { get; set; }
        internal int ReplacementCount { get; set; }
    }

    private sealed record AdvancedTextRewrite(
        FileRewriteBackup Backup,
        byte[] UpdatedBytes,
        int ReplacementCount);

    private sealed record AdvancedMetadataRewrite(
        AssetRecord Original,
        string UpdatedSource,
        IReadOnlyList<string> UpdatedDependencies,
        IReadOnlyList<KeyValuePair<string, string>> UpdatedSettings,
        int ReplacementCount);

    private sealed record AdvancedReplacePlan(
        AssetReplaceReferencesPreview Preview,
        IReadOnlyList<AdvancedTextRewrite> TextRewrites,
        IReadOnlyList<AdvancedMetadataRewrite> MetadataRewrites);

    private sealed record AdvancedRedirectorFixupPlan(
        AssetRedirectorFixupPreview Preview,
        Dictionary<string, AssetRedirectorEntry> Redirectors);

    private sealed record AdvancedMigrationFile(
        string SourcePath,
        string DestinationPath,
        string RelativePath,
        long Length,
        long LastWriteUtcTicks,
        string Sha256);

    private sealed record AdvancedMigrationPlan(
        AssetMigrationPreview Preview,
        string TargetAssetsRoot,
        IReadOnlyList<AssetRecord> Records,
        IReadOnlyList<AdvancedMigrationFile> Files);

    private sealed record PublishedMigrationFile(
        string StagedPath,
        string DestinationPath);
}
