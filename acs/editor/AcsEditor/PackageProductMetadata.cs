// SPDX-License-Identifier: Apache-2.0
// Strict, deterministic product metadata shared by Editor and CLI packaging.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AcsEditor.Packaging;

public sealed record PackageProductMetadata(
    [property: JsonPropertyName("schemaVersion")]
    int SchemaVersion,
    [property: JsonPropertyName("publisher")]
    string Publisher,
    [property: JsonPropertyName("description")]
    string Description,
    [property: JsonPropertyName("copyright")]
    string Copyright,
    [property: JsonPropertyName("supportUrl")]
    string SupportUrl)
{
    public static PackageProductMetadata Empty { get; } =
        new(1, "", "", "", "");

    [JsonIgnore]
    public bool IsEmpty =>
        Publisher.Length == 0 &&
        Description.Length == 0 &&
        Copyright.Length == 0 &&
        SupportUrl.Length == 0;
}

public sealed record PackageProductMetadataValidationIssue(
    string Field,
    string Message);

internal readonly record struct PackageProductMetadataSourceFingerprint(
    bool Exists,
    string RawSha256)
{
    internal static PackageProductMetadataSourceFingerprint Missing { get; } =
        new(false, "");
}

internal sealed record PackageProductMetadataSnapshot(
    PackageProductMetadata Metadata,
    PackageProductMetadataSourceFingerprint Fingerprint);

internal sealed record PackageProductMetadataPublicationHooks(
    Action<string>? BeforeDeleteMove = null,
    Action<string, string>? AfterDeleteMove = null,
    Action<string>? BeforePublish = null,
    Action<string, string>? AfterReplace = null);

/// <summary>
/// Loads optional distribution metadata from
/// <c>Config/PackageMetadata.json</c>. The file is part of the same immutable
/// Config snapshot as Project Settings, so the manifest cannot mix metadata
/// from a later project state into an earlier package.
/// </summary>
public static class PackageProductMetadataContract
{
    public const string FileName = "PackageMetadata.json";
    public const int MaximumFileBytes = 64 * 1024;
    public const int MaximumPublisherLength = 160;
    public const int MaximumDescriptionLength = 2048;
    public const int MaximumCopyrightLength = 512;
    public const int MaximumSupportUrlLength = 2048;
    private static readonly UTF8Encoding Utf8Strict = new(false, true);

    public static PackageProductMetadata LoadOptional(string configDirectory) =>
        LoadOptionalSnapshot(configDirectory).Metadata;

    internal static PackageProductMetadataSnapshot LoadOptionalSnapshot(
        string configDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(configDirectory);
        EnsureSafeConfigDirectory(configDirectory);
        string path = Path.Combine(configDirectory, FileName);
        if (Directory.Exists(path))
            throw new InvalidDataException($"{FileName} must be an ordinary file.");
        if (!File.Exists(path))
        {
            return new(
                PackageProductMetadata.Empty,
                PackageProductMetadataSourceFingerprint.Missing);
        }
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"{FileName} must not be a reparse point.");

        using FileStream stream = new(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read);
        if (stream.Length is < 2 or > MaximumFileBytes)
        {
            throw new InvalidDataException(
                $"{FileName} must be between 2 and {MaximumFileBytes} bytes.");
        }
        byte[] bytes = new byte[stream.Length];
        stream.ReadExactly(bytes);
        if (stream.ReadByte() != -1 || stream.Length != bytes.LongLength)
            throw new IOException($"{FileName} changed while it was being read.");
        _ = Utf8Strict.GetString(bytes);
        PackageProductMetadata metadata = Parse(bytes);
        return new(
            metadata,
            new(
                true,
                Convert.ToHexString(SHA256.HashData(bytes))));
    }

    private static PackageProductMetadata Parse(byte[] bytes)
    {
        using JsonDocument document = JsonDocument.Parse(
            bytes,
            new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 8,
            });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException($"{FileName} root must be an object.");

        var names = new HashSet<string>(StringComparer.Ordinal);
        int schemaVersion = 0;
        string publisher = "";
        string description = "";
        string copyright = "";
        string supportUrl = "";
        foreach (JsonProperty property in root.EnumerateObject())
        {
            if (!names.Add(property.Name))
            {
                throw new InvalidDataException(
                    $"{FileName} contains duplicate property '{property.Name}'.");
            }
            switch (property.Name)
            {
                case "schemaVersion":
                    if (property.Value.ValueKind != JsonValueKind.Number ||
                        !property.Value.TryGetInt32(out schemaVersion))
                    {
                        throw new InvalidDataException(
                            $"{FileName}.schemaVersion must be an integer.");
                    }
                    break;
                case "publisher":
                    publisher = ReadString(property, MaximumPublisherLength);
                    break;
                case "description":
                    description = ReadString(property, MaximumDescriptionLength);
                    break;
                case "copyright":
                    copyright = ReadString(property, MaximumCopyrightLength);
                    break;
                case "supportUrl":
                    supportUrl = ReadString(property, MaximumSupportUrlLength);
                    break;
                default:
                    throw new InvalidDataException(
                        $"{FileName} contains unknown property '{property.Name}'.");
            }
        }

        var metadata = new PackageProductMetadata(
            schemaVersion,
            publisher,
            description,
            copyright,
            supportUrl);
        Validate(metadata);
        return metadata;
    }

    public static void Validate(PackageProductMetadata metadata)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        PackageProductMetadataValidationIssue? issue =
            GetValidationIssues(metadata).FirstOrDefault();
        if (issue is not null)
            throw new InvalidDataException(issue.Message);
    }

    /// <summary>
    /// Returns every validation issue using the exact same rules as package
    /// preflight. Editor UI consumes these issues directly instead of keeping
    /// a second, potentially divergent validator.
    /// </summary>
    public static IReadOnlyList<PackageProductMetadataValidationIssue>
        GetValidationIssues(PackageProductMetadata metadata)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        var issues = new List<PackageProductMetadataValidationIssue>();
        if (metadata.SchemaVersion != 1)
        {
            issues.Add(new(
                "schemaVersion",
                $"{FileName}.schemaVersion must be 1."));
        }
        AddTextIssues(
            issues,
            "publisher",
            metadata.Publisher,
            MaximumPublisherLength);
        AddTextIssues(
            issues,
            "description",
            metadata.Description,
            MaximumDescriptionLength);
        AddTextIssues(
            issues,
            "copyright",
            metadata.Copyright,
            MaximumCopyrightLength);
        AddTextIssues(
            issues,
            "supportUrl",
            metadata.SupportUrl,
            MaximumSupportUrlLength);

        if (metadata.SupportUrl is { Length: > 0 } &&
            !issues.Any(issue => issue.Field == "supportUrl") &&
            (!Uri.TryCreate(metadata.SupportUrl, UriKind.Absolute, out Uri? uri) ||
             !string.Equals(uri.Scheme, Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase) ||
             string.IsNullOrWhiteSpace(uri.Host) ||
             uri.UserInfo.Length != 0 ||
             !string.Equals(uri.AbsoluteUri, metadata.SupportUrl, StringComparison.Ordinal)))
        {
            issues.Add(new(
                "supportUrl",
                $"{FileName}.supportUrl must be a canonical absolute HTTPS URL."));
        }
        return issues;
    }

    /// <summary>
    /// Writes a canonical UTF-8 document through a same-directory temporary
    /// file. Empty metadata means "not configured" and removes the optional
    /// document; a failed write or replace leaves the previous file in place.
    /// </summary>
    public static void SaveOptionalAtomic(
        string configDirectory,
        PackageProductMetadata metadata)
    {
        _ = SaveOptionalAtomicCore(
            configDirectory,
            metadata,
            expectedFingerprint: null,
            hooks: null);
    }

    internal static PackageProductMetadataSourceFingerprint
        SaveOptionalAtomicExpected(
            string configDirectory,
            PackageProductMetadata metadata,
            PackageProductMetadataSourceFingerprint expectedFingerprint) =>
        SaveOptionalAtomicCore(
            configDirectory,
            metadata,
            expectedFingerprint,
            hooks: null);

    internal static PackageProductMetadataSourceFingerprint
        SaveOptionalAtomicExpectedForTest(
            string configDirectory,
            PackageProductMetadata metadata,
            PackageProductMetadataSourceFingerprint expectedFingerprint,
            PackageProductMetadataPublicationHooks hooks) =>
        SaveOptionalAtomicCore(
            configDirectory,
            metadata,
            expectedFingerprint,
            hooks);

    private static PackageProductMetadataSourceFingerprint
        SaveOptionalAtomicCore(
            string configDirectory,
            PackageProductMetadata metadata,
            PackageProductMetadataSourceFingerprint? expectedFingerprint,
            PackageProductMetadataPublicationHooks? hooks)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(configDirectory);
        ArgumentNullException.ThrowIfNull(metadata);
        Validate(metadata);
        EnsureSafeConfigDirectory(configDirectory);

        string path = Path.Combine(configDirectory, FileName);
        EnsureSafeMetadataFileOrMissing(path);
        PackageProductMetadataSnapshot original =
            LoadOptionalSnapshot(configDirectory);
        if (expectedFingerprint is { } expected)
            EnsureFingerprintMatches(expected, original.Fingerprint);

        if (metadata.IsEmpty)
        {
            return DeleteOptionalWithQuarantine(
                configDirectory,
                path,
                original.Fingerprint,
                hooks);
        }

        byte[] payload = SerializeCanonical(metadata);
        var publishedFingerprint =
            new PackageProductMetadataSourceFingerprint(
                true,
                Convert.ToHexString(SHA256.HashData(payload)));
        string temporaryPath =
            path + ".tmp-" + Guid.NewGuid().ToString("N");
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
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }

            EnsureSafeConfigDirectory(configDirectory);
            EnsureSafeMetadataFileOrMissing(path);
            PackageProductMetadataSnapshot beforePublish =
                LoadOptionalSnapshot(configDirectory);
            EnsureFingerprintMatches(
                original.Fingerprint,
                beforePublish.Fingerprint);
            hooks?.BeforePublish?.Invoke(path);
            if (!beforePublish.Fingerprint.Exists)
            {
                File.Move(temporaryPath, path);
                return VerifyPublishedMetadata(
                    configDirectory,
                    metadata,
                    publishedFingerprint);
            }

            return ReplaceWithVerifiedBackup(
                configDirectory,
                temporaryPath,
                path,
                metadata,
                original.Fingerprint,
                publishedFingerprint,
                hooks);
        }
        finally
        {
            TryDeletePrivateFile(temporaryPath);
        }
    }

    private static PackageProductMetadataSourceFingerprint
        DeleteOptionalWithQuarantine(
            string configDirectory,
            string destinationPath,
            PackageProductMetadataSourceFingerprint expectedFingerprint,
            PackageProductMetadataPublicationHooks? hooks)
    {
        if (!expectedFingerprint.Exists)
        {
            PackageProductMetadataSnapshot stillMissing =
                LoadOptionalSnapshot(configDirectory);
            EnsureFingerprintMatches(
                PackageProductMetadataSourceFingerprint.Missing,
                stillMissing.Fingerprint);
            return stillMissing.Fingerprint;
        }

        hooks?.BeforeDeleteMove?.Invoke(destinationPath);
        string quarantinePath =
            CreateRecoveryPath(destinationPath, "delete");
        File.Move(destinationPath, quarantinePath);
        hooks?.AfterDeleteMove?.Invoke(
            destinationPath,
            quarantinePath);

        PackageProductMetadataSourceFingerprint movedFingerprint =
            CaptureRawFingerprint(quarantinePath);
        if (movedFingerprint != expectedFingerprint)
        {
            string recoveryLocation =
                RestoreMovedRecoveryIfDestinationMissing(
                quarantinePath,
                destinationPath);
            throw PublicationConflict(
                "Delete admission moved bytes that no longer match the loaded source.",
                recoveryLocation);
        }
        if (File.Exists(destinationPath) ||
            Directory.Exists(destinationPath))
        {
            throw PublicationConflict(
                "Package metadata changed while the loaded source was quarantined.",
                quarantinePath);
        }

        try
        {
            File.Delete(quarantinePath);
        }
        catch
        {
            _ = RestoreMovedRecoveryIfDestinationMissing(
                quarantinePath,
                destinationPath);
            throw;
        }

        PackageProductMetadataSnapshot afterDelete =
            LoadOptionalSnapshot(configDirectory);
        EnsureFingerprintMatches(
            PackageProductMetadataSourceFingerprint.Missing,
            afterDelete.Fingerprint);
        return afterDelete.Fingerprint;
    }

    private static PackageProductMetadataSourceFingerprint
        ReplaceWithVerifiedBackup(
            string configDirectory,
            string temporaryPath,
            string destinationPath,
            PackageProductMetadata metadata,
            PackageProductMetadataSourceFingerprint expectedFingerprint,
            PackageProductMetadataSourceFingerprint publishedFingerprint,
            PackageProductMetadataPublicationHooks? hooks)
    {
        string backupPath =
            CreateRecoveryPath(destinationPath, "replace");
        File.Replace(
            temporaryPath,
            destinationPath,
            backupPath,
            ignoreMetadataErrors: true);
        hooks?.AfterReplace?.Invoke(
            destinationPath,
            backupPath);

        PackageProductMetadataSourceFingerprint backupFingerprint =
            CaptureRawFingerprint(backupPath);
        if (backupFingerprint != expectedFingerprint)
        {
            string recoveryLocation =
                RestoreReplacementIfStillOwned(
                destinationPath,
                backupPath,
                publishedFingerprint);
            throw PublicationConflict(
                "Replace admission captured bytes that no longer match the loaded source.",
                recoveryLocation);
        }

        PackageProductMetadataSourceFingerprint currentFingerprint =
            CaptureRawFingerprint(destinationPath);
        if (currentFingerprint != publishedFingerprint)
        {
            throw PublicationConflict(
                "Package metadata changed before replacement verification completed.",
                backupPath);
        }

        // The backup is deleted only after proving it is exactly the source
        // version that the editor was authorized to replace.
        File.Delete(backupPath);
        return VerifyPublishedMetadata(
            configDirectory,
            metadata,
            publishedFingerprint);
    }

    private static string RestoreReplacementIfStillOwned(
        string destinationPath,
        string backupPath,
        PackageProductMetadataSourceFingerprint publishedFingerprint)
    {
        PackageProductMetadataSourceFingerprint currentFingerprint =
            CaptureRawFingerprint(destinationPath);
        if (currentFingerprint != publishedFingerprint)
            return backupPath;

        string displacedPath =
            CreateRecoveryPath(destinationPath, "displaced");
        File.Replace(
            backupPath,
            destinationPath,
            displacedPath,
            ignoreMetadataErrors: true);
        PackageProductMetadataSourceFingerprint displacedFingerprint =
            CaptureRawFingerprint(displacedPath);
        if (displacedFingerprint == publishedFingerprint)
        {
            File.Delete(displacedPath);
            return destinationPath;
        }
        // A different displaced fingerprint belongs to an overlapping writer.
        // Keep it as recovery rather than deleting another process's bytes.
        return displacedPath;
    }

    private static string RestoreMovedRecoveryIfDestinationMissing(
        string recoveryPath,
        string destinationPath)
    {
        if (!File.Exists(recoveryPath) ||
            File.Exists(destinationPath) ||
            Directory.Exists(destinationPath))
        {
            return File.Exists(recoveryPath)
                ? recoveryPath
                : destinationPath;
        }
        try
        {
            File.Move(recoveryPath, destinationPath);
            return destinationPath;
        }
        catch (IOException)
        {
            // Another writer won the destination race. Keep recovery intact.
            return recoveryPath;
        }
        catch (UnauthorizedAccessException)
        {
            // Keep recovery intact when restoration cannot be proven safe.
            return recoveryPath;
        }
    }

    private static PackageProductMetadataSourceFingerprint
        VerifyPublishedMetadata(
            string configDirectory,
            PackageProductMetadata metadata,
            PackageProductMetadataSourceFingerprint publishedFingerprint)
    {
        PackageProductMetadataSnapshot afterPublish =
            LoadOptionalSnapshot(configDirectory);
        EnsureFingerprintMatches(
            publishedFingerprint,
            afterPublish.Fingerprint);
        if (afterPublish.Metadata != metadata)
        {
            throw new IOException(
                $"{FileName} changed immediately after publication.");
        }
        return afterPublish.Fingerprint;
    }

    private static PackageProductMetadataSourceFingerprint
        CaptureRawFingerprint(string path)
    {
        EnsureSafeMetadataFileOrMissing(path);
        if (!File.Exists(path))
            return PackageProductMetadataSourceFingerprint.Missing;

        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read);
        if (stream.Length is < 0 or > MaximumFileBytes)
        {
            return new(
                true,
                "INVALID-LENGTH-" +
                stream.Length.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
        byte[] bytes = new byte[stream.Length];
        stream.ReadExactly(bytes);
        if (stream.ReadByte() != -1 || stream.Length != bytes.LongLength)
            throw new IOException($"{FileName} changed while it was being fingerprinted.");
        return new(
            true,
            Convert.ToHexString(SHA256.HashData(bytes)));
    }

    private static string CreateRecoveryPath(
        string destinationPath,
        string operation) =>
        destinationPath +
        ".tmp-recovery-" +
        operation +
        "-" +
        Guid.NewGuid().ToString("N");

    private static IOException PublicationConflict(
        string message,
        string recoveryPath) =>
        new(
            message +
            " Recovery was preserved at: " +
            recoveryPath);

    public static byte[] SerializeCanonical(PackageProductMetadata metadata)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        Validate(metadata);
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(
                   stream,
                   new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", metadata.SchemaVersion);
            writer.WriteString("publisher", metadata.Publisher);
            writer.WriteString("description", metadata.Description);
            writer.WriteString("copyright", metadata.Copyright);
            writer.WriteString("supportUrl", metadata.SupportUrl);
            writer.WriteEndObject();
        }
        string canonicalText = Utf8Strict
            .GetString(stream.ToArray())
            .Replace("\r\n", "\n", StringComparison.Ordinal) + "\n";
        byte[] payload = Utf8Strict.GetBytes(canonicalText);
        if (payload.Length > MaximumFileBytes)
        {
            throw new InvalidDataException(
                $"{FileName} exceeds {MaximumFileBytes} bytes.");
        }
        return payload;
    }

    private static string ReadString(JsonProperty property, int maximumLength)
    {
        if (property.Value.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException(
                $"{FileName}.{property.Name} must be a string.");
        }
        string value = property.Value.GetString() ?? "";
        ValidateText(property.Name, value, maximumLength);
        return value;
    }

    private static void ValidateText(string field, string? value, int maximumLength)
    {
        var issues = new List<PackageProductMetadataValidationIssue>();
        AddTextIssues(issues, field, value, maximumLength);
        if (issues.Count > 0)
            throw new InvalidDataException(issues[0].Message);
    }

    private static void AddTextIssues(
        ICollection<PackageProductMetadataValidationIssue> issues,
        string field,
        string? value,
        int maximumLength)
    {
        if (value is null)
        {
            issues.Add(new(
                field,
                $"{FileName}.{field} must not be null."));
            return;
        }
        if (value.Length > maximumLength)
        {
            issues.Add(new(
                field,
                $"{FileName}.{field} exceeds {maximumLength} characters."));
        }
        if (!string.Equals(value, value.Trim(), StringComparison.Ordinal))
        {
            issues.Add(new(
                field,
                $"{FileName}.{field} must not have leading or trailing whitespace."));
        }
        if (value.Any(character =>
                char.IsControl(character) &&
                character is not ('\r' or '\n' or '\t')))
        {
            issues.Add(new(
                field,
                $"{FileName}.{field} contains a control character."));
        }
    }

    private static void EnsureSafeConfigDirectory(string configDirectory)
    {
        if (!Directory.Exists(configDirectory))
        {
            throw new DirectoryNotFoundException(
                $"Config directory does not exist: {configDirectory}");
        }
        if ((File.GetAttributes(configDirectory) & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                "Config directory must not be a reparse point.");
        }
    }

    private static void EnsureSafeMetadataFileOrMissing(string path)
    {
        if (Directory.Exists(path))
            throw new InvalidDataException($"{FileName} must be an ordinary file.");
        if (File.Exists(path) &&
            (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException($"{FileName} must not be a reparse point.");
        }
    }

    private static void EnsureFingerprintMatches(
        PackageProductMetadataSourceFingerprint expected,
        PackageProductMetadataSourceFingerprint actual)
    {
        if (expected != actual)
        {
            throw new IOException(
                $"{FileName} changed outside this Editor. Reload Project Settings before applying.");
        }
    }

    private static void TryDeletePrivateFile(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
