// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

internal static class EditorOptionalServiceUiSelfTest
{
    internal static int Run(TextWriter output)
    {
        int failures = 0;
        void Check(bool condition, string description)
        {
            output.WriteLine(
                $"{(condition ? "PASS" : "FAIL")}: {description}");
            if (!condition)
                failures++;
        }

        const EditorAbiCapability diagnostics =
            EditorAbiCapability.OptionalServiceDiagnosticsV2;
        var session = new EditorOptionalServiceUiSession();
        ulong expectedGenerationSeen = ulong.MaxValue;
        ulong diagnosticGeneration = 10;
        ulong hostGeneration = 41;
        EditorOptionalServiceDiagnostic next = Enabled(
            EditorOptionalService.Profiler,
            hostGeneration,
            diagnosticGeneration);
        bool querySucceeds = true;
        string queryFailure = "";

        bool Query(
            IntPtr handle,
            EditorOptionalService service,
            ulong expectedHostGeneration,
            out EditorOptionalServiceDiagnostic diagnostic,
            out string failure)
        {
            _ = handle;
            expectedGenerationSeen = expectedHostGeneration;
            diagnostic = next with { Service = service };
            failure = queryFailure;
            return querySucceeds;
        }

        EditorOptionalServiceUiState legacy = session.Evaluate(
            new IntPtr(1),
            1,
            EditorAbiCapability.None,
            EditorOptionalService.Profiler,
            Query,
            static () => true);
        Check(
            legacy.Source ==
                EditorOptionalServiceUiSource.LegacyCompatibility &&
            legacy.CanInvoke &&
            !legacy.UsesExactNativeDiagnostic,
            "missing diagnostics capability preserves legacy service behavior");

        EditorOptionalServiceUiState hostPending = session.Evaluate(
            IntPtr.Zero,
            2,
            diagnostics,
            EditorOptionalService.Profiler,
            Query,
            static () => true);
        Check(
            hostPending.Source ==
                EditorOptionalServiceUiSource.HostPending &&
            !hostPending.CanInvoke &&
            hostPending.IsRetryable,
            "an unpublished host disables only the affected action and remains retryable");

        EditorOptionalServiceUiState enabled = session.Evaluate(
            new IntPtr(2),
            3,
            diagnostics,
            EditorOptionalService.Profiler,
            Query,
            static () => true);
        Check(
            enabled.UsesExactNativeDiagnostic &&
            enabled.CanInvoke &&
            enabled.HostGeneration == 41 &&
            enabled.DiagnosticGeneration == 10 &&
            enabled.ToolTip.Contains(
                "ACS.SERVICE.PROFILER.ENABLED",
                StringComparison.Ordinal) &&
            expectedGenerationSeen == 0,
            "first exact result publishes typed reason and native host identity");

        next = Pending(
            EditorOptionalService.VolumetricCloudWorkload,
            hostGeneration,
            ++diagnosticGeneration);
        EditorOptionalServiceUiState pending = session.Evaluate(
            new IntPtr(2),
            3,
            diagnostics,
            EditorOptionalService.VolumetricCloudWorkload,
            Query,
            static () => true);
        Check(
            !pending.CanInvoke &&
            pending.IsRetryable &&
            pending.StatusText.Contains(
                "waiting for incremental renderer startup",
                StringComparison.Ordinal) &&
            expectedGenerationSeen == hostGeneration,
            "pending cloud state exposes the exact native reason and expected host generation");

        EditorOptionalServiceActionPolicy.ProfilerControlPlan
            pendingProfilerPlan =
                EditorOptionalServiceActionPolicy.PlanProfilerControls(
                    pending with
                    {
                        Service = EditorOptionalService.Profiler,
                    },
                    pending);
        Check(
            !pendingProfilerPlan.ResetEnabled &&
            pendingProfilerPlan.PauseEnabled &&
            pendingProfilerPlan.ExportRetainedHistoryEnabled &&
            pendingProfilerPlan.CloudFilterEnabled,
            "pending services disable native Reset only, preserving local Pause, export, and Cloud visibility");

        next = Inactive(
            EditorOptionalService.VolumetricCloudWorkload,
            hostGeneration,
            ++diagnosticGeneration);
        EditorOptionalServiceUiState inactive = session.Evaluate(
            new IntPtr(2),
            3,
            diagnostics,
            EditorOptionalService.VolumetricCloudWorkload,
            Query,
            static () => true);
        Check(
            inactive.CanInvoke &&
            inactive.IsRetryable &&
            inactive.State == EditorOptionalServiceState.Inactive,
            "inactive cloud workload stays callable while reporting scene inactivity");

        next = CapabilityMissing(
            EditorOptionalService.CameraViewRequests,
            hostGeneration,
            ++diagnosticGeneration);
        EditorOptionalServiceUiState cameraCapabilityMissing =
            session.Evaluate(
                new IntPtr(2),
                3,
                diagnostics,
                EditorOptionalService.CameraViewRequests,
                Query,
                static () => true);
        Check(
            !cameraCapabilityMissing.CanInvoke &&
            EditorOptionalServiceActionPolicy.CanOpenCameraPreview(
                cameraCapabilityMissing) &&
            !EditorOptionalServiceActionPolicy.CanMutateCameraRequests(
                cameraCapabilityMissing,
                usesRequestContract: false),
            "camera capability loss retains one legacy preview but disables request-only controls");

        EditorOptionalServiceActionPolicy.CameraControlPlan
            cameraControlPlan =
                EditorOptionalServiceActionPolicy.PlanCameraControls(
                    cameraCapabilityMissing,
                    usesRequestContract: false);
        Check(
            cameraControlPlan.OpenPreviewEnabled &&
            !cameraControlPlan.RequestMutationEnabled &&
            cameraControlPlan.CloseEnabled &&
            cameraControlPlan.RedockEnabled,
            "camera diagnostics never block cleanup or return of the shared renderer surface");

        next = Enabled(
            EditorOptionalService.Profiler,
            hostGeneration,
            9);
        EditorOptionalServiceUiState regressed = session.Evaluate(
            new IntPtr(2),
            3,
            diagnostics,
            EditorOptionalService.Profiler,
            Query,
            static () => true);
        Check(
            regressed.Source ==
                EditorOptionalServiceUiSource.StaleResult &&
            !regressed.CanInvoke,
            "a regressed per-service diagnostic generation is rejected");

        next = Enabled(
            EditorOptionalService.Profiler,
            hostGeneration,
            10);
        EditorOptionalServiceUiState replayed = session.Evaluate(
            new IntPtr(2),
            3,
            diagnostics,
            EditorOptionalService.Profiler,
            Query,
            static () => true);
        Check(
            replayed.Source ==
                EditorOptionalServiceUiSource.StaleResult &&
            !replayed.CanInvoke,
            "a replayed per-service diagnostic generation is rejected");

        next = Enabled(
            EditorOptionalService.Profiler,
            hostGeneration,
            ++diagnosticGeneration);
        EditorOptionalServiceUiState identityChanged = session.Evaluate(
            new IntPtr(2),
            3,
            diagnostics,
            EditorOptionalService.Profiler,
            Query,
            static () => false);
        Check(
            identityChanged.Source ==
                EditorOptionalServiceUiSource.StaleResult &&
            !identityChanged.CanInvoke,
            "a result racing a managed host replacement is never applied");

        hostGeneration = 77;
        diagnosticGeneration = 1;
        next = Enabled(
            EditorOptionalService.Profiler,
            hostGeneration,
            diagnosticGeneration);
        EditorOptionalServiceUiState replacedHost = session.Evaluate(
            new IntPtr(3),
            4,
            diagnostics,
            EditorOptionalService.Profiler,
            Query,
            static () => true);
        Check(
            replacedHost.CanInvoke &&
            replacedHost.HostGeneration == 77 &&
            expectedGenerationSeen == 0,
            "a new managed host resets native and diagnostic generation expectations");

        querySucceeds = false;
        queryFailure = "bounded diagnostic decode failed";
        EditorOptionalServiceUiState contractFailure = session.Evaluate(
            new IntPtr(3),
            4,
            diagnostics,
            EditorOptionalService.CameraViewRequests,
            Query,
            static () => true);
        Check(
            contractFailure.Source ==
                EditorOptionalServiceUiSource.ContractFailure &&
            !contractFailure.CanInvoke &&
            contractFailure.IsRetryable &&
            contractFailure.Message == queryFailure &&
            contractFailure.ShouldPresentUnavailableStatus &&
            contractFailure.ToolTip.Contains(
                "ACS.EDITOR.SERVICE.CONTRACT_FAILURE",
                StringComparison.Ordinal) &&
            contractFailure.ToolTip.Contains(
                "EditorAbi/-1",
                StringComparison.Ordinal) &&
            contractFailure.ToolTip.Contains(
                "Host generation: unavailable",
                StringComparison.Ordinal),
            "an advertised but malformed diagnostic fails closed with a structured synthetic reason");
        EditorOptionalServiceActionPolicy.CameraControlPlan
            failedCameraPlan =
                EditorOptionalServiceActionPolicy.PlanCameraControls(
                    contractFailure,
                    usesRequestContract: true);
        Check(
            !failedCameraPlan.OpenPreviewEnabled &&
            !failedCameraPlan.RequestMutationEnabled &&
            failedCameraPlan.CloseEnabled &&
            failedCameraPlan.RedockEnabled,
            "camera contract failure blocks new work but never strands the live surface");

        querySucceeds = true;
        EditorOptionalServiceUiState thrownQuery = session.Evaluate(
            new IntPtr(3),
            4,
            diagnostics,
            EditorOptionalService.VolumetricCloudWorkload,
            static (
                IntPtr _,
                EditorOptionalService _,
                ulong _,
                out EditorOptionalServiceDiagnostic diagnostic,
                out string failure) =>
            {
                diagnostic = default;
                failure = "";
                throw new InvalidOperationException(
                    "synthetic query failure");
            },
            static () => true);
        Check(
            thrownQuery.Source ==
                EditorOptionalServiceUiSource.ContractFailure &&
            !thrownQuery.CanInvoke &&
            thrownQuery.Message.Contains(
                nameof(InvalidOperationException),
                StringComparison.Ordinal),
            "an unexpected query exception becomes a service-local contract failure");

        output.WriteLine(
            failures == 0
                ? "Optional service UI self-test passed."
                : $"Optional service UI self-test failed: {failures}");
        return failures;
    }

    private static EditorOptionalServiceDiagnostic Enabled(
        EditorOptionalService service,
        ulong hostGeneration,
        ulong diagnosticGeneration) =>
        new(
            service,
            EditorOptionalServiceState.Enabled,
            EditorOptionalServiceReason.None,
            EditorOptionalServiceFlags.Callable,
            hostGeneration,
            "Profiler snapshots are available.",
            EditorNativeErrorDomain.None,
            0,
            diagnosticGeneration,
            service == EditorOptionalService.Profiler
                ? "ACS.SERVICE.PROFILER.ENABLED"
                : "ACS.SERVICE.ENABLED");

    private static EditorOptionalServiceDiagnostic Pending(
        EditorOptionalService service,
        ulong hostGeneration,
        ulong diagnosticGeneration) =>
        new(
            service,
            EditorOptionalServiceState.Pending,
            EditorOptionalServiceReason.StartupPending,
            EditorOptionalServiceFlags.Retryable,
            hostGeneration,
            "Cloud diagnostics are waiting for incremental renderer startup.",
            EditorNativeErrorDomain.Renderer,
            1003,
            diagnosticGeneration,
            "ACS.SERVICE.CLOUD.STARTUP_PENDING");

    private static EditorOptionalServiceDiagnostic Inactive(
        EditorOptionalService service,
        ulong hostGeneration,
        ulong diagnosticGeneration) =>
        new(
            service,
            EditorOptionalServiceState.Inactive,
            EditorOptionalServiceReason.SceneFeatureInactive,
            EditorOptionalServiceFlags.Callable |
                EditorOptionalServiceFlags.Retryable,
            hostGeneration,
            "Cloud diagnostics are callable, but no cloud workload is active.",
            EditorNativeErrorDomain.Renderer,
            1004,
            diagnosticGeneration,
            "ACS.SERVICE.CLOUD.INACTIVE");

    private static EditorOptionalServiceDiagnostic CapabilityMissing(
        EditorOptionalService service,
        ulong hostGeneration,
        ulong diagnosticGeneration) =>
        new(
            service,
            EditorOptionalServiceState.Disabled,
            EditorOptionalServiceReason.CapabilityNotAdvertised,
            EditorOptionalServiceFlags.None,
            hostGeneration,
            "The native provider did not advertise this optional service.",
            EditorNativeErrorDomain.EditorAbi,
            1001,
            diagnosticGeneration,
            "ACS.SERVICE.CAPABILITY_MISSING");
}
