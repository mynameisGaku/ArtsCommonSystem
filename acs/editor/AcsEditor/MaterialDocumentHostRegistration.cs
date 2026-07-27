// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace AcsEditor;

/// <summary>
/// Builds the common document-host contract for one open material graph.
/// Asset database identity is preferred because it survives rename/move. Loose
/// files fall back to a canonical absolute path and are rebound after a path
/// mutation.
/// </summary>
internal static class MaterialDocumentHostRegistration
{
    internal const int SaveOrder = 100;

    internal static EditorDocumentId CreateId(
        string path,
        string? assetId = null)
    {
        string? normalizedAssetId = NormalizeAssetIdOrNull(assetId);
        if (!string.IsNullOrWhiteSpace(assetId) &&
            normalizedAssetId == null)
        {
            throw new ArgumentException(
                "Material Asset ID must be a non-zero 32-digit GUID.",
                nameof(assetId));
        }
        return normalizedAssetId == null
            ? EditorDocumentId.ForFile("material", path)
            : new EditorDocumentId("material", "asset:" + normalizedAssetId);
    }

    internal static string? ResolveAssetIdForOpen(
        Project? project,
        string path,
        string? suppliedAssetId)
    {
        string? normalizedSupplied =
            NormalizeAssetIdOrNull(suppliedAssetId);
        if (!string.IsNullOrWhiteSpace(suppliedAssetId) &&
            normalizedSupplied == null)
        {
            throw new ArgumentException(
                "Material Asset ID must be a non-zero 32-digit GUID.",
                nameof(suppliedAssetId));
        }
        if (project == null)
            return normalizedSupplied;

        AssetDatabase database = AssetDatabase.ForProject(project);
        if (!database.ContainsAssetPath(path))
            return normalizedSupplied;

        string authoritative =
            database.ResolveAuthoritativeAssetId(path, "material");
        if (normalizedSupplied != null &&
            !string.Equals(
                normalizedSupplied,
                authoritative,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The supplied Material Asset ID does not match its authoritative sidecar.");
        }
        return authoritative;
    }

    internal static EditorDocument Create(
        string path,
        string? assetId,
        string displayName,
        Func<EditorDocumentState> capture,
        Action<EditorDocumentState> restore,
        EditorDocumentSaveContract save,
        bool initiallySaved,
        EditorDocumentState? initialState = null) =>
        new(
            CreateId(path, assetId),
            displayName,
            path,
            capture,
            restore,
            save,
            initiallySaved,
            initialState: initialState,
            saveOrder: SaveOrder);

    internal static EditorDocumentSaveResult CompleteSuccessfulWrite(
        EditorDocumentState before,
        EditorDocumentState committed,
        Action commitLocalCheckpoint)
    {
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(committed);
        ArgumentNullException.ThrowIfNull(commitLocalCheckpoint);
        if (!string.Equals(
                before.ContentFingerprint,
                committed.ContentFingerprint,
                StringComparison.Ordinal))
        {
            return EditorDocumentSaveResult.Failed(
                "The material changed while it was being saved.");
        }

        // This checkpoint belongs only to the Material Editor's local graph-history ledger.
        // EditorDocument.SaveAsync remains the sole publisher of the hosted saved fingerprint.
        commitLocalCheckpoint();
        return EditorDocumentSaveResult.Saved(
            committed.ContentFingerprint);
    }

    internal static string? NormalizeAssetIdOrNull(string? assetId)
    {
        string value = assetId?.Trim() ?? "";
        return value.Length == 32 &&
               Guid.TryParseExact(value, "N", out Guid parsed) &&
               parsed != Guid.Empty
            ? parsed.ToString("N", CultureInfo.InvariantCulture)
            : null;
    }

    internal static bool PathsEqual(string left, string right)
    {
        try
        {
            string canonicalLeft = PathIdentity(left);
            string canonicalRight = PathIdentity(right);
            return string.Equals(
                canonicalLeft,
                canonicalRight,
                StringComparison.Ordinal);
        }
        catch
        {
            return false;
        }
    }

    internal static void RequireEveryExistingDocumentHosted<T>(
        IEnumerable<T> documents,
        Func<T, bool> register,
        Action rollback)
    {
        ArgumentNullException.ThrowIfNull(documents);
        ArgumentNullException.ThrowIfNull(register);
        ArgumentNullException.ThrowIfNull(rollback);

        try
        {
            foreach (T document in documents)
            {
                if (!register(document))
                {
                    throw new InvalidOperationException(
                        "An existing Material Editor could not join Document Host.");
                }
            }
        }
        catch (Exception registrationError)
        {
            try
            {
                rollback();
            }
            catch (Exception rollbackError)
            {
                throw new AggregateException(
                    "Material Document Host initialization failed and rollback was incomplete.",
                    registrationError,
                    rollbackError);
            }
            throw;
        }
    }

    private static string PathIdentity(string path)
    {
        string full = System.IO.Path.GetFullPath(path);
        return OperatingSystem.IsWindows()
            ? full.ToUpperInvariant()
            : full;
    }
}
