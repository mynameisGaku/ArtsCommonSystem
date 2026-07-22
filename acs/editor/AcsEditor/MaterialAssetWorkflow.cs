// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace AcsEditor;

/// <summary>
/// A material entry presented by an Inspector slot. The catalogue is independent from WPF and
/// the native editor handle so 2D and 3D Mesh Renderer slots cannot drift apart.
/// </summary>
internal sealed record MaterialAssetChoice(
    string FullPath,
    string DisplayName,
    bool IsProjectAsset);

internal sealed record MaterialAssetCatalog(
    IReadOnlyList<MaterialAssetChoice> Choices,
    int SelectedIndex);

internal static class MaterialAssetWorkflow
{
    private const int MaxGeneratedSuffix = 9999;

    /// <summary>
    /// Enumerates project materials deterministically and keeps an already assigned material
    /// visible even when it lives outside Assets (or has gone missing). Reparse points are not
    /// followed: an Inspector refresh must not unexpectedly crawl a junction outside the project.
    /// </summary>
    internal static MaterialAssetCatalog BuildCatalog(
        string? assetsDirectory,
        string? currentMaterialPath)
    {
        string? root = TryFullPath(assetsDirectory);
        var choices = new List<MaterialAssetChoice>();

        if (root != null && IsOrdinaryDirectory(root))
        {
            var options = new EnumerationOptions
            {
                RecurseSubdirectories = true,
                IgnoreInaccessible = true,
                AttributesToSkip = FileAttributes.ReparsePoint,
                ReturnSpecialDirectories = false,
            };

            try
            {
                choices.AddRange(
                    Directory.EnumerateFiles(root, "*", options)
                        .Where(path => string.Equals(
                            Path.GetExtension(path),
                            ".acsmat",
                            StringComparison.OrdinalIgnoreCase))
                        .Select(path => MakeChoice(root, path, projectAsset: true))
                        .GroupBy(choice => choice.FullPath, PathComparer)
                        .Select(group => group
                            .OrderBy(choice => choice.FullPath, StringComparer.Ordinal)
                            .First())
                        .OrderBy(choice => choice.DisplayName, StringComparer.OrdinalIgnoreCase)
                        .ThenBy(choice => choice.DisplayName, StringComparer.Ordinal)
                        .ThenBy(choice => choice.FullPath, StringComparer.OrdinalIgnoreCase)
                        .ThenBy(choice => choice.FullPath, StringComparer.Ordinal));
            }
            catch (IOException)
            {
                // A concurrent asset import may replace a directory while it is being walked.
                // The slot still retains its current assignment below and refreshes next time.
            }
            catch (UnauthorizedAccessException)
            {
                // Ignore inaccessible subtrees just like EnumerationOptions.IgnoreInaccessible.
            }
        }

        int selected = -1;
        if (!string.IsNullOrWhiteSpace(currentMaterialPath))
        {
            selected = choices.FindIndex(choice =>
                SamePath(choice.FullPath, currentMaterialPath));
            if (selected < 0)
            {
                choices.Add(MakeChoice(root, currentMaterialPath, projectAsset: false));
                selected = choices.Count - 1;
            }
        }

        return new MaterialAssetCatalog(choices, selected);
    }

    /// <summary>Returns Material.acsmat, Material1.acsmat, ... without overwriting an asset.</summary>
    internal static string NextAvailablePath(
        string assetsDirectory,
        string baseName = "Material")
    {
        string root = ValidateCreationArguments(assetsDirectory, baseName);
        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            string candidate = CandidatePath(root, baseName, suffix);
            if (!File.Exists(candidate) && !Directory.Exists(candidate))
                return candidate;
        }

        throw new IOException($"No free material name was found below {root}.");
    }

    /// <summary>
    /// Atomically reserves the first free generated path with an empty file. The caller must
    /// replace that file with a valid material immediately and delete it if creation fails.
    /// </summary>
    internal static string ReserveNextAvailablePath(
        string assetsDirectory,
        string baseName = "Material")
    {
        string root = ValidateCreationArguments(assetsDirectory, baseName);
        if (!IsOrdinaryDirectory(root))
            throw new InvalidDataException(
                $"Material Assets root must be an ordinary directory: {root}");

        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            string candidate = CandidatePath(root, baseName, suffix);
            if (Directory.Exists(candidate)) continue;

            try
            {
                using var reservation = new FileStream(
                    candidate,
                    FileMode.CreateNew,
                    FileAccess.Write,
                    FileShare.Read);
                return candidate;
            }
            catch (IOException) when (File.Exists(candidate) || Directory.Exists(candidate))
            {
                // Another editor or asset import won the race. Continue with the next suffix.
            }
            catch (UnauthorizedAccessException) when (Directory.Exists(candidate))
            {
                // Windows reports a directory/file name collision as access denied.
            }
        }

        throw new IOException($"No free material name was found below {root}.");
    }

    internal static bool SamePath(string? left, string? right)
    {
        if (string.IsNullOrWhiteSpace(left) || string.IsNullOrWhiteSpace(right))
            return false;

        string? fullLeft = TryFullPath(left);
        string? fullRight = TryFullPath(right);
        return fullLeft != null && fullRight != null
            ? string.Equals(fullLeft, fullRight, StringComparison.OrdinalIgnoreCase)
            : string.Equals(left, right, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Failures expected from validating or reserving a user-selected material location. UI
    /// callers report these inline instead of allowing a filesystem validation error to escape.
    /// </summary>
    internal static bool IsRecoverableCreationFailure(Exception error)
    {
        ArgumentNullException.ThrowIfNull(error);
        return error is IOException or
               UnauthorizedAccessException or
               ArgumentException or
               InvalidDataException;
    }

    internal static string DisplayName(string? assetsDirectory, string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        string? root = TryFullPath(assetsDirectory);
        return MakeChoice(root, path, projectAsset: root != null).DisplayName;
    }

    private static MaterialAssetChoice MakeChoice(
        string? assetsRoot,
        string path,
        bool projectAsset)
    {
        string full = TryFullPath(path) ?? path;
        bool insideAssets = projectAsset &&
            assetsRoot != null &&
            IsBelowRoot(assetsRoot, full);
        string display = insideAssets
            ? Path.GetRelativePath(assetsRoot!, full).Replace('\\', '/')
            : Path.GetFileName(full);
        if (string.IsNullOrWhiteSpace(display))
            display = full;
        return new MaterialAssetChoice(full, display, insideAssets);
    }

    private static bool IsBelowRoot(string root, string candidate)
    {
        string relative;
        try
        {
            relative = Path.GetRelativePath(root, candidate);
        }
        catch (ArgumentException)
        {
            return false;
        }

        return relative.Length > 0 &&
               relative != "." &&
               relative != ".." &&
               !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
               !relative.StartsWith(".." + Path.AltDirectorySeparatorChar, StringComparison.Ordinal) &&
               !Path.IsPathRooted(relative);
    }

    private static string? TryFullPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        try
        {
            return Path.GetFullPath(path);
        }
        catch (Exception error) when (
            error is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return null;
        }
    }

    private static bool IsOrdinaryDirectory(string path)
    {
        try
        {
            FileAttributes attributes = File.GetAttributes(path);
            return (attributes & FileAttributes.Directory) != 0 &&
                   (attributes & FileAttributes.ReparsePoint) == 0;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return false;
        }
    }

    private static string ValidateCreationArguments(
        string assetsDirectory,
        string baseName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(assetsDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(baseName);
        if (baseName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            baseName.Contains(Path.DirectorySeparatorChar) ||
            baseName.Contains(Path.AltDirectorySeparatorChar))
        {
            throw new ArgumentException("Material base name must be a file name.", nameof(baseName));
        }

        return Path.GetFullPath(assetsDirectory);
    }

    private static string CandidatePath(
        string root,
        string baseName,
        int suffix)
    {
        string name = suffix == 0 ? baseName : baseName + suffix;
        return Path.Combine(root, name + ".acsmat");
    }

    private static readonly StringComparer PathComparer =
        StringComparer.OrdinalIgnoreCase;
}
