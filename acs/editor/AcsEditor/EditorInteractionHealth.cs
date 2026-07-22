// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;

namespace AcsEditor;

internal enum EditorInteractionHealthKind
{
    Healthy,
    ExpectedVisibleModal,
    DisabledWithoutVisibleOwner,
    RecoveryPromptDisabledOwner,
    InteractiveHitTestingDisabled,
    StaleViewportCapture,
}

internal readonly record struct EditorInteractionHealthSample(
    bool WindowVisible,
    bool WindowClosing,
    bool WindowEnabled,
    bool WindowHitTestVisible,
    bool NonInteractiveLaunch,
    bool WindowMoveSizeActive,
    bool ProfilerAdvanced,
    bool ThreadModal,
    int VisibleOwnedWindowCount,
    bool NativeOwnedPopupVisible,
    bool RecoveryPromptVisible,
    bool ViewportOwnsCapture,
    int ActivePointerButtonMask,
    int PhysicallyDownPointerButtonMask,
    double PointerMismatchAgeMilliseconds);

internal readonly record struct EditorInteractionHealthAssessment(
    EditorInteractionHealthKind Kind,
    bool IsFault,
    string Code)
{
    internal bool ShouldLog => Kind != EditorInteractionHealthKind.Healthy;
}

/// <summary>
/// Pure classification for the failure mode where rendering/profiling keeps
/// advancing but the editor no longer accepts input or window movement.
/// Keeping this separate from WPF/Win32 sampling makes every branch available
/// to the managed reliability self-test.
/// </summary>
internal static class EditorInteractionHealthPolicy
{
    internal const double StaleCaptureDiagnosticMilliseconds = 750.0;

    internal static EditorInteractionHealthAssessment Evaluate(
        in EditorInteractionHealthSample sample)
    {
        if (!sample.WindowVisible || sample.WindowClosing)
            return Healthy();

        if (sample.RecoveryPromptVisible && !sample.WindowEnabled)
        {
            return new(
                EditorInteractionHealthKind.RecoveryPromptDisabledOwner,
                IsFault: true,
                "RECOVERY_DISABLED_OWNER");
        }

        if (!sample.WindowEnabled)
        {
            // A visible owned window is not proof of modality: recovery and
            // Package Project deliberately remain modeless. Only WPF's actual
            // thread-modal state may explain a disabled owner.
            if (sample.ThreadModal &&
                (sample.VisibleOwnedWindowCount > 0 ||
                 sample.NativeOwnedPopupVisible))
            {
                return new(
                    EditorInteractionHealthKind.ExpectedVisibleModal,
                    IsFault: false,
                    sample.ProfilerAdvanced
                        ? "VISIBLE_MODAL_RENDERER_ACTIVE"
                        : "VISIBLE_MODAL");
            }

            return new(
                EditorInteractionHealthKind.DisabledWithoutVisibleOwner,
                IsFault: true,
                sample.ProfilerAdvanced
                    ? "OWNER_DISABLED_RENDERER_ACTIVE"
                    : "OWNER_DISABLED_NO_VISIBLE_DIALOG");
        }

        if (!sample.NonInteractiveLaunch && !sample.WindowHitTestVisible)
        {
            return new(
                EditorInteractionHealthKind.InteractiveHitTestingDisabled,
                IsFault: true,
                "INTERACTIVE_HIT_TEST_DISABLED");
        }

        bool initiatingButtonDown =
            (sample.ActivePointerButtonMask &
             sample.PhysicallyDownPointerButtonMask) != 0;
        if (!sample.WindowMoveSizeActive &&
            sample.ViewportOwnsCapture &&
            sample.ActivePointerButtonMask != 0 &&
            !initiatingButtonDown &&
            double.IsFinite(sample.PointerMismatchAgeMilliseconds) &&
            sample.PointerMismatchAgeMilliseconds >=
                StaleCaptureDiagnosticMilliseconds)
        {
            return new(
                EditorInteractionHealthKind.StaleViewportCapture,
                IsFault: true,
                sample.ProfilerAdvanced
                    ? "STALE_CAPTURE_RENDERER_ACTIVE"
                    : "STALE_VIEWPORT_CAPTURE");
        }

        return Healthy();
    }

    private static EditorInteractionHealthAssessment Healthy() =>
        new(EditorInteractionHealthKind.Healthy, IsFault: false, "HEALTHY");
}

internal static class EditorInteractionSoakPolicy
{
    internal const double TimerIntervalMilliseconds = 500.0;
    internal const double RequiredDispatcherTickDensity = 0.75;
    internal const double RequiredProfilerAdvanceDensity = 0.50;
    internal const double MaximumDispatcherGapMilliseconds = 1500.0;
    internal const double MaximumProfilerGapMilliseconds = 2000.0;

    internal static int ExpectedDispatcherTicks(TimeSpan duration) =>
        Math.Max(
            1,
            (int)Math.Floor(
                duration.TotalMilliseconds / TimerIntervalMilliseconds));

    internal static IReadOnlyList<string> Evaluate(
        TimeSpan duration,
        int dispatcherTicks,
        int profilerAdvancedTicks,
        double maximumDispatcherGapMilliseconds,
        double maximumProfilerGapMilliseconds)
    {
        int expected = ExpectedDispatcherTicks(duration);
        int requiredDispatcher = Math.Max(
            2,
            (int)Math.Ceiling(expected * RequiredDispatcherTickDensity));
        int requiredProfiler = Math.Max(
            1,
            (int)Math.Ceiling(expected * RequiredProfilerAdvanceDensity));
        var faults = new List<string>();

        if (dispatcherTicks < 2)
            faults.Add("DISPATCHER_HEARTBEAT_MISSING");
        else if (dispatcherTicks < requiredDispatcher)
            faults.Add("DISPATCHER_TICK_DENSITY_LOW");
        if (!double.IsFinite(maximumDispatcherGapMilliseconds) ||
            maximumDispatcherGapMilliseconds >
                MaximumDispatcherGapMilliseconds)
        {
            faults.Add("DISPATCHER_HEARTBEAT_STALL");
        }

        if (profilerAdvancedTicks == 0)
            faults.Add("PROFILER_NOT_ADVANCING");
        else if (profilerAdvancedTicks < requiredProfiler)
            faults.Add("PROFILER_TICK_DENSITY_LOW");
        if (!double.IsFinite(maximumProfilerGapMilliseconds) ||
            maximumProfilerGapMilliseconds > MaximumProfilerGapMilliseconds)
        {
            faults.Add("PROFILER_HEARTBEAT_STALL");
        }
        return faults;
    }
}

internal static class EditorReliabilityReportContract
{
    internal const int CurrentSchemaVersion = 2;
}
