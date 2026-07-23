// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal sealed record AssetDeleteInspection(
    int AssetCount,
    int FolderCount,
    long TotalBytes,
    IReadOnlyList<string> Blockers)
{
    internal bool CanDelete => Blockers.Count == 0;
}

internal sealed record AssetDeleteResult(
    int AssetCount,
    int FolderCount,
    long TotalBytes,
    string? DeferredCleanupPath);

internal sealed record AssetMoveMapping(
    string OriginalPath,
    string DestinationPath);

internal sealed record AssetMoveResult(
    IReadOnlyList<string> PublishedPaths,
    IReadOnlyList<AssetMoveMapping> Mappings);

internal sealed class AssetOperationBlockedException : IOException
{
    internal AssetOperationBlockedException(string message, IReadOnlyList<string> blockers)
        : base(message)
    {
        Blockers = blockers;
    }

    internal IReadOnlyList<string> Blockers { get; }
}

/// <summary>
/// Transactional filesystem layer behind Content Browser management commands. Asset identity
/// remains owned by <see cref="AssetDatabase"/>: moves keep the authoritative sidecar, copies
/// receive new GUIDs, and deletes are first quarantined below .acsdb so partial operations can be
/// rolled back. Reparse points and paths outside Assets are rejected before any mutation.
/// </summary>
internal sealed partial class AssetManagementWorkflow
{
    private const int MaxGeneratedSuffix = 9999;
    private const long MaxReferenceScanBytes = 8L * 1024L * 1024L;
    private const long MaxReferenceRewriteBackupBytes = 64L * 1024L * 1024L;
    private const string MaterialGraphSuffix = ".graph.json";
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static readonly StringComparer PathComparer = StringComparer.OrdinalIgnoreCase;
    private static readonly HashSet<string> TextReferenceExtensions = new(
        new[]
        {
            ".acscene", ".acs3d", ".acsprefab", ".acsbp", ".acsmat",
            ".txt", ".json", ".xml", ".yaml", ".yml", ".toml", ".ini",
            ".csv", ".md", ".lua", ".hlsl", ".glsl", ".shader", ".compute",
            ".usf", ".ush", ".cs", ".cpp", ".c", ".hpp", ".h", ".js",
            ".ts", ".py",
        },
        StringComparer.OrdinalIgnoreCase);

    private readonly AssetDatabase _database;
    private readonly string _assetsRoot;
    private readonly string _operationsRoot;

    internal AssetManagementWorkflow(AssetDatabase database)
    {
        _database = database ?? throw new ArgumentNullException(nameof(database));
        _assetsRoot = NormalizeDirectory(database.AssetsRoot);
        _operationsRoot = Path.Combine(
            _assetsRoot,
            AssetDatabase.InternalDirectoryName,
            "operations");
        EnsureOrdinaryDirectory(_assetsRoot, "Assets root");
    }

    internal string Rename(
        string fullPath,
        string assetId,
        bool isDirectory,
        string newBaseName)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Rename asset");
        RefreshAuthoritativeState();
        PathTarget source = ValidateTarget(fullPath, requireTreeValidation: isDirectory);
        if (source.IsDirectory != isDirectory)
            throw new InvalidDataException("Asset type changed before rename.");
        string safeName = ValidateBaseName(newBaseName);
        string extension = source.IsDirectory ? "" : Path.GetExtension(source.FullPath);
        string oldBase = source.IsDirectory
            ? Path.GetFileName(source.FullPath)
            : Path.GetFileNameWithoutExtension(source.FullPath);
        if (string.Equals(oldBase, safeName, StringComparison.Ordinal))
            return source.FullPath;
        if (string.Equals(oldBase, safeName, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Case-only rename is not supported safely on Windows.");

        string parent = Path.GetDirectoryName(source.FullPath)
            ?? throw new InvalidDataException("Asset has no parent directory.");
        string destination = Path.Combine(parent, safeName + extension);
        EnsureDestinationAvailable(destination);
        ThrowIfReferenced(new[] { source });

        if (!source.IsDirectory)
        {
            if (string.IsNullOrWhiteSpace(assetId))
                throw new InvalidDataException("Indexed asset identity is required for rename.");
            if (!_database.TryGetByPath(source.FullPath, out AssetRecord? indexed) ||
                indexed == null ||
                !string.Equals(indexed.AssetId, assetId, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "Asset identity changed before rename; refresh the Content Browser and retry.");
            }
            AssetRecord original = indexed;
            string relative = Path.GetRelativePath(_assetsRoot, destination)
                .Replace('\\', '/');
            AssetRecord moved;
            try
            {
                moved = _database.MoveAsset(assetId, relative);
            }
            catch
            {
                TryRefresh();
                throw;
            }
            var companionMoves = new List<StagedMove>();
            try
            {
                MoveMaterialCompanions(original.FullPath, moved.FullPath, companionMoves);
                RewriteMovedReferencesTransactional(
                    new[] { (Original: original, Moved: moved) });
                return moved.FullPath;
            }
            catch (Exception error)
            {
                bool rollbackComplete = RollbackStagedMoves(companionMoves);
                try
                {
                    string originalRelative = Path.GetRelativePath(
                            _assetsRoot,
                            original.FullPath)
                        .Replace('\\', '/');
                    AssetRecord restored = _database.MoveAsset(assetId, originalRelative);
                    rollbackComplete &= PathComparer.Equals(
                        NormalizeDirectory(restored.FullPath),
                        NormalizeDirectory(original.FullPath));
                }
                catch
                {
                    rollbackComplete = false;
                }
                finally
                {
                    TryRefresh();
                }
                if (!rollbackComplete)
                {
                    throw new IOException(
                        "Asset rename failed and the original path could not be restored. " +
                        $"Recoverable data remains at '{moved.FullPath}'.",
                        error);
                }
                throw;
            }
        }

        IReadOnlyList<AssetRecord> originalRecords = RecordsBelow(
            source.FullPath,
            isDirectory: true);
        RevalidateTarget(source);
        EnsureDestinationAvailable(destination);
        Directory.Move(source.FullPath, destination);
        try
        {
            _database.Refresh();
            var movedPairs = new List<(AssetRecord Original, AssetRecord Moved)>();
            foreach (AssetRecord original in originalRecords)
            {
                string relative = Path.GetRelativePath(source.FullPath, original.FullPath);
                string expected = Path.Combine(destination, relative);
                if (!_database.TryGetByAssetId(original.AssetId, out AssetRecord? moved) ||
                    moved == null ||
                    !PathComparer.Equals(
                        NormalizeDirectory(moved.FullPath),
                        NormalizeDirectory(expected)))
                {
                    throw new IOException(
                        $"Renamed folder child could not be re-indexed: {DisplayPath(expected)}");
                }
                movedPairs.Add((original, moved));
            }
            RewriteMovedReferencesTransactional(movedPairs);
            return destination;
        }
        catch (Exception error)
        {
            bool rollbackComplete = false;
            try
            {
                if (Directory.Exists(destination) && !Directory.Exists(source.FullPath))
                    Directory.Move(destination, source.FullPath);
                rollbackComplete = Directory.Exists(source.FullPath) &&
                                   !Directory.Exists(destination);
            }
            catch
            {
                rollbackComplete = false;
            }
            TryRefresh();
            if (!rollbackComplete)
            {
                throw new IOException(
                    "Folder rename failed and automatic rollback was incomplete. " +
                    $"Recoverable data may remain at '{destination}'.",
                    error);
            }
            throw;
        }
    }

    internal IReadOnlyList<string> Duplicate(
        IEnumerable<string> fullPaths,
        string? destinationDirectory = null)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Duplicate assets");
        RefreshAuthoritativeState();
        IReadOnlyList<PathTarget> sources = NormalizeTopLevelTargets(fullPaths);
        if (sources.Count == 0) return Array.Empty<string>();
        string destinationRoot = destinationDirectory == null
            ? ""
            : ValidateDestinationDirectory(destinationDirectory);

        var plans = new List<CopyPlan>(sources.Count);
        var reservedDestinations = new HashSet<string>(PathComparer);
        foreach (PathTarget source in sources)
        {
            string parent = destinationRoot.Length == 0
                ? Path.GetDirectoryName(source.FullPath)
                    ?? throw new InvalidDataException("Asset has no parent directory.")
                : destinationRoot;
            if (source.IsDirectory && IsUnderOrEqual(parent, source.FullPath))
                throw new InvalidDataException("A folder cannot be copied into itself.");
            string destination = FindCopyDestination(
                source,
                parent,
                reservedDestinations);
            reservedDestinations.Add(destination);
            plans.Add(new CopyPlan(
                source,
                destination,
                RecordsBelow(source.FullPath, source.IsDirectory)));
        }

        var published = new List<CopyPlan>();
        try
        {
            foreach (CopyPlan plan in plans)
            {
                PublishCopy(plan.Source, plan.Destination);
                published.Add(plan);
            }

            _database.Refresh();
            var pairs = new List<(AssetRecord Original, AssetRecord Copy)>();
            foreach (CopyPlan plan in plans)
            {
                foreach (AssetRecord original in plan.OriginalRecords)
                {
                    string relative = plan.Source.IsDirectory
                        ? Path.GetRelativePath(plan.Source.FullPath, original.FullPath)
                        : "";
                    string copiedPath = plan.Source.IsDirectory
                        ? Path.Combine(plan.Destination, relative)
                        : plan.Destination;
                    if (!_database.TryGetByPath(copiedPath, out AssetRecord? copy) || copy == null)
                    {
                        throw new IOException(
                            $"Copied asset could not be indexed: {DisplayPath(copiedPath)}");
                    }
                    pairs.Add((original, copy));
                }
            }

            Dictionary<string, string> remappedIds = pairs.ToDictionary(
                static pair => pair.Original.AssetId,
                static pair => pair.Copy.AssetId,
                StringComparer.OrdinalIgnoreCase);
            IReadOnlyList<ReferenceReplacement> replacements =
                BuildReferenceReplacements(pairs);
            if (RemapCopiedTextReferences(pairs, replacements))
                _database.Refresh();
            foreach ((AssetRecord original, AssetRecord copy) in pairs)
            {
                string[] dependencies = original.Metadata.Dependencies
                    .Select(id => remappedIds.GetValueOrDefault(id, id))
                    .ToArray();
                KeyValuePair<string, string>[] settings = original.Metadata.ImportSettings
                    .Select(setting => new KeyValuePair<string, string>(
                        setting.Key,
                        ReplaceReferenceTokens(setting.Value, replacements)))
                    .ToArray();
                _database.UpdateImportMetadata(
                    copy.AssetId,
                    ReplaceReferenceTokens(original.Metadata.Source, replacements),
                    original.Metadata.Importer,
                    original.Metadata.ImporterVersion,
                    dependencies,
                    settings);
            }

            return Array.AsReadOnly(plans.Select(static plan => plan.Destination).ToArray());
        }
        catch
        {
            foreach (CopyPlan plan in published.AsEnumerable().Reverse())
                TryDeletePublishedCopy(plan.Destination);
            TryRefresh();
            throw;
        }
    }

    internal IReadOnlyList<string> Move(
        IEnumerable<string> fullPaths,
        string destinationDirectory) =>
        MoveWithMappings(fullPaths, destinationDirectory).PublishedPaths;

    internal AssetMoveResult MoveWithMappings(
        IEnumerable<string> fullPaths,
        string destinationDirectory)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Move assets");
        RefreshAuthoritativeState();
        IReadOnlyList<PathTarget> sources = NormalizeTopLevelTargets(fullPaths);
        if (sources.Count == 0)
        {
            return new AssetMoveResult(
                Array.Empty<string>(),
                Array.Empty<AssetMoveMapping>());
        }
        string destinationRoot = ValidateDestinationDirectory(destinationDirectory);
        var plans = new List<MovePlan>();
        var reservedDestinations = new HashSet<string>(PathComparer);
        foreach (PathTarget source in sources)
        {
            if (source.IsDirectory && IsUnderOrEqual(destinationRoot, source.FullPath))
                throw new InvalidDataException("A folder cannot be moved into itself.");
            string destination = Path.Combine(
                destinationRoot,
                Path.GetFileName(source.FullPath));
            if (PathComparer.Equals(destination, source.FullPath)) continue;
            if (!reservedDestinations.Add(destination))
            {
                throw new IOException(
                    $"Multiple selected assets would move to the same path: {DisplayPath(destination)}");
            }
            EnsureDestinationAvailable(destination);
            IReadOnlyList<AssetRecord> records = RecordsBelow(
                source.FullPath,
                source.IsDirectory);
            string assetId = records.Count == 1 && !source.IsDirectory
                ? records[0].AssetId
                : "";
            if (!source.IsDirectory && assetId.Length == 0)
            {
                throw new InvalidDataException(
                    $"Asset is not indexed: {DisplayPath(source.FullPath)}");
            }
            plans.Add(new MovePlan(source, destination, assetId, records));
        }
        if (plans.Count == 0)
        {
            return new AssetMoveResult(
                Array.AsReadOnly(sources.Select(static source => source.FullPath).ToArray()),
                Array.Empty<AssetMoveMapping>());
        }
        ThrowIfReferenced(plans.Select(static plan => plan.Source).ToArray());

        var moved = new List<MovePlan>();
        var companionMoves = new List<StagedMove>();
        try
        {
            var movedPairs = new List<(AssetRecord Original, AssetRecord Moved)>();
            foreach (MovePlan plan in plans)
            {
                RevalidateTarget(plan.Source);
                EnsureDestinationAvailable(plan.Destination);
                if (plan.Source.IsDirectory)
                {
                    Directory.Move(plan.Source.FullPath, plan.Destination);
                }
                else
                {
                    string relative = Path.GetRelativePath(_assetsRoot, plan.Destination)
                        .Replace('\\', '/');
                    _database.MoveAsset(plan.AssetId, relative);
                }
                moved.Add(plan);
                if (!plan.Source.IsDirectory)
                {
                    MoveMaterialCompanions(
                        plan.Source.FullPath,
                        plan.Destination,
                        companionMoves);
                }
            }
            _database.Refresh();
            foreach (MovePlan plan in plans)
            {
                foreach (AssetRecord original in plan.OriginalRecords)
                {
                    string relative = plan.Source.IsDirectory
                        ? Path.GetRelativePath(plan.Source.FullPath, original.FullPath)
                        : "";
                    string expected = plan.Source.IsDirectory
                        ? Path.Combine(plan.Destination, relative)
                        : plan.Destination;
                    if (!_database.TryGetByAssetId(original.AssetId, out AssetRecord? current) ||
                        current == null ||
                        !PathComparer.Equals(
                            NormalizeDirectory(current.FullPath),
                            NormalizeDirectory(expected)))
                    {
                        throw new IOException(
                            $"Moved asset could not be re-indexed: {DisplayPath(expected)}");
                    }
                    movedPairs.Add((original, current));
                }
            }
            RewriteMovedReferencesTransactional(movedPairs);
            return new AssetMoveResult(
                Array.AsReadOnly(plans.Select(static plan => plan.Destination).ToArray()),
                Array.AsReadOnly(plans.Select(static plan => new AssetMoveMapping(
                    plan.Source.FullPath,
                    plan.Destination)).ToArray()));
        }
        catch (Exception error)
        {
            bool rollbackComplete = RollbackStagedMoves(companionMoves);
            foreach (MovePlan plan in moved.AsEnumerable().Reverse())
                rollbackComplete &= TryRollbackMove(plan);
            TryRefresh();
            if (!rollbackComplete)
            {
                throw new IOException(
                    "Asset move failed and automatic rollback was incomplete.",
                    error);
            }
            throw;
        }
    }

    internal AssetDeleteInspection InspectDelete(IEnumerable<string> fullPaths)
    {
        IReadOnlyList<PathTarget> targets = NormalizeTopLevelTargets(fullPaths);
        IReadOnlyList<AssetRecord> records = targets
            .SelectMany(target => RecordsBelow(target.FullPath, target.IsDirectory))
            .DistinctBy(static record => record.AssetId, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        IReadOnlyList<string> blockers = FindReferenceBlockers(targets, records);

        int folders = 0;
        long bytes = 0;
        foreach (PathTarget target in targets)
        {
            if (target.IsDirectory)
            {
                folders++;
                foreach (OrdinaryTreeEntry entry in EnumerateOrdinaryTree(target.FullPath))
                {
                    if (entry.IsDirectory)
                    {
                        folders++;
                    }
                    else if (!entry.FullPath.EndsWith(
                                 AssetDatabase.MetadataSuffix,
                                 StringComparison.OrdinalIgnoreCase))
                    {
                        bytes = SaturatingAdd(
                            bytes,
                            new FileInfo(entry.FullPath).Length);
                    }
                }
            }
            else
            {
                bytes = SaturatingAdd(bytes, new FileInfo(target.FullPath).Length);
                if (IsMaterialAssetPath(target.FullPath))
                {
                    string graph = MaterialGraphPath(target.FullPath);
                    if (File.Exists(graph))
                        bytes = SaturatingAdd(bytes, new FileInfo(graph).Length);
                }
            }
        }

        return new AssetDeleteInspection(
            records.Count,
            folders,
            bytes,
            blockers);
    }

    internal AssetDeleteResult Delete(IEnumerable<string> fullPaths)
    {
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            _assetsRoot,
            "Delete assets");
        RefreshAuthoritativeState();
        IReadOnlyList<PathTarget> targets = NormalizeTopLevelTargets(fullPaths);
        AssetDeleteInspection inspection = InspectDelete(
            targets.Select(static target => target.FullPath));
        if (!inspection.CanDelete)
        {
            throw new AssetOperationBlockedException(
                "Referenced assets cannot be deleted safely.",
                inspection.Blockers);
        }
        if (targets.Count == 0)
            return new AssetDeleteResult(0, 0, 0, null);

        string quarantine = CreateOperationDirectory("delete");
        var staged = new List<StagedMove>();
        try
        {
            for (int index = 0; index < targets.Count; index++)
            {
                PathTarget target = targets[index];
                RevalidateTarget(target);
                string slot = Path.Combine(quarantine, index.ToString("D4"));
                Directory.CreateDirectory(slot);
                EnsureOrdinaryDirectory(slot, "Delete quarantine");
                if (target.IsDirectory)
                {
                    string stagedPath = Path.Combine(slot, Path.GetFileName(target.FullPath));
                    Directory.Move(target.FullPath, stagedPath);
                    staged.Add(new StagedMove(stagedPath, target.FullPath, true));
                    ValidateOrdinaryTree(stagedPath);
                }
                else
                {
                    string[] family = EnumerateExistingAssetFamilyFiles(target.FullPath).ToArray();
                    foreach (string sourcePath in family)
                    {
                        EnsureNoReparseParents(sourcePath);
                        EnsureNotReparse(sourcePath, expectDirectory: false);
                    }
                    foreach (string sourcePath in family)
                    {
                        string stagedPath = Path.Combine(slot, Path.GetFileName(sourcePath));
                        File.Move(sourcePath, stagedPath);
                        staged.Add(new StagedMove(stagedPath, sourcePath, false));
                    }
                }
            }
            _database.Refresh();
        }
        catch (Exception error)
        {
            bool rollbackComplete = RollbackStagedMoves(staged);
            TryRefresh();
            if (rollbackComplete)
            {
                TryDeleteDirectory(quarantine);
                throw;
            }
            throw new IOException(
                "Asset delete failed and automatic rollback was incomplete. " +
                $"Recoverable data was retained at '{quarantine}'.",
                error);
        }

        string? deferredCleanup = null;
        if (!TryDeleteOrdinaryDirectoryTree(quarantine))
            deferredCleanup = quarantine;
        return new AssetDeleteResult(
            inspection.AssetCount,
            inspection.FolderCount,
            inspection.TotalBytes,
            deferredCleanup);
    }

    internal string ValidateExternalPath(string fullPath)
    {
        PathTarget target = ValidateTarget(fullPath, requireTreeValidation: false);
        return target.FullPath;
    }

    internal static string ValidateBaseName(string value)
    {
        string name = (value ?? "").Trim();
        if (name.Length == 0 || name.Length > 128 || name is "." or "..")
            throw new InvalidDataException("Asset name must contain 1 to 128 characters.");
        if (name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            name.Contains(Path.DirectorySeparatorChar) ||
            name.Contains(Path.AltDirectorySeparatorChar) ||
            name.EndsWith(' ') || name.EndsWith('.'))
        {
            throw new InvalidDataException("Asset name contains characters Windows cannot use.");
        }
        if (name.Contains(".tmp-", StringComparison.OrdinalIgnoreCase) ||
            name.EndsWith(AssetDatabase.MetadataSuffix, StringComparison.OrdinalIgnoreCase) ||
            name.Equals(AssetDatabase.InternalDirectoryName, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Asset name is reserved by the ACS asset database.");
        }
        string device = name.Split('.')[0];
        if (device.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("PRN", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("NUL", StringComparison.OrdinalIgnoreCase) ||
            IsNumberedDevice(device, "COM") || IsNumberedDevice(device, "LPT"))
        {
            throw new InvalidDataException("Asset name is a reserved Windows device name.");
        }
        return name;
    }

    private void ThrowIfReferenced(IReadOnlyList<PathTarget> targets)
    {
        IReadOnlyList<AssetRecord> records = targets
            .SelectMany(target => RecordsBelow(target.FullPath, target.IsDirectory))
            .ToArray();
        IReadOnlyList<string> blockers = FindReferenceBlockers(targets, records);
        if (blockers.Count != 0)
        {
            throw new AssetOperationBlockedException(
                "Referenced assets cannot be renamed safely.",
                blockers);
        }
    }

    private IReadOnlyList<string> FindReferenceBlockers(
        IReadOnlyList<PathTarget> targets,
        IReadOnlyList<AssetRecord> selectedRecords)
    {
        var blockers = new SortedSet<string>(StringComparer.Ordinal);
        if (selectedRecords.Count == 0) return Array.Empty<string>();
        var selectedIds = selectedRecords
            .Select(static record => record.AssetId)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (AssetRecord selected in selectedRecords)
        {
            foreach (AssetReferenceNode referencer in
                     _database.GetDirectReferencers(selected.AssetId))
            {
                if (!selectedIds.Contains(referencer.AssetId))
                {
                    blockers.Add(
                        $"{selected.RelativePath} is referenced by {referencer.RelativePath}.");
                }
            }
        }

        foreach (AssetRecord referencer in _database.Snapshot())
        {
            if (selectedIds.Contains(referencer.AssetId)) continue;
            foreach (AssetRecord selected in selectedRecords)
            {
                bool sourceReference = ContainsAssetReference(
                    referencer.Metadata.Source,
                    selected);
                bool settingReference = referencer.Metadata.ImportSettings.Values.Any(
                    value => ContainsAssetReference(value, selected));
                if (sourceReference || settingReference)
                {
                    blockers.Add(
                        $"{selected.RelativePath} has a metadata reference in " +
                        $"{referencer.RelativePath}.");
                }
            }
        }

        foreach (ReferenceCandidate candidate in EnumerateReferenceCandidates(targets))
        {
            var info = new FileInfo(candidate.FullPath);
            if (info.Length > MaxReferenceScanBytes)
            {
                blockers.Add(
                    $"{candidate.RelativePath} is too large to verify for path references.");
                continue;
            }
            string text;
            try
            {
                text = File.ReadAllText(candidate.FullPath, StrictUtf8);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or DecoderFallbackException)
            {
                blockers.Add(
                    $"{candidate.RelativePath} could not be checked for path references.");
                continue;
            }
            foreach (AssetRecord selected in selectedRecords)
            {
                if (ContainsAssetReference(text, selected))
                {
                    blockers.Add(
                        $"{selected.RelativePath} has a path reference in {candidate.RelativePath}.");
                }
            }
        }
        return Array.AsReadOnly(blockers.ToArray());
    }

    private IEnumerable<ReferenceCandidate> EnumerateReferenceCandidates(
        IReadOnlyList<PathTarget> selectedTargets)
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
                FileAttributes attributes = entry.Attributes;
                bool isDirectory = (attributes & FileAttributes.Directory) != 0;
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException(
                        "Reference verification encountered an unsupported reparse point.");
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
                if (entry.Name.EndsWith(
                        AssetDatabase.MetadataSuffix,
                        StringComparison.OrdinalIgnoreCase) ||
                    AssetCreationWorkflow.IsTemporaryPath(entry.FullName) ||
                    !TextReferenceExtensions.Contains(Path.GetExtension(entry.FullName)) ||
                    selectedTargets.Any(target => IsPartOfTarget(entry.FullName, target)))
                {
                    continue;
                }
                yield return new ReferenceCandidate(
                    entry.FullName,
                    Path.GetRelativePath(_assetsRoot, entry.FullName).Replace('\\', '/'));
            }
        }
    }

    private static bool ContainsAssetReference(string text, AssetRecord asset)
    {
        return BuildReferenceTokens(asset)
            .Any(token => ContainsReferenceToken(text, token.Value, token.IsAssetId));
    }

    private static IReadOnlyList<ReferenceToken> BuildReferenceTokens(AssetRecord asset)
    {
        var tokens = new Dictionary<string, ReferenceToken>(StringComparer.OrdinalIgnoreCase);
        void Add(string value, bool isAssetId = false)
        {
            if (!string.IsNullOrEmpty(value))
                tokens.TryAdd(value, new ReferenceToken(value, isAssetId));
        }

        Add(asset.AssetId, isAssetId: true);
        AddPathTokenVariants(asset.FullPath, Add);
        string relativeSlash = asset.RelativePath.Replace('\\', '/');
        string relativeBackslash = relativeSlash.Replace('/', '\\');
        Add(relativeSlash);
        Add(relativeBackslash);
        Add(EscapeBackslashes(relativeBackslash));
        Add("Assets/" + relativeSlash);
        Add("Assets\\" + relativeBackslash);
        Add(EscapeBackslashes("Assets\\" + relativeBackslash));
        if (IsMaterialAssetPath(asset.FullPath))
        {
            AddPathTokenVariants(MaterialGraphPath(asset.FullPath), Add);
            string graphSlash = relativeSlash + MaterialGraphSuffix;
            string graphBackslash = graphSlash.Replace('/', '\\');
            Add(graphSlash);
            Add(graphBackslash);
            Add(EscapeBackslashes(graphBackslash));
            Add("Assets/" + graphSlash);
            Add("Assets\\" + graphBackslash);
            Add(EscapeBackslashes("Assets\\" + graphBackslash));
        }
        return Array.AsReadOnly(tokens.Values
            .OrderByDescending(static token => token.Value.Length)
            .ToArray());
    }

    private static void AddPathTokenVariants(
        string path,
        Action<string, bool> add)
    {
        string native = Path.GetFullPath(path);
        string slash = native.Replace('\\', '/');
        add(native, false);
        add(slash, false);
        add(EscapeBackslashes(native), false);
        add("file:///" + slash, false);
    }

    private static bool ContainsReferenceToken(
        string text,
        string token,
        bool isAssetId)
    {
        int search = 0;
        while (search <= text.Length - token.Length)
        {
            int index = text.IndexOf(token, search, StringComparison.OrdinalIgnoreCase);
            if (index < 0) return false;
            if (HasReferenceBoundaries(text, index, token.Length, isAssetId))
                return true;
            search = index + 1;
        }
        return false;
    }

    private static bool HasReferenceBoundaries(
        string text,
        int index,
        int length,
        bool isAssetId)
    {
        bool IsContinuation(char value) => isAssetId
            ? Uri.IsHexDigit(value)
            : char.IsLetterOrDigit(value) || value is '_' or '-' or '.' or '/' or '\\';
        return (index == 0 || !IsContinuation(text[index - 1])) &&
               (index + length == text.Length || !IsContinuation(text[index + length]));
    }

    private static IReadOnlyList<ReferenceReplacement> BuildReferenceReplacements(
        IReadOnlyList<(AssetRecord Original, AssetRecord Copy)> pairs)
    {
        var replacements = new Dictionary<string, ReferenceReplacement>(
            StringComparer.OrdinalIgnoreCase);
        void Add(string source, string destination, bool isAssetId = false)
        {
            if (string.IsNullOrEmpty(source) ||
                string.Equals(source, destination, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }
            if (replacements.TryGetValue(source, out ReferenceReplacement? existing) &&
                !string.Equals(
                    existing.Destination,
                    destination,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    $"Ambiguous copied-asset reference mapping: {source}");
            }
            replacements[source] = new ReferenceReplacement(
                source,
                destination,
                isAssetId);
        }

        foreach ((AssetRecord original, AssetRecord copy) in pairs)
        {
            Add(original.AssetId, copy.AssetId, isAssetId: true);
            AddPathReplacementVariants(original.FullPath, copy.FullPath, Add);
            string originalSlash = original.RelativePath.Replace('\\', '/');
            string copiedSlash = copy.RelativePath.Replace('\\', '/');
            string originalBackslash = originalSlash.Replace('/', '\\');
            string copiedBackslash = copiedSlash.Replace('/', '\\');
            Add(originalSlash, copiedSlash);
            if (!string.Equals(originalBackslash, originalSlash, StringComparison.Ordinal))
            {
                Add(originalBackslash, copiedBackslash);
                Add(
                    EscapeBackslashes(originalBackslash),
                    EscapeBackslashes(copiedBackslash));
            }
            Add("Assets/" + originalSlash, "Assets/" + copiedSlash);
            Add("Assets\\" + originalBackslash, "Assets\\" + copiedBackslash);
            Add(
                EscapeBackslashes("Assets\\" + originalBackslash),
                EscapeBackslashes("Assets\\" + copiedBackslash));

            if (IsMaterialAssetPath(original.FullPath))
            {
                string originalGraphFull = MaterialGraphPath(original.FullPath);
                string copiedGraphFull = MaterialGraphPath(copy.FullPath);
                AddPathReplacementVariants(originalGraphFull, copiedGraphFull, Add);
                string originalGraphSlash = originalSlash + MaterialGraphSuffix;
                string copiedGraphSlash = copiedSlash + MaterialGraphSuffix;
                string originalGraphBackslash = originalGraphSlash.Replace('/', '\\');
                string copiedGraphBackslash = copiedGraphSlash.Replace('/', '\\');
                Add(originalGraphSlash, copiedGraphSlash);
                if (!string.Equals(
                        originalGraphBackslash,
                        originalGraphSlash,
                        StringComparison.Ordinal))
                {
                    Add(originalGraphBackslash, copiedGraphBackslash);
                    Add(
                        EscapeBackslashes(originalGraphBackslash),
                        EscapeBackslashes(copiedGraphBackslash));
                }
                Add("Assets/" + originalGraphSlash, "Assets/" + copiedGraphSlash);
                Add("Assets\\" + originalGraphBackslash, "Assets\\" + copiedGraphBackslash);
                Add(
                    EscapeBackslashes("Assets\\" + originalGraphBackslash),
                    EscapeBackslashes("Assets\\" + copiedGraphBackslash));
            }
        }
        return Array.AsReadOnly(replacements.Values
            .OrderByDescending(static replacement => replacement.Source.Length)
            .ToArray());
    }

    private static void AddPathReplacementVariants(
        string source,
        string destination,
        Action<string, string, bool> add)
    {
        string sourceNative = Path.GetFullPath(source);
        string destinationNative = Path.GetFullPath(destination);
        string sourceSlash = sourceNative.Replace('\\', '/');
        string destinationSlash = destinationNative.Replace('\\', '/');
        add(sourceNative, destinationNative, false);
        add(sourceSlash, destinationSlash, false);
        add(
            EscapeBackslashes(sourceNative),
            EscapeBackslashes(destinationNative),
            false);
        add("file:///" + sourceSlash, "file:///" + destinationSlash, false);
    }

    private bool RemapCopiedTextReferences(
        IReadOnlyList<(AssetRecord Original, AssetRecord Copy)> pairs,
        IReadOnlyList<ReferenceReplacement> replacements)
    {
        bool changed = false;
        foreach (AssetRecord copy in pairs
                     .Select(static pair => pair.Copy)
                     .DistinctBy(static record => record.AssetId, StringComparer.OrdinalIgnoreCase))
        {
            foreach (string path in EnumerateReferenceRewritePaths(copy.FullPath))
            {
                var info = new FileInfo(path);
                if (info.Length > MaxReferenceScanBytes)
                {
                    throw new IOException(
                        $"Copied text asset is too large to remap safely: {DisplayPath(path)}");
                }
                EnsureNoReparseParents(path);
                EnsureNotReparse(path, expectDirectory: false);
                byte[] originalBytes = File.ReadAllBytes(path);
                string originalText = DecodeUtf8Text(
                    originalBytes,
                    $"Copied text asset is not valid UTF-8: {DisplayPath(path)}",
                    out bool hasBom);
                string updated = ReplaceReferenceTokens(originalText, replacements);
                if (string.Equals(originalText, updated, StringComparison.Ordinal))
                    continue;
                WriteCopiedTextAtomically(
                    path,
                    info.LastWriteTimeUtc.Ticks,
                    updated,
                    hasBom);
                changed = true;
            }
        }
        return changed;
    }

    private static IEnumerable<string> EnumerateReferenceRewritePaths(string assetPath)
    {
        if (File.Exists(assetPath) &&
            TextReferenceExtensions.Contains(Path.GetExtension(assetPath)))
        {
            yield return assetPath;
        }
        if (!IsMaterialAssetPath(assetPath)) yield break;

        string graph = MaterialGraphPath(assetPath);
        if (File.Exists(graph)) yield return graph;
        string graphMetadata = graph + AssetDatabase.MetadataSuffix;
        if (File.Exists(graphMetadata)) yield return graphMetadata;
    }

    private static string DecodeUtf8Text(
        byte[] bytes,
        string errorMessage,
        out bool hasBom)
    {
        hasBom = bytes.Length >= 3 &&
                 bytes[0] == 0xEF &&
                 bytes[1] == 0xBB &&
                 bytes[2] == 0xBF;
        int offset = hasBom ? 3 : 0;
        try
        {
            return StrictUtf8.GetString(bytes, offset, bytes.Length - offset);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(errorMessage, error);
        }
    }

    private void WriteCopiedTextAtomically(
        string path,
        long originalLastWriteUtcTicks,
        string text,
        bool includeBom)
    {
        string temporary = path + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            byte[] content = StrictUtf8.GetBytes(text);
            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None))
            {
                if (includeBom) stream.Write(new byte[] { 0xEF, 0xBB, 0xBF });
                stream.Write(content);
                stream.Flush(flushToDisk: true);
            }
            EnsureNotReparse(path, expectDirectory: false);
            EnsureNotReparse(temporary, expectDirectory: false);
            File.Move(temporary, path, overwrite: true);
            if (File.GetLastWriteTimeUtc(path).Ticks == originalLastWriteUtcTicks)
            {
                File.SetLastWriteTimeUtc(
                    path,
                    DateTime.UtcNow.AddSeconds(1));
            }
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    private void RewriteMovedReferencesTransactional(
        IReadOnlyList<(AssetRecord Original, AssetRecord Moved)> pairs)
    {
        if (pairs.Count == 0) return;
        IReadOnlyList<ReferenceReplacement> replacements =
            BuildReferenceReplacements(pairs);
        if (replacements.Count == 0) return;

        var backups = new Dictionary<string, FileRewriteBackup>(PathComparer);
        var textPlans = new List<TextRewritePlan>();
        var metadataPlans = new List<MetadataRewritePlan>();
        long backupBytes = 0;

        foreach (AssetRecord moved in pairs
                     .Select(static pair => pair.Moved)
                     .DistinctBy(static record => record.AssetId, StringComparer.OrdinalIgnoreCase))
        {
            foreach (string path in EnumerateReferenceRewritePaths(moved.FullPath))
            {
                var info = new FileInfo(path);
                if (info.Length > MaxReferenceScanBytes)
                {
                    throw new IOException(
                        $"Moved text asset is too large to remap safely: {DisplayPath(path)}");
                }
                FileRewriteBackup candidate = ReadRewriteBackup(path);
                string originalText = DecodeUtf8Text(
                    candidate.OriginalBytes,
                    $"Moved text asset is not valid UTF-8: {DisplayPath(path)}",
                    out bool hasBom);
                string updatedText = ReplaceReferenceTokens(originalText, replacements);
                if (string.Equals(originalText, updatedText, StringComparison.Ordinal))
                    continue;
                FileRewriteBackup backup = RegisterRewriteBackup(
                    candidate,
                    backups,
                    ref backupBytes);
                textPlans.Add(new TextRewritePlan(
                    backup,
                    EncodeUtf8Text(updatedText, hasBom)));
            }
        }

        foreach ((AssetRecord original, AssetRecord moved) in pairs)
        {
            string updatedSource = ReplaceReferenceTokens(
                original.Metadata.Source,
                replacements);
            KeyValuePair<string, string>[] updatedSettings = original.Metadata.ImportSettings
                .Select(setting => new KeyValuePair<string, string>(
                    setting.Key,
                    ReplaceReferenceTokens(setting.Value, replacements)))
                .ToArray();
            bool metadataChanged = !string.Equals(
                                       original.Metadata.Source,
                                       updatedSource,
                                       StringComparison.Ordinal) ||
                                   updatedSettings.Any(setting => !string.Equals(
                                       original.Metadata.ImportSettings[setting.Key],
                                       setting.Value,
                                       StringComparison.Ordinal));
            if (!metadataChanged) continue;

            FileRewriteBackup sidecarBackup = RegisterRewriteBackup(
                ReadRewriteBackup(moved.FullPath + AssetDatabase.MetadataSuffix),
                backups,
                ref backupBytes);
            metadataPlans.Add(new MetadataRewritePlan(
                original,
                moved,
                updatedSource,
                updatedSettings,
                sidecarBackup));
        }

        if (textPlans.Count == 0 && metadataPlans.Count == 0) return;
        foreach (FileRewriteBackup backup in backups.Values)
            EnsureRewriteBackupUnchanged(backup);

        try
        {
            foreach (TextRewritePlan plan in textPlans)
            {
                WriteRewriteBytesAtomically(
                    plan.Backup,
                    plan.UpdatedBytes,
                    restoreOriginalTimestamp: false);
            }
            foreach (MetadataRewritePlan plan in metadataPlans)
            {
                _database.UpdateImportMetadata(
                    plan.Moved.AssetId,
                    plan.UpdatedSource,
                    plan.Original.Metadata.Importer,
                    plan.Original.Metadata.ImporterVersion,
                    plan.Original.Metadata.Dependencies,
                    plan.UpdatedSettings);
            }
            _database.Refresh(verifyContent: true);
        }
        catch (Exception error)
        {
            bool rollbackComplete = RestoreRewriteBackups(backups.Values);
            TryRefresh();
            if (!rollbackComplete)
            {
                throw new IOException(
                    "Moved-reference rewrite failed and its byte-for-byte rollback was incomplete.",
                    error);
            }
            throw;
        }
    }

    private FileRewriteBackup ReadRewriteBackup(string path)
    {
        EnsureNoReparseParents(path);
        EnsureNotReparse(path, expectDirectory: false);
        var info = new FileInfo(path);
        return new FileRewriteBackup(
            path,
            File.ReadAllBytes(path),
            info.LastWriteTimeUtc.Ticks,
            info.Attributes);
    }

    private static FileRewriteBackup RegisterRewriteBackup(
        FileRewriteBackup candidate,
        IDictionary<string, FileRewriteBackup> backups,
        ref long backupBytes)
    {
        if (backups.TryGetValue(candidate.FullPath, out FileRewriteBackup? existing))
            return existing;
        backupBytes = SaturatingAdd(backupBytes, candidate.OriginalBytes.LongLength);
        if (backupBytes > MaxReferenceRewriteBackupBytes)
        {
            throw new IOException(
                "Moved-reference rollback data exceeds the 64 MiB safety limit.");
        }
        backups.Add(candidate.FullPath, candidate);
        return candidate;
    }

    private void EnsureRewriteBackupUnchanged(FileRewriteBackup backup)
    {
        EnsureNoReparseParents(backup.FullPath);
        EnsureNotReparse(backup.FullPath, expectDirectory: false);
        var info = new FileInfo(backup.FullPath);
        if (info.LastWriteTimeUtc.Ticks != backup.OriginalLastWriteUtcTicks ||
            info.Attributes != backup.OriginalAttributes ||
            !File.ReadAllBytes(backup.FullPath).AsSpan().SequenceEqual(backup.OriginalBytes))
        {
            throw new IOException(
                $"Asset changed while preparing reference updates: {DisplayPath(backup.FullPath)}");
        }
    }

    private static byte[] EncodeUtf8Text(string text, bool includeBom)
    {
        byte[] content = StrictUtf8.GetBytes(text);
        if (!includeBom) return content;
        var result = new byte[content.Length + 3];
        result[0] = 0xEF;
        result[1] = 0xBB;
        result[2] = 0xBF;
        Buffer.BlockCopy(content, 0, result, 3, content.Length);
        return result;
    }

    private void WriteRewriteBytesAtomically(
        FileRewriteBackup backup,
        byte[] bytes,
        bool restoreOriginalTimestamp)
    {
        string temporary = backup.FullPath + ".tmp-" + Guid.NewGuid().ToString("N");
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
            EnsureNoReparseParents(backup.FullPath);
            EnsureNotReparse(backup.FullPath, expectDirectory: false);
            EnsureNotReparse(temporary, expectDirectory: false);
            File.Move(temporary, backup.FullPath, overwrite: true);
            File.SetAttributes(backup.FullPath, backup.OriginalAttributes);
            if (restoreOriginalTimestamp)
            {
                File.SetLastWriteTimeUtc(
                    backup.FullPath,
                    new DateTime(backup.OriginalLastWriteUtcTicks, DateTimeKind.Utc));
            }
            else if (File.GetLastWriteTimeUtc(backup.FullPath).Ticks ==
                     backup.OriginalLastWriteUtcTicks)
            {
                File.SetLastWriteTimeUtc(backup.FullPath, DateTime.UtcNow.AddSeconds(1));
            }
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    private bool RestoreRewriteBackups(IEnumerable<FileRewriteBackup> backups)
    {
        bool complete = true;
        foreach (FileRewriteBackup backup in backups.Reverse())
        {
            try
            {
                WriteRewriteBytesAtomically(
                    backup,
                    backup.OriginalBytes,
                    restoreOriginalTimestamp: true);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                complete = false;
            }
        }
        return complete;
    }

    private static string ReplaceReferenceTokens(
        string text,
        IReadOnlyList<ReferenceReplacement> replacements)
    {
        string result = text;
        foreach (ReferenceReplacement replacement in replacements)
            result = ReplaceReferenceToken(result, replacement);
        return result;
    }

    private static string ReplaceReferenceToken(
        string text,
        ReferenceReplacement replacement)
    {
        StringBuilder? output = null;
        int search = 0;
        int copied = 0;
        while (search <= text.Length - replacement.Source.Length)
        {
            int index = text.IndexOf(
                replacement.Source,
                search,
                StringComparison.OrdinalIgnoreCase);
            if (index < 0) break;
            if (!HasReferenceBoundaries(
                    text,
                    index,
                    replacement.Source.Length,
                    replacement.IsAssetId))
            {
                search = index + 1;
                continue;
            }
            output ??= new StringBuilder(text.Length + 32);
            output.Append(text, copied, index - copied);
            output.Append(replacement.Destination);
            copied = index + replacement.Source.Length;
            search = copied;
        }
        if (output == null) return text;
        output.Append(text, copied, text.Length - copied);
        return output.ToString();
    }

    private static string EscapeBackslashes(string value) =>
        value.Replace("\\", "\\\\", StringComparison.Ordinal);

    private IReadOnlyList<AssetRecord> RecordsBelow(string path, bool isDirectory)
    {
        if (!isDirectory)
        {
            if (!_database.TryGetByPath(path, out AssetRecord? record) || record == null)
                throw new InvalidDataException($"Asset is not indexed: {DisplayPath(path)}");
            return new[] { record };
        }
        AssetRecord[] records = _database.Snapshot()
            .Where(record => IsUnder(record.FullPath, path))
            .ToArray();
        foreach (OrdinaryTreeEntry entry in EnumerateOrdinaryTree(path))
        {
            if (entry.IsDirectory) continue;
            string file = entry.FullPath;
            if (file.EndsWith(AssetDatabase.MetadataSuffix, StringComparison.OrdinalIgnoreCase) ||
                IsMaterialGraphCompanionPath(file) ||
                AssetCreationWorkflow.IsTemporaryPath(file))
            {
                continue;
            }
            if (!_database.TryGetByPath(file, out AssetRecord? indexed) || indexed == null)
            {
                throw new InvalidDataException(
                    $"Folder contains an unindexed asset: {DisplayPath(file)}");
            }
        }
        return records;
    }

    private IReadOnlyList<PathTarget> NormalizeTopLevelTargets(IEnumerable<string> fullPaths)
    {
        ArgumentNullException.ThrowIfNull(fullPaths);
        PathTarget[] validated = fullPaths
            .Where(static path => !string.IsNullOrWhiteSpace(path))
            .Select(path => ValidateTarget(path, requireTreeValidation: true))
            .DistinctBy(static target => target.FullPath, PathComparer)
            .OrderBy(static target => target.FullPath.Length)
            .ThenBy(static target => target.FullPath, PathComparer)
            .ToArray();
        var result = new List<PathTarget>();
        foreach (PathTarget target in validated)
        {
            if (result.Any(parent => parent.IsDirectory &&
                                     IsUnder(target.FullPath, parent.FullPath)))
            {
                continue;
            }
            result.Add(target);
        }
        return result;
    }

    private PathTarget ValidateTarget(string path, bool requireTreeValidation)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnder(full, _assetsRoot) ||
            IsUnderOrEqual(full, Path.Combine(_assetsRoot, AssetDatabase.InternalDirectoryName)) ||
            full.EndsWith(AssetDatabase.MetadataSuffix, StringComparison.OrdinalIgnoreCase) ||
            IsMaterialGraphCompanionPath(full) ||
            AssetCreationWorkflow.IsTemporaryPath(full))
        {
            throw new InvalidDataException("Asset operation target is reserved or outside Assets.");
        }
        EnsureNoReparseParents(full);
        bool directory = Directory.Exists(full);
        if (!directory && !File.Exists(full))
            throw new FileNotFoundException("Asset operation target no longer exists.", full);
        EnsureNotReparse(full, directory);
        if (directory && requireTreeValidation)
            ValidateOrdinaryTree(full);
        return new PathTarget(full, directory);
    }

    private string ValidateDestinationDirectory(string path)
    {
        string full = Path.GetFullPath(path);
        if (!IsUnderOrEqual(full, _assetsRoot) ||
            IsUnderOrEqual(full, Path.Combine(_assetsRoot, AssetDatabase.InternalDirectoryName)))
        {
            throw new InvalidDataException("Copy destination must stay below Assets.");
        }
        EnsureNoReparseParents(full);
        EnsureOrdinaryDirectory(full, "Copy destination");
        return full;
    }

    private void PublishCopy(PathTarget source, string destination)
    {
        RevalidateTarget(source);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new InvalidDataException("Copy destination has no parent.");
        EnsureOrdinaryDirectory(parent, "Copy destination");
        EnsureDestinationAvailable(destination);
        string temporary = Path.Combine(
            parent,
            "." + Path.GetFileName(destination) + ".tmp-" + Guid.NewGuid().ToString("N"));
        bool published = false;
        try
        {
            if (source.IsDirectory)
            {
                CopyDirectoryWithoutDatabaseFiles(source.FullPath, temporary);
                Directory.Move(temporary, destination);
            }
            else
            {
                File.Copy(source.FullPath, temporary, overwrite: false);
                File.Move(temporary, destination);
                CopyMaterialCompanions(source.FullPath, destination);
            }
            published = true;
        }
        finally
        {
            TryDeletePublishedCopy(temporary);
            if (!published) TryDeletePublishedCopy(destination);
        }
    }

    private void CopyDirectoryWithoutDatabaseFiles(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        EnsureOrdinaryDirectory(destination, "Copy staging directory");
        var pending = new Stack<(string Source, string Destination)>();
        pending.Push((source, destination));
        while (pending.Count != 0)
        {
            (string currentSource, string currentDestination) = pending.Pop();
            EnsureNotReparse(currentSource, expectDirectory: true);
            foreach (string directory in Directory.EnumerateDirectories(currentSource))
            {
                EnsureNotReparse(directory, expectDirectory: true);
                string name = Path.GetFileName(directory);
                if (name.Equals(AssetDatabase.InternalDirectoryName, StringComparison.OrdinalIgnoreCase) ||
                    AssetCreationWorkflow.IsTemporaryPath(directory))
                {
                    continue;
                }
                string nextDestination = Path.Combine(currentDestination, name);
                Directory.CreateDirectory(nextDestination);
                EnsureOrdinaryDirectory(nextDestination, "Copy staging directory");
                pending.Push((directory, nextDestination));
            }
            foreach (string file in Directory.EnumerateFiles(currentSource))
            {
                EnsureNotReparse(file, expectDirectory: false);
                if ((file.EndsWith(AssetDatabase.MetadataSuffix, StringComparison.OrdinalIgnoreCase) &&
                     !IsMaterialGraphMetadataPath(file)) ||
                    AssetCreationWorkflow.IsTemporaryPath(file))
                {
                    continue;
                }
                File.Copy(file, Path.Combine(currentDestination, Path.GetFileName(file)), false);
            }
        }
    }

    private string FindCopyDestination(
        PathTarget source,
        string parent,
        ISet<string> reservedDestinations)
    {
        string extension = source.IsDirectory ? "" : Path.GetExtension(source.FullPath);
        string stem = source.IsDirectory
            ? Path.GetFileName(source.FullPath)
            : Path.GetFileNameWithoutExtension(source.FullPath);
        string direct = Path.Combine(parent, stem + extension);
        bool samePath = PathComparer.Equals(direct, source.FullPath);
        if (!samePath &&
            !reservedDestinations.Contains(direct) &&
            !ExistsWithMetadata(direct))
        {
            return direct;
        }
        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            string postfix = suffix == 0 ? "_Copy" : "_Copy" + (suffix + 1);
            string candidate = Path.Combine(parent, stem + postfix + extension);
            if (!reservedDestinations.Contains(candidate) &&
                !ExistsWithMetadata(candidate))
            {
                return candidate;
            }
        }
        throw new IOException("No collision-free duplicate name is available.");
    }

    private void CopyMaterialCompanions(string sourceAsset, string destinationAsset)
    {
        if (!IsMaterialAssetPath(sourceAsset)) return;
        string sourceGraph = MaterialGraphPath(sourceAsset);
        string destinationGraph = MaterialGraphPath(destinationAsset);
        CopyCompanionIfPresent(sourceGraph, destinationGraph);
        CopyCompanionIfPresent(
            sourceGraph + AssetDatabase.MetadataSuffix,
            destinationGraph + AssetDatabase.MetadataSuffix);
    }

    private void CopyCompanionIfPresent(string source, string destination)
    {
        if (!File.Exists(source)) return;
        EnsureNoReparseParents(source);
        EnsureNotReparse(source, expectDirectory: false);
        if (File.Exists(destination) || Directory.Exists(destination))
            throw new IOException($"Destination already exists: {DisplayPath(destination)}");
        string temporary = destination + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            File.Copy(source, temporary, overwrite: false);
            EnsureNotReparse(temporary, expectDirectory: false);
            File.Move(temporary, destination);
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    private void MoveMaterialCompanions(
        string sourceAsset,
        string destinationAsset,
        List<StagedMove> moved)
    {
        if (!IsMaterialAssetPath(sourceAsset)) return;
        string sourceGraph = MaterialGraphPath(sourceAsset);
        string destinationGraph = MaterialGraphPath(destinationAsset);
        var mappings = new[]
        {
            (Source: sourceGraph, Destination: destinationGraph),
            (Source: sourceGraph + AssetDatabase.MetadataSuffix,
             Destination: destinationGraph + AssetDatabase.MetadataSuffix),
        };
        foreach ((string source, string destination) in mappings)
        {
            if (!File.Exists(source)) continue;
            EnsureNoReparseParents(source);
            EnsureNotReparse(source, expectDirectory: false);
            if (File.Exists(destination) || Directory.Exists(destination))
                throw new IOException($"Destination already exists: {DisplayPath(destination)}");
        }
        foreach ((string source, string destination) in mappings)
        {
            if (!File.Exists(source)) continue;
            File.Move(source, destination);
            moved.Add(new StagedMove(destination, source, false));
        }
    }

    private static IEnumerable<string> EnumerateExistingAssetFamilyFiles(string assetPath)
    {
        if (File.Exists(assetPath)) yield return assetPath;
        string metadata = assetPath + AssetDatabase.MetadataSuffix;
        if (File.Exists(metadata)) yield return metadata;
        if (!IsMaterialAssetPath(assetPath)) yield break;
        string graph = MaterialGraphPath(assetPath);
        if (File.Exists(graph)) yield return graph;
        string graphMetadata = graph + AssetDatabase.MetadataSuffix;
        if (File.Exists(graphMetadata)) yield return graphMetadata;
    }

    private static bool IsPartOfTarget(string path, PathTarget target) =>
        PathComparer.Equals(path, target.FullPath) ||
        (target.IsDirectory && IsUnder(path, target.FullPath)) ||
        (!target.IsDirectory && IsMaterialAssetPath(target.FullPath) &&
         (PathComparer.Equals(path, MaterialGraphPath(target.FullPath)) ||
          PathComparer.Equals(
              path,
              MaterialGraphPath(target.FullPath) + AssetDatabase.MetadataSuffix)));

    private static bool IsMaterialAssetPath(string path) =>
        path.EndsWith(".acsmat", StringComparison.OrdinalIgnoreCase);

    private static string MaterialGraphPath(string materialPath) =>
        materialPath + MaterialGraphSuffix;

    private static bool IsMaterialGraphCompanionPath(string path) =>
        path.EndsWith(".acsmat" + MaterialGraphSuffix, StringComparison.OrdinalIgnoreCase);

    private static bool IsMaterialGraphMetadataPath(string path) =>
        path.EndsWith(
            ".acsmat" + MaterialGraphSuffix + AssetDatabase.MetadataSuffix,
            StringComparison.OrdinalIgnoreCase);

    private string CreateOperationDirectory(string operation)
    {
        Directory.CreateDirectory(_operationsRoot);
        EnsureNoReparseParents(_operationsRoot);
        EnsureOrdinaryDirectory(_operationsRoot, "Asset operation storage");
        string path = Path.Combine(
            _operationsRoot,
            operation + "-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        EnsureOrdinaryDirectory(path, "Asset operation storage");
        return path;
    }

    private static bool RollbackStagedMoves(List<StagedMove> staged)
    {
        bool complete = true;
        foreach (StagedMove move in staged.AsEnumerable().Reverse())
        {
            try
            {
                string? parent = Path.GetDirectoryName(move.Original);
                if (parent != null) Directory.CreateDirectory(parent);
                if (move.IsDirectory)
                {
                    if (Directory.Exists(move.Staged) && !Directory.Exists(move.Original))
                        Directory.Move(move.Staged, move.Original);
                }
                else if (File.Exists(move.Staged) && !File.Exists(move.Original))
                {
                    File.Move(move.Staged, move.Original);
                }
                bool originalRestored = move.IsDirectory
                    ? Directory.Exists(move.Original)
                    : File.Exists(move.Original);
                if (File.Exists(move.Staged) || Directory.Exists(move.Staged) ||
                    !originalRestored)
                    complete = false;
            }
            catch
            {
                // Preserve the initiating failure. The quarantine retains the recoverable data.
                complete = false;
            }
        }
        return complete;
    }

    private void RevalidateTarget(PathTarget expected)
    {
        PathTarget current = ValidateTarget(
            expected.FullPath,
            requireTreeValidation: expected.IsDirectory);
        if (current.IsDirectory != expected.IsDirectory ||
            !PathComparer.Equals(current.FullPath, expected.FullPath))
        {
            throw new InvalidDataException("Asset type or path changed during the operation.");
        }
    }

    private bool TryRollbackMove(MovePlan plan)
    {
        try
        {
            if (plan.Source.IsDirectory)
            {
                if (Directory.Exists(plan.Destination) &&
                    !Directory.Exists(plan.Source.FullPath))
                {
                    Directory.Move(plan.Destination, plan.Source.FullPath);
                }
                return Directory.Exists(plan.Source.FullPath) &&
                       !Directory.Exists(plan.Destination);
            }

            if (_database.TryGetByAssetId(plan.AssetId, out AssetRecord? current) &&
                current != null)
            {
                if (!PathComparer.Equals(
                        NormalizeDirectory(current.FullPath),
                        NormalizeDirectory(plan.Source.FullPath)))
                {
                    string relative = Path.GetRelativePath(_assetsRoot, plan.Source.FullPath)
                        .Replace('\\', '/');
                    _database.MoveAsset(plan.AssetId, relative);
                }
                return File.Exists(plan.Source.FullPath) &&
                       !File.Exists(plan.Destination);
            }
            if (File.Exists(plan.Destination) && !File.Exists(plan.Source.FullPath))
                File.Move(plan.Destination, plan.Source.FullPath);
            string destinationMetadata = plan.Destination + AssetDatabase.MetadataSuffix;
            string sourceMetadata = plan.Source.FullPath + AssetDatabase.MetadataSuffix;
            if (File.Exists(destinationMetadata) && !File.Exists(sourceMetadata))
                File.Move(destinationMetadata, sourceMetadata);
            return File.Exists(plan.Source.FullPath) &&
                   File.Exists(sourceMetadata) &&
                   !File.Exists(plan.Destination) &&
                   !File.Exists(destinationMetadata);
        }
        catch
        {
            // Preserve the initiating exception; a following Refresh recovers what it safely can.
            return false;
        }
    }

    private void ValidateOrdinaryTree(string root)
    {
        foreach (OrdinaryTreeEntry entry in EnumerateOrdinaryTree(root))
        {
            if (entry.IsDirectory && Path.GetFileName(entry.FullPath).Equals(
                    AssetDatabase.InternalDirectoryName,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException("Nested .acsdb directories are reserved.");
            }
        }
    }

    private static IEnumerable<OrdinaryTreeEntry> EnumerateOrdinaryTree(string root)
    {
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.Count != 0)
        {
            string directory = pending.Pop();
            EnsureNotReparse(directory, expectDirectory: true);
            foreach (FileSystemInfo entry in new DirectoryInfo(directory)
                         .EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly))
            {
                entry.Refresh();
                FileAttributes attributes = entry.Attributes;
                bool isDirectory = (attributes & FileAttributes.Directory) != 0;
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException(
                        "Only ordinary asset files and folders are supported.");
                }
                yield return new OrdinaryTreeEntry(entry.FullName, isDirectory);
                if (isDirectory) pending.Push(entry.FullName);
            }
        }
    }

    private void EnsureNoReparseParents(string path)
    {
        string parent = Directory.Exists(path)
            ? path
            : Path.GetDirectoryName(path)
                ?? throw new InvalidDataException("Path has no parent directory.");
        if (!IsUnderOrEqual(parent, _assetsRoot))
            throw new InvalidDataException("Path escapes Assets.");
        string relative = Path.GetRelativePath(_assetsRoot, parent);
        string cursor = _assetsRoot;
        EnsureOrdinaryDirectory(cursor, "Assets root");
        if (relative == ".") return;
        foreach (string segment in relative.Split(
                     new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                     StringSplitOptions.RemoveEmptyEntries))
        {
            cursor = Path.Combine(cursor, segment);
            EnsureOrdinaryDirectory(cursor, "Asset directory");
        }
    }

    private static void EnsureNotReparse(string path, bool expectDirectory)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0 ||
            ((attributes & FileAttributes.Directory) != 0) != expectDirectory)
        {
            throw new InvalidDataException("Only ordinary asset files and folders are supported.");
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

    private static bool IsNumberedDevice(string value, string prefix) =>
        value.Length == prefix.Length + 1 &&
        value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
        value[^1] is >= '1' and <= '9';

    private static long SaturatingAdd(long left, long right) =>
        right > 0 && left > long.MaxValue - right ? long.MaxValue : left + right;

    private void EnsureDestinationAvailable(string path)
    {
        if (ExistsWithMetadata(path))
            throw new IOException($"Destination already exists: {DisplayPath(path)}");
        string parent = Path.GetDirectoryName(path)
            ?? throw new InvalidDataException("Destination has no parent directory.");
        EnsureNoReparseParents(parent);
        EnsureOrdinaryDirectory(parent, "Destination directory");
    }

    private static bool ExistsWithMetadata(string path) =>
        File.Exists(path) || Directory.Exists(path) ||
        File.Exists(path + AssetDatabase.MetadataSuffix) ||
        (IsMaterialAssetPath(path) &&
         (File.Exists(MaterialGraphPath(path)) ||
          File.Exists(MaterialGraphPath(path) + AssetDatabase.MetadataSuffix)));

    private void TryDeletePublishedCopy(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                EnsureNotReparse(path, expectDirectory: false);
                File.Delete(path);
            }
            if (File.Exists(path + AssetDatabase.MetadataSuffix))
            {
                EnsureNotReparse(
                    path + AssetDatabase.MetadataSuffix,
                    expectDirectory: false);
                File.Delete(path + AssetDatabase.MetadataSuffix);
            }
            if (IsMaterialAssetPath(path))
            {
                string graph = MaterialGraphPath(path);
                if (File.Exists(graph))
                {
                    EnsureNotReparse(graph, expectDirectory: false);
                    File.Delete(graph);
                }
                string graphMetadata = graph + AssetDatabase.MetadataSuffix;
                if (File.Exists(graphMetadata))
                {
                    EnsureNotReparse(graphMetadata, expectDirectory: false);
                    File.Delete(graphMetadata);
                }
            }
            if (Directory.Exists(path)) TryDeleteOrdinaryDirectoryTree(path);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        TryDeleteOrdinaryDirectoryTree(path);
    }

    private static bool TryDeleteOrdinaryDirectoryTree(string path)
    {
        if (!Directory.Exists(path)) return true;
        try
        {
            OrdinaryTreeEntry[] entries = EnumerateOrdinaryTree(path).ToArray();
            foreach (OrdinaryTreeEntry entry in entries.Where(
                         static entry => !entry.IsDirectory))
            {
                EnsureNotReparse(entry.FullPath, expectDirectory: false);
                File.Delete(entry.FullPath);
            }
            foreach (OrdinaryTreeEntry entry in entries
                         .Where(static entry => entry.IsDirectory)
                         .OrderByDescending(static entry => entry.FullPath.Length))
            {
                EnsureNotReparse(entry.FullPath, expectDirectory: true);
                Directory.Delete(entry.FullPath, recursive: false);
            }
            EnsureNotReparse(path, expectDirectory: true);
            Directory.Delete(path, recursive: false);
            return !Directory.Exists(path);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return false;
        }
    }

    private void TryRefresh()
    {
        try { _database.Refresh(); }
        catch { }
    }

    /// <summary>
    /// Reloads path identity, sidecars, and dependency metadata while the caller owns the
    /// project mutation lease. A refresh performed before acquiring that lease leaves a gap in
    /// which another editor can publish authoritative metadata that this workflow would otherwise
    /// validate or copy from a stale in-memory snapshot.
    /// </summary>
    private void RefreshAuthoritativeState() =>
        _database.Refresh(verifyContent: true);

    private string DisplayPath(string path) =>
        "Assets/" + Path.GetRelativePath(_assetsRoot, path).Replace('\\', '/');

    private static string NormalizeDirectory(string path) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    private static bool IsUnder(string candidate, string root)
    {
        string relative = Path.GetRelativePath(root, candidate);
        return relative != "." && !Path.IsPathRooted(relative) && relative != ".." &&
               !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
               !relative.StartsWith(".." + Path.AltDirectorySeparatorChar, StringComparison.Ordinal);
    }

    private static bool IsUnderOrEqual(string candidate, string root) =>
        PathComparer.Equals(NormalizeDirectory(candidate), NormalizeDirectory(root)) ||
        IsUnder(candidate, root);

    private sealed record PathTarget(string FullPath, bool IsDirectory);
    private sealed record CopyPlan(
        PathTarget Source,
        string Destination,
        IReadOnlyList<AssetRecord> OriginalRecords);
    private sealed record MovePlan(
        PathTarget Source,
        string Destination,
        string AssetId,
        IReadOnlyList<AssetRecord> OriginalRecords);
    private sealed record StagedMove(string Staged, string Original, bool IsDirectory);
    private sealed record ReferenceToken(string Value, bool IsAssetId);
    private sealed record ReferenceReplacement(
        string Source,
        string Destination,
        bool IsAssetId);
    private sealed record FileRewriteBackup(
        string FullPath,
        byte[] OriginalBytes,
        long OriginalLastWriteUtcTicks,
        FileAttributes OriginalAttributes);
    private sealed record TextRewritePlan(
        FileRewriteBackup Backup,
        byte[] UpdatedBytes);
    private sealed record MetadataRewritePlan(
        AssetRecord Original,
        AssetRecord Moved,
        string UpdatedSource,
        IReadOnlyList<KeyValuePair<string, string>> UpdatedSettings,
        FileRewriteBackup SidecarBackup);
    private sealed record OrdinaryTreeEntry(string FullPath, bool IsDirectory);
    private sealed record ReferenceCandidate(string FullPath, string RelativePath);
}
