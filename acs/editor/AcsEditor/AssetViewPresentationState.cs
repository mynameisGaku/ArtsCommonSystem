// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Text.Json;

namespace AcsEditor;

internal enum AssetViewMode
{
    Tiles,
    List,
    Details,
}

/// <summary>
/// Per-project presentation preferences for the Asset View. This state changes only how
/// indexed assets are shown; it never affects asset identity or the authoritative database.
/// </summary>
internal sealed record AssetViewPresentationState(
    AssetViewMode ViewMode,
    int ThumbnailSize,
    bool ShowPreview,
    bool ShowFolders,
    bool ShowEmptyFolders)
{
    internal const int MinimumThumbnailSize = 32;
    internal const int MaximumThumbnailSize = 192;

    internal static AssetViewPresentationState Default { get; } = new(
        AssetViewMode.Tiles,
        ThumbnailSize: 64,
        ShowPreview: true,
        ShowFolders: true,
        ShowEmptyFolders: true);

    internal AssetViewPresentationState Normalize()
    {
        AssetViewMode mode = Enum.IsDefined(ViewMode)
            ? ViewMode
            : AssetViewMode.Tiles;
        return this with
        {
            ViewMode = mode,
            ThumbnailSize = Math.Clamp(
                ThumbnailSize,
                MinimumThumbnailSize,
                MaximumThumbnailSize),
            ShowEmptyFolders = ShowFolders && ShowEmptyFolders,
        };
    }
}

/// <summary>
/// Atomic, bounded persistence for <see cref="AssetViewPresentationState"/> below the
/// project-owned <c>Assets/.acsdb/editor</c> directory.
/// </summary>
internal sealed class AssetViewPresentationStore
{
    private const int CurrentSchemaVersion = 1;
    private const long MaximumFileBytes = 32L * 1024L;
    private const string FileName = "asset-view.v1.json";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
    };

    internal AssetViewPresentationState Load(string assetsRoot)
    {
        string root = ValidateAssetsRoot(assetsRoot);
        string path = GetStorePath(root);
        if (!File.Exists(path)) return AssetViewPresentationState.Default;
        EnsureOrdinaryFile(path);
        var info = new FileInfo(path);
        if (info.Length <= 0 || info.Length > MaximumFileBytes)
            return AssetViewPresentationState.Default;

        try
        {
            using FileStream stream = new(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                16 * 1024,
                FileOptions.SequentialScan);
            StoredState? stored = JsonSerializer.Deserialize<StoredState>(
                stream,
                JsonOptions);
            if (stored == null || stored.SchemaVersion != CurrentSchemaVersion)
                return AssetViewPresentationState.Default;
            if (!Enum.TryParse(
                    stored.ViewMode,
                    ignoreCase: true,
                    out AssetViewMode mode) ||
                !Enum.IsDefined(mode))
            {
                return AssetViewPresentationState.Default;
            }
            return new AssetViewPresentationState(
                mode,
                stored.ThumbnailSize,
                stored.ShowPreview,
                stored.ShowFolders,
                stored.ShowEmptyFolders).Normalize();
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or JsonException)
        {
            return AssetViewPresentationState.Default;
        }
    }

    internal void Save(string assetsRoot, AssetViewPresentationState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        string root = ValidateAssetsRoot(assetsRoot);
        string directory = EnsureStoreDirectory(root);
        string destination = Path.Combine(directory, FileName);
        if (File.Exists(destination)) EnsureOrdinaryFile(destination);

        AssetViewPresentationState normalized = state.Normalize();
        var stored = new StoredState(
            CurrentSchemaVersion,
            normalized.ViewMode.ToString(),
            normalized.ThumbnailSize,
            normalized.ShowPreview,
            normalized.ShowFolders,
            normalized.ShowEmptyFolders);
        string temporary = destination + ".tmp-" + Guid.NewGuid().ToString("N");
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
                JsonSerializer.Serialize(stream, stored, JsonOptions);
                stream.Flush(flushToDisk: true);
            }
            EnsureOrdinaryFile(temporary);
            if (new FileInfo(temporary).Length > MaximumFileBytes)
                throw new IOException("Asset View preferences exceed the size limit.");
            EnsureOrdinaryDirectory(directory, "Asset editor settings directory");
            if (File.Exists(destination)) EnsureOrdinaryFile(destination);
            File.Move(temporary, destination, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException)
            {
            }
        }
    }

    internal static string GetStorePath(string assetsRoot) =>
        Path.Combine(
            Path.GetFullPath(assetsRoot),
            AssetDatabase.InternalDirectoryName,
            "editor",
            FileName);

    private static string ValidateAssetsRoot(string assetsRoot)
    {
        if (string.IsNullOrWhiteSpace(assetsRoot))
            throw new ArgumentException("Assets root cannot be empty.", nameof(assetsRoot));
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
        EnsureOrdinaryDirectory(root, "Assets root");
        return root;
    }

    private static string EnsureStoreDirectory(string assetsRoot)
    {
        string internalDirectory = Path.Combine(
            assetsRoot,
            AssetDatabase.InternalDirectoryName);
        EnsureOrCreateOrdinaryDirectory(internalDirectory, "Asset database directory");
        string editorDirectory = Path.Combine(internalDirectory, "editor");
        EnsureOrCreateOrdinaryDirectory(editorDirectory, "Asset editor settings directory");
        return editorDirectory;
    }

    private static void EnsureOrCreateOrdinaryDirectory(string path, string label)
    {
        if (!Directory.Exists(path))
            Directory.CreateDirectory(path);
        EnsureOrdinaryDirectory(path, label);
    }

    private static void EnsureOrdinaryDirectory(string path, string label)
    {
        var info = new DirectoryInfo(path);
        info.Refresh();
        if (!info.Exists)
            throw new DirectoryNotFoundException($"{label} does not exist: {path}");
        if ((info.Attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException($"{label} cannot be a reparse point.");
    }

    private static void EnsureOrdinaryFile(string path)
    {
        var info = new FileInfo(path);
        info.Refresh();
        if (!info.Exists)
            throw new FileNotFoundException("Asset View preference file does not exist.", path);
        if ((info.Attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException(
                "Asset View preference files cannot be reparse points.");
    }

    private sealed record StoredState(
        int SchemaVersion,
        string ViewMode,
        int ThumbnailSize,
        bool ShowPreview,
        bool ShowFolders,
        bool ShowEmptyFolders);
}
