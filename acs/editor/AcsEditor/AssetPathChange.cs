// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal enum AssetPathMutationKind
{
    Rename,
    Move,
    Delete,
}

internal sealed record AssetPathMapping(string OriginalRoot, string DestinationRoot);

/// <summary>
/// Describes a successful Content Browser filesystem mutation. Roots apply to the complete
/// subtree, not just to the selected file, so open documents below a moved folder can rebind.
/// </summary>
internal sealed class AssetPathsChangedEventArgs : EventArgs
{
    internal AssetPathsChangedEventArgs(
        IEnumerable<AssetPathMapping>? mappings = null,
        IEnumerable<string>? deletedRoots = null)
    {
        Mappings = Array.AsReadOnly((mappings ?? Array.Empty<AssetPathMapping>())
            .Select(static mapping => new AssetPathMapping(
                AssetPathBoundary.Normalize(mapping.OriginalRoot),
                AssetPathBoundary.Normalize(mapping.DestinationRoot)))
            .Where(static mapping => !AssetPathBoundary.Equals(
                mapping.OriginalRoot,
                mapping.DestinationRoot))
            .DistinctBy(
                static mapping => mapping.OriginalRoot,
                StringComparer.OrdinalIgnoreCase)
            .OrderByDescending(static mapping => mapping.OriginalRoot.Length)
            .ToArray());
        DeletedRoots = Array.AsReadOnly((deletedRoots ?? Array.Empty<string>())
            .Select(AssetPathBoundary.Normalize)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderByDescending(static root => root.Length)
            .ToArray());
    }

    internal ReadOnlyCollection<AssetPathMapping> Mappings { get; }
    internal ReadOnlyCollection<string> DeletedRoots { get; }

    internal bool AffectsPath(string path)
    {
        string candidate = AssetPathBoundary.Normalize(path);
        return Mappings.Any(mapping =>
                   AssetPathBoundary.TryGetSuffix(
                       candidate,
                       mapping.OriginalRoot,
                       out _)) ||
               DeletedRoots.Any(root =>
                   AssetPathBoundary.TryGetSuffix(candidate, root, out _));
    }

    internal bool TryRemapPath(string path, out string remappedPath)
    {
        string candidate = AssetPathBoundary.Normalize(path);
        foreach (AssetPathMapping mapping in Mappings)
        {
            if (!AssetPathBoundary.TryGetSuffix(
                    candidate,
                    mapping.OriginalRoot,
                    out string suffix))
                continue;
            remappedPath = mapping.DestinationRoot + suffix;
            return true;
        }
        remappedPath = candidate;
        return false;
    }

    internal bool IsDeletedPath(string path)
    {
        string candidate = AssetPathBoundary.Normalize(path);
        return DeletedRoots.Any(root =>
            AssetPathBoundary.TryGetSuffix(candidate, root, out _));
    }
}

internal sealed class AssetPathMutationStartingEventArgs : EventArgs
{
    internal AssetPathMutationStartingEventArgs(
        Guid operationId,
        AssetPathMutationKind kind,
        IEnumerable<string> affectedRoots)
    {
        OperationId = operationId;
        Kind = kind;
        AffectedRoots = Array.AsReadOnly(AssetPathBoundary.CollapseRoots(affectedRoots));
    }

    internal Guid OperationId { get; }
    internal AssetPathMutationKind Kind { get; }
    internal ReadOnlyCollection<string> AffectedRoots { get; }
    internal bool Cancel { get; set; }
    internal string CancellationReason { get; set; } = "";

    internal bool AffectsPath(string path)
    {
        string candidate = AssetPathBoundary.Normalize(path);
        return AffectedRoots.Any(root =>
            AssetPathBoundary.TryGetSuffix(candidate, root, out _));
    }
}

internal sealed class AssetPathMutationCompletedEventArgs : EventArgs
{
    internal AssetPathMutationCompletedEventArgs(
        AssetPathMutationStartingEventArgs operation,
        bool succeeded)
    {
        Operation = operation;
        Succeeded = succeeded;
    }

    internal AssetPathMutationStartingEventArgs Operation { get; }
    internal bool Succeeded { get; }
}

internal static class AssetPathBoundary
{
    internal static string Normalize(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("Asset path cannot be empty.", nameof(path));
        return Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
    }

    internal static bool Equals(string first, string second) =>
        string.Equals(first, second, StringComparison.OrdinalIgnoreCase);

    internal static bool TryGetSuffix(
        string normalizedCandidate,
        string normalizedRoot,
        out string suffix)
    {
        if (Equals(normalizedCandidate, normalizedRoot))
        {
            suffix = "";
            return true;
        }
        if (normalizedCandidate.Length > normalizedRoot.Length &&
            normalizedCandidate.StartsWith(
                normalizedRoot,
                StringComparison.OrdinalIgnoreCase) &&
            IsSeparator(normalizedCandidate[normalizedRoot.Length]))
        {
            suffix = normalizedCandidate[normalizedRoot.Length..];
            return true;
        }
        suffix = "";
        return false;
    }

    internal static string[] CollapseRoots(IEnumerable<string> roots)
    {
        string[] normalized = roots
            .Select(Normalize)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(static root => root.Length)
            .ThenBy(static root => root, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        var topLevel = new List<string>(normalized.Length);
        foreach (string candidate in normalized)
        {
            if (topLevel.Any(root => TryGetSuffix(candidate, root, out _)))
                continue;
            topLevel.Add(candidate);
        }
        return topLevel.ToArray();
    }

    private static bool IsSeparator(char value) =>
        value == Path.DirectorySeparatorChar ||
        value == Path.AltDirectorySeparatorChar;
}
