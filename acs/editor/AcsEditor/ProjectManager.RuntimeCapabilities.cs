// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Text;

namespace AcsEditor;

internal readonly record struct RuntimeBuildCapabilities(
    bool SupportsLegacyScene3D,
    string Evidence);

public static partial class ProjectManager
{
    internal const string LegacyScene3DCapabilityMarker =
        "// ACS_RUNTIME_CAPABILITY: LEGACY_SCENE3D=1";

    private const long MaximumRuntimeCapabilitySourceBytes = 4L * 1024L * 1024L;

    /// <summary>
    /// Detects support from the project's actual runtime source without modifying it.
    /// This deliberately does not infer capability from the editor version: old or
    /// customized Game.cpp files remain fail-closed until their own bootstrap has a
    /// concrete legacy-3D route.
    /// </summary>
    internal static RuntimeBuildCapabilities DetectRuntimeBuildCapabilities(
        Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        string gameSourcePath = Path.Combine(project.SourceDir, "Game.cpp");
        try
        {
            var info = new FileInfo(gameSourcePath);
            if (!info.Exists)
            {
                return new RuntimeBuildCapabilities(
                    false,
                    $"Runtime source is missing: {gameSourcePath}");
            }
            if (info.Length <= 0L ||
                info.Length > MaximumRuntimeCapabilitySourceBytes)
            {
                return new RuntimeBuildCapabilities(
                    false,
                    $"Runtime source size is outside the capability-scan limit: {gameSourcePath}");
            }

            string source;
            using (var stream = new FileStream(
                       gameSourcePath,
                       FileMode.Open,
                       FileAccess.Read,
                       FileShare.ReadWrite | FileShare.Delete))
            using (var reader = new StreamReader(
                       stream,
                       new UTF8Encoding(
                           encoderShouldEmitUTF8Identifier: false,
                           throwOnInvalidBytes: true),
                       detectEncodingFromByteOrderMarks: true))
            {
                source = reader.ReadToEnd();
            }

            bool hasAdapter =
                source.Contains("FLegacyScene3DAdapter", StringComparison.Ordinal);
            bool hasBootstrapKind =
                source.Contains(
                    "EBootstrapSceneKind::Legacy3D",
                    StringComparison.Ordinal);
            bool hasSceneRoute =
                source.Contains("FMainScene3D", StringComparison.Ordinal);
            bool hasCanonicalLoader =
                source.Contains("LoadFile(\"main.acscene\")", StringComparison.Ordinal) ||
                source.Contains(
                    "LoadAssetPack(m_Pack, \"main.acscene\")",
                    StringComparison.Ordinal);
            bool hasExplicitMarker =
                source.Contains(
                    LegacyScene3DCapabilityMarker,
                    StringComparison.Ordinal);

            // Existing projects generated immediately before the marker was
            // introduced remain compatible through the full structural route.
            // A marker is only an opt-in hint; it never bypasses adapter/bootstrap
            // checks by itself.
            bool structuralRoute =
                hasAdapter && hasBootstrapKind && hasSceneRoute &&
                hasCanonicalLoader;
            bool markedRoute =
                hasExplicitMarker && hasAdapter && hasBootstrapKind &&
                hasSceneRoute;
            if (structuralRoute || markedRoute)
            {
                return new RuntimeBuildCapabilities(
                    true,
                    hasExplicitMarker
                        ? $"Verified legacy-3D runtime capability marker and bootstrap in {gameSourcePath}"
                        : $"Verified legacy-3D adapter/bootstrap route in {gameSourcePath}");
            }

            return new RuntimeBuildCapabilities(
                false,
                $"Source/Game.cpp has no verified FLegacyScene3DAdapter bootstrap route and was left unchanged: {gameSourcePath}");
        }
        catch (Exception ex) when (
            ex is IOException or UnauthorizedAccessException or
            DecoderFallbackException or ArgumentException or
            NotSupportedException)
        {
            return new RuntimeBuildCapabilities(
                false,
                $"Runtime capability scan failed closed and did not modify Source/Game.cpp: {ex.Message}");
        }
    }
}
