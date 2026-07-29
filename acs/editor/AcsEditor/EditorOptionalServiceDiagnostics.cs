// SPDX-License-Identifier: Apache-2.0

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace AcsEditor;

internal enum EditorOptionalService : uint
{
    Profiler = 1,
    VolumetricCloudWorkload = 2,
    CameraViewRequests = 3,
}

internal enum EditorOptionalServiceState : uint
{
    Enabled = 1,
    Disabled = 2,
    Pending = 3,
    Inactive = 4,
    Failed = 5,
}

internal enum EditorOptionalServiceReason : uint
{
    None = 0,
    CapabilityNotAdvertised = 1,
    InvalidHost = 2,
    StartupPending = 3,
    SceneFeatureInactive = 4,
    UnknownService = 5,
    StartupFailed = 6,
}

[Flags]
internal enum EditorOptionalServiceFlags : uint
{
    None = 0,
    Callable = 1u << 0,
    Retryable = 1u << 1,
}

internal enum EditorNativeErrorDomain : uint
{
    None = 0,
    EditorAbi = 1,
    EditorHost = 2,
    Renderer = 3,
}

internal readonly record struct EditorOptionalServiceDiagnostic(
    EditorOptionalService Service,
    EditorOptionalServiceState State,
    EditorOptionalServiceReason Reason,
    EditorOptionalServiceFlags Flags,
    ulong HostGeneration,
    string Message,
    EditorNativeErrorDomain ErrorDomain,
    int ErrorCode,
    ulong DiagnosticGeneration,
    string StableCode)
{
    internal bool IsCallable =>
        Flags.HasFlag(EditorOptionalServiceFlags.Callable);

    internal bool IsRetryable =>
        Flags.HasFlag(EditorOptionalServiceFlags.Retryable);
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal unsafe struct EditorOptionalServiceDiagnosticNative
{
    internal uint Version;
    internal uint StructSize;
    internal uint Service;
    internal uint State;
    internal uint Reason;
    internal uint Flags;
    internal ulong HostGeneration;
    internal fixed byte MessageUtf8[
        EditorOptionalServiceDiagnosticContract.MessageBytes];
    internal uint ErrorDomain;
    internal int ErrorCode;
    internal ulong DiagnosticGeneration;
    internal fixed byte StableCodeUtf8[
        EditorOptionalServiceDiagnosticContract.StableCodeBytes];

    internal static EditorOptionalServiceDiagnosticNative CreateQuery()
    {
        return new EditorOptionalServiceDiagnosticNative
        {
            Version =
                EditorOptionalServiceDiagnosticContract.CurrentVersion,
            StructSize =
                EditorOptionalServiceDiagnosticContract.CurrentSize,
        };
    }

    internal static EditorOptionalServiceDiagnosticNative CreateForTest(
        uint version,
        uint structSize,
        EditorOptionalService service,
        EditorOptionalServiceState state,
        EditorOptionalServiceReason reason,
        EditorOptionalServiceFlags flags,
        ulong hostGeneration,
        string message,
        EditorNativeErrorDomain errorDomain,
        int errorCode,
        ulong diagnosticGeneration,
        string stableCode)
    {
        var native = new EditorOptionalServiceDiagnosticNative
        {
            Version = version,
            StructSize = structSize,
            Service = (uint)service,
            State = (uint)state,
            Reason = (uint)reason,
            Flags = (uint)flags,
            HostGeneration = hostGeneration,
            ErrorDomain = (uint)errorDomain,
            ErrorCode = errorCode,
            DiagnosticGeneration = diagnosticGeneration,
        };
        byte* messageBytes = native.MessageUtf8;
        EditorOptionalServiceDiagnosticContract.WriteBoundedUtf8(
            messageBytes,
            EditorOptionalServiceDiagnosticContract.MessageBytes,
            message);
        byte* stableCodeBytes = native.StableCodeUtf8;
        EditorOptionalServiceDiagnosticContract.WriteBoundedUtf8(
            stableCodeBytes,
            EditorOptionalServiceDiagnosticContract.StableCodeBytes,
            stableCode);
        return native;
    }
}

internal static class EditorOptionalServiceDiagnosticContract
{
    internal const uint LegacyVersion = 1;
    internal const uint LegacySize = 192;
    internal const uint CurrentVersion = 2;
    internal const uint CurrentSize = 256;
    internal const int MessageBytes = 160;
    internal const int StableCodeBytes = 48;

    private static readonly UTF8Encoding StrictUtf8 =
        new(encoderShouldEmitUTF8Identifier: false,
            throwOnInvalidBytes: true);

    internal static unsafe bool TryDecode(
        in EditorOptionalServiceDiagnosticNative native,
        EditorOptionalService expectedService,
        ulong expectedHostGeneration,
        out EditorOptionalServiceDiagnostic diagnostic,
        out string failure)
    {
        diagnostic = default;
        failure = "";

        bool legacy = native.Version == LegacyVersion;
        if ((!legacy && native.Version != CurrentVersion) ||
            native.StructSize <
                (legacy ? LegacySize : CurrentSize))
        {
            failure =
                $"Unsupported optional-service diagnostic header " +
                $"v{native.Version}/{native.StructSize}.";
            return false;
        }
        if (!Enum.IsDefined((EditorOptionalService)native.Service) ||
            !Enum.IsDefined((EditorOptionalServiceState)native.State) ||
            !Enum.IsDefined((EditorOptionalServiceReason)native.Reason))
        {
            failure =
                "Optional-service diagnostic contains an unknown enum value.";
            return false;
        }
        if ((EditorOptionalService)native.Service != expectedService)
        {
            failure =
                $"Optional-service diagnostic returned service " +
                $"{native.Service} for requested service " +
                $"{(uint)expectedService}.";
            return false;
        }

        const EditorOptionalServiceFlags knownFlags =
            EditorOptionalServiceFlags.Callable |
            EditorOptionalServiceFlags.Retryable;
        var flags = (EditorOptionalServiceFlags)native.Flags;
        if ((flags & ~knownFlags) != 0)
        {
            failure =
                "Optional-service diagnostic contains unknown flag bits.";
            return false;
        }
        var state = (EditorOptionalServiceState)native.State;
        var reason = (EditorOptionalServiceReason)native.Reason;
        if (!HasConsistentState(state, reason, flags))
        {
            failure =
                "Optional-service diagnostic state, reason, and flags are inconsistent.";
            return false;
        }
        if (expectedHostGeneration != 0 &&
            native.HostGeneration != expectedHostGeneration)
        {
            failure =
                $"Discarded late optional-service result for host " +
                $"{native.HostGeneration}; current host is " +
                $"{expectedHostGeneration}.";
            return false;
        }

        string message;
        fixed (byte* messageBytes = native.MessageUtf8)
        {
            if (!TryDecodeBoundedUtf8(
                    messageBytes,
                    MessageBytes,
                    out message,
                    out failure))
            {
                return false;
            }
        }
        if (message.Length == 0)
        {
            failure =
                "Optional-service diagnostic message is empty.";
            return false;
        }

        EditorNativeErrorDomain errorDomain =
            EditorNativeErrorDomain.None;
        int errorCode = 0;
        ulong diagnosticGeneration = 0;
        string stableCode = "";
        if (!legacy)
        {
            if (!Enum.IsDefined(
                    (EditorNativeErrorDomain)native.ErrorDomain) ||
                native.DiagnosticGeneration == 0)
            {
                failure =
                    "Typed optional-service diagnostic tail is invalid.";
                return false;
            }
            errorDomain =
                (EditorNativeErrorDomain)native.ErrorDomain;
            errorCode = native.ErrorCode;
            diagnosticGeneration = native.DiagnosticGeneration;
            fixed (byte* stableCodeBytes = native.StableCodeUtf8)
            {
                if (!TryDecodeBoundedUtf8(
                        stableCodeBytes,
                        StableCodeBytes,
                        out stableCode,
                        out failure))
                {
                    return false;
                }
            }
            if (stableCode.Length == 0 ||
                (errorDomain == EditorNativeErrorDomain.None) !=
                    (errorCode == 0) ||
                !HasConsistentTypedError(
                    reason,
                    errorDomain,
                    errorCode))
            {
                failure =
                    "Typed optional-service diagnostic code is inconsistent.";
                return false;
            }
        }

        diagnostic = new EditorOptionalServiceDiagnostic(
            (EditorOptionalService)native.Service,
            state,
            reason,
            flags,
            native.HostGeneration,
            message,
            errorDomain,
            errorCode,
            diagnosticGeneration,
            stableCode);
        return true;
    }

    private static bool HasConsistentState(
        EditorOptionalServiceState state,
        EditorOptionalServiceReason reason,
        EditorOptionalServiceFlags flags)
    {
        bool callable =
            flags.HasFlag(EditorOptionalServiceFlags.Callable);
        bool retryable =
            flags.HasFlag(EditorOptionalServiceFlags.Retryable);
        return state switch
        {
            EditorOptionalServiceState.Enabled =>
                reason == EditorOptionalServiceReason.None &&
                callable &&
                !retryable,
            EditorOptionalServiceState.Disabled =>
                (reason ==
                    EditorOptionalServiceReason.CapabilityNotAdvertised ||
                 reason ==
                    EditorOptionalServiceReason.UnknownService) &&
                !callable &&
                !retryable,
            EditorOptionalServiceState.Pending =>
                reason == EditorOptionalServiceReason.StartupPending &&
                !callable &&
                retryable,
            EditorOptionalServiceState.Inactive =>
                reason ==
                    EditorOptionalServiceReason.SceneFeatureInactive &&
                callable &&
                retryable,
            EditorOptionalServiceState.Failed =>
                (reason == EditorOptionalServiceReason.InvalidHost ||
                 reason ==
                    EditorOptionalServiceReason.StartupFailed) &&
                !callable,
            _ => false,
        };
    }

    private static bool HasConsistentTypedError(
        EditorOptionalServiceReason reason,
        EditorNativeErrorDomain domain,
        int code)
    {
        return reason switch
        {
            EditorOptionalServiceReason.None =>
                domain == EditorNativeErrorDomain.None &&
                code == 0,
            EditorOptionalServiceReason.CapabilityNotAdvertised =>
                domain == EditorNativeErrorDomain.EditorAbi &&
                code == 1001,
            EditorOptionalServiceReason.InvalidHost =>
                domain == EditorNativeErrorDomain.EditorHost &&
                code == 1002,
            EditorOptionalServiceReason.StartupPending =>
                domain == EditorNativeErrorDomain.Renderer &&
                code == 1003,
            EditorOptionalServiceReason.SceneFeatureInactive =>
                domain == EditorNativeErrorDomain.Renderer &&
                code == 1004,
            EditorOptionalServiceReason.UnknownService =>
                domain == EditorNativeErrorDomain.EditorAbi &&
                code == 1005,
            EditorOptionalServiceReason.StartupFailed =>
                domain == EditorNativeErrorDomain.Renderer &&
                code == 1006,
            _ => false,
        };
    }

    internal static unsafe void WriteBoundedUtf8(
        byte* destination,
        int capacity,
        string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (destination == null || capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        Span<byte> output = new(destination, capacity);
        output.Clear();
        StrictUtf8.GetEncoder().Convert(
            value.AsSpan(),
            output[..(capacity - 1)],
            flush: true,
            out _,
            out int bytesUsed,
            out _);
        destination[bytesUsed] = 0;
    }

    private static unsafe bool TryDecodeBoundedUtf8(
        byte* bytes,
        int capacity,
        out string value,
        out string failure)
    {
        int length = 0;
        while (length < capacity && bytes[length] != 0)
            length++;
        if (length == capacity)
        {
            value = "";
            failure =
                "Optional-service UTF-8 field is not NUL terminated.";
            return false;
        }
        try
        {
            value = StrictUtf8.GetString(
                new ReadOnlySpan<byte>(bytes, length));
            failure = "";
            return true;
        }
        catch (DecoderFallbackException)
        {
            value = "";
            failure =
                "Optional-service diagnostic contains invalid UTF-8.";
            return false;
        }
    }
}

internal static class EditorOptionalServiceDiagnosticsInterop
{
    private const string Dll = "acs_editor_abi";

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern int acs_editor_optional_service_diagnostic_get(
        IntPtr handle,
        uint service,
        ref EditorOptionalServiceDiagnosticNative diagnostic,
        uint diagnosticSize);

    internal static bool TryGet(
        IntPtr handle,
        EditorOptionalService service,
        ulong expectedHostGeneration,
        out EditorOptionalServiceDiagnostic diagnostic,
        out string failure)
    {
        EditorOptionalServiceDiagnosticNative native =
            EditorOptionalServiceDiagnosticNative.CreateQuery();
        try
        {
            if (acs_editor_optional_service_diagnostic_get(
                    handle,
                    (uint)service,
                    ref native,
                    EditorOptionalServiceDiagnosticContract.CurrentSize) != 1)
            {
                diagnostic = default;
                failure =
                    "Native optional-service diagnostic query rejected " +
                    "its version/size header.";
                return false;
            }
            return EditorOptionalServiceDiagnosticContract.TryDecode(
                in native,
                service,
                expectedHostGeneration,
                out diagnostic,
                out failure);
        }
        catch (Exception exception) when (
            exception is EntryPointNotFoundException or
                         DllNotFoundException or
                         BadImageFormatException)
        {
            diagnostic = new EditorOptionalServiceDiagnostic(
                service,
                EditorOptionalServiceState.Disabled,
                EditorOptionalServiceReason.CapabilityNotAdvertised,
                EditorOptionalServiceFlags.None,
                0,
                "The loaded native provider does not expose optional-service diagnostics.",
                EditorNativeErrorDomain.EditorAbi,
                1001,
                0,
                "ACS.SERVICE.DIAGNOSTICS_UNAVAILABLE");
            failure = exception.Message;
            return false;
        }
    }
}
