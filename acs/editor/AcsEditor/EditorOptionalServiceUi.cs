// SPDX-License-Identifier: Apache-2.0

using System;
using System.Globalization;

namespace AcsEditor;

internal enum EditorOptionalServiceUiSource
{
    LegacyCompatibility,
    NativeDiagnostic,
    HostPending,
    ContractFailure,
    StaleResult,
}

internal readonly record struct EditorOptionalServiceUiState(
    EditorOptionalService Service,
    EditorOptionalServiceUiSource Source,
    EditorOptionalServiceState State,
    EditorOptionalServiceReason Reason,
    bool CanInvoke,
    bool IsRetryable,
    string Message,
    string StableCode,
    EditorNativeErrorDomain ErrorDomain,
    int ErrorCode,
    ulong HostGeneration,
    ulong DiagnosticGeneration)
{
    internal bool UsesExactNativeDiagnostic =>
        Source == EditorOptionalServiceUiSource.NativeDiagnostic;

    internal bool IsCapabilityNotAdvertised =>
        UsesExactNativeDiagnostic &&
        Reason ==
            EditorOptionalServiceReason.CapabilityNotAdvertised;

    internal bool ShouldPresentUnavailableStatus =>
        Source != EditorOptionalServiceUiSource.LegacyCompatibility &&
        !CanInvoke &&
        !IsCapabilityNotAdvertised;

    internal string StatusText
    {
        get
        {
            string label = State switch
            {
                EditorOptionalServiceState.Enabled => "Ready",
                EditorOptionalServiceState.Disabled => "Unavailable",
                EditorOptionalServiceState.Pending => "Pending",
                EditorOptionalServiceState.Inactive => "Inactive",
                EditorOptionalServiceState.Failed => "Failed",
                _ => "Unavailable",
            };
            return $"{label} — {Message}";
        }
    }

    internal string ToolTip
    {
        get
        {
            if (Source ==
                EditorOptionalServiceUiSource.LegacyCompatibility)
            {
                return Message;
            }

            string typedError =
                ErrorDomain == EditorNativeErrorDomain.None
                    ? "none"
                    : $"{ErrorDomain}/{ErrorCode}";
            string hostGeneration =
                HostGeneration == 0
                    ? "unavailable"
                    : HostGeneration.ToString(
                        CultureInfo.InvariantCulture);
            string diagnosticGeneration =
                DiagnosticGeneration == 0
                    ? "unavailable"
                    : DiagnosticGeneration.ToString(
                        CultureInfo.InvariantCulture);
            return
                $"{Message}\n" +
                $"Code: {StableCode}\n" +
                $"Error: {typedError}\n" +
                $"Host generation: {hostGeneration}; " +
                $"diagnostic generation: {diagnosticGeneration}";
        }
    }

    internal static EditorOptionalServiceUiState Legacy(
        EditorOptionalService service,
        bool hostAvailable) =>
        new(
            service,
            hostAvailable
                ? EditorOptionalServiceUiSource.LegacyCompatibility
                : EditorOptionalServiceUiSource.HostPending,
            hostAvailable
                ? EditorOptionalServiceState.Enabled
                : EditorOptionalServiceState.Pending,
            hostAvailable
                ? EditorOptionalServiceReason.None
                : EditorOptionalServiceReason.StartupPending,
            CanInvoke: hostAvailable,
            IsRetryable: !hostAvailable,
            hostAvailable
                ? "Exact optional-service diagnostics are not advertised; " +
                  "the editor is preserving the legacy service behavior."
                : "The native editor host has not been published yet.",
            hostAvailable
                ? "ACS.EDITOR.SERVICE.LEGACY_COMPATIBILITY"
                : "ACS.EDITOR.SERVICE.HOST_PENDING",
            EditorNativeErrorDomain.None,
            0,
            0,
            0);

    internal static EditorOptionalServiceUiState ContractFailure(
        EditorOptionalService service,
        EditorOptionalServiceUiSource source,
        string failure) =>
        new(
            service,
            source,
            EditorOptionalServiceState.Failed,
            EditorOptionalServiceReason.InvalidHost,
            CanInvoke: false,
            IsRetryable: true,
            string.IsNullOrWhiteSpace(failure)
                ? "The optional-service diagnostic result was rejected."
                : failure.Trim(),
            source == EditorOptionalServiceUiSource.StaleResult
                ? "ACS.EDITOR.SERVICE.STALE_RESULT"
                : "ACS.EDITOR.SERVICE.CONTRACT_FAILURE",
            EditorNativeErrorDomain.EditorAbi,
            -1,
            0,
            0);
}

internal delegate bool EditorOptionalServiceDiagnosticQuery(
    IntPtr handle,
    EditorOptionalService service,
    ulong expectedHostGeneration,
    out EditorOptionalServiceDiagnostic diagnostic,
    out string failure);

/// <summary>
/// Keeps exact optional-service results bound to one managed/native host
/// generation. A late result never changes the cached generation or enables a
/// control. Missing diagnostics remain a compatibility path because the
/// capability is intentionally optional.
/// </summary>
internal sealed class EditorOptionalServiceUiSession
{
    private IntPtr _managedHandle;
    private int _managedHostGeneration;
    private ulong _nativeHostGeneration;
    private readonly ulong[] _latestDiagnosticGenerations = new ulong[4];

    internal EditorOptionalServiceUiState Evaluate(
        IntPtr handle,
        int managedHostGeneration,
        EditorAbiCapability capabilities,
        EditorOptionalService service,
        EditorOptionalServiceDiagnosticQuery query,
        Func<bool> identityStillCurrent)
    {
        ArgumentNullException.ThrowIfNull(query);
        ArgumentNullException.ThrowIfNull(identityStillCurrent);

        if (!Enum.IsDefined(service))
        {
            throw new ArgumentOutOfRangeException(
                nameof(service),
                service,
                "Unknown optional editor service.");
        }

        if (handle != _managedHandle ||
            managedHostGeneration != _managedHostGeneration)
        {
            Reset(handle, managedHostGeneration);
        }

        if (!capabilities.HasFlag(
                EditorAbiCapability.OptionalServiceDiagnosticsV2))
        {
            return EditorOptionalServiceUiState.Legacy(
                service,
                handle != IntPtr.Zero);
        }

        if (handle == IntPtr.Zero)
        {
            return new EditorOptionalServiceUiState(
                service,
                EditorOptionalServiceUiSource.HostPending,
                EditorOptionalServiceState.Pending,
                EditorOptionalServiceReason.StartupPending,
                CanInvoke: false,
                IsRetryable: true,
                "The native editor host has not been published yet.",
                "ACS.EDITOR.SERVICE.HOST_PENDING",
                EditorNativeErrorDomain.None,
                0,
                0,
                0);
        }

        EditorOptionalServiceDiagnostic diagnostic;
        string failure;
        bool querySucceeded;
        try
        {
            querySucceeded = query(
                handle,
                service,
                _nativeHostGeneration,
                out diagnostic,
                out failure);
        }
        catch (Exception error) when (
            error is not OutOfMemoryException)
        {
            return EditorOptionalServiceUiState.ContractFailure(
                service,
                EditorOptionalServiceUiSource.ContractFailure,
                "The optional-service diagnostic query threw " +
                $"{error.GetType().Name}; the result was rejected.");
        }
        if (!querySucceeded)
        {
            return EditorOptionalServiceUiState.ContractFailure(
                service,
                EditorOptionalServiceUiSource.ContractFailure,
                failure);
        }

        if (!identityStillCurrent())
        {
            return EditorOptionalServiceUiState.ContractFailure(
                service,
                EditorOptionalServiceUiSource.StaleResult,
                "The native editor host changed while its service status was queried.");
        }

        if (diagnostic.HostGeneration == 0 ||
            diagnostic.DiagnosticGeneration == 0)
        {
            return EditorOptionalServiceUiState.ContractFailure(
                service,
                EditorOptionalServiceUiSource.ContractFailure,
                "The optional-service diagnostic omitted its generation identity.");
        }

        int serviceIndex = checked((int)service);
        ulong previousGeneration =
            _latestDiagnosticGenerations[serviceIndex];
        if (previousGeneration != 0 &&
            diagnostic.DiagnosticGeneration <= previousGeneration)
        {
            return EditorOptionalServiceUiState.ContractFailure(
                service,
                EditorOptionalServiceUiSource.StaleResult,
                $"Discarded diagnostic generation " +
                $"{diagnostic.DiagnosticGeneration}; the latest accepted " +
                $"generation is {previousGeneration} and results must advance.");
        }

        if (_nativeHostGeneration == 0)
            _nativeHostGeneration = diagnostic.HostGeneration;
        _latestDiagnosticGenerations[serviceIndex] =
            diagnostic.DiagnosticGeneration;

        return new EditorOptionalServiceUiState(
            service,
            EditorOptionalServiceUiSource.NativeDiagnostic,
            diagnostic.State,
            diagnostic.Reason,
            diagnostic.IsCallable,
            diagnostic.IsRetryable,
            diagnostic.Message,
            diagnostic.StableCode,
            diagnostic.ErrorDomain,
            diagnostic.ErrorCode,
            diagnostic.HostGeneration,
            diagnostic.DiagnosticGeneration);
    }

    private void Reset(IntPtr handle, int managedHostGeneration)
    {
        _managedHandle = handle;
        _managedHostGeneration = managedHostGeneration;
        _nativeHostGeneration = 0;
        Array.Clear(_latestDiagnosticGenerations);
    }
}

internal static class EditorOptionalServiceActionPolicy
{
    internal readonly record struct ProfilerControlPlan(
        bool ResetEnabled,
        bool PauseEnabled,
        bool ExportRetainedHistoryEnabled,
        bool CloudFilterEnabled);

    internal readonly record struct CameraControlPlan(
        bool OpenPreviewEnabled,
        bool RequestMutationEnabled,
        bool CloseEnabled,
        bool RedockEnabled);

    internal static ProfilerControlPlan PlanProfilerControls(
        in EditorOptionalServiceUiState profilerService,
        in EditorOptionalServiceUiState cloudService) =>
        new(
            ResetEnabled: profilerService.CanInvoke,
            PauseEnabled: true,
            ExportRetainedHistoryEnabled: true,
            // Visibility of retained Cloud data is a local presentation
            // choice. Keep it usable even when the native service is pending
            // or failed; only the native snapshot query fails closed.
            CloudFilterEnabled: true);

    internal static bool CanOpenCameraPreview(
        in EditorOptionalServiceUiState requestService) =>
        requestService.CanInvoke ||
        requestService.IsCapabilityNotAdvertised;

    internal static bool CanMutateCameraRequests(
        in EditorOptionalServiceUiState requestService,
        bool usesRequestContract) =>
        usesRequestContract
            ? requestService.CanInvoke
            : !requestService.UsesExactNativeDiagnostic;

    internal static CameraControlPlan PlanCameraControls(
        in EditorOptionalServiceUiState requestService,
        bool usesRequestContract) =>
        new(
            OpenPreviewEnabled:
                CanOpenCameraPreview(requestService),
            RequestMutationEnabled:
                CanMutateCameraRequests(
                    requestService,
                    usesRequestContract),
            CloseEnabled: true,
            RedockEnabled: true);
}
