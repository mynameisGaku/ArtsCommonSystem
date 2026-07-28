// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AcsEditor;

internal static class ProfilerVisualFixture
{
    internal static void Capture(
        string outputPath,
        Action<int> shutdown)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);
        ArgumentNullException.ThrowIfNull(shutdown);

        var panel = new ProfilerPanel
        {
            LogPumpProvider = static () => new EditorLogPumpSnapshot(
                EngineEntriesDrained: 18_420,
                UiBatches: 312,
                RetentionTrimmed: 640,
                RetainedEntries: 4_360,
                RetentionCapacity: 5_000,
                LastBatchEntries: 12,
                MaximumBatchEntries: 128,
                LastDrainMilliseconds: 0.18,
                MaximumDrainMilliseconds: 1.42,
                LastApplyMilliseconds: 0.31,
                MaximumApplyMilliseconds: 2.08),
            NativeRenderProvider = static () =>
                new ViewportNativeRenderDiagnostic(
                    NativeCallCount: 42_800,
                    SlowNativeCallCount: 1,
                    GpuBackpressureYieldCount: 8_640,
                    LastNativeCallMilliseconds: 0.24,
                    MaximumNativeCallMilliseconds: 54.3,
                    LastNativeCallKind: "render",
                    GpuBackpressureInputRetryCount: 8_412,
                    GpuBackpressureBackgroundFallbackCount: 18,
                    GpuReadyAfterRetryCount: 8_201,
                    RenderFairnessYieldCount: 5_350,
                    LastGpuBackpressureEpochMilliseconds: 3.74,
                    MaximumGpuBackpressureEpochMilliseconds: 8.11,
                    PeakPresentedRenderBurstFrames: 8,
                    PeakRenderBurstActiveCpuMilliseconds: 7.82,
                    RenderInputContinuationYieldCount: 5_320,
                    RenderMaintenanceYieldCount: 30,
                    LastRenderContinuationQueueWaitMilliseconds: 0.18,
                    MaximumRenderContinuationQueueWaitMilliseconds: 1.42,
                    LastRenderMaintenanceQueueWaitMilliseconds: 0.64,
                    MaximumRenderMaintenanceQueueWaitMilliseconds: 4.80),
            DispatcherWatchdogProvider = static () =>
                new EditorDispatcherWatchdogSnapshot(
                    HeartbeatCount: 1_240,
                    HeartbeatAgeMilliseconds: 18.2,
                    LastDispatcherGapMilliseconds: 11.4,
                    MaximumDispatcherGapMilliseconds: 72.8,
                    StallCount: 0,
                    StallActive: false,
                    ActiveStallMilliseconds: 0,
                    LongestStallMilliseconds: 0,
                    Phase: "Ready"),
        };
        var window = new Window
        {
            Title = "ACS Editor — Profiler Visual Fixture",
            Content = panel,
            Width = 980,
            Height = 820,
            WindowStartupLocation = WindowStartupLocation.Manual,
            Left = -4000,
            Top = -4000,
            ShowInTaskbar = false,
        };
        window.Show();
        _ = window.Dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                int exitCode = 0;
                try
                {
                    panel.Stop();
                    panel.LoadVisualTestHistory();
                    window.UpdateLayout();
                    int width = Math.Max(
                        1,
                        (int)Math.Ceiling(panel.ActualWidth));
                    int height = Math.Max(
                        1,
                        (int)Math.Ceiling(panel.ActualHeight));
                    var bitmap = new RenderTargetBitmap(
                        width,
                        height,
                        96,
                        96,
                        PixelFormats.Pbgra32);
                    bitmap.Render(panel);
                    var encoder = new PngBitmapEncoder();
                    encoder.Frames.Add(BitmapFrame.Create(bitmap));
                    using var stream = new FileStream(
                        outputPath,
                        FileMode.Create,
                        FileAccess.Write,
                        FileShare.None);
                    encoder.Save(stream);
                    Console.Error.WriteLine(
                        $"profilershot saved: {outputPath} " +
                        $"({width}x{height})");
                }
                catch (Exception error)
                {
                    Console.Error.WriteLine(error);
                    exitCode = 1;
                }
                shutdown(exitCode);
            }));
    }
}
