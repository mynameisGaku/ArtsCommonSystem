// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;

namespace AcsEditor;

public enum AssetCookDiagnosticSeverity
{
    Info,
    Warning,
    Error,
}

public sealed record AssetCookDiagnostic(
    AssetCookDiagnosticSeverity Severity,
    string Code,
    string Message,
    string AssetPath = "",
    string AssetId = "");

public sealed record AssetCookPlan(
    AssetRecord? Root,
    IReadOnlyList<AssetRecord> Assets,
    IReadOnlyList<AssetCookDiagnostic> Diagnostics,
    string GraphHash)
{
    public bool HasErrors =>
        Diagnostics.Any(static item => item.Severity == AssetCookDiagnosticSeverity.Error);
}

/// <summary>
/// Builds a deterministic, metadata-authoritative Cook closure from one initial scene.
/// The persistent AssetDatabase supplies stable identities and content hashes; scanners for
/// path-bearing source formats verify that their sidecar dependency lists are current.
/// </summary>
public sealed class AssetCookPlanner
{
    private const int MaxGraphAssets = 65536;
    private const long MaxScannedTextBytes = 64L * 1024 * 1024;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static readonly HashSet<string> CookableExtensions = new(
        [
            ".acscene", ".acsprefab", ".acsmat", ".acsbp", ".acs3d",
            ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".ktx",
            ".ktx2", ".hdr", ".exr", ".webp", ".gif",
            ".wav", ".ogg", ".mp3", ".flac",
            ".fbx", ".gltf", ".glb", ".obj", ".mdl", ".mtl",
            ".ttf", ".otf",
            ".txt", ".json", ".csv", ".xml", ".yaml", ".yml", ".toml",
            ".ini", ".md", ".log",
            ".lua", ".hlsl", ".glsl", ".vert", ".frag",
            ".cso", ".dxil", ".spv", ".bin", ".dat",
        ],
        StringComparer.OrdinalIgnoreCase);

    private static readonly Regex SceneReference = new(
        @"^(?:(?:SPRT|MAT|PFAB)\s+-?\d+\s+)(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex MaterialReference = new(
        @"^(?:(?:albedo|normal|substrateExprTexture\d+)\s+)(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex Scene3DReference = new(
        @"^(?<verb>MSH3D|SPR3D|MAT3D|PFAB3D)\s+-?\d+\s+(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex ObjReference = new(
        @"^mtllib\s+(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex MtlReference = new(
        @"^(?:(?:map_[A-Za-z0-9_]+|bump|disp|decal)\s+)(?<path>.+?)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

    private readonly string _projectRoot;
    private readonly string _assetsRoot;
    private readonly AssetDatabase _database;

    public AssetCookPlanner(string projectRoot, string assetsRoot)
    {
        _projectRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot));
        _assetsRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(assetsRoot));
        _database = new AssetDatabase(_projectRoot, _assetsRoot);
    }

    /// <summary>
    /// Compatibility adapter for path-based project files. The path is resolved exactly once to
    /// its persistent identity; graph traversal itself is always rooted at the canonical Asset ID.
    /// </summary>
    public AssetCookPlan Build(
        string legacyScenePath,
        CancellationToken cancellationToken = default)
    {
        var diagnostics = new List<AssetCookDiagnostic>();
        RefreshStrict(diagnostics, cancellationToken);

        AssetRecord? root = null;
        if (!_database.TryGetByPath(legacyScenePath, out root) || root == null)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "INITIAL_SCENE_NOT_INDEXED",
                "Legacy InitialScene path is not represented by one valid authoritative asset metadata record.",
                PortableAssetPath(legacyScenePath)));
            return FinalizePlan(root, Array.Empty<AssetRecord>(), diagnostics);
        }
        return BuildFromCanonicalRoot(root, diagnostics, cancellationToken);
    }

    /// <summary>
    /// Builds the closure from the canonical scene identity stored by a project/manifest.
    /// Asset kind and source extension are importer concerns and do not select the graph root.
    /// </summary>
    public AssetCookPlan BuildByAssetId(
        string canonicalSceneAssetId,
        CancellationToken cancellationToken = default)
    {
        var diagnostics = new List<AssetCookDiagnostic>();
        string normalizedAssetId = canonicalSceneAssetId?.Trim() ?? "";
        if (normalizedAssetId.Length == 0)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "CANONICAL_SCENE_ASSET_ID_REQUIRED",
                "Packaging requires canonicalSceneAssetId. Open and save the project in the " +
                "Editor to migrate the legacy InitialScene path before retrying."));
            return FinalizePlan(null, Array.Empty<AssetRecord>(), diagnostics);
        }
        if (!Guid.TryParseExact(normalizedAssetId, "N", out Guid parsedAssetId) ||
            parsedAssetId == Guid.Empty)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "CANONICAL_SCENE_ASSET_ID_INVALID",
                "canonicalSceneAssetId must be a non-zero 32-digit GUID.",
                "",
                normalizedAssetId));
            return FinalizePlan(null, Array.Empty<AssetRecord>(), diagnostics);
        }
        normalizedAssetId = parsedAssetId.ToString("N");

        RefreshStrict(diagnostics, cancellationToken);
        AssetRecord? root = null;
        if (!_database.TryGetByAssetId(normalizedAssetId, out root) || root == null)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "CANONICAL_SCENE_ASSET_MISSING",
                "Canonical scene Asset ID does not resolve to one valid authoritative asset metadata record.",
                "",
                normalizedAssetId));
            return FinalizePlan(root, Array.Empty<AssetRecord>(), diagnostics);
        }
        return BuildFromCanonicalRoot(root, diagnostics, cancellationToken);
    }

    private void RefreshStrict(
        List<AssetCookDiagnostic> diagnostics,
        CancellationToken cancellationToken)
    {
        AssetDatabaseRefreshResult refresh = _database.RefreshForCook(cancellationToken);
        foreach (string warning in refresh.Warnings.OrderBy(static item => item, StringComparer.Ordinal))
            diagnostics.Add(MapDatabaseWarning(warning));
    }

    private AssetCookPlan BuildFromCanonicalRoot(
        AssetRecord root,
        List<AssetCookDiagnostic> diagnostics,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(root.Kind, "scene", StringComparison.OrdinalIgnoreCase))
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "CANONICAL_SCENE_KIND_INVALID",
                "Canonical scene Asset ID must identify an asset whose metadata kind is 'scene'.",
                root.RelativePath,
                root.AssetId));
        }
        var byId = _database.Snapshot().ToDictionary(
            static item => item.AssetId,
            StringComparer.OrdinalIgnoreCase);
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var dependencyGraph = new Dictionary<string, IReadOnlyList<string>>(
            StringComparer.OrdinalIgnoreCase);
        var pending = new Queue<string>();
        pending.Enqueue(root.AssetId);

        while (pending.Count != 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string assetId = pending.Dequeue();
            if (!visited.Add(assetId))
                continue;
            if (visited.Count > MaxGraphAssets)
            {
                diagnostics.Add(new(
                    AssetCookDiagnosticSeverity.Error,
                    "ASSET_GRAPH_LIMIT",
                    $"Reachable asset graph exceeds the {MaxGraphAssets} asset safety limit.",
                    root.RelativePath,
                    root.AssetId));
                break;
            }

            if (!byId.TryGetValue(assetId, out AssetRecord? asset))
            {
                diagnostics.Add(new(
                    AssetCookDiagnosticSeverity.Error,
                    "ASSET_DEPENDENCY_MISSING",
                    "Dependency GUID does not resolve to an indexed asset.",
                    "",
                    assetId));
                continue;
            }

            string extension = Path.GetExtension(asset.RelativePath);
            if (extension.Length == 0 || !CookableExtensions.Contains(extension))
            {
                diagnostics.Add(new(
                    AssetCookDiagnosticSeverity.Error,
                    "ASSET_TYPE_UNSUPPORTED",
                    "A required asset uses an unsupported Cook input format. Import or convert " +
                    "the dependency to a supported format.",
                    asset.RelativePath,
                    asset.AssetId));
            }
            if (asset.RelativePath.Any(static value => value > 127))
            {
                diagnostics.Add(new(
                    AssetCookDiagnosticSeverity.Error,
                    "ASSET_PATH_NON_ASCII",
                    "The current standalone runtime cannot safely open non-ASCII asset paths. " +
                    "Rename this required asset to an ASCII path.",
                    asset.RelativePath,
                    asset.AssetId));
            }

            IReadOnlyList<string> effectiveDependencies =
                EffectiveDependencyIds(asset, diagnostics, cancellationToken);
            dependencyGraph[asset.AssetId] = effectiveDependencies;
            foreach (string dependency in effectiveDependencies)
            {
                if (!byId.ContainsKey(dependency))
                {
                    diagnostics.Add(new(
                        AssetCookDiagnosticSeverity.Error,
                        "ASSET_DEPENDENCY_MISSING",
                        $"'{asset.RelativePath}' references a missing dependency GUID.",
                        asset.RelativePath,
                        dependency));
                    continue;
                }
                if (!visited.Contains(dependency))
                    pending.Enqueue(dependency);
            }
        }

        foreach (IReadOnlyList<string> cycle in FindDependencyCycles(
                     root.AssetId,
                     dependencyGraph))
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "ASSET_DEPENDENCY_CYCLE",
                "Reachable dependency cycle: " + string.Join(" -> ", cycle),
                root.RelativePath,
                root.AssetId));
        }

        AssetRecord[] assets = visited
            .Select(id => byId.GetValueOrDefault(id))
            .Where(static item => item != null)
            .Select(static item => item!)
            .OrderBy(static item => item.RelativePath, StringComparer.Ordinal)
            .ThenBy(static item => item.AssetId, StringComparer.Ordinal)
            .ToArray();
        return FinalizePlan(root, assets, diagnostics);
    }

    private IReadOnlyList<string> EffectiveDependencyIds(
        AssetRecord asset,
        List<AssetCookDiagnostic> diagnostics,
        CancellationToken cancellationToken)
    {
        string[] metadata = asset.Metadata.Dependencies
            .OrderBy(static item => item, StringComparer.Ordinal)
            .ToArray();
        if (!Path.GetExtension(asset.RelativePath).Equals(
                ".acsbp",
                StringComparison.OrdinalIgnoreCase))
        {
            VerifyDiscoveredDependencies(asset, diagnostics, cancellationToken);
            return Array.AsReadOnly(metadata);
        }

        // Blueprint inheritance is authored in the ACSBP source itself. The editor historically
        // did not mirror PARENT into sidecar metadata, so making that sidecar authoritative drops
        // nested parents from a package. Treat source-discovered Blueprint edges as authoritative
        // additions while retaining other importer-provided dependencies as a safe superset.
        try
        {
            IReadOnlyList<string> discovered = DiscoverDependencyIds(
                asset,
                diagnostics,
                cancellationToken);
            return Array.AsReadOnly(metadata
                .Concat(discovered)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(static item => item, StringComparer.Ordinal)
                .ToArray());
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException or
                JsonException or ArgumentException or FormatException or NotSupportedException)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "ASSET_DEPENDENCY_SCAN_FAILED",
                error.Message,
                asset.RelativePath,
                asset.AssetId));
            return Array.AsReadOnly(metadata);
        }
    }

    private void VerifyDiscoveredDependencies(
        AssetRecord asset,
        List<AssetCookDiagnostic> diagnostics,
        CancellationToken cancellationToken)
    {
        if (!SupportsDependencyScan(asset.RelativePath))
            return;

        try
        {
            IReadOnlyList<string> discovered = DiscoverDependencyIds(
                asset,
                diagnostics,
                cancellationToken);
            string[] metadata = asset.Metadata.Dependencies
                .OrderBy(static item => item, StringComparer.Ordinal)
                .ToArray();
            if (!discovered.SequenceEqual(metadata, StringComparer.Ordinal))
            {
                diagnostics.Add(new(
                    AssetCookDiagnosticSeverity.Error,
                    "ASSET_METADATA_STALE",
                    "Authoritative dependency metadata does not match references in the current " +
                    $"source. metadata=[{string.Join(",", metadata)}], " +
                    $"source=[{string.Join(",", discovered)}]",
                    asset.RelativePath,
                    asset.AssetId));
            }
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or InvalidDataException or
                JsonException or ArgumentException or FormatException or NotSupportedException)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "ASSET_DEPENDENCY_SCAN_FAILED",
                error.Message,
                asset.RelativePath,
                asset.AssetId));
        }
    }

    private IReadOnlyList<string> DiscoverDependencyIds(
        AssetRecord asset,
        List<AssetCookDiagnostic> diagnostics,
        CancellationToken cancellationToken)
    {
        var ids = new SortedSet<string>(StringComparer.Ordinal);
        string extension = Path.GetExtension(asset.RelativePath);
        byte[] sourceSnapshot =
            ReadVerifiedScanSnapshot(asset, cancellationToken);
        if (string.Equals(extension, ".gltf", StringComparison.OrdinalIgnoreCase))
        {
            ScanGltf(
                asset,
                sourceSnapshot,
                ids,
                diagnostics,
                cancellationToken);
            return Array.AsReadOnly(ids.ToArray());
        }
        if (string.Equals(extension, ".acsbp", StringComparison.OrdinalIgnoreCase))
        {
            ScanBlueprint(
                asset,
                sourceSnapshot,
                ids,
                diagnostics,
                cancellationToken);
            return Array.AsReadOnly(ids.ToArray());
        }

        string sourceText = DecodeScannableText(sourceSnapshot);
        Regex expression = extension.ToLowerInvariant() switch
        {
            ".acscene" => SceneReference,
            ".acsprefab" => SelectPrefabReferenceExpression(sourceText),
            ".acs3d" => Scene3DReference,
            ".acsmat" => MaterialReference,
            ".obj" => ObjReference,
            ".mtl" => MtlReference,
            _ => throw new InvalidDataException(
                $"No deterministic dependency scanner is registered for '{extension}'."),
        };
        using var reader = new StringReader(sourceText);
        while (reader.ReadLine() is { } line)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Match match = expression.Match(line);
            if (!match.Success)
            {
                string directive = FirstToken(line.TrimStart());
                if (IsReferenceDirective(
                        extension,
                        expression,
                        directive))
                {
                    throw new InvalidDataException(
                        $"Dependency directive is malformed: {directive}");
                }
                continue;
            }
            string reference = match.Groups["path"].Value.Trim();
            if (string.Equals(
                    match.Groups["verb"].Value,
                    "MAT3D",
                    StringComparison.Ordinal) &&
                IsLegacyNumeric3DMaterial(reference))
            {
                continue;
            }
            ResolveAndAdd(
                asset,
                reference,
                ids,
                diagnostics,
                relativeToAssetDirectory: extension is ".obj" or ".mtl");
        }
        return Array.AsReadOnly(ids.ToArray());
    }

    private static bool IsReferenceDirective(
        string extension,
        Regex expression,
        string directive)
    {
        if (directive.Length == 0)
            return false;
        if (extension.Equals(".acscene", StringComparison.OrdinalIgnoreCase) ||
            (extension.Equals(".acsprefab", StringComparison.OrdinalIgnoreCase) &&
             ReferenceEquals(expression, SceneReference)))
        {
            return directive is "SPRT" or "MAT" or "PFAB";
        }
        if (extension.Equals(".acs3d", StringComparison.OrdinalIgnoreCase) ||
            (extension.Equals(".acsprefab", StringComparison.OrdinalIgnoreCase) &&
             ReferenceEquals(expression, Scene3DReference)))
        {
            return directive is "MSH3D" or "SPR3D" or "MAT3D" or "PFAB3D";
        }
        if (extension.Equals(".acsmat", StringComparison.OrdinalIgnoreCase))
        {
            return directive is "albedo" or "normal" ||
                   directive.StartsWith(
                       "substrateExprTexture",
                       StringComparison.Ordinal) &&
                   directive["substrateExprTexture".Length..]
                       .All(static value => char.IsAsciiDigit(value));
        }
        if (extension.Equals(".obj", StringComparison.OrdinalIgnoreCase))
            return directive.Equals("mtllib", StringComparison.OrdinalIgnoreCase);
        if (extension.Equals(".mtl", StringComparison.OrdinalIgnoreCase))
        {
            return directive.StartsWith("map_", StringComparison.OrdinalIgnoreCase) ||
                   directive.Equals("bump", StringComparison.OrdinalIgnoreCase) ||
                   directive.Equals("disp", StringComparison.OrdinalIgnoreCase) ||
                   directive.Equals("decal", StringComparison.OrdinalIgnoreCase);
        }
        return false;
    }

    private void ScanBlueprint(
        AssetRecord asset,
        byte[] sourceSnapshot,
        SortedSet<string> ids,
        List<AssetCookDiagnostic> diagnostics,
        CancellationToken cancellationToken)
    {
        string text = DecodeScannableText(sourceSnapshot);
        AcsbpCookDocument document = AcsbpFormat.ParseForCook(text);

        int parentCount = 0;
        for (int index = 1; index < document.Lines.Length; ++index)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (document.IsComponentLine(index))
                continue;
            string line = document.Lines[index];
            if (!AcsbpFormat.TryParseCanonicalParentDirective(
                    line,
                    out string reference))
                continue;
            if (++parentCount > 1)
                throw new InvalidDataException(
                    "Blueprint payload may contain at most one PARENT directive.");

            if (!Path.GetExtension(reference).Equals(
                    ".acsbp",
                    StringComparison.OrdinalIgnoreCase))
            {
                diagnostics.Add(new(
                    AssetCookDiagnosticSeverity.Error,
                    "BLUEPRINT_PARENT_EXTENSION",
                    "Blueprint PARENT must reference an .acsbp asset.",
                    asset.RelativePath,
                    asset.AssetId));
                continue;
            }
            ResolveAndAdd(
                asset,
                reference,
                ids,
                diagnostics,
                relativeToAssetDirectory: false);
        }

        if (!document.HasComponents)
            return;
        Regex componentExpression = document.ComponentExtension == ".acs3d"
            ? Scene3DReference
            : SceneReference;
        int componentEnd = document.ComponentStart + document.ComponentCount;
        for (int index = document.ComponentStart + 1;
             index < componentEnd;
             ++index)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string line = document.Lines[index];
            string directive = FirstToken(line);
            bool referenceDirective = document.ComponentExtension == ".acs3d"
                ? directive is "MSH3D" or "SPR3D" or "MAT3D" or "PFAB3D"
                : directive is "SPRT" or "MAT" or "PFAB";
            if (!referenceDirective)
                continue;

            Match match = componentExpression.Match(line);
            if (!match.Success)
                throw new InvalidDataException(
                    $"Blueprint CMP reference directive '{directive}' is malformed.");
            string reference = match.Groups["path"].Value.Trim();
            if (directive == "MAT3D" && IsLegacyNumeric3DMaterial(reference))
                continue;
            ResolveAndAdd(
                asset,
                reference,
                ids,
                diagnostics,
                relativeToAssetDirectory: false);
        }
    }

    private static Regex SelectPrefabReferenceExpression(string sourceText)
    {
        using var reader = new StringReader(sourceText);
        return reader.ReadLine() switch
        {
            "ACS3D v2" => Scene3DReference,
            "ACSCENE v1" => SceneReference,
            _ => throw new InvalidDataException(
                "Prefab payload must begin with exactly 'ACS3D v2' or 'ACSCENE v1'."),
        };
    }

    private void ScanGltf(
        AssetRecord asset,
        byte[] sourceSnapshot,
        SortedSet<string> ids,
        List<AssetCookDiagnostic> diagnostics,
        CancellationToken cancellationToken)
    {
        using var stream = new MemoryStream(sourceSnapshot, writable: false);
        using JsonDocument document = JsonDocument.Parse(stream, new JsonDocumentOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 128,
        });
        if (document.RootElement.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("glTF root must be a JSON object.");
        ScanUriArray("buffers");
        ScanUriArray("images");

        void ScanUriArray(string property)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!document.RootElement.TryGetProperty(property, out JsonElement values) ||
                values.ValueKind != JsonValueKind.Array)
                return;
            foreach (JsonElement value in values.EnumerateArray())
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!value.TryGetProperty("uri", out JsonElement uri) ||
                    uri.ValueKind != JsonValueKind.String)
                    continue;
                string reference = uri.GetString() ?? "";
                if (reference.StartsWith("data:", StringComparison.OrdinalIgnoreCase))
                    continue;
                if (reference.Length == 0 || reference.Contains('#') || reference.Contains('?') ||
                    Uri.TryCreate(reference, UriKind.Absolute, out _))
                {
                    diagnostics.Add(new(
                        AssetCookDiagnosticSeverity.Error,
                        "ASSET_REFERENCE_INVALID",
                        $"glTF URI is not a portable Assets-relative file reference: {reference}",
                        asset.RelativePath,
                        asset.AssetId));
                    continue;
                }
                ResolveAndAdd(
                    asset,
                    Uri.UnescapeDataString(reference),
                    ids,
                    diagnostics,
                    relativeToAssetDirectory: true);
            }
        }
    }

    private void ResolveAndAdd(
        AssetRecord source,
        string reference,
        SortedSet<string> ids,
        List<AssetCookDiagnostic> diagnostics,
        bool relativeToAssetDirectory)
    {
        string? resolved = ResolveReference(
            source.FullPath,
            reference,
            relativeToAssetDirectory,
            out string code,
            out string message);
        if (resolved == null)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                code,
                message,
                source.RelativePath,
                source.AssetId));
            return;
        }
        if (!_database.TryGetByPath(resolved, out AssetRecord? dependency) || dependency == null)
        {
            diagnostics.Add(new(
                AssetCookDiagnosticSeverity.Error,
                "ASSET_REFERENCE_NOT_INDEXED",
                $"Referenced file has no unique valid asset metadata record: {reference}",
                source.RelativePath,
                source.AssetId));
            return;
        }
        ids.Add(dependency.AssetId);
    }

    private string? ResolveReference(
        string sourceFile,
        string reference,
        bool relativeToAssetDirectory,
        out string code,
        out string message)
    {
        code = "";
        message = "";
        try
        {
            string value = reference.Replace('/', Path.DirectorySeparatorChar);
            string[] candidates;
            if (Path.IsPathRooted(value))
            {
                candidates = [Path.GetFullPath(value)];
            }
            else if (relativeToAssetDirectory)
            {
                candidates =
                [
                    Path.GetFullPath(Path.Combine(Path.GetDirectoryName(sourceFile)!, value)),
                ];
            }
            else
            {
                candidates =
                [
                    Path.GetFullPath(Path.Combine(_projectRoot, value)),
                    Path.GetFullPath(Path.Combine(_assetsRoot, value)),
                ];
            }

            if (candidates.All(candidate => !IsUnder(candidate, _assetsRoot)))
            {
                code = "ASSET_REFERENCE_ESCAPE";
                message = $"Referenced path escapes Assets: {reference}";
                return null;
            }
            string[] existing = candidates
                .Where(File.Exists)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToArray();
            if (existing.Length == 0)
            {
                code = "ASSET_REFERENCE_MISSING";
                message = $"Referenced file does not exist: {reference}";
                return null;
            }
            if (existing.Length > 1)
            {
                code = "ASSET_REFERENCE_AMBIGUOUS";
                message = "Reference resolves to more than one file: " +
                          string.Join(", ", existing.OrderBy(static item => item, StringComparer.Ordinal));
                return null;
            }

            string resolved = existing[0];
            if (!IsUnder(resolved, _assetsRoot))
            {
                code = "ASSET_REFERENCE_ESCAPE";
                message = $"Referenced file is outside Assets: {reference}";
                return null;
            }
            EnsureOrdinaryAssetPath(resolved);
            return resolved;
        }
        catch (InvalidDataException error)
        {
            code = "ASSET_REFERENCE_UNSAFE";
            message = $"Reference path is unsafe: {reference}. {error.Message}";
            return null;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            code = "ASSET_REFERENCE_INVALID";
            message = $"Reference path is invalid: {reference}. {error.Message}";
            return null;
        }
    }

    private void EnsureOrdinaryAssetPath(string file)
    {
        string current = Path.GetFullPath(file);
        string root = Path.TrimEndingDirectorySeparator(_assetsRoot);
        while (IsUnderOrEqual(current, root))
        {
            FileAttributes attributes = File.GetAttributes(current);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException($"Asset reference crosses a reparse point: {current}");
            if (string.Equals(
                    Path.TrimEndingDirectorySeparator(current),
                    root,
                    StringComparison.OrdinalIgnoreCase))
                return;
            current = Path.GetDirectoryName(current)
                ?? throw new InvalidDataException("Asset reference has no parent.");
        }
        throw new InvalidDataException("Asset reference escapes Assets.");
    }

    private static bool SupportsDependencyScan(string relativePath)
    {
        string extension = Path.GetExtension(relativePath);
        return extension.Equals(".acscene", StringComparison.OrdinalIgnoreCase) ||
               extension.Equals(".acsprefab", StringComparison.OrdinalIgnoreCase) ||
               extension.Equals(".acsbp", StringComparison.OrdinalIgnoreCase) ||
               extension.Equals(".acs3d", StringComparison.OrdinalIgnoreCase) ||
               extension.Equals(".acsmat", StringComparison.OrdinalIgnoreCase) ||
               extension.Equals(".gltf", StringComparison.OrdinalIgnoreCase) ||
               extension.Equals(".obj", StringComparison.OrdinalIgnoreCase) ||
               extension.Equals(".mtl", StringComparison.OrdinalIgnoreCase);
    }

    private static IReadOnlyList<IReadOnlyList<string>> FindDependencyCycles(
        string rootAssetId,
        IReadOnlyDictionary<string, IReadOnlyList<string>> dependencyGraph)
    {
        var state = new Dictionary<string, byte>(StringComparer.OrdinalIgnoreCase);
        var path = new List<string>();
        var stackIndex = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var cycles = new SortedDictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal);
        var frames = new Stack<(string AssetId, string[] Dependencies, int NextIndex)>();

        void Push(string assetId)
        {
            state[assetId] = 1;
            stackIndex[assetId] = path.Count;
            path.Add(assetId);
            string[] dependencies = dependencyGraph.TryGetValue(
                    assetId,
                    out IReadOnlyList<string>? values)
                ? values
                    .Where(dependencyGraph.ContainsKey)
                    .OrderBy(static id => id, StringComparer.Ordinal)
                    .ToArray()
                : Array.Empty<string>();
            frames.Push((assetId, dependencies, 0));
        }

        Push(rootAssetId);
        while (frames.Count != 0)
        {
            (string assetId, string[] dependencies, int nextIndex) = frames.Pop();
            if (nextIndex >= dependencies.Length)
            {
                if (path.Count == 0 ||
                    !string.Equals(path[^1], assetId, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException(
                        "Cook dependency traversal stack is inconsistent.");
                }
                path.RemoveAt(path.Count - 1);
                stackIndex.Remove(assetId);
                state[assetId] = 2;
                continue;
            }

            string dependency = dependencies[nextIndex];
            frames.Push((assetId, dependencies, nextIndex + 1));
            state.TryGetValue(dependency, out byte dependencyState);
            if (dependencyState == 0)
            {
                Push(dependency);
            }
            else if (dependencyState == 1 &&
                     stackIndex.TryGetValue(dependency, out int start))
            {
                string[] cycle = CanonicalizeCycle(
                    path.Skip(start).Append(dependency).ToArray());
                cycles.TryAdd(
                    string.Join(">", cycle),
                    Array.AsReadOnly(cycle));
            }
        }

        return Array.AsReadOnly(cycles.Values.ToArray());
    }

    private static string[] CanonicalizeCycle(IReadOnlyList<string> cycle)
    {
        if (cycle.Count < 2 ||
            !string.Equals(cycle[0], cycle[^1], StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Cook dependency cycle must repeat its first asset id.");
        }

        string[] body = cycle.Take(cycle.Count - 1).ToArray();
        string[]? best = null;
        for (int start = 0; start < body.Length; ++start)
        {
            var rotated = new string[body.Length];
            for (int index = 0; index < body.Length; ++index)
                rotated[index] = body[(start + index) % body.Length];
            if (best == null || CompareAssetIdSequence(rotated, best) < 0)
                best = rotated;
        }

        var canonical = new string[body.Length + 1];
        Array.Copy(best!, canonical, body.Length);
        canonical[^1] = canonical[0];
        return canonical;
    }

    private static int CompareAssetIdSequence(
        IReadOnlyList<string> left,
        IReadOnlyList<string> right)
    {
        for (int index = 0; index < Math.Min(left.Count, right.Count); ++index)
        {
            int comparison = string.CompareOrdinal(left[index], right[index]);
            if (comparison != 0)
                return comparison;
        }
        return left.Count.CompareTo(right.Count);
    }

    private static bool IsLegacyNumeric3DMaterial(string value)
    {
        string[] fields = value.Split(
            [' ', '\t'],
            StringSplitOptions.RemoveEmptyEntries);
        return fields.Length == 2 &&
               float.TryParse(
                   fields[0],
                   System.Globalization.NumberStyles.Float,
                   System.Globalization.CultureInfo.InvariantCulture,
                   out _) &&
               float.TryParse(
                   fields[1],
                   System.Globalization.NumberStyles.Float,
                   System.Globalization.CultureInfo.InvariantCulture,
                   out _);
    }

    private static string FirstToken(string line)
    {
        int separator = line.IndexOfAny([' ', '\t']);
        return separator < 0 ? line : line[..separator];
    }

    private static void EnsureScannableTextFile(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists)
            throw new FileNotFoundException("Asset source does not exist.", path);
        if (info.Length > MaxScannedTextBytes)
            throw new InvalidDataException(
                $"Dependency-scanned text asset exceeds {MaxScannedTextBytes} bytes.");
    }

    private byte[] ReadVerifiedScanSnapshot(
        AssetRecord asset,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        EnsureScannableTextFile(asset.FullPath);
        EnsureOrdinaryAssetPath(asset.FullPath);
        using var stream = new FileStream(
            asset.FullPath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        if (stream.Length != asset.SizeBytes ||
            stream.Length > MaxScannedTextBytes)
        {
            throw new InvalidDataException(
                $"Dependency source changed after the Cook snapshot: {asset.RelativePath}");
        }

        var source = new byte[checked((int)stream.Length)];
        int offset = 0;
        while (offset != source.Length)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int read = stream.Read(source, offset, source.Length - offset);
            if (read == 0)
            {
                throw new EndOfStreamException(
                    $"Dependency source ended during snapshot read: {asset.RelativePath}");
            }
            offset += read;
        }
        cancellationToken.ThrowIfCancellationRequested();
        string hash = Convert.ToHexString(SHA256.HashData(source))
            .ToLowerInvariant();
        if (!string.Equals(hash, asset.ContentHash, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Dependency source changed after the Cook snapshot: {asset.RelativePath}");
        }
        return source;
    }

    internal byte[] ReadVerifiedScanSnapshotForSelfTest(
        AssetRecord asset,
        CancellationToken cancellationToken = default) =>
        ReadVerifiedScanSnapshot(asset, cancellationToken);

    private static string DecodeScannableText(byte[] source)
    {
        string text = StrictUtf8.GetString(source);
        return text.Length > 0 && text[0] == '\uFEFF'
            ? text[1..]
            : text;
    }

    private AssetCookDiagnostic MapDatabaseWarning(string warning)
    {
        if (warning.StartsWith("Metadata missing", StringComparison.Ordinal))
            return Error("ASSET_METADATA_MISSING", warning);
        if (warning.StartsWith("Duplicate asset id", StringComparison.Ordinal))
            return Error("ASSET_ID_AMBIGUOUS", warning);
        if (warning.StartsWith("Metadata rejected", StringComparison.Ordinal))
            return Error("ASSET_METADATA_INVALID", warning);
        if (warning.Contains("reparse", StringComparison.OrdinalIgnoreCase))
            return Error("ASSET_PATH_UNSAFE", warning);
        if (warning.StartsWith("Asset index cache ignored", StringComparison.Ordinal))
        {
            return new(
                AssetCookDiagnosticSeverity.Warning,
                "ASSET_INDEX_CACHE_IGNORED",
                warning);
        }
        return Error("ASSET_DATABASE_INVALID", warning);

        static AssetCookDiagnostic Error(string code, string message) =>
            new(AssetCookDiagnosticSeverity.Error, code, message);
    }

    private static AssetCookPlan FinalizePlan(
        AssetRecord? root,
        IReadOnlyList<AssetRecord> assets,
        List<AssetCookDiagnostic> diagnostics)
    {
        AssetCookDiagnostic[] orderedDiagnostics = diagnostics
            .Distinct()
            .OrderByDescending(static item => item.Severity)
            .ThenBy(static item => item.Code, StringComparer.Ordinal)
            .ThenBy(static item => item.AssetPath, StringComparer.Ordinal)
            .ThenBy(static item => item.AssetId, StringComparer.Ordinal)
            .ThenBy(static item => item.Message, StringComparer.Ordinal)
            .ToArray();
        string graphHash = ComputeGraphHash(assets);
        return new(
            root,
            Array.AsReadOnly(assets.ToArray()),
            Array.AsReadOnly(orderedDiagnostics),
            graphHash);
    }

    private static string ComputeGraphHash(IReadOnlyList<AssetRecord> assets)
    {
        var canonical = new StringBuilder();
        foreach (AssetRecord asset in assets
                     .OrderBy(static item => item.RelativePath, StringComparer.Ordinal))
        {
            Append("asset", asset.AssetId);
            Append("path", asset.RelativePath);
            Append("kind", asset.Kind);
            Append("content", asset.ContentHash);
            Append("source", asset.Metadata.Source);
            Append("importer", asset.Metadata.Importer);
            Append(
                "importerVersion",
                asset.Metadata.ImporterVersion.ToString(
                    System.Globalization.CultureInfo.InvariantCulture));
            foreach (string dependency in asset.Metadata.Dependencies.OrderBy(
                         static item => item,
                         StringComparer.Ordinal))
                Append("dependency", dependency);
            foreach (KeyValuePair<string, string> setting in
                     asset.Metadata.ImportSettings.OrderBy(
                         static item => item.Key,
                         StringComparer.Ordinal))
                Append("import." + setting.Key, setting.Value);
            canonical.Append('\n');
        }
        return Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(canonical.ToString())))
            .ToLowerInvariant();

        void Append(string name, string value)
        {
            canonical.Append(name.Length).Append(':').Append(name)
                .Append('=').Append(value.Length).Append(':').Append(value).Append('\n');
        }
    }

    private string PortableAssetPath(string path)
    {
        try
        {
            string full = Path.GetFullPath(path);
            return IsUnder(full, _assetsRoot)
                ? Path.GetRelativePath(_assetsRoot, full).Replace('\\', '/')
                : full;
        }
        catch
        {
            return path;
        }
    }

    private static bool IsUnder(string path, string root)
    {
        string fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string fullPath = Path.GetFullPath(path);
        return fullPath.StartsWith(
            fullRoot + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsUnderOrEqual(string path, string root) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)),
            StringComparison.OrdinalIgnoreCase) ||
        IsUnder(path, root);
}
