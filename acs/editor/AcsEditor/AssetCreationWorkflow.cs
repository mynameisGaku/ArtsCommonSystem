// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;

namespace AcsEditor;

internal enum AcsAssetTemplate
{
    Folder,
    Scene,
    Material,
    Blueprint,
    Prefab,
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
/// Self-contained filename policy shared by creation requests without taking a dependency on
/// the much larger management workflow. AssetCreationWorkflow is also linked into the
/// command-line packager, so this boundary must remain limited to BCL and AssetDatabase rules.
/// </summary>
internal static class AssetCreationNameRules
{
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

    private static bool IsNumberedDevice(string value, string prefix) =>
        value.Length == prefix.Length + 1 &&
        value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
        value[^1] is >= '1' and <= '9';
}

internal enum AssetScenePlacementDecision
{
    Allow,
    RejectUnified3DIntoLegacy2D,
    RejectLegacy2DIntoUnified3D,
}

/// <summary>
/// Keeps legacy 2D and unified 3D asset payloads on the source graph that will
/// actually be serialized. A mismatched placement must be rejected before any
/// native paste call; otherwise it mutates the hidden compatibility graph and
/// the instance disappears on the next save/reopen.
/// </summary>
internal static class AssetScenePlacementPolicy
{
    internal static AssetScenePlacementDecision Evaluate(
        bool activeSourceUses3D,
        bool payloadUses3D) =>
        (activeSourceUses3D, payloadUses3D) switch
        {
            (true, false) =>
                AssetScenePlacementDecision.RejectLegacy2DIntoUnified3D,
            (false, true) =>
                AssetScenePlacementDecision.RejectUnified3DIntoLegacy2D,
            _ => AssetScenePlacementDecision.Allow,
        };

    internal static string RejectionMessage(
        AssetScenePlacementDecision decision,
        string assetLabel,
        string fileName) =>
        decision switch
        {
            AssetScenePlacementDecision.RejectUnified3DIntoLegacy2D =>
                $"{assetLabel}「{fileName}」は3Dアセットのため、現在のレガシー " +
                ".acscene には配置できません。.acs3d ドキュメントを開くか、現在の" +
                "ワールドを .acs3d へ移行してから再試行してください。" +
                "シーングラフは変更されていません。",
            AssetScenePlacementDecision.RejectLegacy2DIntoUnified3D =>
                $"{assetLabel}「{fileName}」はレガシー2Dアセットのため、現在の " +
                ".acs3d ドキュメントには配置できません。3D版へ変換するか、対応する " +
                ".acscene を開いてください。シーングラフは変更されていません。",
            _ => string.Empty,
        };
}

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
    private const string MaterialGraphSuffix = ".graph.json";
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
                AcsAssetTemplate.Scene,
                "Scene",
                "Scene",
                ".acs3d",
                IsDirectory: false),
            new AcsAssetTemplateDefinition(
                AcsAssetTemplate.Blueprint,
                "Blueprint",
                "Blueprint",
                ".acsbp",
                IsDirectory: false),
            new AcsAssetTemplateDefinition(
                AcsAssetTemplate.Prefab,
                "Prefab",
                "Prefab",
                ".acsprefab",
                IsDirectory: false),
        };

    internal static bool IsTemporaryPath(string path) =>
        !string.IsNullOrWhiteSpace(path) &&
        Path.GetFileName(path).Contains(TemporaryMarker, StringComparison.OrdinalIgnoreCase);

    internal static AcsAssetCreationResult Create(
        string assetsDirectory,
        string currentDirectory,
        AcsAssetTemplate template,
        Func<string, string, bool>? canonicalMaterialWriter = null,
        CancellationToken cancellationToken = default)
    {
        AcsAssetTemplateDefinition definition = FindDefinition(template);
        return CreateCore(
            assetsDirectory,
            currentDirectory,
            definition,
            definition.BaseName,
            canonicalMaterialWriter,
            cancellationToken);
    }

    /// <summary>
    /// Creates an ACS asset from an explicit editor-provided base name. The extension remains
    /// owned by the selected template, and a case-insensitive numeric suffix is added if any
    /// member of the asset family (payload, metadata, or material graph) already occupies it.
    /// </summary>
    internal static AcsAssetCreationResult CreateNamed(
        string assetsDirectory,
        string currentDirectory,
        AcsAssetTemplate template,
        string requestedBaseName,
        Func<string, string, bool>? canonicalMaterialWriter = null,
        CancellationToken cancellationToken = default)
    {
        AcsAssetTemplateDefinition definition = FindDefinition(template);
        string validatedName = AssetCreationNameRules.ValidateBaseName(
            requestedBaseName);
        return CreateCore(
            assetsDirectory,
            currentDirectory,
            definition,
            validatedName,
            canonicalMaterialWriter,
            cancellationToken);
    }

    private static AcsAssetCreationResult CreateCore(
        string assetsDirectory,
        string currentDirectory,
        AcsAssetTemplateDefinition definition,
        string baseName,
        Func<string, string, bool>? canonicalMaterialWriter,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        using AssetMutationLock mutationLock = AssetMutationLock.Acquire(
            assetsDirectory,
            $"Create {definition.DisplayName} asset");
        cancellationToken.ThrowIfCancellationRequested();
        string directory = ValidateTargetDirectory(assetsDirectory, currentDirectory);
        HashSet<string> occupiedNames = SnapshotOccupiedNames(directory);

        for (int suffix = 0; suffix <= MaxGeneratedSuffix; suffix++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string stem = suffix == 0
                ? baseName
                : baseName + suffix;
            string destinationName = stem + definition.Extension;
            string destination = Path.Combine(directory, destinationName);
            if (IsAssetFamilyOccupied(
                    destinationName,
                    definition.Template,
                    occupiedNames))
                continue;

            if (definition.IsDirectory)
            {
                AcsAssetCreationResult? folder = TryCreateDirectory(
                    assetsDirectory,
                    directory,
                    destination,
                    definition,
                    cancellationToken);
                if (folder != null)
                    return folder;
                occupiedNames.Add(destinationName);
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
                    canonicalMaterialWriter,
                    cancellationToken);

                // The directory may have been replaced while the temporary file was written.
                // Revalidate before publishing so a stale browser path cannot escape Assets.
                ValidateTargetDirectory(assetsDirectory, directory);
                cancellationToken.ThrowIfCancellationRequested();
                occupiedNames = SnapshotOccupiedNames(directory);
                if (IsAssetFamilyOccupied(
                        destinationName,
                        definition.Template,
                        occupiedNames))
                {
                    continue;
                }
                try
                {
                    File.Move(temporary, destination, overwrite: false);
                }
                catch (Exception error) when (
                    error is IOException or UnauthorizedAccessException &&
                    (File.Exists(destination) || Directory.Exists(destination)))
                {
                    // Another editor/import won this generated name. Retry the next suffix.
                    occupiedNames.Add(destinationName);
                    continue;
                }

                return new AcsAssetCreationResult(destination, definition);
            }
            finally
            {
                TryDeleteTemporaryFamily(temporary, definition.Template);
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
            AcsAssetTemplate.Scene => "ACS3D v2\n",
            AcsAssetTemplate.Blueprint => "ACSBP 1\n",
            AcsAssetTemplate.Prefab =>
                // New projects use one canonical world graph; the editor's 2D
                // preset is an orthographic view of that graph. Keep loading
                // legacy ACSCENE prefabs, but never create another 2D-only asset.
                "ACS3D v2\n" +
                "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 PrefabRoot\n" +
                "EMPTY3D 1\n",
            _ => throw new ArgumentOutOfRangeException(
                nameof(template), template, "Unknown ACS asset template."),
        };

    private static void WriteCompleteTemporaryAsset(
        string path,
        AcsAssetTemplate template,
        string stem,
        Func<string, string, bool>? canonicalMaterialWriter,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (template == AcsAssetTemplate.Material)
        {
            if (canonicalMaterialWriter == null)
            {
                throw new InvalidOperationException(
                    "Material creation requires the canonical native ACSMAT serializer.");
            }
            bool serialized = canonicalMaterialWriter(path, stem);
            cancellationToken.ThrowIfCancellationRequested();
            if (!serialized ||
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
        cancellationToken.ThrowIfCancellationRequested();
    }

    private static AcsAssetCreationResult? TryCreateDirectory(
        string assetsDirectory,
        string directory,
        string destination,
        AcsAssetTemplateDefinition definition,
        CancellationToken cancellationToken)
    {
        string temporary = Path.Combine(
            directory,
            "." + Path.GetFileName(destination) + TemporaryMarker +
            Guid.NewGuid().ToString("N"));
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            Directory.CreateDirectory(temporary);
            ValidateTargetDirectory(assetsDirectory, directory);
            cancellationToken.ThrowIfCancellationRequested();
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

    private static HashSet<string> SnapshotOccupiedNames(string directory) =>
        Directory.EnumerateFileSystemEntries(
                directory,
                "*",
                SearchOption.TopDirectoryOnly)
            .Select(Path.GetFileName)
            .Where(static name => !string.IsNullOrEmpty(name))
            .Cast<string>()
            .ToHashSet(StringComparer.OrdinalIgnoreCase);

    private static bool IsAssetFamilyOccupied(
        string destinationName,
        AcsAssetTemplate template,
        IReadOnlySet<string> occupiedNames)
    {
        if (occupiedNames.Contains(destinationName) ||
            occupiedNames.Contains(destinationName + AssetDatabase.MetadataSuffix))
        {
            return true;
        }

        if (template != AcsAssetTemplate.Material)
            return false;
        string graphName = destinationName + MaterialGraphSuffix;
        return occupiedNames.Contains(graphName) ||
               occupiedNames.Contains(graphName + AssetDatabase.MetadataSuffix);
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
                if (segment.Equals(
                        AssetDatabase.InternalDirectoryName,
                        StringComparison.OrdinalIgnoreCase) ||
                    segment.EndsWith(
                        AssetDatabase.MetadataSuffix,
                        StringComparison.OrdinalIgnoreCase) ||
                    segment.Contains(TemporaryMarker, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException(
                        $"Asset creation target is reserved for metadata or staging: {target}");
                }
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

    private static void TryDeleteTemporaryFamily(
        string path,
        AcsAssetTemplate template)
    {
        TryDeleteTemporary(path);
        TryDeleteTemporary(path + AssetDatabase.MetadataSuffix);
        if (template != AcsAssetTemplate.Material)
            return;
        string graphPath = path + MaterialGraphSuffix;
        TryDeleteTemporary(graphPath);
        TryDeleteTemporary(graphPath + AssetDatabase.MetadataSuffix);
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
