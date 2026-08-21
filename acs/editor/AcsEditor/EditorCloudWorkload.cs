// SPDX-License-Identifier: Apache-2.0

using System.Globalization;
using System.Runtime.InteropServices;

namespace AcsEditor;

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal struct EditorCloudWorkloadSnapshot
{
    public uint Version;
    public uint StructSize;
    public uint Flags;
    public uint SkipReason;

    public ulong ProfilerFrameIndex;
    public ulong SubmissionIndex;

    public uint TraceWidth;
    public uint TraceHeight;
    public uint OutputWidth;
    public uint OutputHeight;

    public uint SteadyDispatches;
    public uint OneTimeBakeDispatches;
    public uint ShadowCacheDispatches;
    public uint TotalComputeDispatches;
    public uint CompositeDraws;
    public uint Reserved0;

    public ulong TraceLogicalInvocations;
    public ulong TraceLaunchedThreads;
    public ulong ResolveLogicalInvocations;
    public ulong ResolveLaunchedThreads;
    public ulong OneTimeBakeLogicalInvocations;
    public ulong OneTimeBakeLaunchedThreads;
    public ulong ShadowCacheLogicalInvocations;
    public ulong ShadowCacheLaunchedThreads;
    public ulong TotalLogicalInvocations;
    public ulong TotalLaunchedThreads;
    public ulong MaximumViewSamples;
    public ulong MaximumLightSamples;

    public uint WorldShadowDispatches;
    public uint Reserved1;
    public ulong WorldShadowLogicalInvocations;
    public ulong WorldShadowLaunchedThreads;
    public ulong MaximumWorldShadowSamples;
}

internal enum EditorCloudWorkloadQueryStatus
{
    Unsupported,
    RuntimeUnavailable,
    Available,
    ContractError,
}

internal static class EditorCloudWorkloadContract
{
    internal const uint Version = 2;
    internal const uint FlagAttempted = 1u << 0;
    internal const uint FlagSubmitted = 1u << 1;
    internal const uint FlagHistoryWasAvailable = 1u << 2;
    internal const uint FlagHistoryReused = 1u << 3;
    internal const uint FlagHistoryInvalidated = 1u << 4;
    internal const uint FlagTemporalSuperResolution = 1u << 5;
    private const uint KnownFlags =
        FlagAttempted |
        FlagSubmitted |
        FlagHistoryWasAvailable |
        FlagHistoryReused |
        FlagHistoryInvalidated |
        FlagTemporalSuperResolution;

    internal const uint SkipNone = 0;
    internal const uint SkipResourcesNotReady = 1;
    internal const uint SkipInvalidCamera = 2;
    internal const uint SkipInvalidProjection = 3;
    private const ulong MaximumViewMarchSamples = 192;
    private const ulong MaximumLightMarchSamples = 8;
    private const ulong MaximumWorldShadowMarchSamples = 32;

    internal static uint SnapshotSize =>
        checked((uint)Marshal.SizeOf<EditorCloudWorkloadSnapshot>());

    internal static bool IsSupported(EditorAbiCapability capabilities) =>
        capabilities.HasFlag(
            EditorAbiCapability.VolumetricCloudWorkloadV2);

    internal static bool HasValidSemantics(
        int nativeResult,
        in EditorCloudWorkloadSnapshot snapshot)
    {
        if ((snapshot.Flags & ~KnownFlags) != 0u ||
            snapshot.Reserved0 != 0u ||
            snapshot.Reserved1 != 0u ||
            snapshot.SkipReason > SkipInvalidProjection ||
            (nativeResult != 0 && nativeResult != 1))
        {
            return false;
        }

        bool attempted = HasFlag(snapshot.Flags, FlagAttempted);
        bool submitted = HasFlag(snapshot.Flags, FlagSubmitted);
        bool historyWasAvailable =
            HasFlag(snapshot.Flags, FlagHistoryWasAvailable);
        bool historyReused =
            HasFlag(snapshot.Flags, FlagHistoryReused);
        bool historyInvalidated =
            HasFlag(snapshot.Flags, FlagHistoryInvalidated);
        bool temporalSuperResolution =
            HasFlag(snapshot.Flags, FlagTemporalSuperResolution);

        if (nativeResult == 0)
        {
            bool dormant =
                snapshot.Flags == 0u &&
                snapshot.SkipReason == SkipNone &&
                snapshot.ProfilerFrameIndex == 0u &&
                HasNoRecordedWork(snapshot);
            bool resourcesNotReady =
                attempted &&
                !submitted &&
                !historyReused &&
                !historyInvalidated &&
                !temporalSuperResolution &&
                (snapshot.Flags &
                 ~(FlagAttempted | FlagHistoryWasAvailable)) == 0u &&
                snapshot.SkipReason == SkipResourcesNotReady &&
                HasNoRecordedWork(snapshot);
            return dormant || resourcesNotReady;
        }

        if (!attempted)
            return false;

        if (!submitted)
        {
            bool invalidView =
                snapshot.SkipReason == SkipInvalidCamera ||
                snapshot.SkipReason == SkipInvalidProjection;
            return invalidView &&
                   !historyReused &&
                   !temporalSuperResolution &&
                   historyInvalidated == historyWasAvailable &&
                   HasNoRecordedWork(snapshot);
        }

        if (snapshot.SkipReason != SkipNone ||
            snapshot.SubmissionIndex == 0u ||
            snapshot.TraceWidth == 0u ||
            snapshot.TraceHeight == 0u ||
            snapshot.OutputWidth == 0u ||
            snapshot.OutputHeight == 0u ||
            historyReused && historyInvalidated ||
            historyWasAvailable !=
                (historyReused || historyInvalidated))
        {
            return false;
        }

        bool expectedTemporalSuperResolution =
            snapshot.TraceWidth ==
                CeilDivide(snapshot.OutputWidth, 4u) &&
            snapshot.TraceHeight ==
                CeilDivide(snapshot.OutputHeight, 4u);
        if (temporalSuperResolution !=
            expectedTemporalSuperResolution)
        {
            return false;
        }

        // 定常描画では、視線積分、時間再構成、雲内部影、ワールド雲影を各1回だけ処理する。
        if (snapshot.SteadyDispatches != 2u ||
            snapshot.OneTimeBakeDispatches > 4u ||
            snapshot.ShadowCacheDispatches > 1u ||
            snapshot.WorldShadowDispatches > 1u ||
            (ulong)snapshot.TotalComputeDispatches !=
                (ulong)snapshot.SteadyDispatches +
                snapshot.OneTimeBakeDispatches +
                snapshot.ShadowCacheDispatches +
                snapshot.WorldShadowDispatches)
        {
            return false;
        }

        ulong traceLogical = SaturatingMultiply(
            snapshot.TraceWidth,
            snapshot.TraceHeight);
        ulong resolveLogical = SaturatingMultiply(
            snapshot.OutputWidth,
            snapshot.OutputHeight);
        if (snapshot.TraceLogicalInvocations != traceLogical ||
            snapshot.ResolveLogicalInvocations != resolveLogical ||
            snapshot.TraceLaunchedThreads != LaunchedThreads2D(
                snapshot.TraceWidth,
                snapshot.TraceHeight) ||
            snapshot.ResolveLaunchedThreads != LaunchedThreads2D(
                snapshot.OutputWidth,
                snapshot.OutputHeight))
        {
            return false;
        }

        if (!InvocationPairIsValid(
                snapshot.TraceLogicalInvocations,
                snapshot.TraceLaunchedThreads) ||
            !InvocationPairIsValid(
                snapshot.ResolveLogicalInvocations,
                snapshot.ResolveLaunchedThreads) ||
            !InvocationComponentMatchesDispatches(
                snapshot.OneTimeBakeDispatches,
                snapshot.OneTimeBakeLogicalInvocations,
                snapshot.OneTimeBakeLaunchedThreads) ||
            !InvocationComponentMatchesDispatches(
                snapshot.ShadowCacheDispatches,
                snapshot.ShadowCacheLogicalInvocations,
                snapshot.ShadowCacheLaunchedThreads) ||
            !InvocationComponentMatchesDispatches(
                snapshot.WorldShadowDispatches,
                snapshot.WorldShadowLogicalInvocations,
                snapshot.WorldShadowLaunchedThreads) ||
            !InvocationPairIsValid(
                snapshot.TotalLogicalInvocations,
                snapshot.TotalLaunchedThreads))
        {
            return false;
        }

        ulong logicalTotal = SaturatingAdd(
            SaturatingAdd(
                snapshot.TraceLogicalInvocations,
                snapshot.ResolveLogicalInvocations),
            SaturatingAdd(
                SaturatingAdd(
                    snapshot.OneTimeBakeLogicalInvocations,
                    snapshot.ShadowCacheLogicalInvocations),
                snapshot.WorldShadowLogicalInvocations));
        ulong launchedTotal = SaturatingAdd(
            SaturatingAdd(
                snapshot.TraceLaunchedThreads,
                snapshot.ResolveLaunchedThreads),
            SaturatingAdd(
                SaturatingAdd(
                    snapshot.OneTimeBakeLaunchedThreads,
                    snapshot.ShadowCacheLaunchedThreads),
                snapshot.WorldShadowLaunchedThreads));
        ulong maximumViewSamples = SaturatingMultiply(
            snapshot.TraceLogicalInvocations,
            MaximumViewMarchSamples);
        return snapshot.TotalLogicalInvocations == logicalTotal &&
               snapshot.TotalLaunchedThreads == launchedTotal &&
               snapshot.MaximumViewSamples == maximumViewSamples &&
               snapshot.MaximumLightSamples == SaturatingMultiply(
                   maximumViewSamples,
                   MaximumLightMarchSamples) &&
               snapshot.MaximumWorldShadowSamples == SaturatingMultiply(
                   snapshot.WorldShadowLogicalInvocations,
                   MaximumWorldShadowMarchSamples);
    }

    internal static EditorCloudWorkloadQueryStatus ClassifyNativeResult(
        int nativeResult,
        in EditorCloudWorkloadSnapshot snapshot)
    {
        if (snapshot.Version != Version ||
            snapshot.StructSize < SnapshotSize)
        {
            return EditorCloudWorkloadQueryStatus.ContractError;
        }

        if (!HasValidSemantics(nativeResult, snapshot))
            return EditorCloudWorkloadQueryStatus.ContractError;

        return nativeResult == 0
            ? EditorCloudWorkloadQueryStatus.RuntimeUnavailable
            : EditorCloudWorkloadQueryStatus.Available;
    }

    internal static bool BelongsToProfilerFrame(
        EditorCloudWorkloadQueryStatus status,
        in EditorCloudWorkloadSnapshot snapshot,
        ulong profilerFrameIndex) =>
        status switch
        {
            EditorCloudWorkloadQueryStatus.Unsupported => true,
            EditorCloudWorkloadQueryStatus.RuntimeUnavailable =>
                snapshot.ProfilerFrameIndex == 0 ||
                snapshot.ProfilerFrameIndex == profilerFrameIndex,
            EditorCloudWorkloadQueryStatus.Available =>
                profilerFrameIndex > 0 &&
                snapshot.ProfilerFrameIndex == profilerFrameIndex,
            _ => false,
        };

    private static bool HasFlag(uint flags, uint flag) =>
        (flags & flag) != 0u;

    private static bool HasNoRecordedWork(
        in EditorCloudWorkloadSnapshot snapshot) =>
        snapshot.SubmissionIndex == 0u &&
        snapshot.TraceWidth == 0u &&
        snapshot.TraceHeight == 0u &&
        snapshot.OutputWidth == 0u &&
        snapshot.OutputHeight == 0u &&
        snapshot.SteadyDispatches == 0u &&
        snapshot.OneTimeBakeDispatches == 0u &&
        snapshot.ShadowCacheDispatches == 0u &&
        snapshot.WorldShadowDispatches == 0u &&
        snapshot.TotalComputeDispatches == 0u &&
        snapshot.CompositeDraws == 0u &&
        snapshot.TraceLogicalInvocations == 0u &&
        snapshot.TraceLaunchedThreads == 0u &&
        snapshot.ResolveLogicalInvocations == 0u &&
        snapshot.ResolveLaunchedThreads == 0u &&
        snapshot.OneTimeBakeLogicalInvocations == 0u &&
        snapshot.OneTimeBakeLaunchedThreads == 0u &&
        snapshot.ShadowCacheLogicalInvocations == 0u &&
        snapshot.ShadowCacheLaunchedThreads == 0u &&
        snapshot.WorldShadowLogicalInvocations == 0u &&
        snapshot.WorldShadowLaunchedThreads == 0u &&
        snapshot.TotalLogicalInvocations == 0u &&
        snapshot.TotalLaunchedThreads == 0u &&
        snapshot.MaximumViewSamples == 0u &&
        snapshot.MaximumLightSamples == 0u &&
        snapshot.MaximumWorldShadowSamples == 0u;

    private static bool InvocationPairIsValid(
        ulong logical,
        ulong launched) =>
        (logical == 0u) == (launched == 0u) &&
        launched >= logical;

    private static bool InvocationComponentMatchesDispatches(
        uint dispatches,
        ulong logical,
        ulong launched) =>
        InvocationPairIsValid(logical, launched) &&
        (dispatches == 0u) == (logical == 0u);

    private static uint CeilDivide(uint value, uint divisor) =>
        value / divisor + (value % divisor == 0u ? 0u : 1u);

    private static ulong RoundedThreadExtent(
        uint extent,
        uint groupSize) =>
        SaturatingMultiply(
            CeilDivide(extent, groupSize),
            groupSize);

    private static ulong LaunchedThreads2D(
        uint width,
        uint height) =>
        SaturatingMultiply(
            RoundedThreadExtent(width, 8u),
            RoundedThreadExtent(height, 8u));

    private static ulong SaturatingAdd(
        ulong left,
        ulong right) =>
        right > ulong.MaxValue - left
            ? ulong.MaxValue
            : left + right;

    private static ulong SaturatingMultiply(
        ulong left,
        ulong right) =>
        left == 0u || right == 0u
            ? 0u
            : right > ulong.MaxValue / left
                ? ulong.MaxValue
                : left * right;
}

internal static class EditorCloudWorkloadFormatting
{
    private static string Count(ulong value) =>
        value.ToString("N0", CultureInfo.InvariantCulture);

    private static string Count(uint value) =>
        value.ToString("N0", CultureInfo.InvariantCulture);

    internal static string State(
        EditorCloudWorkloadQueryStatus status,
        in EditorCloudWorkloadSnapshot snapshot) =>
        status switch
        {
            EditorCloudWorkloadQueryStatus.Unsupported =>
                "UNSUPPORTED - cloud-workload-v2",
            EditorCloudWorkloadQueryStatus.RuntimeUnavailable
                when snapshot.SkipReason ==
                     EditorCloudWorkloadContract
                         .SkipResourcesNotReady =>
                "UNAVAILABLE - cloud resources not ready",
            EditorCloudWorkloadQueryStatus.RuntimeUnavailable =>
                "UNAVAILABLE - renderer/cloud not ready",
            EditorCloudWorkloadQueryStatus.ContractError =>
                "ABI CONTRACT ERROR",
            _ when (snapshot.Flags &
                    EditorCloudWorkloadContract.FlagSubmitted) != 0u =>
                $"SUBMITTED - frame {Count(snapshot.ProfilerFrameIndex)} - " +
                $"cloud #{Count(snapshot.SubmissionIndex)}",
            _ => "SKIPPED - " + SkipReason(snapshot.SkipReason),
        };

    internal static string SkipReason(uint reason) =>
        reason switch
        {
            EditorCloudWorkloadContract.SkipNone => "none",
            EditorCloudWorkloadContract.SkipResourcesNotReady =>
                "resources not ready",
            EditorCloudWorkloadContract.SkipInvalidCamera =>
                "invalid camera",
            EditorCloudWorkloadContract.SkipInvalidProjection =>
                "invalid projection",
            _ => $"unknown ({reason})",
        };

    internal static string Dispatches(
        in EditorCloudWorkloadSnapshot snapshot) =>
        $"{Count(snapshot.TotalComputeDispatches)} = " +
        $"{Count(snapshot.SteadyDispatches)} steady + " +
        $"{Count(snapshot.OneTimeBakeDispatches)} bake + " +
        $"{Count(snapshot.ShadowCacheDispatches)} internal shadow + " +
        $"{Count(snapshot.WorldShadowDispatches)} world shadow; " +
        $"{Count(snapshot.CompositeDraws)} composite draw";

    internal static string Invocations(
        ulong logical,
        ulong launched) =>
        $"{Count(logical)} logical / {Count(launched)} launched";

    internal static string OneTimeBake(
        in EditorCloudWorkloadSnapshot snapshot) =>
        $"{Count(snapshot.OneTimeBakeDispatches)} dispatch; " +
        Invocations(
            snapshot.OneTimeBakeLogicalInvocations,
            snapshot.OneTimeBakeLaunchedThreads);

    internal static string History(
        in EditorCloudWorkloadSnapshot snapshot)
    {
        string state =
            (snapshot.Flags &
             EditorCloudWorkloadContract.FlagHistoryInvalidated) != 0u
                ? "INVALIDATED"
                : (snapshot.Flags &
                   EditorCloudWorkloadContract.FlagHistoryReused) != 0u
                    ? "REUSED"
                    : (snapshot.Flags &
                       EditorCloudWorkloadContract
                           .FlagHistoryWasAvailable) != 0u
                        ? "AVAILABLE"
                        : "COLD";
        if ((snapshot.Flags &
             EditorCloudWorkloadContract
                 .FlagTemporalSuperResolution) != 0u)
        {
            state += " - TSR 4x4";
        }
        return state;
    }
}
