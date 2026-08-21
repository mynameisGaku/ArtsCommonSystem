// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Runtime.InteropServices;

namespace AcsEditor;

internal static class EditorAbiContractSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int failures = 0;

        void Check(bool condition, string description)
        {
            if (condition)
            {
                output.WriteLine($"[PASS] {description}");
                return;
            }
            output.WriteLine($"[FAIL] {description}");
            failures++;
        }

        Check(
            sizeof(int) == 4 &&
            sizeof(uint) == 4 &&
            sizeof(ulong) == 8,
            "managed P/Invoke scalar widths match the native ABI");

        EditorAbiCapability complete =
            EditorAbiContract.RequiredCapabilities |
            EditorAbiCapability.ProfilerV4 |
            EditorAbiCapability.ProfilerV5 |
            EditorAbiCapability.UnifiedSceneDocument |
            EditorAbiCapability.VolumetricCloudWorkloadV1 |
            EditorAbiCapability.CameraAuthoringV1 |
            EditorAbiCapability.CameraViewRequestsV1 |
            EditorAbiCapability.OptionalServiceDiagnosticsV2;
        EditorAbiSnapshot compatible = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits: (ulong)complete,
            productVersion: "ACS Editor test",
            renderBackend: "Test RHI");
        Check(compatible.Compatible && compatible.MissingRequired == 0,
            "current provider with every required capability is compatible");
        Check(compatible.ToDisplayText().Contains(
                  "profiler-v4",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "profiler-v5",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "cloud-workload-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "camera-authoring-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "camera-view-requests-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "optional-service-diagnostics-v2",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "sparse-transform-mutation-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-instance-refresh-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-stable-instance-id-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-root-property-override-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-root-property-selective-revert-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-root-component-property-override-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-root-component-property-selective-revert-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-root-component-property-selective-apply-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-source-node-identity-3d-v1",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "prefab-node-property-override-3d-v1",
                  StringComparison.Ordinal) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.SparseTransformMutationV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabInstanceRefresh3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabStableInstanceId3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabRootPropertyOverride3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabRootPropertySelectiveRevert3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabRootComponentPropertyOverride3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabRootComponentPropertySelectiveRevert3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabRootComponentPropertySelectiveApply3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabSourceNodeIdentity3DV1) &&
              EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.PrefabNodePropertyOverride3DV1) &&
              !EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.VolumetricCloudWorkloadV1) &&
              !EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.CameraAuthoringV1) &&
              !EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.CameraViewRequestsV1) &&
              !EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.OptionalServiceDiagnosticsV2) &&
              !compatible.Capabilities.HasFlag(
                  EditorAbiCapability.ProfilerV3),
            "diagnostics distinguish required editor mutation contracts from optional services");

        Check(
            Marshal.SizeOf<EditorOptionalServiceDiagnosticNative>() ==
                EditorOptionalServiceDiagnosticContract.CurrentSize &&
            Marshal.OffsetOf<EditorOptionalServiceDiagnosticNative>(
                nameof(EditorOptionalServiceDiagnosticNative.HostGeneration)).
                ToInt32() == 24 &&
            Marshal.OffsetOf<EditorOptionalServiceDiagnosticNative>(
                nameof(EditorOptionalServiceDiagnosticNative.MessageUtf8)).
                ToInt32() == 32 &&
            Marshal.OffsetOf<EditorOptionalServiceDiagnosticNative>(
                nameof(EditorOptionalServiceDiagnosticNative.ErrorDomain)).
                ToInt32() ==
                EditorOptionalServiceDiagnosticContract.LegacySize &&
            Marshal.OffsetOf<EditorOptionalServiceDiagnosticNative>(
                nameof(EditorOptionalServiceDiagnosticNative.DiagnosticGeneration)).
                ToInt32() == 200 &&
            Marshal.OffsetOf<EditorOptionalServiceDiagnosticNative>(
                nameof(EditorOptionalServiceDiagnosticNative.StableCodeUtf8)).
                ToInt32() == 208 &&
            EditorOptionalServiceDiagnosticContract.LegacySize == 192 &&
            EditorOptionalServiceDiagnosticContract.CurrentSize == 256,
            "optional-service diagnostic preserves its 192-byte prefix and 256-byte typed payload");

        EditorOptionalServiceDiagnosticNative currentDiagnostic =
            EditorOptionalServiceDiagnosticNative.CreateForTest(
                EditorOptionalServiceDiagnosticContract.CurrentVersion,
                EditorOptionalServiceDiagnosticContract.CurrentSize,
                EditorOptionalService.VolumetricCloudWorkload,
                EditorOptionalServiceState.Pending,
                EditorOptionalServiceReason.StartupPending,
                EditorOptionalServiceFlags.Retryable,
                hostGeneration: 42,
                message: "雲の初期化を待機中",
                EditorNativeErrorDomain.Renderer,
                errorCode: 1003,
                diagnosticGeneration: 7,
                stableCode: "ACS.SERVICE.CLOUD.STARTUP_PENDING");
        bool decodedCurrent =
            EditorOptionalServiceDiagnosticContract.TryDecode(
                in currentDiagnostic,
                EditorOptionalService.VolumetricCloudWorkload,
                expectedHostGeneration: 42,
                out EditorOptionalServiceDiagnostic currentResult,
                out string currentFailure);
        Check(
            decodedCurrent &&
            currentFailure.Length == 0 &&
            currentResult.State ==
                EditorOptionalServiceState.Pending &&
            currentResult.Reason ==
                EditorOptionalServiceReason.StartupPending &&
            currentResult.IsRetryable &&
            !currentResult.IsCallable &&
            currentResult.Message == "雲の初期化を待機中" &&
            currentResult.ErrorDomain ==
                EditorNativeErrorDomain.Renderer &&
            currentResult.ErrorCode == 1003 &&
            currentResult.DiagnosticGeneration == 7 &&
            currentResult.StableCode ==
                "ACS.SERVICE.CLOUD.STARTUP_PENDING",
            "typed optional-service payload decodes bounded UTF-8 and exact reason codes");

        string longUtf8Message = new('\u96F2', 100);
        EditorOptionalServiceDiagnosticNative boundedUtf8 =
            EditorOptionalServiceDiagnosticNative.CreateForTest(
                EditorOptionalServiceDiagnosticContract.CurrentVersion,
                EditorOptionalServiceDiagnosticContract.CurrentSize,
                EditorOptionalService.VolumetricCloudWorkload,
                EditorOptionalServiceState.Pending,
                EditorOptionalServiceReason.StartupPending,
                EditorOptionalServiceFlags.Retryable,
                hostGeneration: 42,
                message: longUtf8Message,
                EditorNativeErrorDomain.Renderer,
                errorCode: 1003,
                diagnosticGeneration: 8,
                stableCode: "ACS.SERVICE.CLOUD.STARTUP_PENDING");
        Check(
            EditorOptionalServiceDiagnosticContract.TryDecode(
                in boundedUtf8,
                EditorOptionalService.VolumetricCloudWorkload,
                expectedHostGeneration: 42,
                out EditorOptionalServiceDiagnostic boundedResult,
                out _) &&
            boundedResult.Message.Length > 0 &&
            boundedResult.Message.Length < longUtf8Message.Length &&
            !boundedResult.Message.Contains(
                "\uFFFD",
                StringComparison.Ordinal),
            "bounded native UTF-8 writes truncate only at a complete code point");

        bool acceptedLateResult =
            EditorOptionalServiceDiagnosticContract.TryDecode(
                in currentDiagnostic,
                EditorOptionalService.VolumetricCloudWorkload,
                expectedHostGeneration: 43,
                out _,
                out string lateFailure);
        Check(
            !acceptedLateResult &&
            lateFailure.Contains(
                "Discarded late",
                StringComparison.Ordinal),
            "host generation rejects a late result from a destroyed editor host");

        Check(
            !EditorOptionalServiceDiagnosticContract.TryDecode(
                in currentDiagnostic,
                EditorOptionalService.Profiler,
                expectedHostGeneration: 42,
                out _,
                out string serviceMismatchFailure) &&
            serviceMismatchFailure.Contains(
                "requested service",
                StringComparison.Ordinal),
            "a provider cannot substitute a different optional service payload");

        EditorOptionalServiceDiagnosticNative inconsistentDiagnostic =
            currentDiagnostic;
        inconsistentDiagnostic.Flags |=
            (uint)EditorOptionalServiceFlags.Callable;
        Check(
            !EditorOptionalServiceDiagnosticContract.TryDecode(
                in inconsistentDiagnostic,
                EditorOptionalService.VolumetricCloudWorkload,
                expectedHostGeneration: 42,
                out _,
                out string inconsistentStateFailure) &&
            inconsistentStateFailure.Contains(
                "inconsistent",
                StringComparison.Ordinal),
            "state, reason, and callable flags fail closed when contradictory");

        EditorOptionalServiceDiagnosticNative wrongTypedCode =
            currentDiagnostic;
        wrongTypedCode.ErrorCode = 1004;
        Check(
            !EditorOptionalServiceDiagnosticContract.TryDecode(
                in wrongTypedCode,
                EditorOptionalService.VolumetricCloudWorkload,
                expectedHostGeneration: 42,
                out _,
                out string inconsistentCodeFailure) &&
            inconsistentCodeFailure.Contains(
                "inconsistent",
                StringComparison.Ordinal),
            "typed error domain and code must exactly match the reason");

        EditorOptionalServiceDiagnosticNative legacyDiagnostic =
            EditorOptionalServiceDiagnosticNative.CreateForTest(
                EditorOptionalServiceDiagnosticContract.LegacyVersion,
                EditorOptionalServiceDiagnosticContract.LegacySize,
                EditorOptionalService.Profiler,
                EditorOptionalServiceState.Enabled,
                EditorOptionalServiceReason.None,
                EditorOptionalServiceFlags.Callable,
                hostGeneration: 42,
                message: "Profiler snapshots are available.",
                EditorNativeErrorDomain.Renderer,
                errorCode: 9999,
                diagnosticGeneration: 999,
                stableCode: "IGNORED.V2.TAIL");
        Check(
            EditorOptionalServiceDiagnosticContract.TryDecode(
                in legacyDiagnostic,
                EditorOptionalService.Profiler,
                expectedHostGeneration: 42,
                out EditorOptionalServiceDiagnostic legacyResult,
                out _) &&
            legacyResult.IsCallable &&
            legacyResult.ErrorDomain ==
                EditorNativeErrorDomain.None &&
            legacyResult.ErrorCode == 0 &&
            legacyResult.DiagnosticGeneration == 0 &&
            legacyResult.StableCode.Length == 0,
            "legacy version decodes only the complete 192-byte readable prefix");

        EditorOptionalServiceDiagnosticNative malformedDiagnostic =
            currentDiagnostic;
        malformedDiagnostic.Version = 99;
        Check(
            !EditorOptionalServiceDiagnosticContract.TryDecode(
                in malformedDiagnostic,
                EditorOptionalService.VolumetricCloudWorkload,
                expectedHostGeneration: 42,
                out _,
                out string malformedFailure) &&
            malformedFailure.Contains(
                "Unsupported",
                StringComparison.Ordinal),
            "unknown optional-service payload versions fail closed");

        unsafe
        {
            EditorOptionalServiceDiagnosticNative invalidUtf8 =
                currentDiagnostic;
            byte* messageBytes = invalidUtf8.MessageUtf8;
            messageBytes[0] = 0xC3;
            messageBytes[1] = 0x28;
            messageBytes[2] = 0;
            Check(
                !EditorOptionalServiceDiagnosticContract.TryDecode(
                    in invalidUtf8,
                    EditorOptionalService.VolumetricCloudWorkload,
                    expectedHostGeneration: 42,
                    out _,
                    out string utf8Failure) &&
                utf8Failure.Contains(
                    "invalid UTF-8",
                    StringComparison.Ordinal),
                "malformed native UTF-8 fails closed instead of reaching editor UI");

            EditorOptionalServiceDiagnosticNative unterminatedStableCode =
                currentDiagnostic;
            byte* stableCodeBytes =
                unterminatedStableCode.StableCodeUtf8;
            for (int index = 0;
                 index <
                    EditorOptionalServiceDiagnosticContract.StableCodeBytes;
                 ++index)
            {
                stableCodeBytes[index] = (byte)'A';
            }
            Check(
                !EditorOptionalServiceDiagnosticContract.TryDecode(
                    in unterminatedStableCode,
                    EditorOptionalService.VolumetricCloudWorkload,
                    expectedHostGeneration: 42,
                    out _,
                    out string unterminatedFailure) &&
                unterminatedFailure.Contains(
                    "not NUL terminated",
                    StringComparison.Ordinal),
                "typed stable codes require an in-bounds NUL terminator");
        }

        EditorAbiSnapshot missing = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 0,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits:
                (ulong)(complete &
                    ~EditorAbiCapability.ResizeResultContract),
            productVersion: "ACS Editor test",
            renderBackend: "Test RHI");
        Check(!missing.Compatible &&
              missing.MissingRequired.HasFlag(
                  EditorAbiCapability.ResizeResultContract),
            "missing required capability fails closed with an exact reason");

        EditorAbiSnapshot missingSparseMutation =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.SparseTransformMutationV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingSparseMutation.Compatible &&
            missingSparseMutation.MissingRequired.HasFlag(
                EditorAbiCapability.SparseTransformMutationV1),
            "provider without sparse transform mutation fails before the editor can call its export");

        EditorAbiSnapshot missingPrefabRefresh =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabInstanceRefresh3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabRefresh.Compatible &&
            missingPrefabRefresh.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabInstanceRefresh3DV1),
            "provider without transactional 3D Prefab refresh fails before the editor can call its export");

        EditorAbiSnapshot missingPrefabIdentity =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabStableInstanceId3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabIdentity.Compatible &&
            missingPrefabIdentity.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabStableInstanceId3DV1),
            "provider without stable 3D Prefab identity fails before the editor can call its exports");

        EditorAbiSnapshot missingPrefabRootOverrides =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabRootPropertyOverride3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabRootOverrides.Compatible &&
            missingPrefabRootOverrides.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabRootPropertyOverride3DV1),
            "provider without 3D Prefab root property overrides fails before the editor can call its exports");

        EditorAbiSnapshot missingPrefabSelectiveRevert =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabRootPropertySelectiveRevert3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabSelectiveRevert.Compatible &&
            missingPrefabSelectiveRevert.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabRootPropertySelectiveRevert3DV1),
            "provider without selective 3D Prefab root Revert fails before the editor can call its export");

        EditorAbiSnapshot missingPrefabComponentOverrides =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabRootComponentPropertyOverride3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabComponentOverrides.Compatible &&
            missingPrefabComponentOverrides.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabRootComponentPropertyOverride3DV1),
            "provider without 3D Prefab root component property overrides fails before the editor can call its exports");

        EditorAbiSnapshot missingPrefabComponentSelectiveRevert =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabRootComponentPropertySelectiveRevert3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabComponentSelectiveRevert.Compatible &&
            missingPrefabComponentSelectiveRevert.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabRootComponentPropertySelectiveRevert3DV1),
            "provider without selective 3D Prefab root component Revert fails before the editor can call its export");

        EditorAbiSnapshot missingPrefabComponentSelectiveApply =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabRootComponentPropertySelectiveApply3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabComponentSelectiveApply.Compatible &&
            missingPrefabComponentSelectiveApply.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabRootComponentPropertySelectiveApply3DV1),
            "provider without selective 3D Prefab root component Apply fails before the editor can call its export");

        EditorAbiSnapshot missingPrefabSourceNodeIdentity =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabSourceNodeIdentity3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabSourceNodeIdentity.Compatible &&
            missingPrefabSourceNodeIdentity.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabSourceNodeIdentity3DV1),
            "provider without stable 3D Prefab source node identity fails before the editor can call its exports");

        EditorAbiSnapshot missingPrefabNodePropertyOverride =
            EditorAbiContract.Evaluate(
                queryAvailable: true,
                queryResult: 0,
                providerVersion: EditorAbiContract.RequestedVersion,
                capabilityBits:
                    (ulong)(complete &
                        ~EditorAbiCapability.PrefabNodePropertyOverride3DV1),
                productVersion: "ACS Editor test",
                renderBackend: "Test RHI");
        Check(
            !missingPrefabNodePropertyOverride.Compatible &&
            missingPrefabNodePropertyOverride.MissingRequired.HasFlag(
                EditorAbiCapability.PrefabNodePropertyOverride3DV1),
            "provider without 3D Prefab child node overrides fails before the editor can call its exports");

        EditorAbiSnapshot falseSuccess = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits:
                (ulong)(complete &
                    ~EditorAbiCapability.FrameResultContract),
            productVersion: "false-success",
            renderBackend: "test");
        Check(!falseSuccess.Compatible &&
              falseSuccess.MissingRequired.HasFlag(
                  EditorAbiCapability.FrameResultContract),
            "provider success cannot mask a missing required capability");

        EditorAbiSnapshot legacy = EditorAbiContract.Evaluate(
            queryAvailable: false,
            queryResult: 0,
            providerVersion: 0,
            capabilityBits: 0,
            productVersion: null,
            renderBackend: null);
        Check(!legacy.Compatible &&
              legacy.Diagnostic.Contains(
                  "does not expose",
                  StringComparison.Ordinal),
            "legacy DLL without the query export is reported, not guessed");

        EditorAbiSnapshot disguisedLegacy = EditorAbiContract.Evaluate(
            queryAvailable: false,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits: (ulong)complete,
            productVersion: "legacy-with-current-label",
            renderBackend: "test");
        Check(!disguisedLegacy.Compatible,
            "matching metadata cannot mask a missing legacy query export");

        EditorAbiSnapshot future = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion + 1,
            capabilityBits: (ulong)complete | (1UL << 63),
            productVersion: "future",
            renderBackend: "future");
        Check(future.Compatible &&
              future.UnknownCapabilityBits == (1UL << 63) &&
              future.ToDisplayText().Contains(
                  "0x8000000000000000",
                  StringComparison.Ordinal),
            "additive provider versions and unknown capability bits are preserved");

        EditorAbiSnapshot rejected = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 0,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits: (ulong)complete,
            productVersion: "rejected",
            renderBackend: "test");
        Check(!rejected.Compatible &&
              rejected.Diagnostic.Contains(
                  "rejected",
                  StringComparison.Ordinal),
            "provider rejection cannot be masked by matching local data");

        output.WriteLine(
            failures == 0
                ? "Editor ABI contract self-test PASS."
                : $"Editor ABI contract self-test FAIL ({failures}).");
        return failures;
    }
}
