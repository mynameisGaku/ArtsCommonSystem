// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;

namespace AcsEditor;

/// <summary>
/// One folder in the Asset View Sources tree. The tree builder never follows reparse points.
/// </summary>
internal sealed class AssetBrowserSourceNode : INotifyPropertyChanged
{
    private bool _isCurrent;

    internal AssetBrowserSourceNode(
        string name,
        string fullPath,
        bool isExpanded = false)
    {
        Name = name;
        FullPath = fullPath;
        IsExpanded = isExpanded;
    }

    public string Name { get; }
    public string FullPath { get; }
    public bool IsExpanded { get; }
    public ObservableCollection<AssetBrowserSourceNode> Children { get; } = new();

    public bool IsCurrent
    {
        get => _isCurrent;
        set
        {
            if (_isCurrent == value) return;
            _isCurrent = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsCurrent)));
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}

internal sealed record AssetBrowserFavoriteView(
    string RelativePath,
    string DisplayName,
    string? FullPath,
    bool IsAvailable);

internal sealed record AssetBrowserCollectionView(
    string Name,
    int ItemCount)
{
    public string DisplayText => $"{Name}  ({ItemCount})";
}

/// <summary>
/// Builds the physical folder tree shown by Sources. Enumeration is bounded and skips every
/// reparse point, including a reparse hidden below an ordinary parent.
/// </summary>
internal static class AssetBrowserSourceTreeBuilder
{
    private const int MaxNodes = 4096;
    private const int MaxDepth = 64;

    internal static AssetBrowserSourceNode? Build(
        string assetsRoot,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                assetsRoot,
                assetsRoot,
                requireDirectory: true,
                out string safeRoot))
        {
            return null;
        }

        var root = new AssetBrowserSourceNode("Assets", safeRoot, isExpanded: true);
        int nodeCount = 1;
        Populate(
            root,
            safeRoot,
            depth: 0,
            ref nodeCount,
            cancellationToken);
        return root;
    }

    private static void Populate(
        AssetBrowserSourceNode parent,
        string assetsRoot,
        int depth,
        ref int nodeCount,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (depth >= MaxDepth || nodeCount >= MaxNodes) return;

        string[] directories;
        try
        {
            directories = Directory.GetDirectories(parent.FullPath);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException)
        {
            return;
        }

        foreach (string candidate in directories
                     .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (nodeCount >= MaxNodes) return;
            if (Path.GetFileName(candidate).Equals(
                    AssetDatabase.InternalDirectoryName,
                    StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            if (!AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                    assetsRoot,
                    candidate,
                    requireDirectory: true,
                    out string safeDirectory))
            {
                continue;
            }

            var child = new AssetBrowserSourceNode(
                Path.GetFileName(safeDirectory),
                safeDirectory);
            parent.Children.Add(child);
            nodeCount++;
            Populate(
                child,
                assetsRoot,
                depth + 1,
                ref nodeCount,
                cancellationToken);
        }
    }
}

/// <summary>
/// Project-local Favorites and Collections. Only Assets-relative paths are persisted. Each
/// mutation takes the project-wide asset lease, reloads the latest durable document, writes a
/// complete candidate beside the target, and atomically replaces it. Reload-under-lock prevents
/// two editor processes from silently overwriting one another's Favorites or Collections.
/// </summary>
internal sealed class AssetBrowserSourcesStore
{
    internal const int CurrentVersion = 1;
    internal const string RelativeStatePath =
        "Assets/.acsdb/editor/asset-sources.v1.json";

    private const int MaxFavorites = 256;
    private const int MaxCollections = 128;
    private const int MaxItemsPerCollection = 4096;
    private const long MaxStateBytes = 1024 * 1024;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
    };

    private readonly string _assetsRoot;
    private readonly string _stateDirectory;
    private readonly string _statePath;
    private StateDocument _state = new();

    internal AssetBrowserSourcesStore(
        string projectRoot,
        string assetsRoot,
        out string? loadWarning)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(projectRoot);
        ArgumentException.ThrowIfNullOrWhiteSpace(assetsRoot);

        string canonicalProjectRoot =
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        _assetsRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
        if (!AssetBrowserSourcePathPolicy.IsUnderOrEqual(
                _assetsRoot,
                canonicalProjectRoot))
            throw new ArgumentException("The Assets directory must be inside the project root.");
        if (!AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                canonicalProjectRoot,
                _assetsRoot,
                requireDirectory: true,
                out _))
        {
            throw new IOException("The Assets directory crosses a reparse point.");
        }

        _stateDirectory = Path.Combine(
            _assetsRoot,
            AssetDatabase.InternalDirectoryName,
            "editor");
        EnsureSafeStateDirectory();
        _statePath = Path.Combine(_stateDirectory, "asset-sources.v1.json");
        _state = Load(out loadWarning);
    }

    internal string StatePath => _statePath;

    internal IReadOnlyList<AssetBrowserFavoriteView> Favorites =>
        _state.Favorites
            .Select(relative =>
            {
                bool resolved = TryResolveExisting(relative, out string fullPath);
                bool available = resolved && Directory.Exists(fullPath);
                string name = relative == "."
                    ? "Assets"
                    : Path.GetFileName(relative.Replace('/', Path.DirectorySeparatorChar));
                if (name.Length == 0) name = relative;
                return new AssetBrowserFavoriteView(
                    relative,
                    available ? name : $"{name}  (Missing)",
                    available ? fullPath : null,
                    available);
            })
            .ToArray();

    internal IReadOnlyList<AssetBrowserCollectionView> Collections =>
        _state.Collections
            .OrderBy(static collection => collection.Name, StringComparer.OrdinalIgnoreCase)
            .Select(static collection =>
                new AssetBrowserCollectionView(collection.Name, collection.Items.Count))
            .ToArray();

    internal bool AddFavorite(string fullDirectory)
    {
        if (!TryNormalizeExisting(
                fullDirectory,
                requireDirectory: true,
                out string relative))
        {
            throw new IOException("Favorites must reference a safe folder inside Assets.");
        }
        return Commit(candidate =>
        {
            if (candidate.Favorites.Contains(relative, StringComparer.OrdinalIgnoreCase))
                return false;
            if (candidate.Favorites.Count >= MaxFavorites)
                throw new InvalidOperationException($"Favorites are limited to {MaxFavorites} folders.");
            candidate.Favorites.Add(relative);
            candidate.Favorites.Sort(StringComparer.OrdinalIgnoreCase);
            return true;
        });
    }

    internal bool RemoveFavorite(string relativePath)
    {
        string normalized = AssetBrowserSourcePathPolicy.NormalizePersistedRelativePath(
            relativePath);
        return Commit(candidate =>
        {
            int index = candidate.Favorites.FindIndex(path =>
                path.Equals(normalized, StringComparison.OrdinalIgnoreCase));
            if (index < 0) return false;
            candidate.Favorites.RemoveAt(index);
            return true;
        });
    }

    internal bool CreateCollection(string name)
    {
        string normalized = NormalizeCollectionName(name);
        return Commit(candidate =>
        {
            if (candidate.Collections.Any(collection =>
                    collection.Name.Equals(normalized, StringComparison.OrdinalIgnoreCase)))
            {
                return false;
            }
            if (candidate.Collections.Count >= MaxCollections)
            {
                throw new InvalidOperationException(
                    $"Collections are limited to {MaxCollections} entries.");
            }
            candidate.Collections.Add(new CollectionDocument { Name = normalized });
            return true;
        });
    }

    internal bool DeleteCollection(string name)
    {
        string normalized = NormalizeCollectionName(name);
        return Commit(candidate =>
        {
            int index = candidate.Collections.FindIndex(collection =>
                collection.Name.Equals(normalized, StringComparison.OrdinalIgnoreCase));
            if (index < 0) return false;
            candidate.Collections.RemoveAt(index);
            return true;
        });
    }

    internal bool AddCollectionItems(string collectionName, IEnumerable<string> fullPaths)
    {
        ArgumentNullException.ThrowIfNull(fullPaths);
        string name = NormalizeCollectionName(collectionName);
        string[] relativePaths = fullPaths
            .Select(path =>
            {
                if (!TryNormalizeExisting(path, requireDirectory: false, out string relative))
                    throw new IOException("Collection items must stay inside Assets and cannot cross reparse points.");
                return relative;
            })
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
        if (relativePaths.Length == 0) return false;

        return Commit(candidate =>
        {
            CollectionDocument collection = FindCollection(candidate, name);
            var items = new HashSet<string>(
                collection.Items,
                StringComparer.OrdinalIgnoreCase);
            bool changed = false;
            foreach (string relative in relativePaths)
            {
                if (!items.Add(relative)) continue;
                if (items.Count > MaxItemsPerCollection)
                {
                    throw new InvalidOperationException(
                        $"A collection is limited to {MaxItemsPerCollection} items.");
                }
                changed = true;
            }
            if (!changed) return false;
            collection.Items = items
                .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
                .ToList();
            return true;
        });
    }

    internal bool RemoveCollectionItems(
        string collectionName,
        IEnumerable<string> fullPaths)
    {
        ArgumentNullException.ThrowIfNull(fullPaths);
        string name = NormalizeCollectionName(collectionName);
        string[] relativePaths = fullPaths
            .Select(path =>
            {
                if (!TryNormalizeExisting(path, requireDirectory: false, out string relative))
                    throw new IOException("Collection items must stay inside Assets and cannot cross reparse points.");
                return relative;
            })
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();

        return Commit(candidate =>
        {
            CollectionDocument collection = FindCollection(candidate, name);
            int removed = collection.Items.RemoveAll(item =>
                relativePaths.Contains(item, StringComparer.OrdinalIgnoreCase));
            return removed != 0;
        });
    }

    internal IReadOnlyList<string> GetCollectionPaths(string collectionName)
    {
        string name = NormalizeCollectionName(collectionName);
        CollectionDocument collection = FindCollection(_state, name);
        var result = new List<string>(collection.Items.Count);
        foreach (string relative in collection.Items)
        {
            if (TryResolveExisting(relative, out string fullPath))
                result.Add(fullPath);
        }
        return result;
    }

    /// <summary>
    /// Keeps saved source entries coherent with transactional Content Browser rename, move, and
    /// delete operations. A path that can no longer be proven safe is removed instead of guessed.
    /// </summary>
    internal bool ApplyPathChanges(AssetPathsChangedEventArgs change)
    {
        ArgumentNullException.ThrowIfNull(change);
        return Commit(candidate =>
        {
            bool changed = RewritePaths(candidate.Favorites, change);
            foreach (CollectionDocument collection in candidate.Collections)
                changed |= RewritePaths(collection.Items, change);
            return changed;
        });
    }

    internal bool TryResolveExisting(string relativePath, out string fullPath)
    {
        fullPath = "";
        string relative;
        try
        {
            relative = AssetBrowserSourcePathPolicy.NormalizePersistedRelativePath(relativePath);
        }
        catch (ArgumentException)
        {
            return false;
        }
        string candidate = relative == "."
            ? _assetsRoot
            : Path.Combine(
                _assetsRoot,
                relative.Replace('/', Path.DirectorySeparatorChar));
        return AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
            _assetsRoot,
            candidate,
            requireDirectory: false,
            out fullPath);
    }

    private bool TryNormalizeExisting(
        string path,
        bool requireDirectory,
        out string relative)
    {
        relative = "";
        if (!AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                _assetsRoot,
                path,
                requireDirectory,
                out string fullPath))
        {
            return false;
        }
        relative = Path.GetRelativePath(_assetsRoot, fullPath).Replace('\\', '/');
        if (relative.Length == 0) relative = ".";
        return true;
    }

    private bool RewritePaths(
        List<string> paths,
        AssetPathsChangedEventArgs change)
    {
        var rewritten = new List<string>(paths.Count);
        bool changed = false;
        foreach (string relative in paths)
        {
            string original = relative == "."
                ? _assetsRoot
                : Path.Combine(
                    _assetsRoot,
                    relative.Replace('/', Path.DirectorySeparatorChar));
            if (change.IsDeletedPath(original))
            {
                changed = true;
                continue;
            }

            string destination = original;
            bool remapped = change.TryRemapPath(original, out string mapped);
            if (remapped) destination = mapped;
            if (!TryNormalizeExisting(
                    destination,
                    requireDirectory: false,
                    out string destinationRelative))
            {
                // Unaffected missing entries remain removable from the UI. A mapped path that is
                // missing or unsafe is discarded because carrying the old root would be wrong.
                if (!remapped)
                {
                    destinationRelative = relative;
                }
                else
                {
                    changed = true;
                    continue;
                }
            }
            changed |= !relative.Equals(
                destinationRelative,
                StringComparison.OrdinalIgnoreCase);
            if (!rewritten.Contains(
                    destinationRelative,
                    StringComparer.OrdinalIgnoreCase))
            {
                rewritten.Add(destinationRelative);
            }
            else
            {
                changed = true;
            }
        }
        if (!changed) return false;
        paths.Clear();
        paths.AddRange(rewritten.OrderBy(
            static path => path,
            StringComparer.OrdinalIgnoreCase));
        return true;
    }

    private StateDocument Load(out string? warning)
    {
        warning = null;
        try
        {
            return ReadStateDocument();
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
            InvalidDataException or JsonException or ArgumentException)
        {
            warning = "Asset View saved sources could not be loaded: " + error.Message;
            return new StateDocument();
        }
    }

    private StateDocument ReadStateDocument()
    {
        if (!File.Exists(_statePath)) return new StateDocument();
        FileAttributes attributes = File.GetAttributes(_statePath);
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new IOException("The state file is a reparse point.");
        var info = new FileInfo(_statePath);
        if (info.Length > MaxStateBytes)
            throw new InvalidDataException("The state file exceeds the 1 MiB safety limit.");
        using var stream = new FileStream(
            _statePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 16 * 1024,
            FileOptions.SequentialScan);
        StateDocument? document =
            JsonSerializer.Deserialize<StateDocument>(stream, JsonOptions);
        if (document == null)
            throw new InvalidDataException("The state document is empty.");
        if (document.Version != CurrentVersion)
        {
            throw new InvalidDataException(
                $"Asset View saved sources use unsupported schema version {document.Version}.");
        }
        return Sanitize(document);
    }

    private StateDocument Sanitize(StateDocument source)
    {
        var sanitized = new StateDocument { Version = CurrentVersion };
        foreach (string raw in source.Favorites ?? new List<string>())
        {
            if (sanitized.Favorites.Count >= MaxFavorites) break;
            try
            {
                string relative =
                    AssetBrowserSourcePathPolicy.NormalizePersistedRelativePath(raw);
                if (!sanitized.Favorites.Contains(
                        relative,
                        StringComparer.OrdinalIgnoreCase))
                {
                    sanitized.Favorites.Add(relative);
                }
            }
            catch (ArgumentException)
            {
                // Ignore hostile or obsolete entries. They are never resolved outside Assets.
            }
        }

        foreach (CollectionDocument rawCollection in
                 source.Collections ?? new List<CollectionDocument>())
        {
            if (sanitized.Collections.Count >= MaxCollections) break;
            string name;
            try
            {
                name = NormalizeCollectionName(rawCollection.Name);
            }
            catch (ArgumentException)
            {
                continue;
            }
            if (sanitized.Collections.Any(collection =>
                    collection.Name.Equals(name, StringComparison.OrdinalIgnoreCase)))
            {
                continue;
            }

            var collection = new CollectionDocument { Name = name };
            foreach (string rawItem in rawCollection.Items ?? new List<string>())
            {
                if (collection.Items.Count >= MaxItemsPerCollection) break;
                try
                {
                    string relative =
                        AssetBrowserSourcePathPolicy.NormalizePersistedRelativePath(rawItem);
                    if (!collection.Items.Contains(
                            relative,
                            StringComparer.OrdinalIgnoreCase))
                    {
                        collection.Items.Add(relative);
                    }
                }
                catch (ArgumentException)
                {
                    // Same fail-closed rule as Favorites.
                }
            }
            sanitized.Collections.Add(collection);
        }
        return sanitized;
    }

    private bool Commit(Func<StateDocument, bool> mutation)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Update Asset View saved sources");
        StateDocument authoritative = ReadStateDocument();
        StateDocument candidate = Clone(authoritative);
        if (!mutation(candidate))
        {
            // Even a no-op observes commits made by another editor since this instance loaded.
            _state = authoritative;
            return false;
        }
        SaveAtomically(candidate);
        _state = candidate;
        return true;
    }

    private void SaveAtomically(StateDocument document)
    {
        EnsureSafeStateDirectory();
        string token = Guid.NewGuid().ToString("N");
        string temporaryPath = Path.Combine(
            _stateDirectory,
            $".asset-sources.{token}.tmp");
        string backupPath = Path.Combine(
            _stateDirectory,
            $".asset-sources.{token}.bak");
        try
        {
            using (var stream = new FileStream(
                       temporaryPath,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       bufferSize: 16 * 1024,
                       FileOptions.WriteThrough))
            {
                JsonSerializer.Serialize(stream, document, JsonOptions);
                stream.Flush(flushToDisk: true);
            }

            if (File.Exists(_statePath))
            {
                FileAttributes attributes = File.GetAttributes(_statePath);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Refusing to replace a reparse-point state file.");
                File.Replace(
                    temporaryPath,
                    _statePath,
                    backupPath,
                    ignoreMetadataErrors: true);
                TryDelete(backupPath);
            }
            else
            {
                File.Move(temporaryPath, _statePath);
            }
        }
        finally
        {
            TryDelete(temporaryPath);
            TryDelete(backupPath);
        }
    }

    private void EnsureSafeStateDirectory()
    {
        string current = _assetsRoot;
        foreach (string segment in new[]
                 {
                     AssetDatabase.InternalDirectoryName,
                     "editor",
                 })
        {
            current = Path.Combine(current, segment);
            if (!Directory.Exists(current)) Directory.CreateDirectory(current);
            FileAttributes attributes = File.GetAttributes(current);
            if ((attributes & FileAttributes.Directory) == 0 ||
                (attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException(
                    "The Asset View state directory crosses a reparse point.");
            }
        }
    }

    private static CollectionDocument FindCollection(
        StateDocument state,
        string normalizedName) =>
        state.Collections.FirstOrDefault(collection =>
            collection.Name.Equals(normalizedName, StringComparison.OrdinalIgnoreCase))
        ?? throw new KeyNotFoundException($"Collection '{normalizedName}' was not found.");

    private static string NormalizeCollectionName(string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        string normalized = name.Trim();
        if (normalized.Length > 64)
            throw new ArgumentException("Collection names are limited to 64 characters.");
        if (normalized.Any(character =>
                char.IsControl(character) ||
                character is '/' or '\\' or ':' or '*' or '?' or '"' or '<' or '>' or '|'))
        {
            throw new ArgumentException("The collection name contains an invalid character.");
        }
        return normalized;
    }

    private static StateDocument Clone(StateDocument source) => new()
    {
        Version = CurrentVersion,
        Favorites = source.Favorites.ToList(),
        Collections = source.Collections.Select(collection => new CollectionDocument
        {
            Name = collection.Name,
            Items = collection.Items.ToList(),
        }).ToList(),
    };

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch
        {
            // The committed state is authoritative; orphan cleanup can be retried externally.
        }
    }

    private sealed class StateDocument
    {
        public int Version { get; set; }
        public List<string> Favorites { get; set; } = new();
        public List<CollectionDocument> Collections { get; set; } = new();
    }

    private sealed class CollectionDocument
    {
        public string Name { get; set; } = "";
        public List<string> Items { get; set; } = new();
    }
}

/// <summary>Shared lexical and physical boundary checks for Sources, Favorites, and Collections.</summary>
internal static class AssetBrowserSourcePathPolicy
{
    internal static bool TryCanonicalizeExisting(
        string assetsRoot,
        string candidate,
        bool requireDirectory,
        out string fullPath)
    {
        fullPath = "";
        try
        {
            string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
            string full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(candidate));
            if (!IsUnderOrEqual(full, root)) return false;
            if (!Directory.Exists(root) || IsReparsePoint(root)) return false;
            if (!Directory.Exists(full) && !File.Exists(full)) return false;
            if (requireDirectory && !Directory.Exists(full)) return false;

            string relative = Path.GetRelativePath(root, full);
            if (relative != ".")
            {
                string current = root;
                string[] parts = relative.Split(
                    new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                    StringSplitOptions.RemoveEmptyEntries);
                foreach (string part in parts)
                {
                    current = Path.Combine(current, part);
                    if ((!Directory.Exists(current) && !File.Exists(current)) ||
                        IsReparsePoint(current))
                    {
                        return false;
                    }
                }
            }
            fullPath = full;
            return true;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
            ArgumentException or NotSupportedException)
        {
            return false;
        }
    }

    internal static string NormalizePersistedRelativePath(string relativePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(relativePath);
        string normalized = relativePath.Trim().Replace('\\', '/');
        if (normalized.Length > 1024 || Path.IsPathRooted(normalized) ||
            normalized.StartsWith("/", StringComparison.Ordinal))
        {
            throw new ArgumentException("The saved asset path is not relative.");
        }
        string[] parts = normalized.Split(
            '/',
            StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0 ||
            parts.Any(part =>
                part is "." or ".." ||
                part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0))
        {
            if (normalized == ".") return ".";
            throw new ArgumentException("The saved asset path contains an unsafe segment.");
        }
        return string.Join('/', parts);
    }

    internal static bool IsUnderOrEqual(string candidate, string root)
    {
        string canonicalCandidate =
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(candidate));
        string canonicalRoot =
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        if (canonicalCandidate.Equals(
                canonicalRoot,
                StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        string relative = Path.GetRelativePath(canonicalRoot, canonicalCandidate);
        return !Path.IsPathRooted(relative) &&
               relative != ".." &&
               !relative.StartsWith(
                   ".." + Path.DirectorySeparatorChar,
                   StringComparison.Ordinal) &&
               !relative.StartsWith(
                   ".." + Path.AltDirectorySeparatorChar,
                   StringComparison.Ordinal);
    }

    private static bool IsReparsePoint(string path) =>
        (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
}
