// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AcsEditor;

[Flags]
internal enum EditorAbiCapability : ulong
{
    None                   = 0,
    FrameResultContract    = 1UL << 0,
    IncrementalStartup     = 1UL << 1,
    ProfilerV3             = 1UL << 2,
    UnifiedSceneDocument   = 1UL << 3,
    MaterialPreviewQuality = 1UL << 4,
    SubstrateGraph         = 1UL << 5,
    InteractiveWater3D     = 1UL << 6,
    ResizeResultContract   = 1UL << 7,
    VolumetricCloudWorkloadV1 = 1UL << 8,
    ProfilerV4             = 1UL << 9,
    CameraAuthoringV1      = 1UL << 10,
    CameraViewRequestsV1   = 1UL << 11,
    ProfilerV5             = 1UL << 12,
    OptionalServiceDiagnosticsV2 = 1UL << 13,
    SparseTransformMutationV1 = 1UL << 14,
    PrefabInstanceRefresh3DV1 = 1UL << 15,
    PrefabStableInstanceId3DV1 = 1UL << 16,
    PrefabRootPropertyOverride3DV1 = 1UL << 17,
    PrefabRootPropertySelectiveRevert3DV1 = 1UL << 18,
    PrefabRootComponentPropertyOverride3DV1 = 1UL << 19,
    PrefabRootComponentPropertySelectiveRevert3DV1 = 1UL << 20,
    PrefabRootComponentPropertySelectiveApply3DV1 = 1UL << 21,
}

internal readonly record struct EditorAbiSnapshot(
    bool QueryAvailable,
    bool Compatible,
    uint ProviderVersion,
    EditorAbiCapability Capabilities,
    EditorAbiCapability MissingRequired,
    ulong UnknownCapabilityBits,
    string ProductVersion,
    string RenderBackend,
    string Diagnostic)
{
    internal string ToDisplayText()
    {
        EditorAbiCapability currentCapabilities = Capabilities;
        EditorAbiCapability currentMissingRequired = MissingRequired;
        string compatibility = Compatible ? "Compatible" : "INCOMPATIBLE";
        string capabilities = string.Join(
            ", ",
            EditorAbiContract.KnownCapabilities
                .Where(capability =>
                    currentCapabilities.HasFlag(capability))
                .Select(EditorAbiContract.DisplayName));
        if (capabilities.Length == 0)
            capabilities = "(none)";

        string text =
            $"{ProductVersion}\n" +
            $"Render backend: {RenderBackend}\n" +
            $"Editor ABI: v{ProviderVersion} ({compatibility})\n" +
            $"Capabilities: {capabilities}";
        if (currentMissingRequired != EditorAbiCapability.None)
        {
            text += "\nMissing required: " +
                    string.Join(
                        ", ",
                        EditorAbiContract.KnownCapabilities
                            .Where(capability =>
                                currentMissingRequired.HasFlag(capability))
                            .Select(EditorAbiContract.DisplayName));
        }
        if (UnknownCapabilityBits != 0UL)
        {
            text += "\nAdditional provider bits: 0x" +
                    UnknownCapabilityBits.ToString(
                        "X16",
                        CultureInfo.InvariantCulture);
        }
        if (!string.IsNullOrWhiteSpace(Diagnostic))
            text += "\nDiagnostic: " + Diagnostic;
        return text;
    }
}

internal static class EditorAbiContract
{
    internal const uint RequestedVersion = 1;

    internal const EditorAbiCapability RequiredCapabilities =
        EditorAbiCapability.FrameResultContract |
        EditorAbiCapability.IncrementalStartup |
        EditorAbiCapability.ResizeResultContract |
        EditorAbiCapability.SparseTransformMutationV1 |
        EditorAbiCapability.PrefabInstanceRefresh3DV1 |
        EditorAbiCapability.PrefabStableInstanceId3DV1 |
        EditorAbiCapability.PrefabRootPropertyOverride3DV1 |
        EditorAbiCapability.PrefabRootPropertySelectiveRevert3DV1 |
        EditorAbiCapability.PrefabRootComponentPropertyOverride3DV1 |
        EditorAbiCapability.PrefabRootComponentPropertySelectiveRevert3DV1 |
        EditorAbiCapability.PrefabRootComponentPropertySelectiveApply3DV1;

    internal static readonly IReadOnlyList<EditorAbiCapability>
        KnownCapabilities = Array.AsReadOnly(new[]
        {
            EditorAbiCapability.FrameResultContract,
            EditorAbiCapability.IncrementalStartup,
            EditorAbiCapability.ProfilerV3,
            EditorAbiCapability.ProfilerV4,
            EditorAbiCapability.ProfilerV5,
            EditorAbiCapability.UnifiedSceneDocument,
            EditorAbiCapability.MaterialPreviewQuality,
            EditorAbiCapability.SubstrateGraph,
            EditorAbiCapability.InteractiveWater3D,
            EditorAbiCapability.ResizeResultContract,
            EditorAbiCapability.VolumetricCloudWorkloadV1,
            EditorAbiCapability.CameraAuthoringV1,
            EditorAbiCapability.CameraViewRequestsV1,
            EditorAbiCapability.OptionalServiceDiagnosticsV2,
            EditorAbiCapability.SparseTransformMutationV1,
            EditorAbiCapability.PrefabInstanceRefresh3DV1,
            EditorAbiCapability.PrefabStableInstanceId3DV1,
            EditorAbiCapability.PrefabRootPropertyOverride3DV1,
            EditorAbiCapability.PrefabRootPropertySelectiveRevert3DV1,
            EditorAbiCapability.PrefabRootComponentPropertyOverride3DV1,
            EditorAbiCapability.PrefabRootComponentPropertySelectiveRevert3DV1,
            EditorAbiCapability.PrefabRootComponentPropertySelectiveApply3DV1,
        });

    private const EditorAbiCapability AllKnownCapabilities =
        EditorAbiCapability.FrameResultContract |
        EditorAbiCapability.IncrementalStartup |
        EditorAbiCapability.ProfilerV3 |
        EditorAbiCapability.ProfilerV4 |
        EditorAbiCapability.ProfilerV5 |
        EditorAbiCapability.UnifiedSceneDocument |
        EditorAbiCapability.MaterialPreviewQuality |
        EditorAbiCapability.SubstrateGraph |
        EditorAbiCapability.InteractiveWater3D |
        EditorAbiCapability.ResizeResultContract |
        EditorAbiCapability.VolumetricCloudWorkloadV1 |
        EditorAbiCapability.CameraAuthoringV1 |
        EditorAbiCapability.CameraViewRequestsV1 |
        EditorAbiCapability.OptionalServiceDiagnosticsV2 |
        EditorAbiCapability.SparseTransformMutationV1 |
        EditorAbiCapability.PrefabInstanceRefresh3DV1 |
        EditorAbiCapability.PrefabStableInstanceId3DV1 |
        EditorAbiCapability.PrefabRootPropertyOverride3DV1 |
        EditorAbiCapability.PrefabRootPropertySelectiveRevert3DV1 |
        EditorAbiCapability.PrefabRootComponentPropertyOverride3DV1 |
        EditorAbiCapability.PrefabRootComponentPropertySelectiveRevert3DV1 |
        EditorAbiCapability.PrefabRootComponentPropertySelectiveApply3DV1;

    internal static EditorAbiSnapshot Evaluate(
        bool queryAvailable,
        int queryResult,
        uint providerVersion,
        ulong capabilityBits,
        string? productVersion,
        string? renderBackend,
        string? diagnostic = null)
    {
        var capabilities = (EditorAbiCapability)capabilityBits;
        EditorAbiCapability missing =
            RequiredCapabilities & ~capabilities;
        bool versionCompatible =
            providerVersion >= RequestedVersion;
        bool compatible =
            queryAvailable &&
            queryResult == 1 &&
            versionCompatible &&
            missing == EditorAbiCapability.None;
        ulong unknown =
            capabilityBits & ~(ulong)AllKnownCapabilities;

        string normalizedDiagnostic = diagnostic?.Trim() ?? "";
        if (!compatible && normalizedDiagnostic.Length == 0)
        {
            normalizedDiagnostic = !queryAvailable
                ? "The loaded DLL does not expose capability negotiation."
                : !versionCompatible
                    ? $"Provider ABI v{providerVersion} is older than requested v{RequestedVersion}."
                    : missing != EditorAbiCapability.None
                        ? "The provider is missing capabilities required by this editor host."
                        : "The provider rejected the compatibility query.";
        }

        return new EditorAbiSnapshot(
            queryAvailable,
            compatible,
            providerVersion,
            capabilities,
            missing,
            unknown,
            string.IsNullOrWhiteSpace(productVersion)
                ? "(unknown product version)"
                : productVersion.Trim(),
            string.IsNullOrWhiteSpace(renderBackend)
                ? "(unknown backend)"
                : renderBackend.Trim(),
            normalizedDiagnostic);
    }

    internal static string DisplayName(EditorAbiCapability capability) =>
        capability switch
        {
            EditorAbiCapability.FrameResultContract =>
                "frame-result-v1",
            EditorAbiCapability.IncrementalStartup =>
                "incremental-startup",
            EditorAbiCapability.ProfilerV3 =>
                "profiler-v3",
            EditorAbiCapability.ProfilerV4 =>
                "profiler-v4",
            EditorAbiCapability.ProfilerV5 =>
                "profiler-v5",
            EditorAbiCapability.UnifiedSceneDocument =>
                "unified-scene-document",
            EditorAbiCapability.MaterialPreviewQuality =>
                "material-preview-quality",
            EditorAbiCapability.SubstrateGraph =>
                "substrate-graph",
            EditorAbiCapability.InteractiveWater3D =>
                "interactive-water-3d",
            EditorAbiCapability.ResizeResultContract =>
                "resize-result-v1",
            EditorAbiCapability.VolumetricCloudWorkloadV1 =>
                "cloud-workload-v1",
            EditorAbiCapability.CameraAuthoringV1 =>
                "camera-authoring-v1",
            EditorAbiCapability.CameraViewRequestsV1 =>
                "camera-view-requests-v1",
            EditorAbiCapability.OptionalServiceDiagnosticsV2 =>
                "optional-service-diagnostics-v2",
            EditorAbiCapability.SparseTransformMutationV1 =>
                "sparse-transform-mutation-v1",
            EditorAbiCapability.PrefabInstanceRefresh3DV1 =>
                "prefab-instance-refresh-3d-v1",
            EditorAbiCapability.PrefabStableInstanceId3DV1 =>
                "prefab-stable-instance-id-3d-v1",
            EditorAbiCapability.PrefabRootPropertyOverride3DV1 =>
                "prefab-root-property-override-3d-v1",
            EditorAbiCapability.PrefabRootPropertySelectiveRevert3DV1 =>
                "prefab-root-property-selective-revert-3d-v1",
            EditorAbiCapability.PrefabRootComponentPropertyOverride3DV1 =>
                "prefab-root-component-property-override-3d-v1",
            EditorAbiCapability.PrefabRootComponentPropertySelectiveRevert3DV1 =>
                "prefab-root-component-property-selective-revert-3d-v1",
            EditorAbiCapability.PrefabRootComponentPropertySelectiveApply3DV1 =>
                "prefab-root-component-property-selective-apply-3d-v1",
            _ => "unknown",
        };
}
