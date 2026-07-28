// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

public sealed record AssetPackageReadinessRequest(
    string ProjectName,
    string ProjectRoot,
    string AssetsRoot,
    string CanonicalSceneAssetId);

public sealed record AssetPackageReadinessDiagnostic(
    AssetCookDiagnosticSeverity Severity,
    string Code,
    string Message,
    string AssetPath,
    string AssetId,
    string Resolution)
{
    [JsonIgnore]
    public string SeverityLabel => Severity.ToString().ToUpperInvariant();

    [JsonIgnore]
    public bool CanLocate => AssetPath.Length != 0;
}

public sealed record AssetPackageReadinessAsset(
    string AssetId,
    string Path,
    string Kind,
    long SizeBytes,
    string ContentHash);

public sealed record AssetPackageReadinessReport(
    int SchemaVersion,
    bool Ready,
    string ProjectName,
    string CanonicalSceneAssetId,
    string RootAssetPath,
    int RequiredAssetCount,
    int ErrorCount,
    int WarningCount,
    string GraphHash,
    IReadOnlyList<AssetPackageReadinessDiagnostic> Diagnostics,
    IReadOnlyList<AssetPackageReadinessAsset> Assets);

/// <summary>
/// Runs the exact metadata-authoritative Cook closure used by Package without
/// mutating project files. The result is presentation-neutral and suitable for
/// the Content Browser, CI, and a durable JSON report.
/// </summary>
public static class AssetPackageReadiness
{
    public static Task<AssetPackageReadinessReport> AnalyzeAsync(
        AssetPackageReadinessRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        return Task.Run(
            () => Analyze(request, cancellationToken),
            cancellationToken);
    }

    public static AssetPackageReadinessReport Analyze(
        AssetPackageReadinessRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        cancellationToken.ThrowIfCancellationRequested();
        string projectRoot = Path.GetFullPath(request.ProjectRoot);
        string assetsRoot = Path.GetFullPath(request.AssetsRoot);
        ValidateProjectPaths(projectRoot, assetsRoot);

        AssetCookPlan plan = new AssetCookPlanner(
            projectRoot,
            assetsRoot).BuildByAssetId(
                request.CanonicalSceneAssetId,
                cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();

        AssetPackageReadinessDiagnostic[] diagnostics = plan.Diagnostics
            .Select(diagnostic => new AssetPackageReadinessDiagnostic(
                diagnostic.Severity,
                diagnostic.Code,
                diagnostic.Message,
                diagnostic.AssetPath,
                diagnostic.AssetId,
                ResolutionFor(diagnostic.Code)))
            .ToArray();
        AssetPackageReadinessAsset[] assets = plan.Assets
            .Select(asset => new AssetPackageReadinessAsset(
                asset.AssetId,
                asset.RelativePath,
                asset.Kind,
                asset.SizeBytes,
                asset.ContentHash))
            .ToArray();
        int errors = diagnostics.Count(static item =>
            item.Severity == AssetCookDiagnosticSeverity.Error);
        int warnings = diagnostics.Count(static item =>
            item.Severity == AssetCookDiagnosticSeverity.Warning);
        return new(
            1,
            errors == 0,
            request.ProjectName.Trim(),
            request.CanonicalSceneAssetId.Trim(),
            plan.Root?.RelativePath ?? "",
            assets.Length,
            errors,
            warnings,
            plan.GraphHash,
            Array.AsReadOnly(diagnostics),
            Array.AsReadOnly(assets));
    }

    public static byte[] SerializeJson(
        AssetPackageReadinessReport report)
    {
        ArgumentNullException.ThrowIfNull(report);
        return JsonSerializer.SerializeToUtf8Bytes(
            report,
            JsonOptions);
    }

    public static async Task WriteNewJsonAsync(
        string destinationPath,
        AssetPackageReadinessReport report,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(destinationPath);
        ArgumentNullException.ThrowIfNull(report);
        string destination = Path.GetFullPath(destinationPath);
        ValidateNewDestination(destination);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new IOException(
                "Package Readiness report parent could not be resolved.");
        RejectExistingReparsePoints(parent);
        Directory.CreateDirectory(parent);
        RejectExistingReparsePoints(parent);
        ValidateNewDestination(destination);

        string temporary = Path.Combine(
            parent,
            "." + Path.GetFileName(destination) + "." +
            Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            await using (var output = new FileStream(
                temporary,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                16 * 1024,
                FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(
                    output,
                    report,
                    JsonOptions,
                    cancellationToken).ConfigureAwait(false);
                await output.FlushAsync(cancellationToken)
                    .ConfigureAwait(false);
                output.Flush(flushToDisk: true);
            }
            cancellationToken.ThrowIfCancellationRequested();
            RejectExistingReparsePoints(parent);
            ValidateNewDestination(destination);
            File.Move(temporary, destination, overwrite: false);
        }
        finally
        {
            TryDeleteOwnedTemporary(temporary, parent);
        }
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        Converters =
        {
            new JsonStringEnumConverter(JsonNamingPolicy.CamelCase),
        },
    };

    private static void ValidateProjectPaths(
        string projectRoot,
        string assetsRoot)
    {
        if (!Directory.Exists(projectRoot))
        {
            throw new DirectoryNotFoundException(
                "Package Readiness project root does not exist: " +
                projectRoot);
        }
        if (!Directory.Exists(assetsRoot))
        {
            throw new DirectoryNotFoundException(
                "Package Readiness Assets root does not exist: " +
                assetsRoot);
        }
        string relative = Path.GetRelativePath(projectRoot, assetsRoot);
        if (relative == "." ||
            relative == ".." ||
            Path.IsPathRooted(relative) ||
            relative.StartsWith(
                ".." + Path.DirectorySeparatorChar,
                StringComparison.Ordinal) ||
            relative.StartsWith(
                ".." + Path.AltDirectorySeparatorChar,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Package Readiness Assets root must be inside the project root.");
        }
        RejectExistingReparsePoints(projectRoot);
        RejectExistingReparsePoints(assetsRoot);
    }

    private static string ResolutionFor(string code) => code switch
    {
        "CANONICAL_SCENE_ASSET_ID_REQUIRED" =>
            "Open and save the project so the initial Scene receives a canonical Asset ID.",
        "CANONICAL_SCENE_ASSET_ID_INVALID" or
        "CANONICAL_SCENE_ASSET_MISSING" or
        "CANONICAL_SCENE_KIND_INVALID" =>
            "Open Project Settings, select a valid Scene as the startup Scene, then save.",
        "ASSET_METADATA_MISSING" or
        "ASSET_METADATA_INVALID" or
        "ASSET_ID_AMBIGUOUS" or
        "ASSET_METADATA_STALE" =>
            "Locate the asset, run Reimport when available, then refresh Asset View.",
        "ASSET_DEPENDENCY_MISSING" or
        "ASSET_REFERENCE_MISSING" =>
            "Open Reference Viewer for the owning asset and replace or restore the missing dependency.",
        "ASSET_TYPE_UNSUPPORTED" =>
            "Import or convert this required file to a supported Cook input format.",
        "ASSET_PATH_NON_ASCII" =>
            "Rename this required asset to an ASCII-only path.",
        "ASSET_PATH_UNSAFE" =>
            "Remove the symlink/junction and import an ordinary file inside Assets.",
        "ASSET_DEPENDENCY_CYCLE" =>
            "Open Reference Viewer and remove one dependency edge in the reported cycle.",
        "ASSET_DEPENDENCY_SCAN_FAILED" or
        "ASSET_REFERENCE_INVALID" =>
            "Repair the source reference syntax or external URI, then Reimport the asset.",
        "ASSET_INDEX_CACHE_IGNORED" =>
            "No action is normally required; refresh Asset View if the warning persists.",
        _ =>
            "Inspect the diagnostic, repair the referenced asset, and run Package Readiness again.",
    };

    private static void ValidateNewDestination(string path)
    {
        RejectExistingReparsePoints(path);
        if (File.Exists(path) || Directory.Exists(path))
        {
            throw new IOException(
                "Package Readiness reports never overwrite an existing path: " +
                path);
        }
    }

    private static void RejectExistingReparsePoints(string path)
    {
        string current = Path.GetFullPath(path);
        while (!string.IsNullOrEmpty(current))
        {
            if ((File.Exists(current) || Directory.Exists(current)) &&
                (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException(
                    "Package Readiness path crosses a reparse point: " +
                    current);
            }
            string? parent = Path.GetDirectoryName(current);
            if (string.IsNullOrEmpty(parent) ||
                string.Equals(
                    Path.TrimEndingDirectorySeparator(parent),
                    Path.TrimEndingDirectorySeparator(current),
                    StringComparison.OrdinalIgnoreCase))
            {
                break;
            }
            current = parent;
        }
    }

    private static void TryDeleteOwnedTemporary(
        string temporary,
        string parent)
    {
        try
        {
            string fullTemporary = Path.GetFullPath(temporary);
            string fullParent = Path.GetFullPath(parent);
            if (!string.Equals(
                    Path.GetDirectoryName(fullTemporary),
                    fullParent,
                    StringComparison.OrdinalIgnoreCase) ||
                !Path.GetFileName(fullTemporary).StartsWith(
                    ".",
                    StringComparison.Ordinal) ||
                !Path.GetFileName(fullTemporary).EndsWith(
                    ".tmp",
                    StringComparison.Ordinal))
            {
                return;
            }
            RejectExistingReparsePoints(fullParent);
            RejectExistingReparsePoints(fullTemporary);
            if (File.Exists(fullTemporary) &&
                (File.GetAttributes(fullTemporary) &
                 (FileAttributes.Directory | FileAttributes.ReparsePoint)) == 0)
            {
                File.Delete(fullTemporary);
            }
        }
        catch
        {
            // Cleanup is best effort and constrained to our private sibling.
        }
    }
}
