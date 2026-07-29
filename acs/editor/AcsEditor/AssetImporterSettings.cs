// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AcsEditor;

/// <summary>
/// Project-local defaults used by the built-in importers. The values are
/// normalized before they can participate in metadata or a derived-data key.
/// </summary>
internal sealed record AssetImporterSettings(
    string TextureColorSpace,
    string TextureCompression,
    bool TextureGenerateMipmaps,
    bool TextureDetectNormalMap,
    double MeshUniformScale,
    bool MeshImportTangents,
    bool MeshGenerateCollision,
    bool AudioStreaming,
    bool AudioNormalize,
    int AudioSampleRate)
{
    internal const int CurrentSchemaVersion = 1;

    internal static AssetImporterSettings Default { get; } = new(
        TextureColorSpace: "auto",
        TextureCompression: "default",
        TextureGenerateMipmaps: true,
        TextureDetectNormalMap: true,
        MeshUniformScale: 1.0,
        MeshImportTangents: true,
        MeshGenerateCollision: false,
        AudioStreaming: false,
        AudioNormalize: false,
        AudioSampleRate: 0);

    internal AssetImporterSettings Normalize()
    {
        string colorSpace = NormalizeChoice(
            TextureColorSpace,
            "texture color space",
            "auto",
            "srgb",
            "linear");
        string compression = NormalizeChoice(
            TextureCompression,
            "texture compression",
            "default",
            "normal-map",
            "hdr",
            "ui",
            "mask");
        if (!double.IsFinite(MeshUniformScale) ||
            MeshUniformScale < 0.0001 ||
            MeshUniformScale > 10000.0)
        {
            throw new InvalidDataException(
                "Mesh uniform scale must be finite and between 0.0001 and 10000.");
        }
        if (AudioSampleRate is not (0 or 22050 or 44100 or 48000 or 96000))
        {
            throw new InvalidDataException(
                "Audio sample rate must be Auto, 22050, 44100, 48000, or 96000 Hz.");
        }

        return this with
        {
            TextureColorSpace = colorSpace,
            TextureCompression = compression,
        };
    }

    private static string NormalizeChoice(
        string value,
        string displayName,
        params string[] accepted)
    {
        string normalized = value?.Trim().ToLowerInvariant() ?? "";
        if (!accepted.Contains(normalized, StringComparer.Ordinal))
        {
            throw new InvalidDataException(
                $"Unsupported {displayName} '{value}'.");
        }
        return normalized;
    }
}

/// <summary>
/// Canonical importer identity and settings stored in <c>.acsmeta</c>. The
/// recipe hash is destination-independent, so the same source/options pair has
/// the same derived-data identity after rename or move.
/// </summary>
internal sealed record AssetImporterRecipe(
    string Importer,
    int ImporterVersion,
    IReadOnlyDictionary<string, string> Settings,
    string RecipeHash);

internal static class AssetImporterRecipeContract
{
    internal const int RecipeSchemaVersion = 1;

    internal static AssetImporterRecipe Create(
        string assetKind,
        AssetImporterSettings? settings)
    {
        AssetImporterSettings normalized =
            (settings ?? AssetImporterSettings.Default).Normalize();
        string kind = assetKind?.Trim().ToLowerInvariant() ?? "";

        string importer;
        int version;
        KeyValuePair<string, string>[] values;
        switch (kind)
        {
            case "image":
                importer = "texture";
                version = 2;
                values =
                [
                    Pair("colorSpace", normalized.TextureColorSpace),
                    Pair("compression", normalized.TextureCompression),
                    Pair("generateMipmaps", Bool(normalized.TextureGenerateMipmaps)),
                    Pair("detectNormalMap", Bool(normalized.TextureDetectNormalMap)),
                ];
                break;

            case "mesh":
                importer = "mesh";
                version = 2;
                values =
                [
                    Pair(
                        "uniformScale",
                        normalized.MeshUniformScale.ToString(
                            "R",
                            CultureInfo.InvariantCulture)),
                    Pair("importTangents", Bool(normalized.MeshImportTangents)),
                    Pair("generateCollision", Bool(normalized.MeshGenerateCollision)),
                ];
                break;

            case "audio":
                importer = "audio";
                version = 2;
                values =
                [
                    Pair("streaming", Bool(normalized.AudioStreaming)),
                    Pair("normalize", Bool(normalized.AudioNormalize)),
                    Pair(
                        "sampleRate",
                        normalized.AudioSampleRate.ToString(
                            CultureInfo.InvariantCulture)),
                ];
                break;

            case "scene":
            case "material":
            case "blueprint":
            case "prefab":
                importer = kind;
                version = 1;
                values = [];
                break;

            default:
                importer = "passthrough";
                version = 1;
                values = [];
                break;
        }

        var canonicalSettings = new SortedDictionary<string, string>(
            StringComparer.Ordinal)
        {
            ["recipeSchema"] =
                RecipeSchemaVersion.ToString(CultureInfo.InvariantCulture),
        };
        foreach (KeyValuePair<string, string> pair in values)
            canonicalSettings.Add(pair.Key, pair.Value);

        string recipeHash = ComputeRecipeHash(
            importer,
            version,
            canonicalSettings);
        canonicalSettings.Add("recipeHash", recipeHash);
        return new(
            importer,
            version,
            new ReadOnlyDictionary<string, string>(canonicalSettings),
            recipeHash);
    }

    internal static string ComputeRecipeHash(
        string importer,
        int importerVersion,
        IEnumerable<KeyValuePair<string, string>> settings)
    {
        if (string.IsNullOrWhiteSpace(importer))
            throw new InvalidDataException("Importer identity is required.");
        if (importerVersion <= 0)
            throw new InvalidDataException("Importer version must be positive.");

        var canonical = new StringBuilder();
        Append("importer", importer.Trim().ToLowerInvariant());
        Append(
            "version",
            importerVersion.ToString(CultureInfo.InvariantCulture));
        foreach (KeyValuePair<string, string> pair in settings
                     .Where(static pair =>
                         !string.Equals(
                             pair.Key,
                             "recipeHash",
                             StringComparison.Ordinal))
                     .OrderBy(static pair => pair.Key, StringComparer.Ordinal))
        {
            if (string.IsNullOrWhiteSpace(pair.Key) ||
                pair.Key.Length > 64 ||
                pair.Value == null ||
                pair.Value.Length > 1024)
            {
                throw new InvalidDataException(
                    "Importer recipe contains an invalid setting.");
            }
            Append(pair.Key, pair.Value);
        }
        return Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(canonical.ToString())))
            .ToLowerInvariant();

        void Append(string name, string value)
        {
            canonical.Append(name.Length)
                .Append(':')
                .Append(name)
                .Append('=')
                .Append(value.Length)
                .Append(':')
                .Append(value)
                .Append('\n');
        }
    }

    private static KeyValuePair<string, string> Pair(
        string key,
        string value) =>
        KeyValuePair.Create(key, value);

    private static string Bool(bool value) => value ? "true" : "false";
}

/// <summary>
/// Strict, bounded, project-local persistence for the last accepted importer
/// defaults. The profile is editor state, not a source asset, and is therefore
/// kept below <c>Saved/Editor</c>.
/// </summary>
internal static class AssetImporterSettingsStore
{
    private const int MaximumProfileBytes = 64 * 1024;
    private const string RelativeDirectory = "Saved/Editor";
    private const string FileName = "AssetImporterSettings.v1.json";
    private static readonly UTF8Encoding Utf8NoBom = new(false, true);
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        MaxDepth = 8,
    };

    internal static string GetProfilePath(string projectRoot)
    {
        string root = ValidateProjectRoot(projectRoot);
        return Path.Combine(root, RelativeDirectory, FileName);
    }

    internal static AssetImporterSettings Load(
        string projectRoot,
        out string? warning)
    {
        warning = null;
        string path = GetProfilePath(projectRoot);
        if (!File.Exists(path) && !Directory.Exists(path))
            return AssetImporterSettings.Default;

        try
        {
            EnsureOrdinaryFile(path);
            byte[] bytes = File.ReadAllBytes(path);
            if (bytes.Length == 0 || bytes.Length > MaximumProfileBytes)
            {
                throw new InvalidDataException(
                    "Importer settings profile size is invalid.");
            }
            ValidateProfileJson(bytes);
            StoredProfile stored =
                JsonSerializer.Deserialize<StoredProfile>(bytes, JsonOptions)
                ?? throw new InvalidDataException(
                    "Importer settings profile is empty.");
            if (stored.SchemaVersion != AssetImporterSettings.CurrentSchemaVersion)
            {
                throw new InvalidDataException(
                    $"Unsupported importer settings schema {stored.SchemaVersion}.");
            }
            return stored.ToSettings().Normalize();
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                JsonException or InvalidDataException or ArgumentException)
        {
            warning =
                "Saved importer settings were rejected; safe defaults are in use. " +
                error.Message;
            return AssetImporterSettings.Default;
        }
    }

    internal static void Save(
        string projectRoot,
        AssetImporterSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        AssetImporterSettings normalized = settings.Normalize();
        string root = ValidateProjectRoot(projectRoot);
        string directory = EnsureProfileDirectory(root);
        string path = Path.Combine(directory, FileName);
        if (Directory.Exists(path))
            throw new InvalidDataException(
                "Importer settings profile path is a directory.");
        if (File.Exists(path))
            EnsureOrdinaryFile(path);

        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(
            StoredProfile.FromSettings(normalized),
            JsonOptions);
        if (bytes.Length == 0 || bytes.Length > MaximumProfileBytes)
            throw new InvalidDataException(
                "Importer settings profile exceeds its bounded size.");

        string temporary =
            path + ".tmp-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
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
            EnsureOrdinaryDirectory(directory);
            if (File.Exists(path))
                EnsureOrdinaryFile(path);
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            TryDeleteTemporary(temporary);
        }
    }

    private static void ValidateProfileJson(ReadOnlySpan<byte> utf8)
    {
        using JsonDocument document = JsonDocument.Parse(
            utf8.ToArray(),
            new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 8,
            });
        if (document.RootElement.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException(
                "Importer settings profile root must be a JSON object.");
        }

        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (JsonProperty property in document.RootElement.EnumerateObject())
        {
            if (!names.Add(property.Name))
            {
                throw new InvalidDataException(
                    "Importer settings profile contains a duplicate JSON property.");
            }
        }
    }

    private static string EnsureProfileDirectory(string projectRoot)
    {
        string current = projectRoot;
        foreach (string segment in new[] { "Saved", "Editor" })
        {
            current = Path.Combine(current, segment);
            if (!Directory.Exists(current))
            {
                if (File.Exists(current))
                    throw new InvalidDataException(
                        $"Importer settings directory segment is a file: {current}");
                Directory.CreateDirectory(current);
            }
            EnsureOrdinaryDirectory(current);
        }
        return current;
    }

    private static string ValidateProjectRoot(string projectRoot)
    {
        if (string.IsNullOrWhiteSpace(projectRoot))
            throw new ArgumentException(
                "Project root is required.",
                nameof(projectRoot));
        string root =
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        EnsureOrdinaryDirectory(root);
        return root;
    }

    private static void EnsureOrdinaryDirectory(string path)
    {
        if (!Directory.Exists(path))
            throw new DirectoryNotFoundException(path);
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                $"Importer settings path is not an ordinary directory: {path}");
        }
    }

    private static void EnsureOrdinaryFile(string path)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) != 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                $"Importer settings path is not an ordinary file: {path}");
        }
    }

    private static void TryDeleteTemporary(string path)
    {
        try
        {
            if (File.Exists(path) &&
                (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0)
            {
                File.Delete(path);
            }
        }
        catch
        {
            // A failed cleanup remains a non-authoritative, uniquely named
            // sibling and can never be loaded as the committed profile.
        }
    }

    private sealed class StoredProfile
    {
        [JsonRequired]
        public int SchemaVersion { get; init; }

        [JsonRequired]
        public required string TextureColorSpace { get; init; }

        [JsonRequired]
        public required string TextureCompression { get; init; }

        [JsonRequired]
        public bool TextureGenerateMipmaps { get; init; }

        [JsonRequired]
        public bool TextureDetectNormalMap { get; init; }

        [JsonRequired]
        public double MeshUniformScale { get; init; }

        [JsonRequired]
        public bool MeshImportTangents { get; init; }

        [JsonRequired]
        public bool MeshGenerateCollision { get; init; }

        [JsonRequired]
        public bool AudioStreaming { get; init; }

        [JsonRequired]
        public bool AudioNormalize { get; init; }

        [JsonRequired]
        public int AudioSampleRate { get; init; }

        internal AssetImporterSettings ToSettings() => new(
            TextureColorSpace,
            TextureCompression,
            TextureGenerateMipmaps,
            TextureDetectNormalMap,
            MeshUniformScale,
            MeshImportTangents,
            MeshGenerateCollision,
            AudioStreaming,
            AudioNormalize,
            AudioSampleRate);

        internal static StoredProfile FromSettings(
            AssetImporterSettings settings) => new()
        {
            SchemaVersion = AssetImporterSettings.CurrentSchemaVersion,
            TextureColorSpace = settings.TextureColorSpace,
            TextureCompression = settings.TextureCompression,
            TextureGenerateMipmaps = settings.TextureGenerateMipmaps,
            TextureDetectNormalMap = settings.TextureDetectNormalMap,
            MeshUniformScale = settings.MeshUniformScale,
            MeshImportTangents = settings.MeshImportTangents,
            MeshGenerateCollision = settings.MeshGenerateCollision,
            AudioStreaming = settings.AudioStreaming,
            AudioNormalize = settings.AudioNormalize,
            AudioSampleRate = settings.AudioSampleRate,
        };
    }
}
