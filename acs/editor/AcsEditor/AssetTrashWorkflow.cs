// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace AcsEditor;

internal sealed record AssetTrashResult(
    string EntryId,
    int AssetCount,
    int FolderCount,
    long TotalBytes);

internal sealed record AssetTrashEntry(
    string EntryId,
    DateTimeOffset CreatedUtc,
    IReadOnlyList<string> OriginalRelativePaths,
    int AssetCount,
    int FolderCount,
    long TotalBytes,
    long StoredBytes);

internal sealed record AssetTrashRestoreInspection(
    AssetTrashEntry Entry,
    IReadOnlyList<string> Collisions)
{
    internal bool CanRestore => Collisions.Count == 0;
}

internal sealed record AssetTrashRestoreResult(
    string EntryId,
    IReadOnlyList<string> RestoredPaths,
    string? DeferredCleanupPath);

internal sealed record AssetTrashCleanupResult(
    int RemovedEntries,
    long ReclaimedBytes,
    IReadOnlyList<string> DeferredPaths,
    IReadOnlyList<string> PurgedOriginalRelativePaths);

internal sealed record AssetTrashRetentionPolicy(
    TimeSpan MaxAge,
    int MaxEntries,
    long MaxBytes)
{
    internal static AssetTrashRetentionPolicy Default { get; } =
        new(TimeSpan.FromDays(30), 256, 10L * 1024L * 1024L * 1024L);
}

internal sealed class AssetTrashCollisionException : IOException
{
    internal AssetTrashCollisionException(IReadOnlyList<string> collisions)
        : base("Trash entry cannot be restored because original paths are occupied.")
    {
        Collisions = collisions;
    }

    internal IReadOnlyList<string> Collisions { get; }
}

internal enum AssetTrashFaultPoint
{
    AfterTrashItemMoved,
    BeforeTrashRefresh,
    AfterRestoreItemMoved,
    BeforeRestoreRefresh,
}

/// <summary>
/// Project-local, transactional trash storage for Content Browser assets.
/// Published entries live below Assets/.acsdb/trash/entries and retain source
/// sidecars, material graphs, and folder trees byte-for-byte until restored or purged.
/// </summary>
internal sealed class AssetTrashWorkflow
{
    private const int ManifestSchemaVersion = 1;
    private const int MaxManifestBytes = 1024 * 1024;
    private const string ManifestFileName = "manifest.v1.json";
    private const string MaterialGraphSuffix = ".graph.json";
    private static readonly StringComparer PathComparer =
        StringComparer.OrdinalIgnoreCase;
    private static readonly UTF8Encoding Utf8NoBom = new(false, true);
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        MaxDepth = 32,
    };

    private readonly object _gate = new();
    private readonly AssetDatabase _database;
    private readonly string _assetsRoot;
    private readonly string _trashRoot;
    private readonly string _entriesRoot;
    private readonly string _stagingRoot;
    private readonly string _restoringRoot;
    private readonly string _purgingRoot;
    private readonly Action<AssetTrashFaultPoint, int>? _faultInjector;

    internal AssetTrashWorkflow(AssetDatabase database)
        : this(database, faultInjector: null)
    {
    }

    internal AssetTrashWorkflow(
        AssetDatabase database,
        Action<AssetTrashFaultPoint, int>? faultInjector)
    {
        _database = database ?? throw new ArgumentNullException(nameof(database));
        _assetsRoot = Normalize(database.AssetsRoot);
        _trashRoot = Path.Combine(
            _assetsRoot,
            AssetDatabase.InternalDirectoryName,
            "trash");
        _entriesRoot = Path.Combine(_trashRoot, "entries");
        _stagingRoot = Path.Combine(_trashRoot, "staging");
        _restoringRoot = Path.Combine(_trashRoot, "restoring");
        _purgingRoot = Path.Combine(_trashRoot, "purging");
        _faultInjector = faultInjector;
        EnsureOrdinaryDirectory(_assetsRoot, "Assets root");
    }

    internal string TrashRoot => _trashRoot;

    internal AssetTrashResult Trash(IEnumerable<string> fullPaths)
    {
        lock (_gate)
        {
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Move assets to Trash");
            // Rebuild the dependency/index view under the same project lease used for the
            // destructive operation. Otherwise a second editor can publish a new sidecar
            // dependency after this instance's last refresh and the delete preflight will miss it.
            _database.Refresh(verifyContent: true);
            IReadOnlyList<TrashTarget> targets = NormalizeTopLevelTargets(fullPaths);
            if (targets.Count == 0)
                throw new ArgumentException("At least one asset is required.", nameof(fullPaths));

            // Reuse the authoritative index/reference verification used by permanent delete.
            var management = new AssetManagementWorkflow(_database);
            AssetDeleteInspection inspection = management.InspectDelete(
                targets.Select(static target => target.FullPath));
            if (!inspection.CanDelete)
            {
                throw new AssetOperationBlockedException(
                    "Referenced assets cannot be moved to Trash safely.",
                    inspection.Blockers);
            }

            EnsureStorage();
            string entryId = Guid.NewGuid().ToString("N");
            string staging = Path.Combine(_stagingRoot, "txn-" + entryId);
            string published = Path.Combine(_entriesRoot, entryId);
            string payload = Path.Combine(staging, "payload");
            var moved = new List<MovedPath>();
            try
            {
                CreateOrdinaryDirectoryChain(staging, created: null);
                CreateOrdinaryDirectoryChain(payload, created: null);
                TrashManifest manifest = BuildManifest(
                    entryId,
                    targets,
                    inspection,
                    payload);
                WriteManifest(staging, manifest);

                for (int index = 0; index < manifest.Items.Count; index++)
                {
                    ManifestItem item = manifest.Items[index];
                    string source = ResolveAssetsRelative(item.RelativePath);
                    string destination = ResolvePayloadRelative(payload, item.RelativePath);
                    CreateOrdinaryDirectoryChain(
                        Path.GetDirectoryName(destination)!,
                        created: null);
                    RevalidateExistingPath(source, item.IsDirectory);
                    EnsureDestinationVacant(destination);
                    MovePath(source, destination, item.IsDirectory);
                    moved.Add(new MovedPath(source, destination, item.IsDirectory));
                    _faultInjector?.Invoke(
                        AssetTrashFaultPoint.AfterTrashItemMoved,
                        index + 1);
                }

                _faultInjector?.Invoke(
                    AssetTrashFaultPoint.BeforeTrashRefresh,
                    moved.Count);
                _database.Refresh();
                manifest.State = "ready";
                WriteManifest(staging, manifest);
                EnsureDestinationVacant(published);
                Directory.Move(staging, published);
                return new AssetTrashResult(
                    entryId,
                    manifest.AssetCount,
                    manifest.FolderCount,
                    manifest.TotalBytes);
            }
            catch (Exception error)
            {
                bool rollbackComplete = TryRollbackMoves(moved);
                TryRefresh();
                if (rollbackComplete)
                {
                    TryDeleteOrdinaryTree(staging);
                    throw;
                }
                throw new IOException(
                    "Moving assets to Trash failed and automatic rollback was incomplete. " +
                    $"Recoverable data remains at '{staging}'.",
                    error);
            }
        }
    }

    internal IReadOnlyList<AssetTrashEntry> ListEntries()
    {
        lock (_gate)
        {
            EnsureStorage();
            var result = new List<AssetTrashEntry>();
            foreach (string path in Directory.EnumerateDirectories(_entriesRoot))
            {
                EnsureOrdinaryDirectory(path, "Trash entry");
                string entryId = Path.GetFileName(path);
                TrashManifest manifest = LoadManifest(
                    path,
                    entryId,
                    requirePayload: true);
                result.Add(ToEntry(manifest));
            }
            return Array.AsReadOnly(result
                .OrderByDescending(static entry => entry.CreatedUtc)
                .ThenBy(static entry => entry.EntryId, StringComparer.Ordinal)
                .ToArray());
        }
    }

    internal AssetTrashRestoreInspection InspectRestore(string entryId)
    {
        lock (_gate)
        {
            string normalizedId = ValidateEntryId(entryId);
            EnsureStorage();
            string entryPath = Path.Combine(_entriesRoot, normalizedId);
            TrashManifest manifest = LoadManifest(
                entryPath,
                normalizedId,
                requirePayload: true);
            return new AssetTrashRestoreInspection(
                ToEntry(manifest),
                FindRestoreCollisions(manifest));
        }
    }

    internal AssetTrashRestoreResult Restore(string entryId)
    {
        lock (_gate)
        {
            string normalizedId = ValidateEntryId(entryId);
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Restore assets from Trash");
            EnsureStorage();
            string entryPath = Path.Combine(_entriesRoot, normalizedId);
            TrashManifest initial = LoadManifest(
                entryPath,
                normalizedId,
                requirePayload: true);
            IReadOnlyList<string> initialCollisions = FindRestoreCollisions(initial);
            if (initialCollisions.Count != 0)
                throw new AssetTrashCollisionException(initialCollisions);

            string claimed = Path.Combine(_restoringRoot, normalizedId);
            EnsureDestinationVacant(claimed);
            Directory.Move(entryPath, claimed);

            TrashManifest manifest;
            try
            {
                // Re-read after the atomic claim and re-check destinations to close the
                // inspection-to-restore race.
                manifest = LoadManifest(claimed, normalizedId, requirePayload: true);
                IReadOnlyList<string> collisions = FindRestoreCollisions(manifest);
                if (collisions.Count != 0)
                {
                    Directory.Move(claimed, entryPath);
                    throw new AssetTrashCollisionException(collisions);
                }
            }
            catch
            {
                if (Directory.Exists(claimed) && !PathExists(entryPath))
                {
                    try { Directory.Move(claimed, entryPath); }
                    catch { }
                }
                throw;
            }

            string payload = Path.Combine(claimed, "payload");
            var moved = new List<MovedPath>();
            var createdDirectories = new List<string>();
            try
            {
                for (int index = 0; index < manifest.Items.Count; index++)
                {
                    ManifestItem item = manifest.Items[index];
                    string source = ResolvePayloadRelative(payload, item.RelativePath);
                    string destination = ResolveAssetsRelative(item.RelativePath);
                    CreateOrdinaryDirectoryChain(
                        Path.GetDirectoryName(destination)!,
                        createdDirectories);
                    RevalidateExistingPath(source, item.IsDirectory);
                    EnsureDestinationVacant(destination);
                    MovePath(source, destination, item.IsDirectory);
                    moved.Add(new MovedPath(source, destination, item.IsDirectory));
                    _faultInjector?.Invoke(
                        AssetTrashFaultPoint.AfterRestoreItemMoved,
                        index + 1);
                }

                _faultInjector?.Invoke(
                    AssetTrashFaultPoint.BeforeRestoreRefresh,
                    moved.Count);
                _database.Refresh();
            }
            catch (Exception error)
            {
                bool rollbackComplete = TryRollbackMoves(moved);
                RemoveCreatedDirectories(createdDirectories);
                TryRefresh();
                if (rollbackComplete)
                {
                    try
                    {
                        if (!PathExists(entryPath))
                            Directory.Move(claimed, entryPath);
                    }
                    catch
                    {
                        rollbackComplete = false;
                    }
                }
                if (rollbackComplete) throw;
                throw new IOException(
                    "Trash restore failed and automatic rollback was incomplete. " +
                    $"Recoverable data remains at '{claimed}'.",
                    error);
            }

            string? deferredCleanup = null;
            if (!TryDeleteOrdinaryTree(claimed))
                deferredCleanup = claimed;
            return new AssetTrashRestoreResult(
                normalizedId,
                Array.AsReadOnly(manifest.Targets
                    .Select(target => ResolveAssetsRelative(target.RelativePath))
                    .ToArray()),
                deferredCleanup);
        }
    }

    internal AssetTrashCleanupResult EmptyTrash(
        Action<IReadOnlyList<string>>? beforePermanentDelete = null)
    {
        lock (_gate)
        {
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Empty Trash");
            EnsureStorage();
            string[] paths = Directory.EnumerateDirectories(_entriesRoot).ToArray();
            return PurgePaths(paths, beforePermanentDelete);
        }
    }

    internal IReadOnlyList<string> ListDeferredTransactionPaths()
    {
        lock (_gate)
        {
            EnsureStorage();
            return Array.AsReadOnly(
                new[] { _stagingRoot, _restoringRoot, _purgingRoot }
                    .SelectMany(static root => Directory.EnumerateDirectories(root))
                    .Select(Normalize)
                    .OrderBy(static path => path, StringComparer.Ordinal)
                    .ToArray());
        }
    }

    internal AssetTrashCleanupResult ApplyRetention(
        AssetTrashRetentionPolicy? policy = null,
        DateTimeOffset? nowUtc = null,
        Action<IReadOnlyList<string>>? beforePermanentDelete = null)
    {
        lock (_gate)
        {
            AssetTrashRetentionPolicy effective =
                policy ?? AssetTrashRetentionPolicy.Default;
            ValidateRetention(effective);
            using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
                _assetsRoot,
                "Apply Trash retention");
            DateTimeOffset now = nowUtc ?? DateTimeOffset.UtcNow;
            AssetTrashEntry[] entries = ListEntries().ToArray();
            var purgeIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (AssetTrashEntry entry in entries)
            {
                TimeSpan age = now > entry.CreatedUtc
                    ? now - entry.CreatedUtc
                    : TimeSpan.Zero;
                if (age > effective.MaxAge)
                    purgeIds.Add(entry.EntryId);
            }

            int keptCount = 0;
            long keptBytes = 0;
            foreach (AssetTrashEntry entry in entries
                         .Where(entry => !purgeIds.Contains(entry.EntryId))
                         .OrderByDescending(static entry => entry.CreatedUtc)
                         .ThenBy(static entry => entry.EntryId, StringComparer.Ordinal))
            {
                bool countFits = keptCount < effective.MaxEntries;
                bool bytesFit = entry.StoredBytes <=
                                effective.MaxBytes - Math.Min(
                                    keptBytes,
                                    effective.MaxBytes);
                if (!countFits || !bytesFit)
                {
                    purgeIds.Add(entry.EntryId);
                    continue;
                }
                keptCount++;
                keptBytes += entry.StoredBytes;
            }

            string[] paths = purgeIds
                .Select(id => Path.Combine(_entriesRoot, id))
                .Where(Directory.Exists)
                .ToArray();
            return PurgePaths(paths, beforePermanentDelete);
        }
    }

    private TrashManifest BuildManifest(
        string entryId,
        IReadOnlyList<TrashTarget> targets,
        AssetDeleteInspection inspection,
        string payload)
    {
        var manifest = new TrashManifest
        {
            SchemaVersion = ManifestSchemaVersion,
            EntryId = entryId,
            State = "staging",
            CreatedUtc = DateTimeOffset.UtcNow,
            AssetCount = inspection.AssetCount,
            FolderCount = inspection.FolderCount,
            TotalBytes = inspection.TotalBytes,
        };
        foreach (TrashTarget target in targets)
        {
            string relative = ToRelative(target.FullPath);
            manifest.Targets.Add(new ManifestTarget
            {
                RelativePath = relative,
                IsDirectory = target.IsDirectory,
            });
            if (target.IsDirectory)
            {
                manifest.Items.Add(new ManifestItem
                {
                    RelativePath = relative,
                    IsDirectory = true,
                });
                manifest.StoredBytes = SaturatingAdd(
                    manifest.StoredBytes,
                    MeasureOrdinaryTree(target.FullPath));
                continue;
            }

            foreach (string familyPath in EnumerateAssetFamily(target.FullPath))
            {
                RevalidateExistingPath(familyPath, isDirectory: false);
                manifest.Items.Add(new ManifestItem
                {
                    RelativePath = ToRelative(familyPath),
                    IsDirectory = false,
                });
                manifest.StoredBytes = SaturatingAdd(
                    manifest.StoredBytes,
                    new FileInfo(familyPath).Length);
            }
        }
        ValidateManifest(manifest, payload, requirePayload: false);
        return manifest;
    }

    private IReadOnlyList<TrashTarget> NormalizeTopLevelTargets(
        IEnumerable<string> fullPaths)
    {
        ArgumentNullException.ThrowIfNull(fullPaths);
        TrashTarget[] targets = fullPaths
            .Where(static path => !string.IsNullOrWhiteSpace(path))
            .Select(ValidateTarget)
            .DistinctBy(static target => target.FullPath, PathComparer)
            .OrderBy(static target => target.FullPath.Length)
            .ThenBy(static target => target.FullPath, PathComparer)
            .ToArray();
        var result = new List<TrashTarget>();
        foreach (TrashTarget target in targets)
        {
            if (result.Any(parent =>
                    parent.IsDirectory &&
                    IsUnder(target.FullPath, parent.FullPath)))
            {
                continue;
            }
            result.Add(target);
        }
        return Array.AsReadOnly(result.ToArray());
    }

    private TrashTarget ValidateTarget(string path)
    {
        string full = Normalize(path);
        if (!IsUnder(full, _assetsRoot) ||
            IsUnderOrEqual(
                full,
                Path.Combine(_assetsRoot, AssetDatabase.InternalDirectoryName)) ||
            full.EndsWith(
                AssetDatabase.MetadataSuffix,
                StringComparison.OrdinalIgnoreCase) ||
            IsMaterialGraphPath(full) ||
            AssetCreationWorkflow.IsTemporaryPath(full))
        {
            throw new InvalidDataException(
                "Trash target is reserved or outside the Assets directory.");
        }
        EnsureNoReparseParents(full, _assetsRoot);
        bool isDirectory = Directory.Exists(full);
        if (!isDirectory && !File.Exists(full))
            throw new FileNotFoundException("Trash target no longer exists.", full);
        RevalidateExistingPath(full, isDirectory);
        if (isDirectory) ValidateOrdinaryTree(full);
        return new TrashTarget(full, isDirectory);
    }

    private IReadOnlyList<string> FindRestoreCollisions(TrashManifest manifest)
    {
        var collisions = new SortedSet<string>(StringComparer.Ordinal);
        foreach (ManifestItem item in manifest.Items)
        {
            string destination = ResolveAssetsRelative(item.RelativePath);
            AddParentCollision(destination, collisions);
            if (PathExists(destination))
                collisions.Add(DisplayRelative(item.RelativePath));
        }

        // A companion created after trashing must also block restore even when it was
        // absent from the original payload.
        foreach (ManifestTarget target in manifest.Targets)
        {
            foreach (string relative in ReservedTargetFamily(target))
            {
                string destination = ResolveAssetsRelative(relative);
                if (PathExists(destination))
                    collisions.Add(DisplayRelative(relative));
            }
        }
        return Array.AsReadOnly(collisions.ToArray());
    }

    private void AddParentCollision(
        string destination,
        ISet<string> collisions)
    {
        string relativeParent = Path.GetRelativePath(
            _assetsRoot,
            Path.GetDirectoryName(destination)!);
        string cursor = _assetsRoot;
        if (relativeParent == ".") return;
        foreach (string segment in SplitRelative(relativeParent))
        {
            cursor = Path.Combine(cursor, segment);
            if (!PathExists(cursor)) return;
            try
            {
                FileAttributes attributes = File.GetAttributes(cursor);
                if ((attributes & FileAttributes.Directory) == 0 ||
                    (attributes & FileAttributes.ReparsePoint) != 0)
                {
                    collisions.Add(
                        DisplayRelative(ToRelative(cursor)) +
                        " (parent is not an ordinary folder)");
                    return;
                }
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException)
            {
                collisions.Add(
                    DisplayRelative(ToRelative(cursor)) +
                    " (parent could not be verified)");
                return;
            }
        }
    }

    private IEnumerable<string> ReservedTargetFamily(ManifestTarget target)
    {
        yield return NormalizeManifestRelative(target.RelativePath);
        if (target.IsDirectory) yield break;
        yield return NormalizeManifestRelative(
            target.RelativePath + AssetDatabase.MetadataSuffix);
        if (!target.RelativePath.EndsWith(
                ".acsmat",
                StringComparison.OrdinalIgnoreCase))
        {
            yield break;
        }
        string graph = target.RelativePath + MaterialGraphSuffix;
        yield return NormalizeManifestRelative(graph);
        yield return NormalizeManifestRelative(
            graph + AssetDatabase.MetadataSuffix);
    }

    private AssetTrashCleanupResult PurgePaths(
        IEnumerable<string> entryPaths,
        Action<IReadOnlyList<string>>? beforePermanentDelete)
    {
        var deferred = new List<string>();
        var staged = new List<PurgeStaging>();
        foreach (string path in entryPaths)
        {
            long bytes = 0;
            TrashManifest? manifest = null;
            try
            {
                string entryId = ValidateEntryId(Path.GetFileName(path));
                EnsureOrdinaryDirectory(path, "Trash entry");
                ValidateOrdinaryTree(path);
                try
                {
                    manifest = LoadManifest(path, entryId, requirePayload: true);
                    bytes = manifest.StoredBytes;
                }
                catch (Exception error) when (
                    error is IOException or UnauthorizedAccessException or InvalidDataException)
                {
                    // Empty Trash must still be able to remove a corrupt but ordinary local
                    // entry. Its untrusted accounting is simply omitted.
                }

                string purgePath = Path.Combine(
                    _purgingRoot,
                    entryId + "-" + Guid.NewGuid().ToString("N"));
                EnsureDestinationVacant(purgePath);
                Directory.Move(path, purgePath);
                staged.Add(new PurgeStaging(
                    path,
                    purgePath,
                    bytes,
                    manifest == null
                        ? Array.Empty<string>()
                        : manifest.Targets
                            .Select(static target => target.RelativePath)
                            .ToArray()));
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                deferred.Add(path + ": " + error.Message);
            }
        }

        string[] purgedOriginalPaths = staged
            .SelectMany(static item => item.OriginalRelativePaths)
            .Distinct(PathComparer)
            .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        try
        {
            if (purgedOriginalPaths.Length != 0)
            {
                beforePermanentDelete?.Invoke(
                    Array.AsReadOnly(purgedOriginalPaths));
            }
        }
        catch (Exception error)
        {
            bool rollbackComplete = true;
            foreach (PurgeStaging item in staged.AsEnumerable().Reverse())
            {
                try
                {
                    EnsureDestinationVacant(item.PublishedPath);
                    Directory.Move(item.StagedPath, item.PublishedPath);
                }
                catch
                {
                    rollbackComplete = false;
                }
            }
            throw new IOException(
                rollbackComplete
                    ? "Trash purge was cancelled because saved Asset View sources could not " +
                      "be updated. All entries were restored."
                    : "Trash purge source cleanup failed and one or more entries could not " +
                      "be restored. Recover data from the deferred purge transaction paths.",
                error);
        }

        long reclaimed = 0;
        foreach (PurgeStaging item in staged)
        {
            if (TryDeleteOrdinaryTree(item.StagedPath))
                reclaimed = SaturatingAdd(reclaimed, item.StoredBytes);
            else
                deferred.Add(item.StagedPath);
        }
        return new AssetTrashCleanupResult(
            staged.Count,
            reclaimed,
            Array.AsReadOnly(deferred.ToArray()),
            Array.AsReadOnly(purgedOriginalPaths));
    }

    private sealed record PurgeStaging(
        string PublishedPath,
        string StagedPath,
        long StoredBytes,
        IReadOnlyList<string> OriginalRelativePaths);

    private TrashManifest LoadManifest(
        string entryPath,
        string expectedEntryId,
        bool requirePayload)
    {
        EnsureOrdinaryDirectory(entryPath, "Trash entry");
        string manifestPath = Path.Combine(entryPath, ManifestFileName);
        RevalidateExistingPath(manifestPath, isDirectory: false);
        var info = new FileInfo(manifestPath);
        if (info.Length <= 0 || info.Length > MaxManifestBytes)
            throw new InvalidDataException("Trash manifest has an invalid size.");
        TrashManifest manifest;
        try
        {
            byte[] bytes = File.ReadAllBytes(manifestPath);
            manifest = JsonSerializer.Deserialize<TrashManifest>(bytes, JsonOptions)
                       ?? throw new InvalidDataException("Trash manifest is empty.");
        }
        catch (JsonException error)
        {
            throw new InvalidDataException("Trash manifest is invalid JSON.", error);
        }
        if (!string.Equals(
                ValidateEntryId(manifest.EntryId),
                expectedEntryId,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Trash manifest identity does not match its directory.");
        }
        if (!string.Equals(manifest.State, "ready", StringComparison.Ordinal))
            throw new InvalidDataException("Trash entry is not in the ready state.");
        string payload = Path.Combine(entryPath, "payload");
        ValidateManifest(manifest, payload, requirePayload);
        return manifest;
    }

    private void ValidateManifest(
        TrashManifest manifest,
        string payload,
        bool requirePayload)
    {
        if (manifest.SchemaVersion != ManifestSchemaVersion ||
            manifest.AssetCount < 0 ||
            manifest.FolderCount < 0 ||
            manifest.TotalBytes < 0 ||
            manifest.StoredBytes < 0 ||
            manifest.CreatedUtc == default ||
            manifest.CreatedUtc.Offset != TimeSpan.Zero ||
            manifest.CreatedUtc > DateTimeOffset.UtcNow.AddDays(1) ||
            manifest.Targets.Count == 0 ||
            manifest.Items.Count == 0)
        {
            throw new InvalidDataException("Trash manifest fields are invalid.");
        }
        ValidateEntryId(manifest.EntryId);

        var itemPaths = new HashSet<string>(PathComparer);
        foreach (ManifestItem item in manifest.Items)
        {
            item.RelativePath = NormalizeManifestRelative(item.RelativePath);
            if (!itemPaths.Add(item.RelativePath))
                throw new InvalidDataException("Trash manifest contains duplicate paths.");
        }
        foreach (ManifestItem parent in manifest.Items.Where(
                     static item => item.IsDirectory))
        {
            if (manifest.Items.Any(item =>
                    !ReferenceEquals(item, parent) &&
                    IsRelativeUnder(item.RelativePath, parent.RelativePath)))
            {
                throw new InvalidDataException(
                    "Trash manifest contains overlapping payload items.");
            }
        }
        var targetPaths = new HashSet<string>(PathComparer);
        foreach (ManifestTarget target in manifest.Targets)
        {
            target.RelativePath = NormalizeManifestRelative(target.RelativePath);
            if (!targetPaths.Add(target.RelativePath))
                throw new InvalidDataException("Trash manifest contains duplicate targets.");
            if (!manifest.Items.Any(item =>
                    item.IsDirectory == target.IsDirectory &&
                    PathComparer.Equals(item.RelativePath, target.RelativePath)))
            {
                throw new InvalidDataException(
                    "Trash manifest target is missing its payload item.");
            }
        }

        if (!requirePayload) return;
        EnsureOrdinaryDirectory(payload, "Trash payload");
        long measuredBytes = 0;
        foreach (ManifestItem item in manifest.Items)
        {
            string source = ResolvePayloadRelative(payload, item.RelativePath);
            RevalidateExistingPath(source, item.IsDirectory);
            if (item.IsDirectory)
            {
                ValidateOrdinaryTree(source);
                measuredBytes = SaturatingAdd(
                    measuredBytes,
                    MeasureOrdinaryTree(source));
            }
            else
            {
                measuredBytes = SaturatingAdd(
                    measuredBytes,
                    new FileInfo(source).Length);
            }
        }
        if (measuredBytes != manifest.StoredBytes)
            throw new InvalidDataException("Trash payload size does not match its manifest.");
    }

    private void WriteManifest(string entryPath, TrashManifest manifest)
    {
        byte[] content = JsonSerializer.SerializeToUtf8Bytes(manifest, JsonOptions);
        if (content.Length > MaxManifestBytes)
            throw new InvalidDataException("Trash manifest is too large.");
        string destination = Path.Combine(entryPath, ManifestFileName);
        string temporary = destination + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            File.WriteAllBytes(temporary, content);
            RevalidateExistingPath(temporary, isDirectory: false);
            File.Move(temporary, destination, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException)
            {
            }
        }
    }

    private AssetTrashEntry ToEntry(TrashManifest manifest) =>
        new(
            manifest.EntryId,
            manifest.CreatedUtc,
            Array.AsReadOnly(manifest.Targets
                .Select(static target => target.RelativePath)
                .ToArray()),
            manifest.AssetCount,
            manifest.FolderCount,
            manifest.TotalBytes,
            manifest.StoredBytes);

    private void EnsureStorage()
    {
        CreateOrdinaryDirectoryChain(_entriesRoot, created: null);
        CreateOrdinaryDirectoryChain(_stagingRoot, created: null);
        CreateOrdinaryDirectoryChain(_restoringRoot, created: null);
        CreateOrdinaryDirectoryChain(_purgingRoot, created: null);
    }

    private void CreateOrdinaryDirectoryChain(
        string directory,
        List<string>? created)
    {
        string full = Normalize(directory);
        if (!IsUnderOrEqual(full, _assetsRoot))
            throw new InvalidDataException("Directory escapes Assets.");
        string relative = Path.GetRelativePath(_assetsRoot, full);
        string cursor = _assetsRoot;
        EnsureOrdinaryDirectory(cursor, "Assets root");
        if (relative == ".") return;
        foreach (string segment in SplitRelative(relative))
        {
            cursor = Path.Combine(cursor, segment);
            if (!PathExists(cursor))
            {
                Directory.CreateDirectory(cursor);
                created?.Add(cursor);
            }
            EnsureOrdinaryDirectory(cursor, "Asset directory");
        }
    }

    private void EnsureNoReparseParents(string path, string root)
    {
        string full = Normalize(path);
        string fullRoot = Normalize(root);
        if (!IsUnderOrEqual(full, fullRoot))
            throw new InvalidDataException("Path escapes its allowed root.");
        string parent = Directory.Exists(full)
            ? full
            : Path.GetDirectoryName(full)
              ?? throw new InvalidDataException("Path has no parent.");
        string relative = Path.GetRelativePath(fullRoot, parent);
        string cursor = fullRoot;
        EnsureOrdinaryDirectory(cursor, "Path root");
        if (relative == ".") return;
        foreach (string segment in SplitRelative(relative))
        {
            cursor = Path.Combine(cursor, segment);
            EnsureOrdinaryDirectory(cursor, "Path parent");
        }
    }

    private static void RevalidateExistingPath(string path, bool isDirectory)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0 ||
            ((attributes & FileAttributes.Directory) != 0) != isDirectory)
        {
            throw new InvalidDataException(
                "Only ordinary asset files and folders are supported.");
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

    private static void ValidateOrdinaryTree(string root)
    {
        EnsureOrdinaryDirectory(root, "Asset tree");
        foreach (TreeEntry entry in EnumerateOrdinaryTree(root))
        {
            if (entry.IsDirectory &&
                Path.GetFileName(entry.FullPath).Equals(
                    AssetDatabase.InternalDirectoryName,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "Nested .acsdb directories are reserved.");
            }
        }
    }

    private static IEnumerable<TreeEntry> EnumerateOrdinaryTree(string root)
    {
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.Count != 0)
        {
            string directory = pending.Pop();
            EnsureOrdinaryDirectory(directory, "Asset tree");
            foreach (FileSystemInfo entry in new DirectoryInfo(directory)
                         .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                entry.Refresh();
                FileAttributes attributes = entry.Attributes;
                bool isDirectory = (attributes & FileAttributes.Directory) != 0;
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException(
                        "Asset tree contains a reparse point.");
                }
                yield return new TreeEntry(entry.FullName, isDirectory);
                if (isDirectory) pending.Push(entry.FullName);
            }
        }
    }

    private static long MeasureOrdinaryTree(string root)
    {
        long total = 0;
        foreach (TreeEntry entry in EnumerateOrdinaryTree(root))
        {
            if (!entry.IsDirectory)
                total = SaturatingAdd(total, new FileInfo(entry.FullPath).Length);
        }
        return total;
    }

    private static bool TryDeleteOrdinaryTree(string path)
    {
        if (!Directory.Exists(path)) return true;
        try
        {
            ValidateOrdinaryTree(path);
            TreeEntry[] entries = EnumerateOrdinaryTree(path).ToArray();
            foreach (TreeEntry file in entries.Where(static entry => !entry.IsDirectory))
            {
                RevalidateExistingPath(file.FullPath, isDirectory: false);
                File.Delete(file.FullPath);
            }
            foreach (TreeEntry directory in entries
                         .Where(static entry => entry.IsDirectory)
                         .OrderByDescending(static entry => entry.FullPath.Length))
            {
                EnsureOrdinaryDirectory(directory.FullPath, "Trash directory");
                Directory.Delete(directory.FullPath, recursive: false);
            }
            EnsureOrdinaryDirectory(path, "Trash directory");
            Directory.Delete(path, recursive: false);
            return !Directory.Exists(path);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return false;
        }
    }

    private static void MovePath(string source, string destination, bool isDirectory)
    {
        if (isDirectory) Directory.Move(source, destination);
        else File.Move(source, destination);
    }

    private bool TryRollbackMoves(IReadOnlyList<MovedPath> moved)
    {
        bool complete = true;
        foreach (MovedPath move in moved.Reverse())
        {
            try
            {
                if (PathExists(move.Source))
                {
                    complete = false;
                    continue;
                }
                if (!PathExists(move.Destination))
                {
                    complete = false;
                    continue;
                }
                string parent = Path.GetDirectoryName(move.Source)!;
                CreateOrdinaryDirectoryChain(parent, created: null);
                RevalidateExistingPath(move.Destination, move.IsDirectory);
                EnsureDestinationVacant(move.Source);
                MovePath(move.Destination, move.Source, move.IsDirectory);
            }
            catch
            {
                complete = false;
            }
        }
        return complete;
    }

    private static void RemoveCreatedDirectories(IEnumerable<string> created)
    {
        foreach (string directory in created
                     .Distinct(PathComparer)
                     .OrderByDescending(static path => path.Length))
        {
            try
            {
                if (Directory.Exists(directory) &&
                    !Directory.EnumerateFileSystemEntries(directory).Any())
                {
                    EnsureOrdinaryDirectory(directory, "Created asset directory");
                    Directory.Delete(directory, recursive: false);
                }
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
            }
        }
    }

    private void EnsureDestinationVacant(string path)
    {
        if (PathExists(path))
            throw new IOException($"Destination already exists: {path}");
        string parent = Path.GetDirectoryName(path)
            ?? throw new InvalidDataException("Destination has no parent.");
        EnsureNoReparseParents(parent, _assetsRoot);
        EnsureOrdinaryDirectory(parent, "Destination parent");
    }

    private string ResolveAssetsRelative(string relative)
    {
        string canonical = NormalizeManifestRelative(relative);
        string full = Normalize(Path.Combine(
            _assetsRoot,
            canonical.Replace('/', Path.DirectorySeparatorChar)));
        if (!IsUnder(full, _assetsRoot) ||
            IsUnderOrEqual(
                full,
                Path.Combine(_assetsRoot, AssetDatabase.InternalDirectoryName)))
        {
            throw new InvalidDataException("Trash manifest path escapes Assets.");
        }
        return full;
    }

    private static string ResolvePayloadRelative(string payload, string relative)
    {
        string canonical = NormalizeManifestRelative(relative);
        string root = Normalize(payload);
        string full = Normalize(Path.Combine(
            root,
            canonical.Replace('/', Path.DirectorySeparatorChar)));
        if (!IsUnder(full, root))
            throw new InvalidDataException("Trash payload path escapes its entry.");
        return full;
    }

    private string ToRelative(string fullPath)
    {
        string full = Normalize(fullPath);
        if (!IsUnder(full, _assetsRoot))
            throw new InvalidDataException("Path escapes Assets.");
        return NormalizeManifestRelative(
            Path.GetRelativePath(_assetsRoot, full).Replace('\\', '/'));
    }

    private static string NormalizeManifestRelative(string value)
    {
        string relative = (value ?? "").Replace('\\', '/');
        if (relative.Length == 0 ||
            relative.StartsWith("/", StringComparison.Ordinal) ||
            Path.IsPathRooted(relative) ||
            relative.Contains('\0'))
        {
            throw new InvalidDataException("Trash manifest contains an invalid path.");
        }
        string[] segments = relative.Split('/');
        foreach (string segment in segments)
        {
            if (segment.Length == 0 ||
                segment is "." or ".." ||
                segment.EndsWith(' ') ||
                segment.EndsWith('.') ||
                segment.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                segment.Equals(
                    AssetDatabase.InternalDirectoryName,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "Trash manifest contains an invalid path segment.");
            }
        }

        string canonical;
        try
        {
            string probeRoot = Path.Combine(Path.GetTempPath(), "acs-trash-path-root");
            string full = Path.GetFullPath(Path.Combine(
                probeRoot,
                relative.Replace('/', Path.DirectorySeparatorChar)));
            canonical = Path.GetRelativePath(probeRoot, full).Replace('\\', '/');
        }
        catch (Exception error) when (
            error is ArgumentException or NotSupportedException or PathTooLongException)
        {
            throw new InvalidDataException(
                "Trash manifest contains an invalid path.",
                error);
        }
        if (canonical == "." ||
            canonical == ".." ||
            canonical.StartsWith("../", StringComparison.Ordinal) ||
            !string.Equals(canonical, relative, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Trash manifest path is not canonical or escapes its root.");
        }
        return canonical;
    }

    private static string ValidateEntryId(string entryId)
    {
        if (!Guid.TryParseExact(entryId, "N", out Guid parsed))
            throw new InvalidDataException("Trash entry identity is invalid.");
        return parsed.ToString("N");
    }

    private static IEnumerable<string> EnumerateAssetFamily(string assetPath)
    {
        yield return assetPath;
        string metadata = assetPath + AssetDatabase.MetadataSuffix;
        if (File.Exists(metadata)) yield return metadata;
        if (!assetPath.EndsWith(".acsmat", StringComparison.OrdinalIgnoreCase))
            yield break;
        string graph = assetPath + MaterialGraphSuffix;
        if (File.Exists(graph)) yield return graph;
        string graphMetadata = graph + AssetDatabase.MetadataSuffix;
        if (File.Exists(graphMetadata)) yield return graphMetadata;
    }

    private static bool IsMaterialGraphPath(string path) =>
        path.EndsWith(
            ".acsmat" + MaterialGraphSuffix,
            StringComparison.OrdinalIgnoreCase);

    private static bool IsRelativeUnder(string candidate, string parent) =>
        candidate.Length > parent.Length &&
        candidate.StartsWith(parent, StringComparison.OrdinalIgnoreCase) &&
        candidate[parent.Length] == '/';

    private static bool PathExists(string path)
    {
        if (File.Exists(path) || Directory.Exists(path)) return true;
        string? parent = Path.GetDirectoryName(path);
        if (parent == null || !Directory.Exists(parent)) return false;
        string name = Path.GetFileName(path);
        return Directory.EnumerateFileSystemEntries(parent)
            .Any(entry => Path.GetFileName(entry).Equals(
                name,
                StringComparison.OrdinalIgnoreCase));
    }

    private static string[] SplitRelative(string relative) =>
        relative.Split(
            new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
            StringSplitOptions.RemoveEmptyEntries);

    private static string DisplayRelative(string relative) =>
        "Assets/" + relative.Replace('\\', '/');

    private static string Normalize(string path) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

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
        PathComparer.Equals(Normalize(candidate), Normalize(root)) ||
        IsUnder(candidate, root);

    private static long SaturatingAdd(long left, long right) =>
        right > 0 && left > long.MaxValue - right ? long.MaxValue : left + right;

    private static void ValidateRetention(AssetTrashRetentionPolicy policy)
    {
        if (policy.MaxAge < TimeSpan.Zero ||
            policy.MaxEntries < 0 ||
            policy.MaxBytes < 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(policy),
                "Trash retention limits cannot be negative.");
        }
    }

    private void TryRefresh()
    {
        try { _database.Refresh(); }
        catch { }
    }

    private sealed record TrashTarget(string FullPath, bool IsDirectory);
    private sealed record MovedPath(
        string Source,
        string Destination,
        bool IsDirectory);
    private sealed record TreeEntry(string FullPath, bool IsDirectory);

    private sealed class TrashManifest
    {
        public int SchemaVersion { get; set; }
        public string EntryId { get; set; } = "";
        public string State { get; set; } = "";
        public DateTimeOffset CreatedUtc { get; set; }
        public int AssetCount { get; set; }
        public int FolderCount { get; set; }
        public long TotalBytes { get; set; }
        public long StoredBytes { get; set; }
        public List<ManifestTarget> Targets { get; set; } = new();
        public List<ManifestItem> Items { get; set; } = new();
    }

    private sealed class ManifestTarget
    {
        public string RelativePath { get; set; } = "";
        public bool IsDirectory { get; set; }
    }

    private sealed class ManifestItem
    {
        public string RelativePath { get; set; } = "";
        public bool IsDirectory { get; set; }
    }
}
