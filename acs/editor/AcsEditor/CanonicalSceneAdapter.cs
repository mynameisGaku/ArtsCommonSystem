// SPDX-License-Identifier: Apache-2.0
// Reversible bootstrap adapter for legacy ACS scene documents.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace AcsEditor.Packaging;

public enum CanonicalSceneAdapterSeverity
{
    Warning,
    Error,
}

public sealed record CanonicalSceneAdapterDiagnostic(
    CanonicalSceneAdapterSeverity Severity,
    string Code,
    string Message,
    int Line = 0,
    string Path = "");

public enum CanonicalSceneReferenceKind
{
    Texture,
    Mesh,
    Material,
}

public sealed record CanonicalSceneReference(
    CanonicalSceneReferenceKind Kind,
    string Directive,
    string Path,
    int Line);

/// <summary>
/// Package-level bootstrap metadata. adapterProjectionHint is only a legacy-import hint; it is
/// deliberately not authoritative scene/camera metadata.
/// </summary>
public sealed record CanonicalSceneBootstrapEnvelope(
    string path,
    string contract,
    string sourceFormat,
    string adapterProjectionHint);

public sealed record CanonicalSceneAdapterInspection(
    CanonicalSceneBootstrapEnvelope Envelope,
    IReadOnlyList<CanonicalSceneReference> References,
    IReadOnlyList<CanonicalSceneAdapterDiagnostic> Diagnostics)
{
    public bool HasErrors => Diagnostics.Any(
        static diagnostic => diagnostic.Severity == CanonicalSceneAdapterSeverity.Error);
}

/// <summary>
/// Validates legacy .acscene/.acs3d inputs at the package boundary and exposes a reversible
/// canonical bootstrap envelope. It does not convert either legacy document into the other.
/// </summary>
public static class CanonicalSceneAdapter
{
    public const string BootstrapPath = "main.acscene";
    public const string BootstrapContract = "acs.scene.bootstrap.v1";
    public const string LegacyScene2DFormat = "legacy-acscene-v1";
    public const string LegacyScene3DFormat = "legacy-acs3d-v2";

    private const int MaxInputBytes = 4 * 1024 * 1024;
    private const int MaxLineChars = 4095;
    private const int MaxNodes = 65536;
    private const int MaxDirectives = 262144;

    public static CanonicalSceneAdapterInspection InspectFile(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var diagnostics = new List<CanonicalSceneAdapterDiagnostic>();
        string extension = Path.GetExtension(path);
        CanonicalSceneBootstrapEnvelope fallback = EnvelopeForExtension(extension);

        try
        {
            var info = new FileInfo(path);
            if (!info.Exists)
            {
                diagnostics.Add(Error(
                    "SCENE_ADAPTER_FILE_MISSING",
                    "The legacy scene source does not exist.",
                    path: path));
                return new(fallback, Array.Empty<CanonicalSceneReference>(), diagnostics);
            }
            if (info.Length > MaxInputBytes)
            {
                diagnostics.Add(Error(
                    "SCENE_ADAPTER_INPUT_LIMIT",
                    $"The scene exceeds the {MaxInputBytes}-byte bootstrap safety limit.",
                    path: path));
                return new(fallback, Array.Empty<CanonicalSceneReference>(), diagnostics);
            }

            byte[] bytes = File.ReadAllBytes(path);
            if (Array.IndexOf(bytes, (byte)0) >= 0)
            {
                diagnostics.Add(Error(
                    "SCENE_ADAPTER_EMBEDDED_NUL",
                    "The scene contains an embedded NUL byte.",
                    path: path));
                return new(fallback, Array.Empty<CanonicalSceneReference>(), diagnostics);
            }

            string text;
            try
            {
                text = new UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false,
                    throwOnInvalidBytes: true).GetString(bytes);
            }
            catch (DecoderFallbackException error)
            {
                diagnostics.Add(Error(
                    "SCENE_ADAPTER_UTF8_INVALID",
                    $"The scene is not valid UTF-8: {error.Message}",
                    path: path));
                return new(fallback, Array.Empty<CanonicalSceneReference>(), diagnostics);
            }

            CanonicalSceneAdapterInspection inspected = InspectText(text, extension);
            return inspected with
            {
                Diagnostics = inspected.Diagnostics
                    .Select(item => string.IsNullOrEmpty(item.Path)
                        ? item with { Path = path }
                        : item)
                    .ToArray(),
            };
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            diagnostics.Add(Error(
                "SCENE_ADAPTER_READ_FAILED",
                $"The scene could not be read safely: {error.Message}",
                path: path));
            return new(fallback, Array.Empty<CanonicalSceneReference>(), diagnostics);
        }
    }

    public static CanonicalSceneAdapterInspection InspectText(
        string text,
        string sourceExtension)
    {
        ArgumentNullException.ThrowIfNull(text);
        sourceExtension ??= "";
        return sourceExtension.Equals(".acs3d", StringComparison.OrdinalIgnoreCase)
            ? InspectLegacy3D(text)
            : sourceExtension.Equals(".acscene", StringComparison.OrdinalIgnoreCase)
                ? InspectLegacy2D(text)
                : UnsupportedExtension(sourceExtension);
    }

    public static string RewriteReferences(
        string text,
        string sourceExtension,
        Func<CanonicalSceneReference, string> rewrite)
    {
        ArgumentNullException.ThrowIfNull(text);
        ArgumentNullException.ThrowIfNull(rewrite);
        CanonicalSceneAdapterInspection inspection = InspectText(text, sourceExtension);
        if (inspection.HasErrors)
        {
            throw new InvalidDataException(string.Join(
                Environment.NewLine,
                inspection.Diagnostics
                    .Where(static item =>
                        item.Severity == CanonicalSceneAdapterSeverity.Error)
                    .Select(static item =>
                        $"[{item.Code}] line {item.Line}: {item.Message}")));
        }

        var references = inspection.References.ToDictionary(
            static item => item.Line,
            static item => item);
        string normalized = NormalizeNewlines(text);
        string[] lines = normalized.Split('\n');
        for (int index = 0; index < lines.Length; ++index)
        {
            if (!references.TryGetValue(index + 1, out CanonicalSceneReference? reference))
                continue;
            string rewrittenPath = rewrite(reference)?.Trim() ?? "";
            if (string.IsNullOrEmpty(rewrittenPath) ||
                rewrittenPath.Contains('\r') ||
                rewrittenPath.Contains('\n') ||
                rewrittenPath.Contains('\0'))
            {
                throw new InvalidDataException(
                    $"The reference rewriter returned an unsafe path for line {reference.Line}.");
            }

            int directiveEnd = IndexAfterDirectiveAndId(lines[index]);
            if (directiveEnd < 0)
                throw new InvalidDataException(
                    $"The reference-bearing directive on line {reference.Line} is malformed.");
            lines[index] = lines[index][..directiveEnd] + rewrittenPath;
        }

        while (lines.Length > 0 && lines[^1].Length == 0)
            Array.Resize(ref lines, lines.Length - 1);
        return string.Join('\n', lines) + "\n";
    }

    /// <summary>
    /// Runtime mesh decoding currently receives one isolated glTF payload. Non-data URIs would
    /// therefore be silently unavailable even if a cooker happened to include the target file.
    /// </summary>
    public static IReadOnlyList<CanonicalSceneAdapterDiagnostic>
        ValidateStandaloneGltf(string path)
    {
        var diagnostics = new List<CanonicalSceneAdapterDiagnostic>();
        try
        {
            using JsonDocument document = JsonDocument.Parse(
                File.ReadAllBytes(path),
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                    MaxDepth = 128,
                });
            InspectStandaloneGltfUriArray(
                document.RootElement, "buffers", path, diagnostics);
            InspectStandaloneGltfUriArray(
                document.RootElement, "images", path, diagnostics);
        }
        catch (JsonException error)
        {
            diagnostics.Add(Error(
                "SCENE3D_GLTF_INVALID",
                $"The referenced glTF is invalid: {error.Message}",
                path: path));
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            diagnostics.Add(Error(
                "SCENE3D_GLTF_READ_FAILED",
                $"The referenced glTF could not be inspected: {error.Message}",
                path: path));
        }
        return diagnostics;
    }

    private static void InspectStandaloneGltfUriArray(
        JsonElement root,
        string property,
        string path,
        List<CanonicalSceneAdapterDiagnostic> diagnostics)
    {
        if (!root.TryGetProperty(property, out JsonElement values) ||
            values.ValueKind != JsonValueKind.Array)
            return;
        foreach (JsonElement value in values.EnumerateArray())
        {
            if (!value.TryGetProperty("uri", out JsonElement uriElement) ||
                uriElement.ValueKind != JsonValueKind.String)
                continue;
            string uri = uriElement.GetString() ?? "";
            if (uri.StartsWith("data:", StringComparison.OrdinalIgnoreCase))
                continue;
            diagnostics.Add(Error(
                "SCENE3D_GLTF_EXTERNAL_URI_UNSUPPORTED",
                "Standalone 3D mesh loading requires .glb or data: URIs; external glTF " +
                $"'{property}' URI '{uri}' cannot be resolved from an isolated pack entry.",
                path: path));
        }
    }

    private static CanonicalSceneAdapterInspection InspectLegacy2D(string text)
    {
        var diagnostics = new List<CanonicalSceneAdapterDiagnostic>();
        var references = new List<CanonicalSceneReference>();
        string[] lines = PrepareLines(text, diagnostics);
        if (lines.Length == 0 ||
            !string.Equals(lines[0], "ACSCENE v1", StringComparison.Ordinal))
        {
            diagnostics.Add(Error(
                "SCENE2D_HEADER_UNSUPPORTED",
                "Legacy 2D bootstrap input must begin with exactly 'ACSCENE v1'.",
                1));
        }

        for (int index = 1; index < lines.Length; ++index)
        {
            string line = lines[index];
            if (line.StartsWith("SPRT ", StringComparison.Ordinal))
                AddLegacy2DReference(line, index + 1, "SPRT", CanonicalSceneReferenceKind.Texture);
            else if (line.StartsWith("MAT ", StringComparison.Ordinal))
                AddLegacy2DReference(line, index + 1, "MAT", CanonicalSceneReferenceKind.Material);
        }
        return new(
            new(BootstrapPath, BootstrapContract, LegacyScene2DFormat, "orthographic"),
            references,
            diagnostics);

        void AddLegacy2DReference(
            string line,
            int lineNumber,
            string directive,
            CanonicalSceneReferenceKind kind)
        {
            string? path = RemainderAfterDirectiveAndId(line);
            if (string.IsNullOrWhiteSpace(path))
            {
                diagnostics.Add(Error(
                    "SCENE2D_REFERENCE_INVALID",
                    $"{directive} requires a node id and asset path.",
                    lineNumber));
                return;
            }
            references.Add(new(kind, directive, path.Trim(), lineNumber));
        }
    }

    private static CanonicalSceneAdapterInspection InspectLegacy3D(string text)
    {
        var diagnostics = new List<CanonicalSceneAdapterDiagnostic>();
        var references = new List<CanonicalSceneReference>();
        var nodes = new Dictionary<int, NodeRecord>();
        var componentCounts = new Dictionary<int, int>();
        var meshes = new HashSet<int>();
        var materials = new HashSet<int>();
        string[] lines = PrepareLines(text, diagnostics);

        if (lines.Length == 0 ||
            !string.Equals(lines[0], "ACS3D v2", StringComparison.Ordinal))
        {
            diagnostics.Add(Error(
                "SCENE3D_HEADER_UNSUPPORTED",
                "Legacy 3D bootstrap input must begin with exactly 'ACS3D v2'.",
                1));
        }

        int directiveCount = 0;
        for (int index = 1; index < lines.Length; ++index)
        {
            string line = lines[index];
            int lineNumber = index + 1;
            if (line.Length == 0)
                continue;
            if (++directiveCount > MaxDirectives)
            {
                diagnostics.Add(Error(
                    "SCENE3D_DIRECTIVE_LIMIT",
                    $"The scene exceeds the {MaxDirectives}-directive safety limit.",
                    lineNumber));
                break;
            }

            string directive = FirstToken(line);
            switch (directive)
            {
                case "N3D":
                    InspectNode(line, lineNumber);
                    break;
                case "MSH3D":
                    InspectPathReference(
                        line,
                        lineNumber,
                        CanonicalSceneReferenceKind.Mesh,
                        meshes,
                        requireMeshPrimitive: true);
                    break;
                case "MAT3D":
                    InspectMaterial(line, lineNumber);
                    break;
                case "CMP3D":
                    InspectComponent(line, lineNumber);
                    break;
                case "CPROP3D":
                    InspectComponentProperty(line, lineNumber);
                    break;
                case "FLG3D":
                    InspectFlags(line, lineNumber);
                    break;
                case "EMPTY3D":
                    InspectSingleNodeId(line, lineNumber, directive);
                    break;
                case "SEL3D":
                    InspectSelection(line, lineNumber);
                    break;
                case "SPR3D":
                case "PLY3D":
                case "PFAB3D":
                    diagnostics.Add(Error(
                        "SCENE3D_RUNTIME_DIRECTIVE_UNSUPPORTED",
                        $"{directive} is preserved by the editor but is not yet implemented by " +
                        "the standalone legacy 3D runtime adapter.",
                        lineNumber));
                    break;
                default:
                    diagnostics.Add(Error(
                        "SCENE3D_DIRECTIVE_UNKNOWN",
                        $"Unknown ACS3D directive '{directive}'.",
                        lineNumber));
                    break;
            }
        }

        if (nodes.Count == 0 && !diagnostics.Any(static item =>
                item.Code is "SCENE3D_HEADER_UNSUPPORTED" or "SCENE_ADAPTER_LINE_LIMIT"))
        {
            // The editor can serialize an empty document. Runtime creates one synthetic root.
            diagnostics.Add(new(
                CanonicalSceneAdapterSeverity.Warning,
                "SCENE3D_EMPTY_DOCUMENT",
                "The empty editor scene will load as one synthetic runtime root."));
        }
        return new(
            new(BootstrapPath, BootstrapContract, LegacyScene3DFormat, "perspective"),
            references,
            diagnostics);

        void InspectNode(string line, int lineNumber)
        {
            string[] tokens = Tokens(line);
            if (tokens.Length < 17 ||
                !TryInt(tokens[1], out int id) ||
                !TryInt(tokens[2], out int parent) ||
                !TryInt(tokens[3], out int primitive) ||
                primitive is < -1 or > 3 ||
                !AllFinite(tokens, 4, 13))
            {
                diagnostics.Add(Error(
                    "SCENE3D_NODE_INVALID",
                    "N3D requires id, parent, primitive, 13 finite transform/color values, " +
                    "and an optional name.",
                    lineNumber));
                return;
            }
            if (id < 0 || nodes.ContainsKey(id))
            {
                diagnostics.Add(Error(
                    "SCENE3D_NODE_ID_INVALID",
                    "N3D ids must be unique non-negative integers.",
                    lineNumber));
                return;
            }
            if (nodes.Count >= MaxNodes)
            {
                diagnostics.Add(Error(
                    "SCENE3D_NODE_LIMIT",
                    $"The scene exceeds the {MaxNodes}-node safety limit.",
                    lineNumber));
                return;
            }
            if (parent != -1 && (parent < 0 || !nodes.ContainsKey(parent)))
            {
                diagnostics.Add(Error(
                    "SCENE3D_PARENT_INVALID",
                    "N3D parents must be -1 or an earlier node id.",
                    lineNumber));
                return;
            }
            nodes.Add(id, new(primitive));
            componentCounts.Add(id, 0);
        }

        void InspectPathReference(
            string line,
            int lineNumber,
            CanonicalSceneReferenceKind kind,
            HashSet<int> seen,
            bool requireMeshPrimitive)
        {
            string[] tokens = Tokens(line);
            string? path = RemainderAfterDirectiveAndId(line);
            if (tokens.Length < 3 ||
                !TryInt(tokens[1], out int id) ||
                !nodes.TryGetValue(id, out NodeRecord? node) ||
                string.IsNullOrWhiteSpace(path))
            {
                diagnostics.Add(Error(
                    "SCENE3D_REFERENCE_INVALID",
                    $"{tokens.FirstOrDefault() ?? "reference"} requires an existing node id and path.",
                    lineNumber));
                return;
            }
            if (!seen.Add(id))
            {
                diagnostics.Add(Error(
                    "SCENE3D_REFERENCE_DUPLICATE",
                    $"Node {id} has more than one {kind.ToString().ToLowerInvariant()} reference.",
                    lineNumber));
                return;
            }
            if (requireMeshPrimitive && node.Primitive != 3)
            {
                diagnostics.Add(Error(
                    "SCENE3D_MESH_PRIMITIVE_INVALID",
                    "MSH3D is only valid for an N3D node whose primitive is Mesh (3).",
                    lineNumber));
                return;
            }
            string trimmed = path.Trim();
            if (kind == CanonicalSceneReferenceKind.Mesh)
            {
                string extension = Path.GetExtension(trimmed);
                if (!(extension.Equals(".glb", StringComparison.OrdinalIgnoreCase) ||
                      extension.Equals(".gltf", StringComparison.OrdinalIgnoreCase) ||
                      extension.Equals(".obj", StringComparison.OrdinalIgnoreCase) ||
                      extension.Equals(".fbx", StringComparison.OrdinalIgnoreCase)))
                {
                    diagnostics.Add(Error(
                        "SCENE3D_MESH_FORMAT_UNSUPPORTED",
                        "Standalone 3D meshes must use .glb, .gltf, .obj, or .fbx.",
                        lineNumber));
                    return;
                }
            }
            references.Add(new(kind, FirstToken(line), trimmed, lineNumber));
        }

        void InspectMaterial(string line, int lineNumber)
        {
            string? remainder = RemainderAfterDirectiveAndId(line);
            string[] tokens = Tokens(line);
            if (tokens.Length < 3 ||
                !TryInt(tokens[1], out int id) ||
                !nodes.ContainsKey(id) ||
                string.IsNullOrWhiteSpace(remainder))
            {
                diagnostics.Add(Error(
                    "SCENE3D_MATERIAL_INVALID",
                    "MAT3D requires an existing node id and either an .acsmat path or two PBR values.",
                    lineNumber));
                return;
            }

            string[] values = remainder.Split(
                (char[]?)null,
                StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            if (values.Length == 2 &&
                TryFinite(values[0], out _) &&
                TryFinite(values[1], out _))
                return;
            if (!remainder.Trim().EndsWith(".acsmat", StringComparison.OrdinalIgnoreCase))
            {
                diagnostics.Add(Error(
                    "SCENE3D_MATERIAL_INVALID",
                    "Path-based MAT3D references must point to an .acsmat asset.",
                    lineNumber));
                return;
            }
            if (!materials.Add(id))
            {
                diagnostics.Add(Error(
                    "SCENE3D_REFERENCE_DUPLICATE",
                    $"Node {id} has more than one material reference.",
                    lineNumber));
                return;
            }
            references.Add(new(
                CanonicalSceneReferenceKind.Material,
                "MAT3D",
                remainder.Trim(),
                lineNumber));
        }

        void InspectComponent(string line, int lineNumber)
        {
            string[] tokens = Tokens(line);
            string? typeName = RemainderAfterDirectiveAndId(line);
            if (tokens.Length < 3 ||
                !TryInt(tokens[1], out int id) ||
                !componentCounts.TryGetValue(id, out int count) ||
                string.IsNullOrWhiteSpace(typeName) ||
                typeName.Any(char.IsWhiteSpace))
            {
                diagnostics.Add(Error(
                    "SCENE3D_COMPONENT_INVALID",
                    "CMP3D requires an existing node id and one reflected component type name.",
                    lineNumber));
                return;
            }
            componentCounts[id] = checked(count + 1);
        }

        void InspectComponentProperty(string line, int lineNumber)
        {
            string[] tokens = Tokens(line);
            if (tokens.Length != 8 ||
                !TryInt(tokens[1], out int id) ||
                !TryNonNegativeInt(tokens[2], out int slot) ||
                !TryNonNegativeInt(tokens[3], out _) ||
                !componentCounts.TryGetValue(id, out int count) ||
                slot >= count ||
                !AllFinite(tokens, 4, 4))
            {
                diagnostics.Add(Error(
                    "SCENE3D_COMPONENT_PROPERTY_INVALID",
                    "CPROP3D requires an existing component slot, property index, and four finite values.",
                    lineNumber));
            }
        }

        void InspectFlags(string line, int lineNumber)
        {
            string[] tokens = Tokens(line);
            if (tokens.Length != 4 ||
                !TryInt(tokens[1], out int id) ||
                !nodes.ContainsKey(id) ||
                !TryInt(tokens[2], out int visible) ||
                !TryInt(tokens[3], out int enabled) ||
                visible is < 0 or > 1 ||
                enabled is < 0 or > 1)
            {
                diagnostics.Add(Error(
                    "SCENE3D_FLAGS_INVALID",
                    "FLG3D requires an existing node id and 0/1 visible and enabled flags.",
                    lineNumber));
            }
        }

        void InspectSingleNodeId(string line, int lineNumber, string directive)
        {
            string[] tokens = Tokens(line);
            if (tokens.Length != 2 ||
                !TryInt(tokens[1], out int id) ||
                !nodes.ContainsKey(id))
            {
                diagnostics.Add(Error(
                    "SCENE3D_NODE_DIRECTIVE_INVALID",
                    $"{directive} requires exactly one existing node id.",
                    lineNumber));
            }
        }

        void InspectSelection(string line, int lineNumber)
        {
            string[] tokens = Tokens(line);
            if (tokens.Length != 2 || !TryInt(tokens[1], out _))
            {
                diagnostics.Add(Error(
                    "SCENE3D_SELECTION_INVALID",
                    "SEL3D requires exactly one integer node id.",
                    lineNumber));
            }
        }
    }

    private static CanonicalSceneAdapterInspection UnsupportedExtension(string extension)
    {
        CanonicalSceneBootstrapEnvelope envelope = EnvelopeForExtension(extension);
        return new(
            envelope,
            Array.Empty<CanonicalSceneReference>(),
            new[]
            {
                Error(
                    "SCENE_ADAPTER_EXTENSION_UNSUPPORTED",
                    "Canonical bootstrap adapters currently accept .acscene and .acs3d sources."),
            });
    }

    private static CanonicalSceneBootstrapEnvelope EnvelopeForExtension(string extension) =>
        extension.Equals(".acs3d", StringComparison.OrdinalIgnoreCase)
            ? new(BootstrapPath, BootstrapContract, LegacyScene3DFormat, "perspective")
            : new(BootstrapPath, BootstrapContract, LegacyScene2DFormat, "orthographic");

    private static string[] PrepareLines(
        string text,
        List<CanonicalSceneAdapterDiagnostic> diagnostics)
    {
        if (Encoding.UTF8.GetByteCount(text) > MaxInputBytes)
        {
            diagnostics.Add(Error(
                "SCENE_ADAPTER_INPUT_LIMIT",
                $"The scene exceeds the {MaxInputBytes}-byte bootstrap safety limit."));
            return Array.Empty<string>();
        }
        string normalized = NormalizeNewlines(text);
        string[] lines = normalized.Split('\n');
        for (int index = 0; index < lines.Length; ++index)
        {
            if (lines[index].Length > MaxLineChars)
            {
                diagnostics.Add(Error(
                    "SCENE_ADAPTER_LINE_LIMIT",
                    $"A scene line exceeds the {MaxLineChars}-character runtime limit.",
                    index + 1));
            }
            if (lines[index].Any(static value =>
                    (value < 0x20 && value != '\t') || value == 0x7f))
            {
                diagnostics.Add(Error(
                    "SCENE_ADAPTER_CONTROL_CHARACTER",
                    "A scene line contains a forbidden control character.",
                    index + 1));
            }
        }
        while (lines.Length > 0 && lines[^1].Length == 0)
            Array.Resize(ref lines, lines.Length - 1);
        return lines;
    }

    private static string NormalizeNewlines(string text) =>
        text.Replace("\r\n", "\n", StringComparison.Ordinal)
            .Replace('\r', '\n');

    private static string[] Tokens(string line) =>
        line.Split(
            (char[]?)null,
            StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

    private static string FirstToken(string line)
    {
        int end = line.IndexOfAny([' ', '\t']);
        return end < 0 ? line : line[..end];
    }

    private static string? RemainderAfterDirectiveAndId(string line)
    {
        int end = IndexAfterDirectiveAndId(line);
        return end < 0 || end >= line.Length ? null : line[end..];
    }

    private static int IndexAfterDirectiveAndId(string line)
    {
        int cursor = 0;
        SkipWhitespace(line, ref cursor);
        SkipToken(line, ref cursor);
        if (cursor >= line.Length)
            return -1;
        SkipWhitespace(line, ref cursor);
        int idStart = cursor;
        SkipToken(line, ref cursor);
        if (cursor == idStart)
            return -1;
        SkipWhitespace(line, ref cursor);
        return cursor;
    }

    private static void SkipWhitespace(string text, ref int cursor)
    {
        while (cursor < text.Length && char.IsWhiteSpace(text[cursor]))
            ++cursor;
    }

    private static void SkipToken(string text, ref int cursor)
    {
        while (cursor < text.Length && !char.IsWhiteSpace(text[cursor]))
            ++cursor;
    }

    private static bool TryInt(string text, out int value) =>
        int.TryParse(
            text,
            NumberStyles.AllowLeadingSign,
            CultureInfo.InvariantCulture,
            out value);

    private static bool TryNonNegativeInt(string text, out int value) =>
        TryInt(text, out value) && value >= 0;

    private static bool TryFinite(string text, out float value) =>
        float.TryParse(
            text,
            NumberStyles.Float,
            CultureInfo.InvariantCulture,
            out value) &&
        float.IsFinite(value);

    private static bool AllFinite(
        IReadOnlyList<string> tokens,
        int start,
        int count)
    {
        if (start < 0 || count < 0 || start + count > tokens.Count)
            return false;
        for (int index = start; index < start + count; ++index)
        {
            if (!TryFinite(tokens[index], out _))
                return false;
        }
        return true;
    }

    private static CanonicalSceneAdapterDiagnostic Error(
        string code,
        string message,
        int line = 0,
        string path = "") =>
        new(CanonicalSceneAdapterSeverity.Error, code, message, line, path);

    private sealed record NodeRecord(int Primitive);
}
