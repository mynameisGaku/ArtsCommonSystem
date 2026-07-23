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
