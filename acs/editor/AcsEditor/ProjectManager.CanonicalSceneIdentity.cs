// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AcsEditor;

internal enum CanonicalSceneIdentityMigrationPoint
{
    AfterAuthoritativeRefresh,
    BeforeManifestPublish,
    AfterManifestPublish,
}

public static partial class ProjectManager
{
    private const int MaxCanonicalSceneMetadataBytes = 1024 * 1024;

    /// <summary>
    /// Gives a legacy path-only startup scene its persistent Asset Database identity.
    ///
    /// The caller must hold the project recovery lease. Asset metadata is made authoritative
    /// before the manifest is published, so interruption is retry-safe: a later open either
    /// observes the already-published ID or reuses the durable sidecar created by this attempt.
    /// </summary>
    internal static bool BackfillCanonicalSceneAssetId(
        Project project,
        AssetDatabase database) =>
        BackfillCanonicalSceneAssetId(
            project,
            database,
            faultInjector: null);

    internal static bool BackfillCanonicalSceneAssetId(
        Project project,
        AssetDatabase database,
        Action<CanonicalSceneIdentityMigrationPoint>? faultInjector)
    {
        ArgumentNullException.ThrowIfNull(project);
        ArgumentNullException.ThrowIfNull(database);

        if (!AssetMutationLock.IsRecoveryLeaseHeldByCurrentThread(project.AssetsDir))
        {
            throw new InvalidOperationException(
                "Canonical scene identity migration requires the current thread to own the " +
                "project recovery lease.");
        }
        if (!PathsEqual(database.ProjectRoot, project.RootDir) ||
            !PathsEqual(database.AssetsRoot, project.AssetsDir))
        {
            throw new InvalidOperationException(
                "The Asset Database does not belong to the project being migrated.");
        }

        // Current manifests already carry the durable identity. Their full graph validation is a
        // Cook concern and must not turn every editor open into a content-hashing scan.
        if (!string.IsNullOrEmpty(project.CanonicalSceneAssetId))
            return false;

        ReferenceFileSnapshot manifest = CaptureRequiredOrdinaryFile(
            project.ProjectFilePath,
            MaxProjectManifestBytes,
            ".acsproject manifest");
        Project persisted = ParseManifestSnapshot(manifest.Path, manifest.Bytes);
        EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
        if (!string.Equals(
                persisted.InitialScene,
                project.InitialScene,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The startup scene changed after the project was loaded; canonical identity " +
                "migration remains fail-closed.");
        }

        AssetDatabaseRefreshResult refresh =
            database.RefreshWithinAssetTransaction(verifyContent: true);
        faultInjector?.Invoke(
            CanonicalSceneIdentityMigrationPoint.AfterAuthoritativeRefresh);
        string scenePath = SceneSourceFile.ResolveProjectSceneReference(
            project.RootDir,
            project.AssetsDir,
            project.InitialScene);
        if (!database.TryGetByPath(scenePath, out AssetRecord? scene) ||
            scene == null ||
            !PathsEqual(scene.FullPath, scenePath) ||
            !string.Equals(scene.Kind, "scene", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The initial scene has no unique authoritative Asset Database record; " +
                "canonical identity migration remains fail-closed.");
        }

        if (scene.SizeBytes < 0 || scene.SizeBytes > int.MaxValue)
        {
            throw new InvalidDataException(
                "The initial scene is too large to capture for canonical identity migration.");
        }
        ReferenceFileSnapshot sceneSnapshot = CaptureRequiredOrdinaryFile(
            scenePath,
            checked((int)scene.SizeBytes),
            "Initial scene",
            requireWritable: false);
        string sceneHash = Convert.ToHexString(
                SHA256.HashData(sceneSnapshot.Bytes))
            .ToLowerInvariant();
        if (sceneSnapshot.Bytes.LongLength != scene.SizeBytes ||
            !string.Equals(
                sceneHash,
                scene.ContentHash,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The initial scene changed after the authoritative Asset Database refresh.");
        }

        string metadataPath = scenePath + AssetDatabase.MetadataSuffix;
        ReferenceFileSnapshot metadataSnapshot = CaptureRequiredOrdinaryFile(
            metadataPath,
            MaxCanonicalSceneMetadataBytes,
            "Initial scene metadata",
            requireWritable: false);
        string canonicalAssetId =
            ReadCanonicalSceneAssetId(metadataSnapshot.Bytes);
        string databaseAssetId = NormalizeCanonicalAssetId(scene.AssetId);
        if (!string.Equals(
                canonicalAssetId,
                databaseAssetId,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The captured initial-scene metadata Asset ID does not match the " +
                "authoritative Asset Database record.");
        }
        if (refresh.Warnings.Any(warning =>
                warning.Contains(canonicalAssetId, StringComparison.OrdinalIgnoreCase) ||
                warning.Contains(scene.RelativePath, StringComparison.OrdinalIgnoreCase)))
        {
            throw new InvalidDataException(
                "Asset Database diagnostics make the initial scene identity ambiguous; " +
                "canonical identity migration remains fail-closed.");
        }
        if (!database.TryGetByAssetId(canonicalAssetId, out AssetRecord? byId) ||
            byId == null ||
            !PathsEqual(byId.FullPath, scenePath))
        {
            throw new InvalidDataException(
                "The initial scene path and Asset ID do not resolve to the same record.");
        }

        string persistedAssetId = persisted.CanonicalSceneAssetId;
        if (!string.IsNullOrEmpty(persistedAssetId))
        {
            persistedAssetId = NormalizeCanonicalAssetId(persistedAssetId);
            if (!string.Equals(
                    persistedAssetId,
                    canonicalAssetId,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "The manifest already names a different canonical startup-scene Asset ID.");
            }

            // Covers a retry after manifest publication but before the in-memory Project object
            // was updated.
            project.CanonicalSceneAssetId = canonicalAssetId;
            return false;
        }

        byte[] updatedManifest = RewriteManifestCanonicalSceneAssetId(
            manifest.Bytes,
            canonicalAssetId);
        if (updatedManifest.LongLength > MaxProjectManifestBytes)
        {
            throw new InvalidDataException(
                "The migrated .acsproject manifest exceeds its persistence limit.");
        }

        string temporary = CreateSiblingTemporaryPath(manifest.Path);
        try
        {
            WriteTemporaryBytes(temporary, updatedManifest);
            faultInjector?.Invoke(
                CanonicalSceneIdentityMigrationPoint.BeforeManifestPublish);
            EnsureSnapshotUnchanged(manifest, ".acsproject manifest");
            EnsureSnapshotUnchanged(sceneSnapshot, "Initial scene");
            EnsureSnapshotUnchanged(metadataSnapshot, "Initial scene metadata");
            PublishTemporary(temporary, manifest.Path, manifest);
            faultInjector?.Invoke(
                CanonicalSceneIdentityMigrationPoint.AfterManifestPublish);
            project.CanonicalSceneAssetId = canonicalAssetId;
            return true;
        }
        finally
        {
            TryDeleteOrdinaryFile(temporary);
        }
    }

    private static string ReadCanonicalSceneAssetId(byte[] metadataBytes)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(
                metadataBytes,
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                    MaxDepth = 32,
                });
        }
        catch (JsonException error)
        {
            throw new InvalidDataException(
                "The captured initial-scene metadata is not valid JSON.",
                error);
        }

        using (document)
        {
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                throw new InvalidDataException(
                    "The captured initial-scene metadata root must be an object.");
            }

            var propertyNames = new HashSet<string>(
                StringComparer.OrdinalIgnoreCase);
            foreach (JsonProperty property in root.EnumerateObject())
            {
                if (!propertyNames.Add(property.Name))
                {
                    throw new InvalidDataException(
                        "The captured initial-scene metadata contains duplicate " +
                        $"property '{property.Name}'.");
                }
            }

            if (!root.TryGetProperty("schemaVersion", out JsonElement schema) ||
                schema.ValueKind != JsonValueKind.Number ||
                !schema.TryGetInt32(out int schemaVersion) ||
                schemaVersion != AssetDatabase.CurrentSchemaVersion)
            {
                throw new InvalidDataException(
                    "The captured initial-scene metadata schema is unsupported.");
            }
            if (!root.TryGetProperty("kind", out JsonElement kind) ||
                kind.ValueKind != JsonValueKind.String ||
                !string.Equals(
                    kind.GetString(),
                    "scene",
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "The captured initial-scene metadata kind must be 'scene'.");
            }
            if (!root.TryGetProperty("id", out JsonElement id) ||
                id.ValueKind != JsonValueKind.String)
            {
                throw new InvalidDataException(
                    "The captured initial-scene metadata Asset ID is missing.");
            }
            return NormalizeCanonicalAssetId(id.GetString() ?? "");
        }
    }

    private static byte[] RewriteManifestCanonicalSceneAssetId(
        byte[] source,
        string canonicalAssetId)
    {
        ReadOnlySpan<byte> payload = source;
        if (payload.StartsWith(new byte[] { 0xEF, 0xBB, 0xBF }))
            payload = payload[3..];
        ValidateManifestJson(payload);

        JsonNode? node;
        try
        {
            node = JsonNode.Parse(
                payload.ToArray(),
                documentOptions: new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                    MaxDepth = 32,
                });
        }
        catch (JsonException error)
        {
            throw new InvalidDataException(
                "The .acsproject manifest is not valid JSON.",
                error);
        }
        if (node is not JsonObject root)
        {
            throw new InvalidDataException(
                "The .acsproject manifest root must be a JSON object.");
        }

        string propertyName = root
            .Select(static property => property.Key)
            .SingleOrDefault(name => string.Equals(
                name,
                "canonicalSceneAssetId",
                StringComparison.OrdinalIgnoreCase))
            ?? "canonicalSceneAssetId";
        root[propertyName] = canonicalAssetId;
        string json = root.ToJsonString(JsonOpts) + Environment.NewLine;
        return StrictUtf8NoBom.GetBytes(json);
    }

    private static string NormalizeCanonicalAssetId(string assetId)
    {
        if (!Guid.TryParseExact(assetId, "N", out Guid parsed) ||
            parsed == Guid.Empty)
        {
            throw new InvalidDataException(
                "The canonical scene Asset ID is not a non-zero 32-digit GUID.");
        }
        return parsed.ToString("N");
    }

    private static bool PathsEqual(string left, string right) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
            StringComparison.OrdinalIgnoreCase);
}
