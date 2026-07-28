// SPDX-License-Identifier: Apache-2.0

using System;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// Result of creating an unpublished native editor host on a worker thread.
/// Ownership of <see cref="Engine"/> transfers to the caller only after the
/// caller adopts the result on its current HwndHost generation.
/// </summary>
internal readonly record struct EditorNativeBootstrapResult(
    EditorAbiSnapshot Abi,
    IntPtr Engine,
    bool Cancelled,
    string? FailureDetail,
    EditorNativeHostLifetimeLease? LifetimeLease);

/// <summary>
/// Owns the process-wide native-host slot from before creation until the
/// corresponding host has been destroyed. Keeping the lease after worker
/// completion closes the generation-publication race where a newer HwndHost
/// could otherwise create a second global allocator/logger/worker pool while
/// the older unpublished result was still queued on the Dispatcher.
/// </summary>
internal sealed class EditorNativeHostLifetimeLease
{
    private int _released;

    internal void Release()
    {
        if (Interlocked.Exchange(ref _released, 1) == 0)
            EditorNativeBootstrap.ReleaseNativeHostSlot();
    }
}

/// <summary>
/// Keeps DLL loading and global native subsystem creation off the WPF
/// Dispatcher. The host is not attached to an HWND and is not visible to the
/// editor until this operation completes, so cancellation can destroy it
/// without racing a render or input call.
/// </summary>
internal static class EditorNativeBootstrap
{
    // HwndHost generations may overlap while an earlier cold native call is
    // still returning. The slot remains owned for the complete native-host
    // lifetime, not only the create call, so queued publication and teardown
    // cannot overlap a newer process-global logger/allocator/worker pool.
    private static readonly SemaphoreSlim NativeHostSlot = new(1, 1);

    internal static Task<EditorNativeBootstrapResult> StartAsync(
        CancellationToken cancellationToken) =>
        RunAsync(
            EngineInterop.AbiSnapshot,
            EngineInterop.acs_editor_create,
            engine =>
                EngineInterop.acs_editor_set_scene_presentation_suppressed(
                    engine,
                    1),
            EngineInterop.acs_editor_destroy,
            cancellationToken);

    internal static Task<EditorNativeBootstrapResult> RunAsync(
        Func<EditorAbiSnapshot> queryAbi,
        Func<IntPtr> createEngine,
        Action<IntPtr> suppressPresentation,
        Action<IntPtr> destroyEngine,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(queryAbi);
        ArgumentNullException.ThrowIfNull(createEngine);
        ArgumentNullException.ThrowIfNull(suppressPresentation);
        ArgumentNullException.ThrowIfNull(destroyEngine);

        // Cancellation must not be supplied to Task.Run itself. A cancellation
        // that wins just before scheduling still needs a completed result, and
        // a cancellation that wins after native create must run deterministic
        // cleanup on this worker before returning.
        return Task.Run(() =>
        {
            try
            {
                NativeHostSlot.Wait(cancellationToken);
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                return new EditorNativeBootstrapResult(
                    default,
                    IntPtr.Zero,
                    Cancelled: true,
                    FailureDetail: null,
                    LifetimeLease: null);
            }
            var lifetimeLease = new EditorNativeHostLifetimeLease();
            bool transferredLifetime = false;
            try
            {
                EditorAbiSnapshot abi = default;
                IntPtr engine = IntPtr.Zero;
                try
                {
                    if (cancellationToken.IsCancellationRequested)
                    {
                        return new EditorNativeBootstrapResult(
                            abi,
                            IntPtr.Zero,
                            Cancelled: true,
                            FailureDetail: null,
                            LifetimeLease: null);
                    }

                    abi = queryAbi();
                    if (!abi.Compatible)
                    {
                        return new EditorNativeBootstrapResult(
                            abi,
                            IntPtr.Zero,
                            Cancelled: false,
                            FailureDetail:
                                "Native editor ABI is incompatible. " +
                                abi.ToDisplayText(),
                            LifetimeLease: null);
                    }

                    engine = createEngine();
                    if (engine == IntPtr.Zero)
                    {
                        return new EditorNativeBootstrapResult(
                            abi,
                            IntPtr.Zero,
                            Cancelled: false,
                            FailureDetail:
                                "Native editor host creation returned a null handle.",
                            LifetimeLease: null);
                    }

                    suppressPresentation(engine);
                    if (!cancellationToken.IsCancellationRequested)
                    {
                        transferredLifetime = true;
                        return new EditorNativeBootstrapResult(
                            abi,
                            engine,
                            Cancelled: false,
                            FailureDetail: null,
                            LifetimeLease: lifetimeLease);
                    }

                    TryDestroy(engine, destroyEngine);
                    engine = IntPtr.Zero;
                    return new EditorNativeBootstrapResult(
                        abi,
                        IntPtr.Zero,
                        Cancelled: true,
                        FailureDetail: null,
                        LifetimeLease: null);
                }
                catch (Exception error)
                {
                    string cleanupDiagnostic = "";
                    if (engine != IntPtr.Zero)
                    {
                        try
                        {
                            destroyEngine(engine);
                        }
                        catch (Exception cleanupError)
                        {
                            cleanupDiagnostic =
                                " Native host cleanup also failed: " +
                                cleanupError.Message;
                        }
                    }

                    return new EditorNativeBootstrapResult(
                        abi,
                        IntPtr.Zero,
                        Cancelled: cancellationToken.IsCancellationRequested,
                        FailureDetail:
                            "Native editor host bootstrap failed closed: " +
                            error.Message +
                            cleanupDiagnostic,
                        LifetimeLease: null);
                }
            }
            finally
            {
                if (!transferredLifetime)
                    lifetimeLease.Release();
            }
        });
    }

    internal static Task DestroyUnpublishedAsync(
        EditorNativeBootstrapResult result,
        Action<IntPtr>? destroyOverride = null)
    {
        if (result.Engine == IntPtr.Zero)
        {
            result.LifetimeLease?.Release();
            return Task.CompletedTask;
        }
        return Task.Run(() =>
        {
            bool acquiredFallbackSlot = false;
            if (result.LifetimeLease == null)
            {
                NativeHostSlot.Wait();
                acquiredFallbackSlot = true;
            }
            try
            {
                TryDestroy(
                    result.Engine,
                    destroyOverride ?? EngineInterop.acs_editor_destroy);
            }
            finally
            {
                if (result.LifetimeLease != null)
                    result.LifetimeLease.Release();
                else if (acquiredFallbackSlot)
                    NativeHostSlot.Release();
            }
        });
    }

    internal static void ReleaseNativeHostSlot() =>
        NativeHostSlot.Release();

    private static void TryDestroy(
        IntPtr engine,
        Action<IntPtr> destroyEngine)
    {
        try
        {
            destroyEngine(engine);
        }
        catch
        {
            // The result was never published. There is no safe managed
            // recovery action; more importantly, cleanup must never fault an
            // abandoned continuation during window teardown.
        }
    }
}
