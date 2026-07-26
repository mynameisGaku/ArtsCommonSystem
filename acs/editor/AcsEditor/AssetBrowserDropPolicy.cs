// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace AcsEditor;

/// <summary>
/// In-process payload used when assets are reorganized inside the Asset View.
/// Keeping this separate from ASSET_PATH prevents folders and mixed selections
/// from being interpreted as objects that can be placed in the viewport.
/// </summary>
internal sealed record AssetBrowserDragPayload(
    int ProjectGeneration,
    string AssetsRoot,
    AssetBrowserDragEntry[] Entries);

internal sealed record AssetBrowserDragEntry(string FullPath, bool IsDirectory);

internal sealed record AssetBrowserDropPlan(
    string DestinationDirectory,
    IReadOnlyList<string> SourcePaths,
    bool CanMove,
    bool CanCopy,
    string RejectionReason)
{
    internal bool IsValid => CanMove || CanCopy;

    internal static AssetBrowserDropPlan Rejected(string reason) =>
        new("", Array.Empty<string>(), false, false, reason);
}

internal sealed record AssetBrowserImportDropPlan(
    string DestinationDirectory,
    IReadOnlyList<string> SourcePaths,
    string RejectionReason)
{
    internal bool IsValid =>
        DestinationDirectory.Length != 0 && SourcePaths.Count != 0;

    internal static AssetBrowserImportDropPlan Rejected(string reason) =>
        new("", Array.Empty<string>(), reason);
}

/// <summary>
/// Pure, filesystem-independent validation for Asset View drop targets. The
/// transactional workflow performs authoritative filesystem validation again
/// immediately before mutation.
/// </summary>
internal static class AssetBrowserDropPolicy
{
    private const int MaxDraggedEntries = 4096;
    private static readonly StringComparer PathComparer =
        StringComparer.OrdinalIgnoreCase;

    internal static AssetBrowserDropPlan Evaluate(
        AssetBrowserDragPayload? payload,
        int currentProjectGeneration,
        string currentAssetsRoot,
        string destinationDirectory,
        bool destinationIsDirectory)
    {
        if (payload == null)
            return AssetBrowserDropPlan.Rejected("The drag payload is not an Asset View payload.");
        if (payload.ProjectGeneration != currentProjectGeneration)
            return AssetBrowserDropPlan.Rejected("The drag belongs to a stale project view.");
        if (!destinationIsDirectory)
            return AssetBrowserDropPlan.Rejected("Assets can only be dropped on a folder tile.");
        if (payload.Entries == null || payload.Entries.Length == 0 ||
            payload.Entries.Length > MaxDraggedEntries)
        {
            return AssetBrowserDropPlan.Rejected("The drag selection is empty or too large.");
        }
        if (!TryNormalizeAbsolute(currentAssetsRoot, out string assetsRoot) ||
            !TryNormalizeAbsolute(payload.AssetsRoot, out string payloadRoot) ||
            !PathComparer.Equals(assetsRoot, payloadRoot))
        {
            return AssetBrowserDropPlan.Rejected("The drag belongs to a different Assets root.");
        }
        if (!TryNormalizeAbsolute(destinationDirectory, out string destination) ||
            !IsUnderOrEqual(destination, assetsRoot))
        {
            return AssetBrowserDropPlan.Rejected("The destination is outside the current Assets root.");
        }

        var entries = new Dictionary<string, bool>(PathComparer);
        foreach (AssetBrowserDragEntry? entry in payload.Entries)
        {
            if (entry == null ||
                !TryNormalizeAbsolute(entry.FullPath, out string source) ||
                PathComparer.Equals(source, assetsRoot) ||
                !IsUnderOrEqual(source, assetsRoot))
            {
                return AssetBrowserDropPlan.Rejected(
                    "The drag contains an invalid or external asset path.");
            }
            if (entries.TryGetValue(source, out bool knownDirectory))
            {
                if (knownDirectory != entry.IsDirectory)
                {
                    return AssetBrowserDropPlan.Rejected(
                        "The drag contains inconsistent duplicate entries.");
                }
                continue;
            }
            entries.Add(source, entry.IsDirectory);
        }

        foreach ((string source, bool isDirectory) in entries)
        {
            if (PathComparer.Equals(source, destination) ||
                (isDirectory && IsUnderOrEqual(destination, source)))
            {
                return AssetBrowserDropPlan.Rejected(
                    "A folder cannot be moved or copied into itself.");
            }
        }

        string[] sources = entries.Keys
            .OrderBy(static path => path, PathComparer)
            .ToArray();
        bool canMove = sources.Any(source =>
        {
            string? parent = Path.GetDirectoryName(source);
            return parent != null && !PathComparer.Equals(
                NormalizeUnchecked(parent), destination);
        });
        return new AssetBrowserDropPlan(
            destination,
            Array.AsReadOnly(sources),
            canMove,
            CanCopy: true,
            RejectionReason: "");
    }

    /// <summary>
    /// Plans an Explorer/file-manager drop without probing the filesystem on
    /// WPF's drag-over path. The import workflow revalidates every source as an
    /// ordinary file, rejects reparse points, and revalidates the destination
    /// immediately before publishing the transaction.
    /// </summary>
    internal static AssetBrowserImportDropPlan EvaluateExternalImport(
        IEnumerable<string>? sourcePaths,
        string currentAssetsRoot,
        string destinationDirectory,
        bool destinationIsDirectory)
    {
        if (!destinationIsDirectory)
        {
            return AssetBrowserImportDropPlan.Rejected(
                "Imported files require an Asset View folder destination.");
        }
        if (!TryNormalizeAbsolute(currentAssetsRoot, out string assetsRoot) ||
            !TryNormalizeAbsolute(destinationDirectory, out string destination) ||
            !IsUnderOrEqual(destination, assetsRoot))
        {
            return AssetBrowserImportDropPlan.Rejected(
                "The import destination is outside the current Assets root.");
        }

        string databaseRoot = Path.Combine(
            assetsRoot,
            AssetDatabase.InternalDirectoryName);
        if (IsUnderOrEqual(destination, databaseRoot))
        {
            return AssetBrowserImportDropPlan.Rejected(
                "Assets/.acsdb is reserved for the asset database.");
        }
        if (sourcePaths == null)
        {
            return AssetBrowserImportDropPlan.Rejected(
                "The external file drop is empty.");
        }

        var sources = new HashSet<string>(PathComparer);
        int entryCount = 0;
        foreach (string? sourcePath in sourcePaths)
        {
            entryCount++;
            if (entryCount > MaxDraggedEntries)
            {
                return AssetBrowserImportDropPlan.Rejected(
                    "The external file drop is too large.");
            }
            if (!TryNormalizeAbsolute(sourcePath, out string source))
            {
                return AssetBrowserImportDropPlan.Rejected(
                    "The external file drop contains an invalid path.");
            }
            if (IsUnderOrEqual(source, assetsRoot))
            {
                return AssetBrowserImportDropPlan.Rejected(
                    "Files already managed below Assets must be moved or copied " +
                    "inside the Asset View.");
            }
            sources.Add(source);
        }

        if (sources.Count == 0)
        {
            return AssetBrowserImportDropPlan.Rejected(
                "The external file drop is empty.");
        }
        string[] orderedSources = sources
            .OrderBy(static path => path, PathComparer)
            .ToArray();
        return new AssetBrowserImportDropPlan(
            destination,
            Array.AsReadOnly(orderedSources),
            "");
    }

    private static bool TryNormalizeAbsolute(string? path, out string normalized)
    {
        normalized = "";
        if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path))
            return false;
        try
        {
            normalized = NormalizeUnchecked(path);
            return normalized.Length != 0;
        }
        catch (Exception error) when (
            error is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
    }

    private static string NormalizeUnchecked(string path) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    private static bool IsUnderOrEqual(string candidate, string root)
    {
        if (PathComparer.Equals(candidate, root)) return true;
        string relative = Path.GetRelativePath(root, candidate);
        return !Path.IsPathRooted(relative) &&
               relative != ".." &&
               !relative.StartsWith(
                   ".." + Path.DirectorySeparatorChar,
                   StringComparison.Ordinal) &&
               !relative.StartsWith(
                   ".." + Path.AltDirectorySeparatorChar,
                   StringComparison.Ordinal);
    }
}
