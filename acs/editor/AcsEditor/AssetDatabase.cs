using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;

namespace AcsEditor;

/// <summary>
/// Persistent editor identity and import information for one source asset.
/// The adjacent <c>&lt;asset&gt;.acsmeta</c> file is authoritative; the index
/// under <c>Assets/.acsdb</c> is only a deterministic acceleration/recovery cache.
/// </summary>
public sealed record AssetMetadata(
    int SchemaVersion,
    string AssetId,
    string Kind,
    string Source,
    string Importer,
    int ImporterVersion,
    IReadOnlyList<string> Dependencies,
    IReadOnlyDictionary<string, string> ImportSettings);

/// <summary>An immutable snapshot entry returned by <see cref="AssetDatabase"/>.</summary>
public sealed record AssetRecord(
    string AssetId,
    string RelativePath,
    string FullPath,
    string Kind,
    long SizeBytes,
    long LastWriteUtcTicks,
    string ContentHash,
    AssetMetadata Metadata);

public sealed record AssetDatabaseRefreshResult(
    int AssetCount,
    int CreatedMetadataCount,
    int RecoveredIdentityCount,
    IReadOnlyList<string> Warnings);

/// <summary>
/// One deterministic step in a dependency or referencer traversal.
/// Missing dependency IDs are retained so build diagnostics never silently
/// discard a broken reference.
/// </summary>
public sealed record AssetReferenceNode(
    string AssetId,
    string RelativePath,
    string Kind,
    int Depth,
    string ReachedFromAssetId,
    bool IsMissing);

/// <summary>A canonical dependency cycle. The first ID is repeated at the end.</summary>
public sealed record AssetDependencyCycle(IReadOnlyList<string> AssetIds);

/// <summary>Read-only reference graph diagnostics for one root asset.</summary>
public sealed record AssetReferenceAnalysis(
    AssetRecord Root,
    IReadOnlyList<AssetReferenceNode> DirectDependencies,
    IReadOnlyList<AssetReferenceNode> TransitiveDependencies,
    IReadOnlyList<AssetReferenceNode> DirectReferencers,
    IReadOnlyList<AssetReferenceNode> TransitiveReferencers,
    IReadOnlyList<string> MissingAssetIds,
    IReadOnlyList<AssetDependencyCycle> Cycles);

/// <summary>
/// Production-oriented editor asset index.
///
/// Security boundary:
/// - only ordinary files below the configured Assets directory are indexed;
/// - reparse points (symlinks/junctions) are never traversed or indexed;
/// - every metadata mutation revalidates containment and all existing parents.
///
/// Threading:
/// public methods are serialized. Query results are immutable snapshots.
/// </summary>
public sealed class AssetDatabase
{
    public const int CurrentSchemaVersion = 1;
    public const string MetadataSuffix = ".acsmeta";
    public const string InternalDirectoryName = ".acsdb";

    private const int MaxMetadataBytes = 1024 * 1024;
    private static readonly UTF8Encoding Utf8NoBom = new(false, true);
    private static readonly StringComparer PathComparer = StringComparer.OrdinalIgnoreCase;
    private static readonly StringComparer IdComparer = StringComparer.OrdinalIgnoreCase;

    private readonly object _gate = new();
    private readonly string _projectRoot;
    private readonly string _assetsRoot;
    private readonly string _indexDirectory;
    private readonly string _indexPath;
    private Dictionary<string, AssetRecord> _byPath = new(PathComparer);
    private Dictionary<string, AssetRecord> _byId = new(IdComparer);

    public AssetDatabase(string projectRoot, string assetsRoot)
    {
        if (string.IsNullOrWhiteSpace(projectRoot))
            throw new ArgumentException("Project root is required.", nameof(projectRoot));
        if (string.IsNullOrWhiteSpace(assetsRoot))
            throw new ArgumentException("Assets root is required.", nameof(assetsRoot));

        _projectRoot = NormalizeDirectory(projectRoot);
        _assetsRoot = NormalizeDirectory(assetsRoot);
        if (!IsUnderOrEqual(_assetsRoot, _projectRoot))
            throw new ArgumentException("Assets root must be inside the project root.", nameof(assetsRoot));

        _indexDirectory = Path.Combine(_assetsRoot, InternalDirectoryName);
        _indexPath = Path.Combine(_indexDirectory, "index.v1.json");
    }

    public static AssetDatabase ForProject(Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        return new AssetDatabase(project.RootDir, project.AssetsDir);
    }

    public string ProjectRoot => _projectRoot;
    public string AssetsRoot => _assetsRoot;

    public IReadOnlyList<AssetRecord> Snapshot()
    {
        lock (_gate)
            return Array.AsReadOnly(_byPath.Values
                .OrderBy(static entry => entry.RelativePath, StringComparer.Ordinal)
                .ToArray());
    }

    public bool TryGetByAssetId(string assetId, out AssetRecord? record)
    {
        lock (_gate)
            return _byId.TryGetValue(NormalizeAssetIdForLookup(assetId), out record);
    }

    public bool TryGetByPath(string path, out AssetRecord? record)
    {
        lock (_gate)
        {
            string relative;
            try { relative = NormalizeAssetRelativePath(path); }
            catch { record = null; return false; }
            return _byPath.TryGetValue(relative, out record);
        }
    }

    public IReadOnlyList<AssetRecord> Query(
        string? text = null,
        string? kind = null,
        string? underFolder = null)
    {
        lock (_gate)
        {
            string search = text?.Trim() ?? "";
            string kindFilter = kind?.Trim() ?? "";
            string? folder = string.IsNullOrWhiteSpace(underFolder)
                ? null
                : NormalizeFolderRelativePath(underFolder);

            IEnumerable<AssetRecord> query = _byPath.Values;
            if (search.Length != 0)
            {
                query = query.Where(entry =>
                    entry.RelativePath.Contains(search, StringComparison.OrdinalIgnoreCase) ||
                    entry.AssetId.Contains(search, StringComparison.OrdinalIgnoreCase));
            }
            if (kindFilter.Length != 0)
            {
                query = query.Where(entry =>
                    string.Equals(entry.Kind, kindFilter, StringComparison.OrdinalIgnoreCase));
            }
            if (folder != null)
            {
                string prefix = folder.Length == 0 ? "" : folder + "/";
                query = query.Where(entry =>
                    entry.RelativePath.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
            }

            return Array.AsReadOnly(query
                .OrderBy(static entry => entry.RelativePath, StringComparer.Ordinal)
                .ToArray());
        }
    }

    public IReadOnlyList<AssetReferenceNode> GetDirectDependencies(string assetId) =>
        GetTransitiveDependencies(assetId, maxDepth: 1);

    public IReadOnlyList<AssetReferenceNode> GetTransitiveDependencies(
        string assetId,
        int maxDepth = 64)
    {
        lock (_gate)
        {
            AssetRecord root = GetRequiredRecord(assetId);
            return TraverseDependencies(root.AssetId, ValidateGraphDepth(maxDepth));
        }
    }

    /// <summary>
    /// Returns existing assets that directly reference <paramref name="assetId"/>.
    /// The target itself may be missing, which is useful for repair tooling.
    /// </summary>
    public IReadOnlyList<AssetReferenceNode> GetDirectReferencers(string assetId) =>
        GetTransitiveReferencers(assetId, maxDepth: 1);

    public IReadOnlyList<AssetReferenceNode> GetTransitiveReferencers(
        string assetId,
        int maxDepth = 64)
    {
        lock (_gate)
        {
            string normalized = NormalizeAssetId(assetId);
            return TraverseReferencers(normalized, ValidateGraphDepth(maxDepth));
        }
    }

    /// <summary>
    /// Builds the read-only data used by the Reference Viewer and cook
    /// diagnostics. Calls never refresh or modify files.
    /// </summary>
    public AssetReferenceAnalysis AnalyzeReferences(string assetId, int maxDepth = 64)
    {
        lock (_gate)
        {
            AssetRecord root = GetRequiredRecord(assetId);
            int depth = ValidateGraphDepth(maxDepth);
            IReadOnlyList<AssetReferenceNode> dependencies =
                TraverseDependencies(root.AssetId, depth);
            IReadOnlyList<AssetReferenceNode> referencers =
                TraverseReferencers(root.AssetId, depth);
            IReadOnlyList<string> missing = Array.AsReadOnly(dependencies
                .Where(static node => node.IsMissing)
                .Select(static node => node.AssetId)
                .Distinct(StringComparer.Ordinal)
                .OrderBy(static id => id, StringComparer.Ordinal)
                .ToArray());
            IReadOnlyList<AssetDependencyCycle> cycles = FindDependencyCycles(root.AssetId);

            return new AssetReferenceAnalysis(
                root,
                Array.AsReadOnly(dependencies.Where(static node => node.Depth == 1).ToArray()),
                dependencies,
                Array.AsReadOnly(referencers.Where(static node => node.Depth == 1).ToArray()),
                referencers,
                missing,
                cycles);
        }
    }

    /// <summary>
    /// Rebuilds the in-memory index. Unchanged assets reuse the cached hash unless
    /// <paramref name="verifyContent"/> is true (cook/CI should request true). A normal refresh
    /// may synthesize missing metadata and always publishes the acceleration index, so it is an
    /// asset mutation and holds the project lease for the complete scan/write transaction.
    /// </summary>
    public AssetDatabaseRefreshResult Refresh(
        bool verifyContent = false,
        CancellationToken cancellationToken = default)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Refresh asset database");
        return RefreshCore(
            verifyContent,
            createMissingMetadata: true,
            writeIndex: true,
            cancellationToken);
    }

    /// <summary>
    /// Produces a content-verified read-only snapshot for Cook. Missing authoritative sidecars are
    /// reported instead of being synthesized, and the persistent acceleration index is not
    /// rewritten during package validation.
    /// </summary>
    public AssetDatabaseRefreshResult RefreshForCook(
        CancellationToken cancellationToken = default)
        => RefreshCore(
            verifyContent: true,
            createMissingMetadata: false,
            writeIndex: false,
            cancellationToken);

    private AssetDatabaseRefreshResult RefreshCore(
        bool verifyContent,
        bool createMissingMetadata,
        bool writeIndex,
        CancellationToken cancellationToken)
    {
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            EnsureDatabaseRoots(createStorage: createMissingMetadata || writeIndex);

            var warnings = new List<string>();
            Dictionary<string, CachedRecord> cache = LoadIndex(warnings);
            var discovered = EnumerateAssets(warnings, cancellationToken)
                .OrderBy(static item => item.RelativePath, StringComparer.Ordinal)
                .ToArray();
            var discoveredPaths = discovered
                .Select(static item => item.RelativePath)
                .ToHashSet(PathComparer);
            var missingCachePaths = cache.Keys
                .Where(path => !discoveredPaths.Contains(path))
                .ToHashSet(PathComparer);

            int created = 0;
            int recovered = 0;
            var nextByPath = new Dictionary<string, AssetRecord>(PathComparer);
            var nextById = new Dictionary<string, AssetRecord>(IdComparer);

            foreach (DiscoveredAsset item in discovered)
            {
                cancellationToken.ThrowIfCancellationRequested();
                CachedRecord? cachedAtPath = cache.GetValueOrDefault(item.RelativePath);
                string hash = GetContentHash(item, cachedAtPath, verifyContent);
                string metadataPath = MetadataPath(item.FullPath);
                AssetMetadata? metadata = null;
                string? recoveredFrom = null;

                if (File.Exists(metadataPath))
                {
                    try
                    {
                        EnsureSafeOrdinaryFile(metadataPath, allowMetadata: true);
                        metadata = ReadMetadata(metadataPath);
                    }
                    catch (Exception ex) when (
                        ex is IOException or UnauthorizedAccessException or InvalidDataException or JsonException)
                    {
                        warnings.Add($"Metadata rejected for '{item.RelativePath}': {ex.Message}");
                        continue;
                    }
                }
                else
                {
                    if (!createMissingMetadata)
                    {
                        warnings.Add(
                            $"Metadata missing for '{item.RelativePath}'. Cook requires an authoritative {MetadataSuffix} sidecar.");
                        continue;
                    }

                    if (cachedAtPath != null &&
                        CachedContentMatches(cachedAtPath, item, hash))
                    {
                        metadata = cachedAtPath.Metadata;
                    }
                    else
                    {
                        CachedRecord[] moveCandidates = missingCachePaths
                            .Select(path => cache[path])
                            .Where(old => CachedContentMatches(old, item, hash))
                            .Where(old => !nextById.ContainsKey(old.Metadata.AssetId))
                            .ToArray();
                        if (moveCandidates.Length == 1)
                        {
                            metadata = moveCandidates[0].Metadata;
                            recoveredFrom = moveCandidates[0].RelativePath;
                            missingCachePaths.Remove(recoveredFrom);
                            recovered++;
                        }
                    }

                    if (metadata == null)
                    {
                        metadata = CreateDefaultMetadata(item.RelativePath);
                        created++;
                    }

                    WriteMetadata(metadataPath, metadata);
                    if (recoveredFrom != null)
                        TryDeleteOrphanMetadata(recoveredFrom, warnings);
                }

                try
                {
                    metadata = ValidateAndNormalizeMetadata(metadata, item.RelativePath);
                }
                catch (InvalidDataException ex)
                {
                    warnings.Add($"Metadata rejected for '{item.RelativePath}': {ex.Message}");
                    continue;
                }

                if (nextById.TryGetValue(metadata.AssetId, out AssetRecord? duplicate))
                {
                    warnings.Add(
                        $"Duplicate asset id '{metadata.AssetId}' on '{item.RelativePath}' " +
                        $"and '{duplicate.RelativePath}'. The later asset was not indexed.");
                    continue;
                }

                var record = new AssetRecord(
                    metadata.AssetId,
                    item.RelativePath,
                    item.FullPath,
                    metadata.Kind,
                    item.SizeBytes,
                    item.LastWriteUtcTicks,
                    hash,
                    metadata);
                nextByPath.Add(item.RelativePath, record);
                nextById.Add(metadata.AssetId, record);
            }

            if (writeIndex)
                WriteIndex(nextByPath.Values);
            _byPath = nextByPath;
            _byId = nextById;
            return new AssetDatabaseRefreshResult(
                nextByPath.Count,
                created,
                recovered,
                Array.AsReadOnly(warnings.ToArray()));
        }
    }

    /// <summary>
    /// Atomically rewrites import/dependency metadata for an indexed asset.
    /// The source string is metadata only; indexing never follows it.
    /// </summary>
    public AssetRecord UpdateImportMetadata(
        string assetId,
        string source,
        string importer,
        int importerVersion,
        IEnumerable<string>? dependencies = null,
        IEnumerable<KeyValuePair<string, string>>? importSettings = null)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Update asset import metadata");
        lock (_gate)
        {
            AssetRecord current = GetRequiredRecord(assetId);
            var updated = current.Metadata with
            {
                Source = NormalizeMetadataText(source, "source", 4096),
                Importer = NormalizeMetadataText(importer, "importer", 128),
                ImporterVersion = importerVersion,
                Dependencies = NormalizeDependencies(dependencies),
                ImportSettings = NormalizeImportSettings(importSettings),
            };
            updated = ValidateAndNormalizeMetadata(updated, current.RelativePath);
            WriteMetadata(MetadataPath(current.FullPath), updated);

            AssetRecord replacement = current with
            {
                Kind = updated.Kind,
                Metadata = updated,
            };
            _byPath[current.RelativePath] = replacement;
            _byId[current.AssetId] = replacement;
            WriteIndex(_byPath.Values);
            return replacement;
        }
    }

    /// <summary>
    /// Moves/renames an asset together with its authoritative metadata sidecar.
    /// On any second-step failure, the asset move is rolled back.
    /// </summary>
    public AssetRecord MoveAsset(string assetId, string destinationRelativePath)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Move asset");
        lock (_gate)
        {
            AssetRecord current = GetRequiredRecord(assetId);
            string destinationRelative = NormalizeAssetRelativePath(destinationRelativePath);
            string destination = ResolveAssetPath(destinationRelative);
            if (File.Exists(destination) || Directory.Exists(destination))
                throw new IOException($"Destination already exists: {destinationRelative}");

            string destinationParent = Path.GetDirectoryName(destination)
                ?? throw new InvalidDataException("Destination has no parent directory.");
            EnsureSafeDirectory(destinationParent, createIfMissing: true);

            string sourceMetadata = MetadataPath(current.FullPath);
            string destinationMetadata = MetadataPath(destination);
            EnsureSafeOrdinaryFile(current.FullPath, allowMetadata: false);
            EnsureSafeOrdinaryFile(sourceMetadata, allowMetadata: true);

            bool assetMoved = false;
            bool metadataMoved = false;
            try
            {
                File.Move(current.FullPath, destination);
                assetMoved = true;
                File.Move(sourceMetadata, destinationMetadata);
                metadataMoved = true;

                AssetDatabaseRefreshResult result = Refresh(verifyContent: false);
                if (!_byId.TryGetValue(current.AssetId, out AssetRecord? moved))
                {
                    string detail = result.Warnings.Count == 0
                        ? ""
                        : " " + string.Join(" ", result.Warnings);
                    throw new IOException("Moved asset could not be re-indexed." + detail);
                }
                return moved;
            }
            catch (Exception error)
            {
                bool rollbackComplete = true;
                try
                {
                    if (metadataMoved &&
                        File.Exists(destinationMetadata) &&
                        !File.Exists(sourceMetadata))
                    {
                        File.Move(destinationMetadata, sourceMetadata);
                    }
                }
                catch
                {
                    rollbackComplete = false;
                }
                try
                {
                    if (assetMoved &&
                        File.Exists(destination) &&
                        !File.Exists(current.FullPath))
                    {
                        File.Move(destination, current.FullPath);
                    }
                }
                catch
                {
                    rollbackComplete = false;
                }
                try
                {
                    Refresh(verifyContent: false);
                }
                catch
                {
                    rollbackComplete = false;
                }
                rollbackComplete &= File.Exists(current.FullPath) &&
                                    File.Exists(sourceMetadata) &&
                                    !File.Exists(destination) &&
                                    !File.Exists(destinationMetadata);
                if (!rollbackComplete)
                {
                    throw new IOException(
                        "Asset move failed and automatic rollback was incomplete. " +
                        $"Recoverable data may remain at '{destinationRelative}'.",
                        error);
                }
                throw;
            }
        }
    }

    private IReadOnlyList<AssetReferenceNode> TraverseDependencies(
        string rootAssetId,
        int maxDepth)
    {
        var visited = new HashSet<string>(IdComparer) { rootAssetId };
        var pending = new Queue<(string AssetId, int Depth)>();
        var result = new List<AssetReferenceNode>();
        pending.Enqueue((rootAssetId, 0));

        while (pending.Count != 0)
        {
            (string currentId, int currentDepth) = pending.Dequeue();
            if (currentDepth >= maxDepth ||
                !_byId.TryGetValue(currentId, out AssetRecord? current))
                continue;

            foreach (string dependency in current.Metadata.Dependencies
                .OrderBy(static id => id, StringComparer.Ordinal))
            {
                if (!visited.Add(dependency))
                    continue;
                int depth = currentDepth + 1;
                AssetReferenceNode node = MakeReferenceNode(
                    dependency,
                    depth,
                    currentId);
                result.Add(node);
                if (!node.IsMissing && depth < maxDepth)
                    pending.Enqueue((dependency, depth));
            }
        }

        return SortReferenceNodes(result);
    }

    private IReadOnlyList<AssetReferenceNode> TraverseReferencers(
        string rootAssetId,
        int maxDepth)
    {
        Dictionary<string, List<AssetRecord>> reverse = BuildReverseDependencyIndex();
        var visited = new HashSet<string>(IdComparer) { rootAssetId };
        var pending = new Queue<(string AssetId, int Depth)>();
        var result = new List<AssetReferenceNode>();
        pending.Enqueue((rootAssetId, 0));

        while (pending.Count != 0)
        {
            (string currentId, int currentDepth) = pending.Dequeue();
            if (currentDepth >= maxDepth ||
                !reverse.TryGetValue(currentId, out List<AssetRecord>? references))
                continue;

            foreach (AssetRecord referencer in references)
            {
                if (!visited.Add(referencer.AssetId))
                    continue;
                int depth = currentDepth + 1;
                result.Add(new AssetReferenceNode(
                    referencer.AssetId,
                    referencer.RelativePath,
                    referencer.Kind,
                    depth,
                    currentId,
                    IsMissing: false));
                if (depth < maxDepth)
                    pending.Enqueue((referencer.AssetId, depth));
            }
        }

        return SortReferenceNodes(result);
    }

    private Dictionary<string, List<AssetRecord>> BuildReverseDependencyIndex()
    {
        var reverse = new Dictionary<string, List<AssetRecord>>(IdComparer);
        foreach (AssetRecord record in _byPath.Values
            .OrderBy(static item => item.RelativePath, StringComparer.Ordinal)
            .ThenBy(static item => item.AssetId, StringComparer.Ordinal))
        {
            foreach (string dependency in record.Metadata.Dependencies)
            {
                if (!reverse.TryGetValue(dependency, out List<AssetRecord>? referencers))
                {
                    referencers = new List<AssetRecord>();
                    reverse.Add(dependency, referencers);
                }
                referencers.Add(record);
            }
        }
        return reverse;
    }

    private AssetReferenceNode MakeReferenceNode(
        string assetId,
        int depth,
        string reachedFromAssetId)
    {
        if (_byId.TryGetValue(assetId, out AssetRecord? record))
        {
            return new AssetReferenceNode(
                record.AssetId,
                record.RelativePath,
                record.Kind,
                depth,
                reachedFromAssetId,
                IsMissing: false);
        }
        return new AssetReferenceNode(
            assetId,
            "<missing>",
            "missing",
            depth,
            reachedFromAssetId,
            IsMissing: true);
    }

    private static IReadOnlyList<AssetReferenceNode> SortReferenceNodes(
        IEnumerable<AssetReferenceNode> nodes) =>
        Array.AsReadOnly(nodes
            .OrderBy(static node => node.Depth)
            .ThenBy(static node => node.IsMissing)
            .ThenBy(static node => node.RelativePath, StringComparer.Ordinal)
            .ThenBy(static node => node.AssetId, StringComparer.Ordinal)
            .ToArray());

    private IReadOnlyList<AssetDependencyCycle> FindDependencyCycles(string rootAssetId)
    {
        var state = new Dictionary<string, byte>(IdComparer);
        var path = new List<string>();
        var stackIndex = new Dictionary<string, int>(IdComparer);
        var cycles = new SortedDictionary<string, AssetDependencyCycle>(StringComparer.Ordinal);
        var frames = new Stack<(string AssetId, string[] Dependencies, int NextIndex)>();

        void Push(string assetId)
        {
            state[assetId] = 1;
            stackIndex[assetId] = path.Count;
            path.Add(assetId);
            string[] dependencies = _byId.TryGetValue(assetId, out AssetRecord? record)
                ? record.Metadata.Dependencies
                    .Where(_byId.ContainsKey)
                    .OrderBy(static id => id, StringComparer.Ordinal)
                    .ToArray()
                : Array.Empty<string>();
            frames.Push((assetId, dependencies, 0));
        }

        Push(rootAssetId);
        while (frames.Count != 0)
        {
            (string assetId, string[] dependencies, int nextIndex) = frames.Pop();
            if (nextIndex >= dependencies.Length)
            {
                if (path.Count == 0 ||
                    !string.Equals(path[^1], assetId, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Dependency traversal stack is inconsistent.");
                path.RemoveAt(path.Count - 1);
                stackIndex.Remove(assetId);
                state[assetId] = 2;
                continue;
            }

            string dependency = dependencies[nextIndex];
            frames.Push((assetId, dependencies, nextIndex + 1));
            state.TryGetValue(dependency, out byte dependencyState);
            if (dependencyState == 0)
            {
                Push(dependency);
            }
            else if (dependencyState == 1 &&
                     stackIndex.TryGetValue(dependency, out int start))
            {
                AssetDependencyCycle cycle = CanonicalizeCycle(
                    path.Skip(start).Append(dependency).ToArray());
                string key = string.Join(">", cycle.AssetIds);
                cycles.TryAdd(key, cycle);
            }
        }

        return Array.AsReadOnly(cycles.Values.ToArray());
    }

    private static AssetDependencyCycle CanonicalizeCycle(IReadOnlyList<string> cycle)
    {
        if (cycle.Count < 2 ||
            !string.Equals(cycle[0], cycle[^1], StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Cycle must repeat its first asset id.");

        string[] body = cycle.Take(cycle.Count - 1).ToArray();
        string[]? best = null;
        for (int start = 0; start < body.Length; ++start)
        {
            var rotated = new string[body.Length];
            for (int i = 0; i < body.Length; ++i)
                rotated[i] = body[(start + i) % body.Length];
            if (best == null || CompareAssetIdSequence(rotated, best) < 0)
                best = rotated;
        }

        string[] canonical = new string[body.Length + 1];
        Array.Copy(best!, canonical, body.Length);
        canonical[^1] = canonical[0];
        return new AssetDependencyCycle(Array.AsReadOnly(canonical));
    }

    private static int CompareAssetIdSequence(
        IReadOnlyList<string> left,
        IReadOnlyList<string> right)
    {
        for (int i = 0; i < Math.Min(left.Count, right.Count); ++i)
        {
            int comparison = string.CompareOrdinal(left[i], right[i]);
            if (comparison != 0)
                return comparison;
        }
        return left.Count.CompareTo(right.Count);
    }

    private static int ValidateGraphDepth(int maxDepth)
    {
        if (maxDepth is < 1 or > 256)
            throw new ArgumentOutOfRangeException(
                nameof(maxDepth),
                "Reference traversal depth must be between 1 and 256.");
        return maxDepth;
    }

    private AssetRecord GetRequiredRecord(string assetId)
    {
        string normalized = NormalizeAssetIdForLookup(assetId);
        if (!_byId.TryGetValue(normalized, out AssetRecord? record))
            throw new KeyNotFoundException($"Unknown asset id: {assetId}");
        return record;
    }

    private void EnsureDatabaseRoots(bool createStorage)
    {
        EnsureSafeDirectory(_projectRoot, createIfMissing: false);
        EnsureSafeDirectory(_assetsRoot, createIfMissing: createStorage);
        if (createStorage || Directory.Exists(_indexDirectory))
            EnsureSafeDirectory(_indexDirectory, createIfMissing: createStorage);
    }

    private IEnumerable<DiscoveredAsset> EnumerateAssets(
        List<string> warnings,
        CancellationToken cancellationToken)
    {
        var pending = new Stack<string>();
        pending.Push(_assetsRoot);

        while (pending.Count != 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string directory = pending.Pop();
            FileSystemInfo[] entries;
            try
            {
                EnsureSafeDirectory(directory, createIfMissing: false);
                entries = new DirectoryInfo(directory)
                    .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly)
                    .OrderBy(static entry => entry.Name, StringComparer.Ordinal)
                    .ToArray();
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                warnings.Add($"Directory skipped '{DisplayPath(directory)}': {ex.Message}");
                continue;
            }

            foreach (FileSystemInfo entry in entries)
            {
                cancellationToken.ThrowIfCancellationRequested();
                try { entry.Refresh(); }
                catch (IOException ex)
                {
                    warnings.Add($"Entry skipped '{DisplayPath(entry.FullName)}': {ex.Message}");
                    continue;
                }

                FileAttributes attributes;
                try { attributes = entry.Attributes; }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    warnings.Add($"Entry skipped '{DisplayPath(entry.FullName)}': {ex.Message}");
                    continue;
                }

                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    warnings.Add($"Reparse point skipped: {DisplayPath(entry.FullName)}");
                    continue;
                }

                if ((attributes & FileAttributes.Directory) != 0)
                {
                    if (PathEquals(entry.FullName, _indexDirectory))
                        continue;
                    pending.Push(entry.FullName);
                    continue;
                }

                if (entry.Name.EndsWith(MetadataSuffix, StringComparison.OrdinalIgnoreCase) ||
                    IsMaterialGraphCompanionName(entry.Name) ||
                    IsTemporaryMetadataName(entry.Name) ||
                    entry.Name.Contains(".tmp-", StringComparison.OrdinalIgnoreCase))
                    continue;

                if (entry is not FileInfo file)
                    continue;
                string relative = NormalizeAssetRelativePath(file.FullName);
                yield return new DiscoveredAsset(
                    relative,
                    file.FullName,
                    file.Length,
                    file.LastWriteTimeUtc.Ticks);
            }
        }
    }

    private string GetContentHash(
        DiscoveredAsset item,
        CachedRecord? cached,
        bool verifyContent)
    {
        if (!verifyContent &&
            cached != null &&
            cached.SizeBytes == item.SizeBytes &&
            cached.LastWriteUtcTicks == item.LastWriteUtcTicks &&
            IsSha256(cached.ContentHash))
            return cached.ContentHash;

        EnsureSafeOrdinaryFile(item.FullPath, allowMetadata: false);
        using var stream = new FileStream(
            item.FullPath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private Dictionary<string, CachedRecord> LoadIndex(List<string> warnings)
    {
        var result = new Dictionary<string, CachedRecord>(PathComparer);
        if (!File.Exists(_indexPath))
            return result;

        try
        {
            EnsureSafeOrdinaryFile(_indexPath, allowMetadata: true);
            var info = new FileInfo(_indexPath);
            if (info.Length > 64 * 1024 * 1024)
                throw new InvalidDataException("Index exceeds the 64 MiB safety limit.");
            byte[] data = File.ReadAllBytes(_indexPath);
            using JsonDocument document = JsonDocument.Parse(data, StrictDocumentOptions);
            JsonElement root = document.RootElement;
            RequireObject(root, "index");
            if (ReadRequiredInt(root, "schemaVersion") != CurrentSchemaVersion)
                throw new InvalidDataException("Unsupported asset index schema.");

            JsonElement assets = ReadRequired(root, "assets");
            if (assets.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("'assets' must be an array.");
            foreach (JsonElement element in assets.EnumerateArray())
            {
                RequireObject(element, "asset index entry");
                string relative = NormalizeAssetRelativePath(ReadRequiredString(element, "path"));
                if (IsMaterialGraphCompanionName(Path.GetFileName(relative)))
                    continue;
                var metadata = new AssetMetadata(
                    CurrentSchemaVersion,
                    ReadRequiredString(element, "id"),
                    ReadRequiredString(element, "kind"),
                    ReadRequiredString(element, "source"),
                    ReadRequiredString(element, "importer"),
                    ReadRequiredInt(element, "importerVersion"),
                    ReadStringArray(element, "dependencies"),
                    ReadStringMap(element, "importSettings"));
                metadata = ValidateAndNormalizeMetadata(metadata, relative);
                var cached = new CachedRecord(
                    relative,
                    ReadRequiredLong(element, "size"),
                    ReadRequiredLong(element, "lastWriteUtcTicks"),
                    ReadRequiredString(element, "contentHash"),
                    metadata);
                if (!IsSha256(cached.ContentHash))
                    throw new InvalidDataException($"Invalid content hash for '{relative}'.");
                if (!result.TryAdd(relative, cached))
                    throw new InvalidDataException($"Duplicate index path '{relative}'.");
            }
        }
        catch (Exception ex) when (
            ex is IOException or UnauthorizedAccessException or InvalidDataException or JsonException)
        {
            warnings.Add($"Asset index cache ignored: {ex.Message}");
            result.Clear();
        }
        return result;
    }

    private void WriteIndex(IEnumerable<AssetRecord> records)
    {
        EnsureSafeDirectory(_indexDirectory, createIfMissing: true);
        byte[] bytes = SerializeIndex(records);
        AtomicWrite(_indexPath, bytes);
    }

    private byte[] SerializeIndex(IEnumerable<AssetRecord> records)
    {
        using var memory = new MemoryStream();
        using (var writer = new Utf8JsonWriter(memory, PrettyJsonOptions))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", CurrentSchemaVersion);
            writer.WriteStartArray("assets");
            foreach (AssetRecord record in records
                .OrderBy(static item => item.RelativePath, StringComparer.Ordinal))
            {
                writer.WriteStartObject();
                writer.WriteString("path", record.RelativePath);
                writer.WriteString("id", record.AssetId);
                writer.WriteString("kind", record.Kind);
                writer.WriteNumber("size", record.SizeBytes);
                writer.WriteNumber("lastWriteUtcTicks", record.LastWriteUtcTicks);
                writer.WriteString("contentHash", record.ContentHash);
                WriteImportFields(writer, record.Metadata);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        return AddFinalNewline(memory.ToArray());
    }

    private AssetMetadata ReadMetadata(string path)
    {
        var info = new FileInfo(path);
        if (info.Length > MaxMetadataBytes)
            throw new InvalidDataException("Metadata exceeds the 1 MiB safety limit.");
        byte[] data = File.ReadAllBytes(path);
        using JsonDocument document = JsonDocument.Parse(data, StrictDocumentOptions);
        JsonElement root = document.RootElement;
        RequireObject(root, "metadata");
        return new AssetMetadata(
            ReadRequiredInt(root, "schemaVersion"),
            ReadRequiredString(root, "id"),
            ReadRequiredString(root, "kind"),
            ReadRequiredString(root, "source"),
            ReadRequiredString(root, "importer"),
            ReadRequiredInt(root, "importerVersion"),
            ReadStringArray(root, "dependencies"),
            ReadStringMap(root, "importSettings"));
    }

    private void WriteMetadata(string path, AssetMetadata metadata)
    {
        string assetPath = path[..^MetadataSuffix.Length];
        string relative = NormalizeAssetRelativePath(assetPath);
        AssetMetadata normalized = ValidateAndNormalizeMetadata(metadata, relative);
        using var memory = new MemoryStream();
        using (var writer = new Utf8JsonWriter(memory, PrettyJsonOptions))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", normalized.SchemaVersion);
            writer.WriteString("id", normalized.AssetId);
            writer.WriteString("kind", normalized.Kind);
            WriteImportFields(writer, normalized);
            writer.WriteEndObject();
        }
        AtomicWrite(path, AddFinalNewline(memory.ToArray()));
    }

    private static void WriteImportFields(Utf8JsonWriter writer, AssetMetadata metadata)
    {
        writer.WriteString("source", metadata.Source);
        writer.WriteString("importer", metadata.Importer);
        writer.WriteNumber("importerVersion", metadata.ImporterVersion);
        writer.WriteStartArray("dependencies");
        foreach (string dependency in metadata.Dependencies.OrderBy(static id => id, StringComparer.Ordinal))
            writer.WriteStringValue(dependency);
        writer.WriteEndArray();
        writer.WriteStartObject("importSettings");
        foreach (KeyValuePair<string, string> setting in metadata.ImportSettings
            .OrderBy(static item => item.Key, StringComparer.Ordinal))
            writer.WriteString(setting.Key, setting.Value);
        writer.WriteEndObject();
    }

    private void AtomicWrite(string destination, byte[] bytes)
    {
        string full = Path.GetFullPath(destination);
        if (!IsUnder(full, _assetsRoot))
            throw new InvalidDataException("Metadata destination escapes Assets.");
        string parent = Path.GetDirectoryName(full)
            ?? throw new InvalidDataException("Metadata destination has no parent.");
        EnsureSafeDirectory(parent, createIfMissing: false);

        string temporary = full + ".tmp-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
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
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            EnsureSafeDirectory(parent, createIfMissing: false);
            File.Move(temporary, full, overwrite: true);
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); }
            catch { }
        }
    }

    private AssetMetadata CreateDefaultMetadata(string relativePath)
    {
        string extension = Path.GetExtension(relativePath).ToLowerInvariant();
        (string importer, int importerVersion) = extension switch
        {
            ".acscene" => ("legacy-acscene", 1),
            ".acs3d" => ("legacy-acs3d", 2),
            ".acsmat" => ("material", 1),
            ".acsbp" => ("blueprint", 1),
            ".acsprefab" => ("prefab", 1),
            _ => ("passthrough", 1),
        };
        return new(
            CurrentSchemaVersion,
            Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture),
            ClassifyExtension(extension),
            relativePath,
            importer,
            importerVersion,
            Array.Empty<string>(),
            new ReadOnlyDictionary<string, string>(
                new SortedDictionary<string, string>(StringComparer.Ordinal)));
    }

    private AssetMetadata ValidateAndNormalizeMetadata(
        AssetMetadata metadata,
        string relativePath)
    {
        if (metadata.SchemaVersion != CurrentSchemaVersion)
            throw new InvalidDataException(
                $"Unsupported metadata schema {metadata.SchemaVersion}.");
        string id = NormalizeAssetId(metadata.AssetId);
        string kind = NormalizeMetadataText(metadata.Kind, "kind", 64);
        if (kind.Length == 0)
            kind = ClassifyExtension(Path.GetExtension(relativePath));
        string importer = NormalizeMetadataText(metadata.Importer, "importer", 128);
        if (importer.Length == 0)
            throw new InvalidDataException("Importer cannot be empty.");
        if (metadata.ImporterVersion < 0)
            throw new InvalidDataException("Importer version cannot be negative.");
        IReadOnlyList<string> dependencies = NormalizeDependencies(metadata.Dependencies);
        if (dependencies.Contains(id, StringComparer.Ordinal))
            throw new InvalidDataException("An asset cannot depend on itself.");

        return metadata with
        {
            AssetId = id,
            Kind = kind,
            Source = NormalizeMetadataText(metadata.Source, "source", 4096),
            Importer = importer,
            Dependencies = dependencies,
            ImportSettings = NormalizeImportSettings(metadata.ImportSettings),
        };
    }

    private static IReadOnlyList<string> NormalizeDependencies(IEnumerable<string>? dependencies)
    {
        var result = new SortedSet<string>(StringComparer.Ordinal);
        foreach (string dependency in dependencies ?? Array.Empty<string>())
            result.Add(NormalizeAssetId(dependency));
        return Array.AsReadOnly(result.ToArray());
    }

    private static IReadOnlyDictionary<string, string> NormalizeImportSettings(
        IEnumerable<KeyValuePair<string, string>>? settings)
    {
        var result = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (KeyValuePair<string, string> setting in
            settings ?? Array.Empty<KeyValuePair<string, string>>())
        {
            string key = NormalizeMetadataText(setting.Key, "import setting key", 256);
            if (key.Length == 0)
                throw new InvalidDataException("Import setting key cannot be empty.");
            if (!result.TryAdd(
                key,
                NormalizeMetadataText(setting.Value, "import setting value", 4096)))
                throw new InvalidDataException($"Duplicate import setting '{key}'.");
        }
        return new ReadOnlyDictionary<string, string>(result);
    }

    private static string NormalizeMetadataText(string? value, string field, int maxLength)
    {
        string result = (value ?? "").Replace('\\', '/').Trim();
        if (result.Length > maxLength)
            throw new InvalidDataException($"{field} exceeds {maxLength} characters.");
        if (result.IndexOf('\0') >= 0 ||
            result.Any(static c => char.IsControl(c) && c is not '\t'))
            throw new InvalidDataException($"{field} contains control characters.");
        return result;
    }

    private static string NormalizeAssetIdForLookup(string assetId)
    {
        try { return NormalizeAssetId(assetId); }
        catch (InvalidDataException) { return ""; }
    }

    private static string NormalizeAssetId(string? assetId)
    {
        string value = assetId?.Trim() ?? "";
        if (value.Length != 32 ||
            !Guid.TryParseExact(value, "N", out Guid parsed) ||
            parsed == Guid.Empty)
            throw new InvalidDataException("Asset id must be a non-zero 32-digit GUID.");
        return parsed.ToString("N", CultureInfo.InvariantCulture);
    }

    private string NormalizeAssetRelativePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new InvalidDataException("Asset path is empty.");
        string full = Path.IsPathRooted(path)
            ? Path.GetFullPath(path)
            : Path.GetFullPath(Path.Combine(_assetsRoot, path.Replace('/', Path.DirectorySeparatorChar)));
        if (!IsUnder(full, _assetsRoot))
            throw new InvalidDataException("Asset path escapes Assets.");
        string relative = Path.GetRelativePath(_assetsRoot, full).Replace('\\', '/');
        if (relative.Length == 0 || relative == "." ||
            relative.Equals(InternalDirectoryName, StringComparison.OrdinalIgnoreCase) ||
            relative.StartsWith(InternalDirectoryName + "/", StringComparison.OrdinalIgnoreCase) ||
            relative.EndsWith(MetadataSuffix, StringComparison.OrdinalIgnoreCase) ||
            IsTemporaryMetadataName(Path.GetFileName(relative)))
            throw new InvalidDataException("Path is reserved for the asset database.");
        return relative;
    }

    private string NormalizeFolderRelativePath(string path)
    {
        if (path is "" or ".")
            return "";
        string full = Path.IsPathRooted(path)
            ? Path.GetFullPath(path)
            : Path.GetFullPath(Path.Combine(_assetsRoot, path.Replace('/', Path.DirectorySeparatorChar)));
        if (!IsUnderOrEqual(full, _assetsRoot))
            throw new InvalidDataException("Folder path escapes Assets.");
        string relative = Path.GetRelativePath(_assetsRoot, full).Replace('\\', '/');
        return relative == "." ? "" : relative.TrimEnd('/');
    }

    private string ResolveAssetPath(string relativePath) =>
        Path.Combine(_assetsRoot, NormalizeAssetRelativePath(relativePath)
            .Replace('/', Path.DirectorySeparatorChar));

    private void EnsureSafeOrdinaryFile(string path, bool allowMetadata)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnder(full, _assetsRoot))
            throw new InvalidDataException("File escapes Assets.");
        if (!allowMetadata && full.EndsWith(MetadataSuffix, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Metadata is not an asset.");
        string parent = Path.GetDirectoryName(full)
            ?? throw new InvalidDataException("File has no parent directory.");
        EnsureSafeDirectory(parent, createIfMissing: false);
        FileAttributes attributes = File.GetAttributes(full);
        if ((attributes & FileAttributes.Directory) != 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("Only ordinary files are allowed.");
    }

    private void EnsureSafeDirectory(string path, bool createIfMissing)
    {
        string full = NormalizeDirectory(path);
        if (!IsUnderOrEqual(full, _projectRoot))
            throw new InvalidDataException("Directory escapes the project root.");

        if (createIfMissing)
        {
            string? existing = full;
            var missing = new Stack<string>();
            while (existing != null && !Directory.Exists(existing))
            {
                missing.Push(existing);
                existing = Path.GetDirectoryName(existing);
            }
            if (existing == null || !IsUnderOrEqual(existing, _projectRoot))
                throw new InvalidDataException("Directory has no safe project ancestor.");
            EnsureNoReparseSegments(existing);
            while (missing.Count != 0)
            {
                string next = missing.Pop();
                Directory.CreateDirectory(next);
                FileAttributes createdAttributes = File.GetAttributes(next);
                if ((createdAttributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException($"Created directory is a reparse point: {next}");
            }
        }

        if (!Directory.Exists(full))
            throw new DirectoryNotFoundException(full);
        EnsureNoReparseSegments(full);
    }

    private void EnsureNoReparseSegments(string directory)
    {
        string full = NormalizeDirectory(directory);
        if (!IsUnderOrEqual(full, _projectRoot))
            throw new InvalidDataException("Directory escapes the project root.");

        string relative = Path.GetRelativePath(_projectRoot, full);
        string cursor = _projectRoot;
        CheckDirectoryNotReparse(cursor);
        if (relative == ".")
            return;
        foreach (string segment in relative.Split(
            new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
            StringSplitOptions.RemoveEmptyEntries))
        {
            cursor = Path.Combine(cursor, segment);
            CheckDirectoryNotReparse(cursor);
        }
    }

    private static void CheckDirectoryNotReparse(string path)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0)
            throw new InvalidDataException($"Expected a directory: {path}");
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"Reparse directory is not allowed: {path}");
    }

    private void TryDeleteOrphanMetadata(string oldRelativePath, List<string> warnings)
    {
        try
        {
            string oldAssetPath = ResolveAssetPath(oldRelativePath);
            if (File.Exists(oldAssetPath))
                return;
            string oldMetadata = MetadataPath(oldAssetPath);
            if (!File.Exists(oldMetadata))
                return;
            EnsureSafeOrdinaryFile(oldMetadata, allowMetadata: true);
            File.Delete(oldMetadata);
        }
        catch (Exception ex) when (
            ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            warnings.Add($"Orphan metadata could not be removed for '{oldRelativePath}': {ex.Message}");
        }
    }

    private static bool CachedContentMatches(
        CachedRecord cached,
        DiscoveredAsset current,
        string hash) =>
        cached.SizeBytes == current.SizeBytes &&
        string.Equals(cached.ContentHash, hash, StringComparison.OrdinalIgnoreCase);

    private string DisplayPath(string fullPath)
    {
        string full = Path.GetFullPath(fullPath);
        return IsUnderOrEqual(full, _assetsRoot)
            ? "Assets/" + Path.GetRelativePath(_assetsRoot, full).Replace('\\', '/')
            : full;
    }

    public static string ClassifyExtension(string? extension)
    {
        string ext = (extension ?? "").ToLowerInvariant();
        return ext switch
        {
            ".png" or ".jpg" or ".jpeg" or ".bmp" or ".tga" or ".dds" or
                ".ktx" or ".hdr" or ".gif" => "image",
            ".wav" or ".ogg" or ".mp3" or ".flac" => "audio",
            ".fbx" or ".gltf" or ".glb" or ".obj" or ".mdl" => "mesh",
            ".txt" or ".json" or ".xml" or ".yaml" or ".yml" or ".toml" or
                ".ini" or ".csv" or ".md" or ".log" or ".hlsl" or ".glsl" or
                ".lua" => "text",
            ".acscene" or ".acs3d" => "scene",
            ".acsproject" => "project",
            ".acsmat" => "material",
            ".acsprefab" => "prefab",
            ".acsbp" => "blueprint",
            _ => "file",
        };
    }

    private static string MetadataPath(string assetPath) => assetPath + MetadataSuffix;

    private static bool IsMaterialGraphCompanionName(string name) =>
        name.EndsWith(".acsmat.graph.json", StringComparison.OrdinalIgnoreCase);

    private static bool IsTemporaryMetadataName(string name) =>
        name.Contains(MetadataSuffix + ".tmp-", StringComparison.OrdinalIgnoreCase) ||
        name.StartsWith("index.v1.json.tmp-", StringComparison.OrdinalIgnoreCase);

    private static string NormalizeDirectory(string path) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    private static bool IsUnder(string candidate, string root)
    {
        string relative = Path.GetRelativePath(root, candidate);
        return relative != "." &&
               !Path.IsPathRooted(relative) &&
               relative != ".." &&
               !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
               !relative.StartsWith(".." + Path.AltDirectorySeparatorChar, StringComparison.Ordinal);
    }

    private static bool IsUnderOrEqual(string candidate, string root) =>
        PathEquals(candidate, root) || IsUnder(candidate, root);

    private static bool PathEquals(string left, string right) =>
        PathComparer.Equals(NormalizeDirectory(left), NormalizeDirectory(right));

    private static bool IsSha256(string value) =>
        value.Length == 64 && value.All(static c =>
            c is >= '0' and <= '9' or >= 'a' and <= 'f' or >= 'A' and <= 'F');

    private static byte[] AddFinalNewline(byte[] bytes)
    {
        if (bytes.Length != 0 && bytes[^1] == (byte)'\n')
            return bytes;
        var result = new byte[bytes.Length + 1];
        bytes.CopyTo(result, 0);
        result[^1] = (byte)'\n';
        return result;
    }

    private static readonly JsonDocumentOptions StrictDocumentOptions = new()
    {
        AllowTrailingCommas = false,
        CommentHandling = JsonCommentHandling.Disallow,
        MaxDepth = 32,
    };

    private static readonly JsonWriterOptions PrettyJsonOptions = new()
    {
        Indented = true,
        SkipValidation = false,
    };

    private static JsonElement ReadRequired(JsonElement parent, string name)
    {
        if (!parent.TryGetProperty(name, out JsonElement value))
            throw new InvalidDataException($"Missing required property '{name}'.");
        return value;
    }

    private static string ReadRequiredString(JsonElement parent, string name)
    {
        JsonElement value = ReadRequired(parent, name);
        if (value.ValueKind != JsonValueKind.String)
            throw new InvalidDataException($"'{name}' must be a string.");
        return value.GetString() ?? "";
    }

    private static int ReadRequiredInt(JsonElement parent, string name)
    {
        JsonElement value = ReadRequired(parent, name);
        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out int result))
            throw new InvalidDataException($"'{name}' must be a 32-bit integer.");
        return result;
    }

    private static long ReadRequiredLong(JsonElement parent, string name)
    {
        JsonElement value = ReadRequired(parent, name);
        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt64(out long result))
            throw new InvalidDataException($"'{name}' must be a 64-bit integer.");
        return result;
    }

    private static IReadOnlyList<string> ReadStringArray(JsonElement parent, string name)
    {
        JsonElement value = ReadRequired(parent, name);
        if (value.ValueKind != JsonValueKind.Array)
            throw new InvalidDataException($"'{name}' must be an array.");
        var result = new List<string>();
        foreach (JsonElement item in value.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String)
                throw new InvalidDataException($"'{name}' entries must be strings.");
            result.Add(item.GetString() ?? "");
        }
        return result;
    }

    private static IReadOnlyDictionary<string, string> ReadStringMap(
        JsonElement parent,
        string name)
    {
        JsonElement value = ReadRequired(parent, name);
        if (value.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException($"'{name}' must be an object.");
        var result = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (JsonProperty property in value.EnumerateObject())
        {
            if (property.Value.ValueKind != JsonValueKind.String)
                throw new InvalidDataException($"'{name}' values must be strings.");
            if (!result.TryAdd(property.Name, property.Value.GetString() ?? ""))
                throw new InvalidDataException($"Duplicate property '{property.Name}'.");
        }
        return result;
    }

    private static void RequireObject(JsonElement element, string label)
    {
        if (element.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException($"{label} must be an object.");
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (!names.Add(property.Name))
                throw new InvalidDataException(
                    $"{label} contains duplicate property '{property.Name}'.");
        }
    }

    private sealed record DiscoveredAsset(
        string RelativePath,
        string FullPath,
        long SizeBytes,
        long LastWriteUtcTicks);

    private sealed record CachedRecord(
        string RelativePath,
        long SizeBytes,
        long LastWriteUtcTicks,
        string ContentHash,
        AssetMetadata Metadata);
}
