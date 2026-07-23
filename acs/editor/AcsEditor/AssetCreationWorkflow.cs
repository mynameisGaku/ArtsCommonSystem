// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace AcsEditor;

internal enum AcsAssetTemplate
{
    Folder,
    Material,
    Blueprint,
}

internal sealed record AcsAssetTemplateDefinition(
    AcsAssetTemplate Template,
    string DisplayName,
    string BaseName,
    string Extension,
    bool IsDirectory);

internal sealed record AcsAssetCreationResult(
    string FullPath,
    AcsAssetTemplateDefinition Definition);

/// <summary>
/// Pure filesystem workflow used by the Content Browser's New menu. New ACS files are written
/// completely to a same-directory temporary file and then published with a no-overwrite move,
/// so the asset index never observes an empty or partially-written asset. Creation is confined
/// to ordinary directories below the project's Assets root and never follows reparse points.
/// </summary>
internal static class AssetCreationWorkflow
{
    private const int MaxGeneratedSuffix = 9999;
    private const string TemporaryMarker = ".tmp-";
    private static readonly UTF8Encoding Utf8WithoutBom = new(false);

    internal static IReadOnlyList<AcsAssetTemplateDefinition> Definitions { get; } =
        new[]
        {
            new AcsAssetTemplateDefinition(
                AcsAssetTemplate.Folder,
                "Folder",
                "NewFolder",
                "",
                IsDirectory: true),
            new AcsAssetTemplateDefinition(
                AcsAssetTemplate.Material,
                "Material",
                "Material",
                ".acsmat",
                IsDirectory: false),
            new AcsAssetTemplateDefinition(
                AcsAssetTemplate.Blueprint,
                "Blueprint",
                "Blueprint",
                ".acsbp",
                IsDirectory: false),
        };

    internal static bool IsTemporaryPath(string path) =>
        !string.IsNullOrWhiteSpace(path) &&
        Path.GetFileName(path).Contains(TemporaryMarker, StringComparison.OrdinalIgnoreCase);

    internal static AcsAssetCreationResult Create(
        string assetsDirectory,
        string currentDirectory,
        AcsAssetTemplate template,
        Func<string, string, bool>? canonicalMaterialWriter = null)
    {
        AcsAssetTemplateDefinition definition = FindDefinition(template);
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            assetsDirectory,
            $"Create {definition.DisplayName} asset");
        string directory = ValidateTargetDirectory(assetsDirectory, currentDirectory);

        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            string stem = suffix == 0
                ? definition.BaseName
                : definition.BaseName + suffix;
            string destination = Path.Combine(directory, stem + definition.Extension);
            if (File.Exists(destination) || Directory.Exists(destination))
                continue;

            if (definition.IsDirectory)
            {
                AcsAssetCreationResult? folder = TryCreateDirectory(
                    assetsDirectory,
                    directory,
                    destination,
                    definition);
                if (folder != null)
                    return folder;
                continue;
            }

            string temporary = Path.Combine(
                directory,
                "." + Path.GetFileName(destination) + TemporaryMarker +
                Guid.NewGuid().ToString("N"));
            try
            {
                WriteCompleteTemporaryAsset(
                    temporary,
                    definition.Template,
                    stem,
                    canonicalMaterialWriter);

                // The directory may have been replaced while the temporary file was written.
                // Revalidate before publishing so a stale browser path cannot escape Assets.
                ValidateTargetDirectory(assetsDirectory, directory);
                try
                {
                    File.Move(temporary, destination, overwrite: false);
                }
                catch (Exception error) when (
                    error is IOException or UnauthorizedAccessException &&
                    (File.Exists(destination) || Directory.Exists(destination)))
                {
                    // Another editor/import won this generated name. Retry the next suffix.
                    continue;
                }

                return new AcsAssetCreationResult(destination, definition);
            }
            finally
            {
                TryDeleteTemporary(temporary);
            }
        }

        throw new IOException(
            $"No free {definition.DisplayName} asset name was found below {directory}.");
    }

    private static AcsAssetTemplateDefinition FindDefinition(AcsAssetTemplate template)
    {
        foreach (AcsAssetTemplateDefinition definition in Definitions)
        {
            if (definition.Template == template)
                return definition;
        }

        throw new ArgumentOutOfRangeException(nameof(template), template, "Unknown ACS asset template.");
    }

    private static string BuildTemplate(AcsAssetTemplate template) =>
        template switch
        {
            AcsAssetTemplate.Blueprint => "ACSBP 1\n",
            _ => throw new ArgumentOutOfRangeException(
                nameof(template), template, "Unknown ACS asset template."),
        };

    private static void WriteCompleteTemporaryAsset(
        string path,
        AcsAssetTemplate template,
        string stem,
        Func<string, string, bool>? canonicalMaterialWriter)
    {
        if (template == AcsAssetTemplate.Material)
        {
            if (canonicalMaterialWriter == null)
            {
                throw new InvalidOperationException(
                    "Material creation requires the canonical native ACSMAT serializer.");
            }
            if (!canonicalMaterialWriter(path, stem) ||
                !File.Exists(path) ||
                new FileInfo(path).Length == 0)
            {
                throw new InvalidDataException(
                    "The canonical ACSMAT serializer did not produce a valid asset file.");
            }
            return;
        }

        string contents = BuildTemplate(template);
        byte[] bytes = Utf8WithoutBom.GetBytes(contents);
        using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.WriteThrough);
        stream.Write(bytes);
        stream.Flush(flushToDisk: true);
    }

    private static AcsAssetCreationResult? TryCreateDirectory(
        string assetsDirectory,
        string directory,
        string destination,
        AcsAssetTemplateDefinition definition)
    {
        string temporary = Path.Combine(
            directory,
            "." + Path.GetFileName(destination) + TemporaryMarker +
            Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(temporary);
            ValidateTargetDirectory(assetsDirectory, directory);
            try
            {
                Directory.Move(temporary, destination);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException &&
                (File.Exists(destination) || Directory.Exists(destination)))
            {
                return null;
            }
            return new AcsAssetCreationResult(destination, definition);
        }
        finally
        {
            TryDeleteTemporaryDirectory(temporary);
        }
    }

    private static string ValidateTargetDirectory(
        string assetsDirectory,
        string currentDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(assetsDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(currentDirectory);

        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsDirectory));
        string target = Path.TrimEndingDirectorySeparator(Path.GetFullPath(currentDirectory));
        EnsureOrdinaryDirectory(root, "Assets root");

        string relative = Path.GetRelativePath(root, target);
        if (Path.IsPathRooted(relative) ||
            relative == ".." ||
            relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) ||
            relative.StartsWith(".." + Path.AltDirectorySeparatorChar, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Asset creation target must stay below the project's Assets root: {target}");
        }

        string cursor = root;
        if (relative != ".")
        {
            foreach (string segment in relative.Split(
                         new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                         StringSplitOptions.RemoveEmptyEntries))
            {
                cursor = Path.Combine(cursor, segment);
                EnsureOrdinaryDirectory(cursor, "Asset creation target");
            }
        }

        return target;
    }

    private static void EnsureOrdinaryDirectory(string path, string label)
    {
        FileAttributes attributes;
        try
        {
            attributes = File.GetAttributes(path);
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            throw new InvalidDataException($"{label} is unavailable: {path}", error);
        }

        if ((attributes & FileAttributes.Directory) == 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                $"{label} must be an ordinary directory: {path}");
        }
    }

    private static void TryDeleteTemporary(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            // The destination was never published from this path. A later cleanup/index pass can
            // remove an orphaned .tmp- temporary without mistaking it for an ACS asset.
        }
    }

    private static void TryDeleteTemporaryDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
                Directory.Delete(path, recursive: false);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            // Same cleanup policy as temporary files. The browser filters this marker.
        }
    }
}
