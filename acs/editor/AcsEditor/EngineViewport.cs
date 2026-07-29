using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace AcsEditor;

internal readonly record struct ViewportPointerCaptureDiagnostic(
    bool OwnsCapture,
    int ActiveButtonMask,
    int PhysicallyDownButtonMask,
    double MismatchAgeMilliseconds);

internal readonly record struct ViewportNativeRenderDiagnostic(
    long NativeCallCount,
    long SlowNativeCallCount,
    long GpuBackpressureYieldCount,
    double LastNativeCallMilliseconds,
    double MaximumNativeCallMilliseconds,
    string LastNativeCallKind,
    long GpuBackpressureInputRetryCount = 0,
    long GpuBackpressureBackgroundFallbackCount = 0,
    long GpuReadyAfterRetryCount = 0,
    long RenderFairnessYieldCount = 0,
    double LastGpuBackpressureEpochMilliseconds = 0,
    double MaximumGpuBackpressureEpochMilliseconds = 0,
    int PeakPresentedRenderBurstFrames = 0,
    double PeakRenderBurstActiveCpuMilliseconds = 0,
    long RenderInputContinuationYieldCount = 0,
    long RenderMaintenanceYieldCount = 0,
    double LastRenderContinuationQueueWaitMilliseconds = 0,
    double MaximumRenderContinuationQueueWaitMilliseconds = 0,
    double LastRenderMaintenanceQueueWaitMilliseconds = 0,
    double MaximumRenderMaintenanceQueueWaitMilliseconds = 0);

internal readonly record struct ViewportRenderBurstState(
    int PresentedFrames,
    double ActiveCpuMilliseconds);

internal readonly record struct ViewportResizeResultPolicy(
    bool CommitDimensions,
    bool ContinueToRender);

internal enum ViewportGpuBackpressureResumeMode
{
    InputPriorityRetry = 0,
    CooperativeYield = 1,
    // Source-compatible name for older fixtures; the two-tier scheduler may
    // now resume this epoch at Input priority when maintenance is not due.
    BackgroundFairness = CooperativeYield,
}

internal enum ViewportRenderYieldMode
{
    InputContinuation = 0,
    BackgroundMaintenance = 1,
}

/// <summary>
/// エンジンの DX12 描画をホストするネイティブ・ビューポート。
/// 子 HWND を作り、acs_editor_abi にアタッチして、WPF の描画フレームごとに 1 フレーム描く。
/// </summary>
public sealed class EngineViewport : HwndHost
{
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowExW(int exStyle, string className, string windowName,
        int style, int x, int y, int width, int height,
        IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetParent(IntPtr child, IntPtr newParent);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr hwnd,
        IntPtr insertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    // ----- マウス入力のためのウィンドウプロシージャ subclass -----
    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowLongPtrW(IntPtr hWnd, int nIndex, IntPtr dwNewLong);
    [DllImport("user32.dll")]
    private static extern IntPtr CallWindowProcW(IntPtr lpPrevWndFunc, IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProcW(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool ScreenToClient(IntPtr hWnd, ref POINT pt);
    [DllImport("user32.dll")] private static extern IntPtr SetCapture(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern IntPtr GetCapture();
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern uint GetQueueStatus(uint flags);
    [DllImport("user32.dll")] private static extern short GetKeyState(int nVirtKey);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int nVirtKey);
    [DllImport("user32.dll")] private static extern uint GetDoubleClickTime();
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TrackMouseEvent(ref TRACKMOUSEEVENT eventTrack);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessageW(
        IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    // アセット/ファイルのドロップ受付 (CF_HDROP → WM_DROPFILES)。アセットブラウザのドラッグも
    // DataFormats.FileDrop を含むため、これでアプリ内ドラッグ + Explorer ドロップの双方を拾える。
    [DllImport("shell32.dll")] private static extern void DragAcceptFiles(IntPtr hwnd, bool fAccept);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern uint DragQueryFileW(IntPtr hDrop, uint iFile, System.Text.StringBuilder? lpszFile, uint cch);
    [DllImport("shell32.dll")] private static extern bool DragQueryPoint(IntPtr hDrop, ref POINT pt);
    [DllImport("shell32.dll")] private static extern void DragFinish(IntPtr hDrop);
    private const int VK_LBUTTON = 0x01;
    private const int VK_RBUTTON = 0x02;
    private const int VK_MBUTTON = 0x04;
    private const int VK_CONTROL = 0x11;
    private static bool CtrlDown => (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    [StructLayout(LayoutKind.Sequential)] private struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)]
    private struct TRACKMOUSEEVENT
    {
        public uint cbSize;
        public uint dwFlags;
        public IntPtr hwndTrack;
        public uint dwHoverTime;
    }
    private delegate IntPtr WndProcDelegate(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    private const int GWLP_WNDPROC  = -4;
    private const int WM_CANCELMODE = WaterPointerRoutingPolicy.WmCancelMode;
    private const int WM_KILLFOCUS  = 0x0008;
    private const int WM_KEYDOWN    = 0x0100;
    private const int WM_MOUSEMOVE  = 0x0200, WM_LBUTTONDOWN = 0x0201,
                      WM_LBUTTONUP = WaterPointerRoutingPolicy.WmLeftButtonUp,
                      WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
                      WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208,
                      WM_MOUSEWHEEL = WaterPointerRoutingPolicy.WmMouseWheel,
                      WM_MOUSEHWHEEL = WaterPointerRoutingPolicy.WmMouseHWheel,
                      WM_NCHITTEST = 0x0084,
                      WM_CAPTURECHANGED = WaterPointerRoutingPolicy.WmCaptureChanged,
                      WM_DROPFILES = 0x0233,
                      WM_MOUSELEAVE = WaterPointerRoutingPolicy.WmMouseLeave;
    private const int WM_RENDER_PUMP = 0x8000 + 0x5A1; // private WM_APP message
    private const int HTCLIENT = 1;
    private const long MK_LBUTTON = 0x0001;
    private const uint TME_LEAVE = 0x00000002;
    private const uint QS_KEY = 0x0001;
    private const uint QS_MOUSEMOVE = 0x0002;
    private const uint QS_MOUSEBUTTON = 0x0004;
    private const uint QS_RAWINPUT = 0x0400;
    private const uint QS_TOUCH = 0x0800;
    private const uint QS_POINTER = 0x1000;
    private const uint QS_INTERACTIVE_INPUT =
        QS_KEY | QS_MOUSEMOVE | QS_MOUSEBUTTON |
        QS_RAWINPUT | QS_TOUCH | QS_POINTER;

    private const int WS_CHILD = 0x40000000;
    private const int WS_VISIBLE = 0x10000000;
    private const int WS_CLIPCHILDREN = 0x02000000;
    private const int WS_CLIPSIBLINGS = 0x04000000;
    private const uint SWP_NOZORDER = 0x0004;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_SHOWWINDOW = 0x0040;

    private IntPtr _hwnd;
    private IntPtr _embeddedSurfaceParent;
    private IntPtr _externalSurfaceParent;
    private uint _externalSurfaceWidth;
    private uint _externalSurfaceHeight;
    private IntPtr _engine;
    private CancellationTokenSource? _nativeBootstrapCancellation;
    private EditorNativeHostLifetimeLease? _nativeHostLifetimeLease;
    private EditorAbiCapability _abiCapabilities;
    private readonly EditorOptionalServiceUiSession
        _optionalServiceUiSession = new();
    private bool _attached;
    private uint _w, _h;
    private uint _pendW, _pendH;   // attach 前の «サイズ安定待ち» 用 (起動直後のリサイズ連発を回避)
    private uint _resizePendW, _resizePendH;
    private bool _awaitingStableResizeAfterWindowInteraction;
    private readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
    private double _lastSec;
    private bool _frameActive;
    private int _wndProcDepth;
    private bool _destroying;
    private bool _destroyDeferred;
    private bool _redrawPending = true;
    private bool _renderPumpQueued;
    private int _renderPumpGeneration;
    private int _renderPumpToken;
    private long _renderPumpQueuedAt;
    private DispatcherTimer? _dormantRenderTimer;
    private bool _renderPumpSuspended;
    private bool _windowInteractionPaused;
    private bool _renderFairnessYieldQueued;
    private bool _renderMaintenanceYieldQueued;
    private bool _hiddenStartupRenderingAllowed;
    private ViewportRenderBurstState _renderBurstState;
    private int _gpuBackpressureInputRetryCount;
    private long _gpuBackpressureStartedAtTimestamp;
    private long _lastRenderMaintenanceCompletedAtTimestamp;
    private long _nativeCallCount;
    private long _slowNativeCallCount;
    private long _gpuBackpressureYieldCount;
    private long _gpuBackpressureInputRetryTotal;
    private long _gpuBackpressureBackgroundFallbackCount;
    private long _gpuReadyAfterRetryCount;
    private long _renderFairnessYieldCount;
    private double _lastGpuBackpressureEpochMilliseconds;
    private double _maximumGpuBackpressureEpochMilliseconds;
    private int _peakPresentedRenderBurstFrames;
    private double _peakRenderBurstActiveCpuMilliseconds;
    private long _renderInputContinuationYieldCount;
    private long _renderMaintenanceYieldCount;
    private double _lastRenderContinuationQueueWaitMilliseconds;
    private double _maximumRenderContinuationQueueWaitMilliseconds;
    private double _lastRenderMaintenanceQueueWaitMilliseconds;
    private double _maximumRenderMaintenanceQueueWaitMilliseconds;
    private double _lastNativeCallMilliseconds;
    private double _maximumNativeCallMilliseconds;
    private string _lastNativeCallKind = string.Empty;

    // Native rendering must not be driven by CompositionTarget.Rendering:
    // that event is deliberately synchronized to WPF/DWM composition and
    // therefore caps the engine to the monitor cadence (or a divisor of it).
    // A single private Win32 message is kept in flight. Direct messages run in
    // bounded bursts. Real keyboard/pointer input ends a burst early, and every
    // burst deadline ends through an Input-priority Dispatcher checkpoint even
    // during unattended runs. That checkpoint is mandatory: an indefinitely
    // reposted private message can starve Dispatcher timers despite keeping the
    // Win32 queue active. A lower-frequency Background drain owns continuation
    // during startup and at its wall-clock deadline so timer promotion and
    // lower-priority editor finalization cannot starve behind the Input
    // checkpoints. Cooperative GPU-busy retries use the same bounded policy.
    internal const int MaxDirectRenderBurstFrames = 64;
    internal const double MaxDirectRenderBurstMilliseconds = 64.0;
    internal const int MaxGpuBackpressureInputRetries = 256;
    internal const double MaxGpuBackpressureInputRetryMilliseconds = 8.0;
    internal const double MaxRenderMaintenanceIntervalMilliseconds = 500.0;
    internal const double PointerCaptureRecoveryGraceMilliseconds = 100.0;
    internal const double SlowNativeCallThresholdMilliseconds = 50.0;
    internal const double MaximumNativeDeltaSeconds = 0.1;
    internal const int PointerButtonLeftMask = 1 << 0;
    internal const int PointerButtonRightMask = 1 << 1;
    internal const int PointerButtonMiddleMask = 1 << 2;
    internal const DispatcherPriority RenderContinuationPriority =
        DispatcherPriority.Input;
    internal const DispatcherPriority RenderMaintenancePriority =
        DispatcherPriority.Background;
    // Kept as a source-compatible name for older fixtures and diagnostics.
    internal const DispatcherPriority RenderFairnessPriority =
        RenderMaintenancePriority;
    // Compatibility alias: GPU retries now post directly unless real input is
    // pending, in which case they share the input-continuation priority.
    internal const DispatcherPriority GpuBackpressureRetryPriority =
        RenderContinuationPriority;
    internal const DispatcherPriority DormantWakePriority =
        DispatcherPriority.Background;

    internal static bool ShouldRouteEditorViewportInteraction(
        bool gameView) => !gameView;

    internal static bool ShouldRouteGameplayInput(
        bool gameView,
        bool logicPlayActive) =>
        gameView && logicPlayActive;

    private bool CanRouteEditorViewportInteraction() =>
        _engine != IntPtr.Zero &&
        ShouldRouteEditorViewportInteraction(
            EngineInterop.acs_editor_is_game_view(_engine) != 0);

    private bool CanRouteGameplayInput() =>
        _engine != IntPtr.Zero &&
        ShouldRouteGameplayInput(
            EngineInterop.acs_editor_is_game_view(_engine) != 0,
            EngineInterop.acs_editor_logic_play_active(_engine) != 0);

    internal void ResetGameInput()
    {
        if (_engine != IntPtr.Zero)
            EngineInterop.acs_editor_logic_input_reset(_engine);
    }

    internal static bool ShouldRenderContinuously(
        bool destroying,
        bool suspended,
        bool handlesReady,
        bool visible,
        bool minimized,
        double width,
        double height) =>
        !destroying && !suspended && handlesReady && visible && !minimized &&
        double.IsFinite(width) && double.IsFinite(height) &&
        width > 0.0 && height > 0.0;

    internal static bool IsRenderSurfaceVisible(
        bool nativeWindowVisible,
        bool hiddenStartupRenderingAllowed) =>
        nativeWindowVisible || hiddenStartupRenderingAllowed;

    internal static bool ShouldRecoverRenderPumpFromComposition(
        bool frameActive,
        bool eligible,
        bool queued,
        bool queuedPumpStale) =>
        !frameActive && eligible && (!queued || queuedPumpStale);

    internal static bool ShouldAttemptAttach(bool attached, bool attachFailed) =>
        !attached && !attachFailed;

    internal static bool ShouldBeginNativeBootstrapForHostGeneration(
        bool attachFailed,
        bool startupFailureSuspended) =>
        !attachFailed && !startupFailureSuspended;

    internal static bool CanExplicitlyRetryAttach(
        bool destroying,
        bool attached,
        bool attachFailed,
        bool startupFailureSuspended,
        bool hwndReady) =>
        !destroying &&
        !attached &&
        hwndReady &&
        (attachFailed || startupFailureSuspended);

    internal static bool ShouldContinueRenderingAfterAttachCallback(
        bool destroying,
        bool renderPumpSuspended,
        bool hiddenStartupRenderingAllowedBeforeCallback,
        bool hiddenStartupRenderingAllowedAfterCallback) =>
        !destroying &&
        !renderPumpSuspended &&
        (!hiddenStartupRenderingAllowedBeforeCallback ||
         hiddenStartupRenderingAllowedAfterCallback);

    internal static bool IsAnyRenderPumpSuspensionActive(
        bool startupFailureSuspended,
        bool windowInteractionPaused) =>
        startupFailureSuspended || windowInteractionPaused;

    internal static bool ShouldYieldRenderBurst(
        int completedFrames,
        double elapsedMilliseconds) =>
        completedFrames < 0 ||
        !double.IsFinite(elapsedMilliseconds) ||
        elapsedMilliseconds < 0.0 ||
        completedFrames >= MaxDirectRenderBurstFrames ||
        elapsedMilliseconds >= MaxDirectRenderBurstMilliseconds;

    internal static bool ShouldYieldRenderBurst(
        in ViewportRenderBurstState state) =>
        ShouldYieldRenderBurst(
            state.PresentedFrames,
            state.ActiveCpuMilliseconds);

    internal static bool RequiresRenderDispatcherCheckpoint(
        in ViewportRenderBurstState state) =>
        ShouldYieldRenderBurst(state);

    internal static ViewportRenderBurstState AccountRenderBurstAttempt(
        in ViewportRenderBurstState state,
        bool presented,
        bool gpuBackpressure,
        double activeCpuMilliseconds)
    {
        // A cooperative busy result performs no scene/simulation work. Its
        // asynchronous wait belongs to the bounded retry epoch and must never
        // consume the direct-render CPU residency budget.
        if (gpuBackpressure)
            return state;

        if (state.PresentedFrames < 0 ||
            !double.IsFinite(state.ActiveCpuMilliseconds) ||
            state.ActiveCpuMilliseconds < 0.0 ||
            !double.IsFinite(activeCpuMilliseconds) ||
            activeCpuMilliseconds < 0.0)
        {
            // Invalid accounting fails closed into the next fairness boundary.
            return new(
                MaxDirectRenderBurstFrames,
                MaxDirectRenderBurstMilliseconds);
        }

        int presentedFrames = state.PresentedFrames;
        if (presented)
        {
            presentedFrames = state.PresentedFrames == int.MaxValue
                ? MaxDirectRenderBurstFrames
                : state.PresentedFrames + 1;
        }
        double accumulatedCpuMilliseconds =
            state.ActiveCpuMilliseconds + activeCpuMilliseconds;
        if (!double.IsFinite(accumulatedCpuMilliseconds))
            accumulatedCpuMilliseconds = MaxDirectRenderBurstMilliseconds;
        return new(
            presentedFrames,
            accumulatedCpuMilliseconds);
    }

    internal static bool ShouldYieldForGpuBackpressure(
        int nativeRenderResult) =>
        nativeRenderResult == 0;

    internal static ViewportGpuBackpressureResumeMode
        SelectGpuBackpressureResume(
            int inputPriorityRetries,
            double elapsedMilliseconds)
    {
        if (inputPriorityRetries < 0 ||
            !double.IsFinite(elapsedMilliseconds) ||
            elapsedMilliseconds < 0.0)
        {
            return ViewportGpuBackpressureResumeMode.CooperativeYield;
        }

        return inputPriorityRetries < MaxGpuBackpressureInputRetries &&
               elapsedMilliseconds <
                   MaxGpuBackpressureInputRetryMilliseconds
            ? ViewportGpuBackpressureResumeMode.InputPriorityRetry
            : ViewportGpuBackpressureResumeMode.CooperativeYield;
    }

    internal static ViewportRenderYieldMode SelectRenderYieldMode(
        bool startupMaintenanceRequired,
        double millisecondsSinceMaintenance)
    {
        // Startup publishes its own Background-priority completion stages.
        // Invalid accounting also fails closed into Background so a damaged
        // clock cannot permanently starve editor maintenance.
        if (startupMaintenanceRequired ||
            !double.IsFinite(millisecondsSinceMaintenance) ||
            millisecondsSinceMaintenance < 0.0 ||
            millisecondsSinceMaintenance >=
                MaxRenderMaintenanceIntervalMilliseconds)
        {
            return ViewportRenderYieldMode.BackgroundMaintenance;
        }

        // No Background drain is due yet, so the private HWND
        // continuation remains eligible.
        return ViewportRenderYieldMode.InputContinuation;
    }

    internal static bool ShouldYieldToQueuedInput(uint queueStatus)
    {
        uint currentQueueKinds = queueStatus >> 16;
        return (currentQueueKinds & QS_INTERACTIVE_INPUT) != 0;
    }

    internal static bool IsRenderPumpContinuationBlocked(
        bool inputContinuationQueued,
        bool maintenanceQueued,
        bool frameActive)
    {
        // Both Dispatcher checkpoints own the next frame. In particular,
        // allowing CompositionTarget.Rendering to bypass a queued Background
        // drain recreates the private-message starvation it is meant to stop.
        return inputContinuationQueued || maintenanceQueued || frameActive;
    }

    private static bool HasPendingInteractiveInput() =>
        !App.IsNonInteractiveLaunch &&
        ShouldYieldToQueuedInput(GetQueueStatus(QS_INTERACTIVE_INPUT));

    internal static double CommitRenderTimestamp(
        double previousTimestamp,
        double candidateTimestamp,
        int nativeRenderResult)
    {
        if (nativeRenderResult <= 0)
            return previousTimestamp;

        // Consume at most the same delta passed to native code. Any excess
        // remains between the committed timestamp and the monotonic clock so
        // a long GPU/UI stall is recovered by later frames instead of lost.
        return Math.Min(
            candidateTimestamp,
            previousTimestamp + MaximumNativeDeltaSeconds);
    }

    internal static bool IsFatalRenderResult(int nativeRenderResult) =>
        nativeRenderResult < 0;

    internal static int ActivePointerButtonMask(
        bool gizmoDragging,
        bool gizmo3dDragging,
        bool marqueeDragging,
        bool panning,
        int panMode)
    {
        int mask = gizmoDragging || gizmo3dDragging || marqueeDragging
            ? PointerButtonLeftMask
            : 0;
        if (panning)
        {
            mask |= panMode == 1
                ? PointerButtonMiddleMask
                : PointerButtonRightMask;
        }
        return mask;
    }

    internal static bool IsInitiatingPointerButtonUpMessage(
        int message,
        int activeButtonMask) =>
        (message == WM_LBUTTONUP &&
         (activeButtonMask & PointerButtonLeftMask) != 0) ||
        (message == WM_RBUTTONUP &&
         (activeButtonMask & PointerButtonRightMask) != 0) ||
        (message == WM_MBUTTONUP &&
         (activeButtonMask & PointerButtonMiddleMask) != 0);

    internal static bool ShouldRecoverStalePointerCapture(
        bool viewportOwnsCapture,
        bool windowInteractionPaused,
        bool finalizingButtonUp,
        int activeButtonMask,
        int physicallyDownButtonMask,
        int currentMessage,
        double mismatchAgeMilliseconds,
        bool destroying = false,
        bool ownerClosing = false,
        bool generationMatches = true) =>
        viewportOwnsCapture &&
        !destroying &&
        !ownerClosing &&
        generationMatches &&
        !windowInteractionPaused &&
        !finalizingButtonUp &&
        activeButtonMask != 0 &&
        !IsInitiatingPointerButtonUpMessage(currentMessage, activeButtonMask) &&
        (activeButtonMask & physicallyDownButtonMask) == 0 &&
        double.IsFinite(mismatchAgeMilliseconds) &&
        mismatchAgeMilliseconds >= PointerCaptureRecoveryGraceMilliseconds;

    internal static bool IsQueuedPumpStale(
        bool queued,
        long queuedAtMilliseconds,
        long nowMilliseconds,
        long timeoutMilliseconds = 100) =>
        queued && timeoutMilliseconds > 0 &&
        nowMilliseconds - queuedAtMilliseconds >= timeoutMilliseconds;

    internal static bool ShouldDeferFinalResize(
        bool awaitingStableSize,
        uint requestedWidth,
        uint requestedHeight,
        uint candidateWidth,
        uint candidateHeight) =>
        awaitingStableSize &&
        (requestedWidth != candidateWidth ||
         requestedHeight != candidateHeight);

    internal static ViewportResizeResultPolicy ClassifyResizeResult(
        int nativeResizeResult) =>
        new(
            CommitDimensions: nativeResizeResult > 0,
            // A failed resize keeps _w/_h unchanged so it is retried. Still
            // enter render: device loss is reported by render_try as fatal,
            // while a transient resize failure can keep presenting old buffers.
            ContinueToRender: true);

    private bool RenderPumpSuspended =>
        IsAnyRenderPumpSuspensionActive(
            _renderPumpSuspended,
            _windowInteractionPaused);

    internal static bool ShouldPublishGizmoTransformChange(
        bool captureEnded,
        bool wasDragging) =>
        captureEnded && wasDragging;

    private WndProcDelegate? _wndProc;   // GC で回収されないよう保持
    private IntPtr _origProc;            // 元の STATIC ウィンドウプロシージャ
    private bool _panning;
    private int  _panMode;               // ドラッグ中のカメラ操作: 0=軌道(右) / 1=パン(中)
    private bool _gizmoDragging;         // 移動ギズモのドラッグ中
    private bool _giz3dDragging;         // 3D 変形ギズモのドラッグ中
    private int _lastX, _lastY;
    private bool _marqueeDragging;       // ラバーバンド (矩形) 選択のドラッグ中
    private int _marqStartX, _marqStartY, _marqLastX, _marqLastY;
    private bool _marqAdditive;          // gesture 開始時に latch した Ctrl (additive) 状態
    private bool _finalizing;            // 自前の ReleaseCapture か (capture 奪取と区別)
    private bool _trackingMouseLeave;
    private bool _polyMode;
    private long _pointerButtonMismatchStartedAtTimestamp;
    private const int MarqueeThreshold = 3;   // これ未満の移動は drag でなく click 扱い
    private int _lastClickTick, _lastClickX, _lastClickY;   // ダブルクリック検出 (STATIC は WM_*DBLCLK を送らない)

    /// <summary>最後に試行した attach が失敗したか (UI のステータス表示用)。</summary>
    public bool AttachFailed { get; private set; }

    /// <summary>
    /// Stable diagnostic for a fail-closed ABI/create/attach failure. This is
    /// deliberately separate from the native log pump because the DLL may not
    /// be loadable.
    /// </summary>
    public string? AttachmentFailureDetail { get; private set; }

    /// <summary>HWND への attach が失敗し、自動再試行を停止したときに 1 度発火する。</summary>
    public event Action? AttachmentFailed;

    /// <summary>cooperative native render contract が失われ、描画ポンプを停止したときに発火する。</summary>
    public event Action<string>? RenderingFailed;

    /// <summary>エンジンハンドル (シーン API 呼び出し用、未生成時 Zero)。</summary>
    public IntPtr Engine => _destroying ? IntPtr.Zero : _engine;

    internal EditorAbiCapability AbiCapabilities =>
        _destroying
            ? EditorAbiCapability.None
            : _abiCapabilities;

    internal bool SupportsCameraViewRequests =>
        !_destroying &&
        _engine != IntPtr.Zero &&
        _abiCapabilities.HasFlag(
            EditorAbiCapability.CameraViewRequestsV1);

    internal EditorOptionalServiceUiState GetOptionalServiceUiState(
        EditorOptionalService service)
    {
        Dispatcher.VerifyAccess();
        IntPtr handle = Engine;
        int managedGeneration = _renderPumpGeneration;
        EditorAbiCapability capabilities = AbiCapabilities;
        return _optionalServiceUiSession.Evaluate(
            handle,
            managedGeneration,
            capabilities,
            service,
            EditorOptionalServiceDiagnosticsInterop.TryGet,
            () =>
                !_destroying &&
                _engine == handle &&
                _renderPumpGeneration == managedGeneration &&
                _abiCapabilities == capabilities);
    }

    internal bool TryCreateCameraViewRequest(
        int nodeId,
        string stableCameraId,
        uint width,
        uint height,
        out ulong requestId)
    {
        Dispatcher.VerifyAccess();
        requestId = 0;
        return SupportsCameraViewRequests &&
               EngineInterop.TryCreateCameraViewRequest(
                   _engine,
                   nodeId,
                   stableCameraId,
                   width,
                   height,
                   out requestId);
    }

    internal bool TryUpdateCameraViewRequest(
        ulong requestId,
        int nodeId,
        string stableCameraId,
        uint width,
        uint height)
    {
        Dispatcher.VerifyAccess();
        return SupportsCameraViewRequests &&
               EngineInterop.TryUpdateCameraViewRequest(
                   _engine,
                   requestId,
                   nodeId,
                   stableCameraId,
                   width,
                   height);
    }

    internal bool TryBindCameraViewPresenter(ulong requestId)
    {
        Dispatcher.VerifyAccess();
        return SupportsCameraViewRequests &&
               EngineInterop.TryBindCameraViewPresenter(
                   _engine,
                   requestId);
    }

    internal bool TryGetCameraViewRequest(
        ulong requestId,
        out CameraViewRequestSnapshot snapshot)
    {
        Dispatcher.VerifyAccess();
        snapshot = default;
        return SupportsCameraViewRequests &&
               EngineInterop.TryGetCameraViewRequest(
                   _engine,
                   requestId,
                   out snapshot);
    }

    internal bool TryUnbindCameraViewPresenter(ulong requestId)
    {
        Dispatcher.VerifyAccess();
        return SupportsCameraViewRequests &&
               EngineInterop.TryUnbindCameraViewPresenter(
                   _engine,
                   requestId);
    }

    internal bool ReleaseCameraViewRequest(ulong requestId)
    {
        Dispatcher.VerifyAccess();
        if (requestId == 0)
            return true;
        if (!SupportsCameraViewRequests)
            return _destroying || _engine == IntPtr.Zero;
        _ = EngineInterop.TryUnbindCameraViewPresenter(
            _engine,
            requestId);
        return EngineInterop.TryDestroyCameraViewRequest(
            _engine,
            requestId);
    }

    /// <summary>ポリゴン描画モード中か (左クリックで点を置く)。MainWindow が制御。</summary>
    public bool PolyMode
    {
        get => _polyMode;
        set
        {
            if (_polyMode == value) return;
            _polyMode = value;
            if (value) EndWaterPointer();
        }
    }

    /// <summary>描画モード中に Enter/Esc が押されたとき (ポリゴン確定)。MainWindow が購読。</summary>
    public event Action? PolyKeyFinalize;

    /// <summary>HWND へのアタッチ成功時に 1 度発火 (Hierarchy 構築のトリガ)。</summary>
    public event Action? Attached;

    /// <summary>ビューポート左クリックでノードがピックされたとき (node id)。</summary>
    public event Action<int>? Picked;

    /// <summary>ギズモ操作でノードの transform が変わったとき (Inspector 更新用)。</summary>
    public event Action? TransformChanged;

    /// <summary>アセット/ファイルがビューポートへドロップされたとき (パス, クライアント X, Y[物理px])。</summary>
    public event Action<string, int, int>? AssetDropped;

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        Dispatcher.VerifyAccess();
        System.Diagnostics.Debug.Assert(
            _engine == IntPtr.Zero && _nativeHostLifetimeLease == null,
            "A new HwndHost generation cannot replace a live native host.");
        // WPF may rebuild the native child on the same managed HwndHost. The
        // owner's pre-Build hidden-startup allowance and any failure latch
        // therefore outlive the HWND; only RetryAttach may clear a failure.
        bool beginNativeBootstrap =
            ShouldBeginNativeBootstrapForHostGeneration(
                AttachFailed,
                _renderPumpSuspended);
        _nativeBootstrapCancellation?.Cancel();
        _destroying = false;
        _destroyDeferred = false;
        _frameActive = false;
        _wndProcDepth = 0;
        _redrawPending = true;
        _renderPumpQueued = false;
        _renderPumpGeneration++;
        _renderPumpToken++;
        _renderPumpQueuedAt = 0;
        _windowInteractionPaused = false;
        _awaitingStableResizeAfterWindowInteraction = false;
        _resizePendW = 0;
        _resizePendH = 0;
        _renderFairnessYieldQueued = false;
        _renderMaintenanceYieldQueued = false;
        _lastRenderMaintenanceCompletedAtTimestamp = 0;
        _trackingMouseLeave = false;
        _pointerButtonMismatchStartedAtTimestamp = 0;
        _nativeCallCount = 0;
        _slowNativeCallCount = 0;
        _gpuBackpressureYieldCount = 0;
        _gpuBackpressureInputRetryTotal = 0;
        _gpuBackpressureBackgroundFallbackCount = 0;
        _gpuReadyAfterRetryCount = 0;
        _renderFairnessYieldCount = 0;
        _renderInputContinuationYieldCount = 0;
        _renderMaintenanceYieldCount = 0;
        _lastRenderContinuationQueueWaitMilliseconds = 0.0;
        _maximumRenderContinuationQueueWaitMilliseconds = 0.0;
        _lastRenderMaintenanceQueueWaitMilliseconds = 0.0;
        _maximumRenderMaintenanceQueueWaitMilliseconds = 0.0;
        _lastGpuBackpressureEpochMilliseconds = 0.0;
        _maximumGpuBackpressureEpochMilliseconds = 0.0;
        _peakPresentedRenderBurstFrames = 0;
        _peakRenderBurstActiveCpuMilliseconds = 0.0;
        _lastNativeCallMilliseconds = 0.0;
        _maximumNativeCallMilliseconds = 0.0;
        _lastNativeCallKind = string.Empty;
        ResetRenderBurst();
        ResetGpuBackpressureRetryBurst();
        if (beginNativeBootstrap)
        {
            AttachmentFailureDetail = null;
        }
        _abiCapabilities = EditorAbiCapability.None;
        _dormantRenderTimer?.Stop();

        // 子ウィンドウ (予約クラス "STATIC")。DX12 スワップチェインの提示先。
        _hwnd = CreateWindowExW(0, "STATIC", string.Empty,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        _embeddedSurfaceParent = hwndParent.Handle;
        _externalSurfaceParent = IntPtr.Zero;
        _externalSurfaceWidth = 0;
        _externalSurfaceHeight = 0;

        // Loading the editor ABI and creating its global logger, allocator and
        // worker pool may be cold-start expensive. The native host is not yet
        // attached to this HWND, so create it on a worker and publish it only
        // through the current HwndHost generation.
        _engine = IntPtr.Zero;

        // STATIC のウィンドウプロシージャを差し替えてマウス入力 (pick / pan / zoom) を拾う。
        _wndProc = ViewportWndProc;
        _origProc = SetWindowLongPtrW(_hwnd, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(_wndProc));
        DragAcceptFiles(_hwnd, !App.IsNonInteractiveLaunch);

        // WPF's compositor callback is a bootstrap/watchdog only. It never
        // paces an already-running native pump, so monitor refresh cannot cap
        // engine frames. Keeping this wake source makes initial attach robust
        // while the HwndHost is transitioning into the visible tree.
        CompositionTarget.Rendering += OnCompositionWake;
        if (beginNativeBootstrap)
        {
            int loadGeneration = _renderPumpGeneration;
            BeginNativeBootstrap(loadGeneration);
        }
        return new HandleRef(this, _hwnd);
    }

    private void BeginNativeBootstrap(int generation)
    {
        Dispatcher.VerifyAccess();
        _nativeBootstrapCancellation?.Cancel();
        var cancellation = new CancellationTokenSource();
        _nativeBootstrapCancellation = cancellation;
        _ = CompleteNativeBootstrapAsync(
            generation,
            cancellation);
    }

    private async Task CompleteNativeBootstrapAsync(
        int generation,
        CancellationTokenSource cancellation)
    {
        EditorNativeBootstrapResult result =
            await EditorNativeBootstrap.StartAsync(cancellation.Token)
                .ConfigureAwait(false);
        bool adopted = false;
        try
        {
            await Dispatcher.InvokeAsync(
                () =>
                {
                    if (cancellation.IsCancellationRequested ||
                        generation != _renderPumpGeneration ||
                        _destroying ||
                        _hwnd == IntPtr.Zero)
                    {
                        return;
                    }

                    ReleaseNativeBootstrapCancellation(cancellation);

                    _abiCapabilities = result.Abi.Capabilities;
                    if (result.Cancelled)
                        return;
                    if (result.Engine == IntPtr.Zero)
                    {
                        AttachFailed = true;
                        AttachmentFailureDetail =
                            result.FailureDetail ??
                            "Native editor host creation returned a null handle.";
                        SuspendRenderPumpForStartupFailure();
                        AttachmentFailed?.Invoke();
                        return;
                    }

                    _engine = result.Engine;
                    _nativeHostLifetimeLease = result.LifetimeLease;
                    adopted = true;

                    // BuildWindowCore can run before WPF publishes
                    // IsVisible/ActualWidth. Queueing at Loaded priority after
                    // publication avoids a permanently latched private pump
                    // message while preserving the prior attach ordering.
                    QueueRenderPump();
                },
                DispatcherPriority.Loaded);
        }
        catch (TaskCanceledException)
        {
            // Dispatcher shutdown won the publication race.
        }
        catch (InvalidOperationException)
        {
            // Dispatcher teardown can reject a late generation.
        }
        finally
        {
            if (!adopted && result.Engine != IntPtr.Zero)
            {
                await EditorNativeBootstrap.DestroyUnpublishedAsync(result)
                    .ConfigureAwait(false);
            }
            ReleaseNativeBootstrapCancellation(cancellation);
            cancellation.Dispose();
        }
    }

    private void ReleaseNativeBootstrapCancellation(
        CancellationTokenSource cancellation) =>
        Interlocked.CompareExchange(
            ref _nativeBootstrapCancellation,
            null,
            cancellation);

    internal void ResumeRenderingAfterSceneLoad()
    {
        Dispatcher.VerifyAccess();
        if (_destroying || _engine == IntPtr.Zero) return;
        _redrawPending = true;
        QueueRenderPump();
    }

    /// <summary>
    /// Reparents only the already-created native render child into an owned
    /// floating window. The HwndHost and native editor engine stay alive in the
    /// main visual tree; no second renderer, scene clone, or swapchain host is
    /// created.
    /// </summary>
    internal bool TryFloatRenderSurface(
        IntPtr externalParent,
        Int32Rect physicalClientBounds)
    {
        Dispatcher.VerifyAccess();
        if (_destroying ||
            _hwnd == IntPtr.Zero ||
            externalParent == IntPtr.Zero ||
            physicalClientBounds.Width <= 0 ||
            physicalClientBounds.Height <= 0)
        {
            return false;
        }
        if (_externalSurfaceParent == externalParent)
            return UpdateFloatingRenderSurfaceBounds(physicalClientBounds);
        if (_externalSurfaceParent != IntPtr.Zero ||
            _embeddedSurfaceParent == IntPtr.Zero)
        {
            return false;
        }

        IntPtr previousParent = SetParent(_hwnd, externalParent);
        if (previousParent == IntPtr.Zero)
            return false;

        _externalSurfaceParent = externalParent;
        if (!UpdateFloatingRenderSurfaceBounds(physicalClientBounds))
        {
            bool rollbackSucceeded =
                SetParent(_hwnd, _embeddedSurfaceParent) != IntPtr.Zero;
            if (RenderSurfaceTransferPolicy.AfterFailedExternalPosition(
                    rollbackSucceeded) == RenderSurfaceOwnership.Embedded)
            {
                _externalSurfaceParent = IntPtr.Zero;
                _externalSurfaceWidth = 0;
                _externalSurfaceHeight = 0;
                PositionEmbeddedRenderSurface();
            }
            return false;
        }

        RequestRedraw();
        QueueRenderPump();
        return true;
    }

    internal bool UpdateFloatingRenderSurfaceBounds(
        Int32Rect physicalClientBounds)
    {
        Dispatcher.VerifyAccess();
        if (_destroying ||
            _hwnd == IntPtr.Zero ||
            _externalSurfaceParent == IntPtr.Zero ||
            physicalClientBounds.Width <= 0 ||
            physicalClientBounds.Height <= 0)
        {
            return false;
        }

        if (!SetWindowPos(
                _hwnd,
                IntPtr.Zero,
                physicalClientBounds.X,
                physicalClientBounds.Y,
                physicalClientBounds.Width,
                physicalClientBounds.Height,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW))
        {
            return false;
        }

        _externalSurfaceWidth =
            checked((uint)physicalClientBounds.Width);
        _externalSurfaceHeight =
            checked((uint)physicalClientBounds.Height);
        RequestRedraw();
        QueueRenderPump();
        return true;
    }

    internal bool TryDockRenderSurface()
    {
        Dispatcher.VerifyAccess();
        if (_externalSurfaceParent == IntPtr.Zero)
            return true;
        if (_destroying ||
            _hwnd == IntPtr.Zero ||
            _embeddedSurfaceParent == IntPtr.Zero)
        {
            return false;
        }

        IntPtr previousParent = SetParent(_hwnd, _embeddedSurfaceParent);
        if (previousParent == IntPtr.Zero)
            return false;

        _externalSurfaceParent = IntPtr.Zero;
        _externalSurfaceWidth = 0;
        _externalSurfaceHeight = 0;
        PositionEmbeddedRenderSurface();
        RequestRedraw();
        QueueRenderPump();
        return true;
    }

    internal bool IsRenderSurfaceFloating =>
        _externalSurfaceParent != IntPtr.Zero;

    private void PositionEmbeddedRenderSurface()
    {
        if (_hwnd == IntPtr.Zero ||
            _embeddedSurfaceParent == IntPtr.Zero ||
            !IsLoaded)
        {
            return;
        }

        try
        {
            Point screenOrigin = PointToScreen(new Point(0.0, 0.0));
            var clientOrigin = new POINT
            {
                X = checked((int)Math.Round(screenOrigin.X)),
                Y = checked((int)Math.Round(screenOrigin.Y)),
            };
            if (!ScreenToClient(_embeddedSurfaceParent, ref clientOrigin))
                return;
            DpiScale dpi = VisualTreeHelper.GetDpi(this);
            int width = Math.Max(
                1,
                checked((int)Math.Round(ActualWidth * dpi.DpiScaleX)));
            int height = Math.Max(
                1,
                checked((int)Math.Round(ActualHeight * dpi.DpiScaleY)));
            _ = SetWindowPos(
                _hwnd,
                IntPtr.Zero,
                clientOrigin.X,
                clientOrigin.Y,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        catch (InvalidOperationException)
        {
            // The HwndHost is already reparented correctly. A later WPF layout
            // pass will publish its embedded bounds.
        }
        catch (OverflowException)
        {
            // Hostile/transient WPF geometry must not undo truthful ownership.
        }
    }

    protected override void OnWindowPositionChanged(Rect rcBoundingBox)
    {
        // HwndHost assumes its child is parented to the containing HwndSource.
        // While floating, the owned Camera View window owns that child and
        // positions it explicitly in physical pixels.
        if (_externalSurfaceParent == IntPtr.Zero)
            base.OnWindowPositionChanged(rcBoundingBox);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        Dispatcher.VerifyAccess();
        if (_destroying && !_destroyDeferred) return;

        _destroying = true;
        _redrawPending = false;
        CompositionTarget.Rendering -= OnCompositionWake;
        StopRenderPump();

        // Native render/attach can pump window messages. Destruction requested from
        // such a re-entrant callback must wait until both managed entry points unwind,
        // otherwise the engine or HWND can be freed underneath the active call.
        if (_frameActive || _wndProcDepth != 0)
        {
            _destroyDeferred = true;
            return;
        }

        CompleteDestroy();
    }

    private void CompleteDestroy()
    {
        if (!_destroying || _frameActive || _wndProcDepth != 0)
        {
            _destroyDeferred = true;
            return;
        }

        _destroyDeferred = false;
        _nativeBootstrapCancellation?.Cancel();
        IntPtr hwnd = _hwnd;
        IntPtr engine = _engine;

        // Stop future callbacks first. Publish a null engine before native teardown
        // so any nested managed observer sees a dead handle rather than stale memory.
        if (hwnd != IntPtr.Zero)
        {
            DragAcceptFiles(hwnd, false);
            if (_origProc != IntPtr.Zero)
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, _origProc);
            if (GetCapture() == hwnd)
                ReleaseCapture();
        }
        _origProc = IntPtr.Zero;
        _engine = IntPtr.Zero;
        _abiCapabilities = EditorAbiCapability.None;
        _hwnd = IntPtr.Zero;
        _embeddedSurfaceParent = IntPtr.Zero;
        _externalSurfaceParent = IntPtr.Zero;
        _externalSurfaceWidth = 0;
        _externalSurfaceHeight = 0;
        _attached = false;
        _windowInteractionPaused = false;
        _awaitingStableResizeAfterWindowInteraction = false;
        _resizePendW = 0;
        _resizePendH = 0;
        _panning = false;
        _gizmoDragging = false;
        _giz3dDragging = false;
        _marqueeDragging = false;
        _finalizing = false;
        _trackingMouseLeave = false;
        _pointerButtonMismatchStartedAtTimestamp = 0;

        try
        {
            if (engine != IntPtr.Zero)
                EngineInterop.acs_editor_destroy(engine);
        }
        finally
        {
            _nativeHostLifetimeLease?.Release();
            _nativeHostLifetimeLease = null;
        }
        if (hwnd != IntPtr.Zero) DestroyWindow(hwnd);
        _wndProc = null;
    }

    private void CompleteDeferredDestroyIfReady()
    {
        if (_destroyDeferred && !_frameActive && _wndProcDepth == 0)
            CompleteDestroy();
    }

    private void RequestRedraw()
    {
        if (!_destroying) _redrawPending = true;
    }

    private bool IsContinuousRenderEligible()
    {
        Window? owner = Window.GetWindow(this);
        bool minimized = owner?.WindowState == WindowState.Minimized;
        bool renderSurfaceVisible = IsRenderSurfaceVisible(
            _hwnd != IntPtr.Zero && IsWindowVisible(_hwnd),
            _hiddenStartupRenderingAllowed);
        return ShouldRenderContinuously(
            _destroying,
            RenderPumpSuspended,
            _engine != IntPtr.Zero && _hwnd != IntPtr.Zero,
            renderSurfaceVisible,
            minimized,
            ActualWidth,
            ActualHeight);
    }

    internal void SetHiddenStartupRenderingAllowed(bool allowed)
    {
        Dispatcher.VerifyAccess();
        if (_hiddenStartupRenderingAllowed == allowed)
            return;

        _hiddenStartupRenderingAllowed = allowed;
        _lastRenderMaintenanceCompletedAtTimestamp = allowed
            ? 0
            : System.Diagnostics.Stopwatch.GetTimestamp();
        if (allowed)
        {
            RequestRedraw();
            QueueRenderPump();
        }
    }

    private void OnCompositionWake(object? sender, EventArgs e)
    {
        Dispatcher.VerifyAccess();
        if (_destroying || RenderPumpSuspended ||
            _engine == IntPtr.Zero || _hwnd == IntPtr.Zero)
        {
            return;
        }

        bool eligible = IsContinuousRenderEligible();
        bool queuedPumpStale = IsQueuedPumpStale(
            _renderPumpQueued,
            _renderPumpQueuedAt,
            Environment.TickCount64);
        if (!ShouldRecoverRenderPumpFromComposition(
                _frameActive,
                eligible,
                _renderPumpQueued,
                queuedPumpStale))
        {
            if (!eligible) ArmDormantRenderTimer();
            return;
        }

        if (_renderPumpQueued)
        {
            // A successfully posted message can still be discarded while an
            // HwndHost is reparented. Invalidate its token so a late delivery
            // is harmless, then recover from the compositor watchdog.
            _renderPumpQueued = false;
            _renderPumpToken++;
        }

        // CompositionTarget.Rendering is a wake signal only. Posting the one
        // private token never runs native work at compositor priority.
        ContinueRenderPumpInputAware();
    }

    private void QueueRenderPump()
    {
        Dispatcher.VerifyAccess();
        if (_renderPumpQueued ||
            IsRenderPumpContinuationBlocked(
                _renderFairnessYieldQueued,
                _renderMaintenanceYieldQueued,
                _frameActive) ||
            _destroying ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return;
        }

        if (!IsContinuousRenderEligible())
        {
            ResetRenderBurst();
            ResetGpuBackpressureRetryBurst();
            ArmDormantRenderTimer();
            return;
        }

        ArmDormantRenderTimer();
        _renderPumpQueued = true;
        _renderPumpQueuedAt = Environment.TickCount64;
        int token = ++_renderPumpToken;
        if (!PostMessageW(
                _hwnd,
                WM_RENDER_PUMP,
                (IntPtr)token,
                IntPtr.Zero))
        {
            _renderPumpQueued = false;
            ArmDormantRenderTimer();
        }
    }

    private void ScheduleNextRenderPumpAfterFrame(
        bool presented,
        double activeCpuMilliseconds)
    {
        Dispatcher.VerifyAccess();
        if (_frameActive || _destroying || RenderPumpSuspended ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return;
        }

        _renderBurstState = AccountRenderBurstAttempt(
            _renderBurstState,
            presented,
            gpuBackpressure: false,
            activeCpuMilliseconds);
        _peakPresentedRenderBurstFrames = Math.Max(
            _peakPresentedRenderBurstFrames,
            _renderBurstState.PresentedFrames);
        _peakRenderBurstActiveCpuMilliseconds = Math.Max(
            _peakRenderBurstActiveCpuMilliseconds,
            _renderBurstState.ActiveCpuMilliseconds);

        if (RequiresRenderDispatcherCheckpoint(_renderBurstState))
        {
            ResetRenderBurst();
            if (!QueueRenderMaintenanceIfDue())
                QueueRenderInputContinuation();
            return;
        }

        QueueRenderPump();
    }

    private void ScheduleNextRenderPumpAfterGpuBackpressure()
    {
        Dispatcher.VerifyAccess();
        if (_frameActive || _destroying || RenderPumpSuspended ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return;
        }

        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        if (_gpuBackpressureStartedAtTimestamp == 0)
            _gpuBackpressureStartedAtTimestamp = now;
        double elapsedMilliseconds =
            (now - _gpuBackpressureStartedAtTimestamp) * 1000.0 /
            System.Diagnostics.Stopwatch.Frequency;
        ViewportGpuBackpressureResumeMode resumeMode =
            SelectGpuBackpressureResume(
                _gpuBackpressureInputRetryCount,
                elapsedMilliseconds);
        if (resumeMode ==
            ViewportGpuBackpressureResumeMode.CooperativeYield)
        {
            ObserveGpuBackpressureEpoch(
                now,
                readyAfterRetry: false);
            ResetGpuBackpressureRetryBurst();
            if (QueueRenderMaintenanceIfDue())
            {
                _gpuBackpressureBackgroundFallbackCount++;
                return;
            }
            // The bounded GPU-busy epoch is also a mandatory Dispatcher
            // checkpoint. In particular, unattended rendering must not reset
            // the epoch and immediately start another private-message chain.
            QueueRenderInputContinuation();
            return;
        }

        _gpuBackpressureInputRetryCount++;
        _gpuBackpressureInputRetryTotal++;
        if (QueueRenderMaintenanceIfDue())
            return;
        ContinueRenderPumpInputAware();
    }

    private void ContinueRenderPumpInputAware()
    {
        Dispatcher.VerifyAccess();
        if (HasPendingInteractiveInput() &&
            QueueRenderInputContinuation())
        {
            return;
        }
        QueueRenderPump();
    }

    private bool QueueRenderInputContinuation()
    {
        Dispatcher.VerifyAccess();
        if (_renderFairnessYieldQueued || _destroying ||
            RenderPumpSuspended ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return false;
        }

        long queuedAtTimestamp =
            System.Diagnostics.Stopwatch.GetTimestamp();
        _renderFairnessYieldQueued = true;
        _renderFairnessYieldCount++;
        _renderInputContinuationYieldCount++;
        int generation = _renderPumpGeneration;
        _ = Dispatcher.BeginInvoke(
            RenderContinuationPriority,
            new Action(() =>
            {
                if (generation != _renderPumpGeneration)
                    return;

                long resumedAtTimestamp =
                    System.Diagnostics.Stopwatch.GetTimestamp();
                double queueWaitMilliseconds =
                    (resumedAtTimestamp - queuedAtTimestamp) *
                    1000.0 /
                    System.Diagnostics.Stopwatch.Frequency;
                if (!double.IsFinite(queueWaitMilliseconds) ||
                    queueWaitMilliseconds < 0.0)
                {
                    queueWaitMilliseconds = 0.0;
                }
                _lastRenderContinuationQueueWaitMilliseconds =
                    queueWaitMilliseconds;
                _maximumRenderContinuationQueueWaitMilliseconds =
                    Math.Max(
                        _maximumRenderContinuationQueueWaitMilliseconds,
                        queueWaitMilliseconds);
                _renderFairnessYieldQueued = false;
                QueueRenderPump();
            }));
        return true;
    }

    private bool QueueRenderMaintenanceIfDue()
    {
        Dispatcher.VerifyAccess();
        if (_renderMaintenanceYieldQueued || _destroying ||
            RenderPumpSuspended ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return false;
        }

        long queuedAtTimestamp =
            System.Diagnostics.Stopwatch.GetTimestamp();
        double millisecondsSinceMaintenance =
            _lastRenderMaintenanceCompletedAtTimestamp == 0
                ? double.PositiveInfinity
                : (queuedAtTimestamp -
                   _lastRenderMaintenanceCompletedAtTimestamp) *
                  1000.0 /
                  System.Diagnostics.Stopwatch.Frequency;
        ViewportRenderYieldMode yieldMode = SelectRenderYieldMode(
            _hiddenStartupRenderingAllowed,
            millisecondsSinceMaintenance);
        if (yieldMode != ViewportRenderYieldMode.BackgroundMaintenance)
            return false;

        // This lower-frequency checkpoint is queued behind existing Background
        // work. It deliberately owns the next frame: without occasionally
        // draining below Input priority, startup finalization and Dispatcher
        // timer promotion can remain starved even though Input checkpoints run.
        _renderMaintenanceYieldQueued = true;
        _renderFairnessYieldCount++;
        _renderMaintenanceYieldCount++;
        int generation = _renderPumpGeneration;
        _ = Dispatcher.BeginInvoke(
            RenderMaintenancePriority,
            new Action(() =>
            {
                if (generation != _renderPumpGeneration)
                    return;

                long resumedAtTimestamp =
                    System.Diagnostics.Stopwatch.GetTimestamp();
                double queueWaitMilliseconds =
                    (resumedAtTimestamp - queuedAtTimestamp) *
                    1000.0 /
                    System.Diagnostics.Stopwatch.Frequency;
                if (!double.IsFinite(queueWaitMilliseconds) ||
                    queueWaitMilliseconds < 0.0)
                {
                    queueWaitMilliseconds = 0.0;
                }
                _lastRenderMaintenanceQueueWaitMilliseconds =
                    queueWaitMilliseconds;
                _maximumRenderMaintenanceQueueWaitMilliseconds =
                    Math.Max(
                        _maximumRenderMaintenanceQueueWaitMilliseconds,
                        queueWaitMilliseconds);
                _lastRenderMaintenanceCompletedAtTimestamp =
                    resumedAtTimestamp;
                _renderMaintenanceYieldQueued = false;
                QueueRenderPump();
            }));
        return true;
    }

    private void ResetRenderBurst()
    {
        _renderBurstState = default;
    }

    private void ResetGpuBackpressureRetryBurst()
    {
        _gpuBackpressureInputRetryCount = 0;
        _gpuBackpressureStartedAtTimestamp = 0;
    }

    private void CompleteGpuBackpressureEpoch(bool readyAfterRetry)
    {
        if (_gpuBackpressureStartedAtTimestamp != 0)
        {
            ObserveGpuBackpressureEpoch(
                System.Diagnostics.Stopwatch.GetTimestamp(),
                readyAfterRetry);
        }
        ResetGpuBackpressureRetryBurst();
    }

    private void ObserveGpuBackpressureEpoch(
        long completedAtTimestamp,
        bool readyAfterRetry)
    {
        if (_gpuBackpressureStartedAtTimestamp == 0)
            return;

        double elapsedMilliseconds =
            (completedAtTimestamp - _gpuBackpressureStartedAtTimestamp) *
            1000.0 / System.Diagnostics.Stopwatch.Frequency;
        if (!double.IsFinite(elapsedMilliseconds) ||
            elapsedMilliseconds < 0.0)
        {
            elapsedMilliseconds = 0.0;
        }
        _lastGpuBackpressureEpochMilliseconds = elapsedMilliseconds;
        _maximumGpuBackpressureEpochMilliseconds = Math.Max(
            _maximumGpuBackpressureEpochMilliseconds,
            elapsedMilliseconds);
        if (readyAfterRetry && _gpuBackpressureInputRetryCount > 0)
            _gpuReadyAfterRetryCount++;
    }

    private void ArmDormantRenderTimer()
    {
        if (_destroying || RenderPumpSuspended ||
            Dispatcher.HasShutdownStarted ||
            Dispatcher.HasShutdownFinished)
        {
            _dormantRenderTimer?.Stop();
            return;
        }

        if (_dormantRenderTimer is null)
        {
            _dormantRenderTimer = new DispatcherTimer(
                TimeSpan.FromMilliseconds(100),
                DormantWakePriority,
                (_, _) =>
                {
                    if (_destroying)
                    {
                        _dormantRenderTimer?.Stop();
                        return;
                    }

                    // Native attach/render can enter a nested Windows message
                    // pump. Never create a private render token inside the
                    // active native frame; its outer handler owns continuation.
                    if (_frameActive)
                        return;

                    if (IsQueuedPumpStale(
                            _renderPumpQueued,
                            _renderPumpQueuedAt,
                            Environment.TickCount64))
                    {
                        _renderPumpQueued = false;
                        _renderPumpToken++;
                    }

                    if (!_renderPumpQueued &&
                        IsContinuousRenderEligible())
                    {
                        QueueRenderPump();
                    }
                },
                Dispatcher);
        }
        _dormantRenderTimer.Start();
    }

    private void StopRenderPump()
    {
        Dispatcher.VerifyAccess();
        _renderPumpGeneration++;
        _renderPumpToken++;
        _renderPumpQueued = false;
        _renderPumpQueuedAt = 0;
        _renderFairnessYieldQueued = false;
        _renderMaintenanceYieldQueued = false;
        ResetRenderBurst();
        ResetGpuBackpressureRetryBurst();
        _dormantRenderTimer?.Stop();
    }

    /// <summary>
    /// Stops every queued/native frame without destroying the host. Used when
    /// startup has failed so the UI does not spin an uncapped no-op pump.
    /// </summary>
    internal void SuspendRenderPumpForStartupFailure()
    {
        Dispatcher.VerifyAccess();
        if (_destroying) return;
        _renderPumpSuspended = true;
        StopRenderPump();
    }

    /// <summary>
    /// Pauses native frames while the top-level window is in the Win32
    /// move/size modal loop. This prevents child WM_APP traffic and repeated
    /// swapchain WaitIdle/resize work from competing with window movement.
    /// </summary>
    internal void PauseRenderPumpForWindowInteraction()
    {
        Dispatcher.VerifyAccess();
        if (_destroying || _windowInteractionPaused) return;
        _windowInteractionPaused = true;
        _awaitingStableResizeAfterWindowInteraction = true;
        _resizePendW = 0;
        _resizePendH = 0;
        CancelPointerInteraction();
        StopRenderPump();
    }

    /// <summary>
    /// Resumes after WM_EXITSIZEMOVE. Rendering waits for two identical WPF
    /// size observations, then applies the final dimensions exactly once.
    /// </summary>
    internal void ResumeRenderPumpAfterWindowInteraction()
    {
        Dispatcher.VerifyAccess();
        if (!_windowInteractionPaused) return;
        _windowInteractionPaused = false;
        if (!_destroying) QueueRenderPump();
    }

    /// <summary>Releases a viewport-owned Win32 capture and lets WM_CAPTURECHANGED tear down the gesture.</summary>
    internal void CancelPointerInteraction()
    {
        Dispatcher.VerifyAccess();
        EndWaterPointer();
        _pointerButtonMismatchStartedAtTimestamp = 0;
        if (_hwnd != IntPtr.Zero && GetCapture() == _hwnd)
            ReleaseCapture();
    }

    private WaterPointerRoutingState WaterPointerState(bool view3d) =>
        new(
            EngineReady: _engine != IntPtr.Zero && !_destroying,
            View3D: view3d,
            PolygonMode: PolyMode,
            GizmoDragging: _gizmoDragging,
            Gizmo3DDragging: _giz3dDragging,
            Panning: _panning,
            MarqueeDragging: _marqueeDragging);

    private void RouteWaterPointer(
        WaterPointerRoutingDecision decision,
        int x = 0,
        int y = 0)
    {
        System.Diagnostics.Debug.Assert(
            !decision.CapturePointer,
            "Interactive water must never own mouse capture.");
        if (!decision.ShouldRoute || _engine == IntPtr.Zero || _destroying)
            return;
        EngineInterop.acs_editor_water3d_pointer_event(
            _engine,
            x,
            y,
            (int)decision.Action);
    }

    private void EndWaterPointer() =>
        RouteWaterPointer(
            WaterPointerRoutingPolicy.ForEnd(
                _engine != IntPtr.Zero && !_destroying));

    private void EnsureMouseLeaveTracking(IntPtr hwnd)
    {
        if (_trackingMouseLeave || hwnd == IntPtr.Zero) return;
        var tracking = new TRACKMOUSEEVENT
        {
            cbSize = (uint)Marshal.SizeOf<TRACKMOUSEEVENT>(),
            dwFlags = TME_LEAVE,
            hwndTrack = hwnd,
            dwHoverTime = 0,
        };
        _trackingMouseLeave = TrackMouseEvent(ref tracking);
    }

    private void BeginPointerCapture(IntPtr hwnd)
    {
        SetCapture(hwnd);
        _pointerButtonMismatchStartedAtTimestamp = 0;
    }

    private static bool IsPhysicalMouseButtonDown(int virtualKey) =>
        (GetAsyncKeyState(virtualKey) & 0x8000) != 0;

    /// <summary>
    /// Recovers a capture whose button-up was lost while the dispatcher was
    /// busy or ownership changed. A captured child HWND receives title-bar
    /// clicks as client input, which makes the whole editor appear immovable.
    /// The grace period plus physical-button check preserves legitimate drags.
    /// ReleaseCapture synchronously routes WM_CAPTURECHANGED through the normal
    /// gesture teardown path on this UI thread.
    /// </summary>
    private int PhysicalPointerButtonMask(int activeButtonMask)
    {
        int mask = 0;
        if ((activeButtonMask & PointerButtonLeftMask) != 0 &&
            IsPhysicalMouseButtonDown(VK_LBUTTON))
        {
            mask |= PointerButtonLeftMask;
        }
        if ((activeButtonMask & PointerButtonRightMask) != 0 &&
            IsPhysicalMouseButtonDown(VK_RBUTTON))
        {
            mask |= PointerButtonRightMask;
        }
        if ((activeButtonMask & PointerButtonMiddleMask) != 0 &&
            IsPhysicalMouseButtonDown(VK_MBUTTON))
        {
            mask |= PointerButtonMiddleMask;
        }
        return mask;
    }

    internal ViewportPointerCaptureDiagnostic GetPointerCaptureDiagnostic()
    {
        Dispatcher.VerifyAccess();
        int activeButtonMask = ActivePointerButtonMask(
            _gizmoDragging,
            _giz3dDragging,
            _marqueeDragging,
            _panning,
            _panMode);
        int physicallyDownButtonMask =
            PhysicalPointerButtonMask(activeButtonMask);
        double mismatchAgeMilliseconds = 0;
        if (_pointerButtonMismatchStartedAtTimestamp > 0)
        {
            long now = System.Diagnostics.Stopwatch.GetTimestamp();
            mismatchAgeMilliseconds = Math.Max(
                0,
                (now - _pointerButtonMismatchStartedAtTimestamp) * 1000.0 /
                System.Diagnostics.Stopwatch.Frequency);
        }
        return new(
            _hwnd != IntPtr.Zero && GetCapture() == _hwnd,
            activeButtonMask,
            physicallyDownButtonMask,
            mismatchAgeMilliseconds);
    }

    /// <summary>
    /// Periodic recovery path for a capture whose final child-HWND message was
    /// lost. The interaction heartbeat invokes this even while no pointer
    /// message is arriving, which is the exact case where the WndProc-only
    /// recovery path could leave the editor title bar unreachable.
    /// </summary>
    internal bool MaintainPointerCapture(bool ownerClosing)
    {
        Dispatcher.VerifyAccess();
        return RecoverStalePointerCaptureIfNeeded(
            currentMessage: 0,
            ownerClosing: ownerClosing);
    }

    private bool RecoverStalePointerCaptureIfNeeded(
        int currentMessage,
        bool ownerClosing = false)
    {
        int generation = _renderPumpGeneration;
        IntPtr hwnd = _hwnd;
        int activeButtonMask = ActivePointerButtonMask(
            _gizmoDragging,
            _giz3dDragging,
            _marqueeDragging,
            _panning,
            _panMode);

        // The matching button-up is the commit path for gizmos and marquee
        // selection. It must run before any recovery decision, even when the
        // physical button is already up and a render token was queued first.
        if (_destroying || ownerClosing || hwnd == IntPtr.Zero ||
            activeButtonMask == 0 ||
            IsInitiatingPointerButtonUpMessage(currentMessage, activeButtonMask))
        {
            _pointerButtonMismatchStartedAtTimestamp = 0;
            return false;
        }

        bool ownsCapture = GetCapture() == hwnd;
        int physicallyDownButtonMask = PhysicalPointerButtonMask(activeButtonMask);
        bool mismatch = ownsCapture &&
            !_windowInteractionPaused &&
            !_finalizing &&
            (activeButtonMask & physicallyDownButtonMask) == 0;
        if (!mismatch)
        {
            _pointerButtonMismatchStartedAtTimestamp = 0;
            return false;
        }

        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        if (_pointerButtonMismatchStartedAtTimestamp <= 0)
        {
            _pointerButtonMismatchStartedAtTimestamp = Math.Max(1L, now);
            return false;
        }

        double mismatchAgeMilliseconds =
            (now - _pointerButtonMismatchStartedAtTimestamp) * 1000.0 /
            System.Diagnostics.Stopwatch.Frequency;
        if (!ShouldRecoverStalePointerCapture(
                ownsCapture,
                _windowInteractionPaused,
                _finalizing,
                activeButtonMask,
                physicallyDownButtonMask,
                currentMessage,
                mismatchAgeMilliseconds,
                _destroying,
                ownerClosing,
                generation == _renderPumpGeneration))
        {
            return false;
        }

        // Revalidate the captured HWND/generation immediately before release.
        // ReleaseCapture synchronously re-enters WM_CAPTURECHANGED, so no
        // stale generation may be allowed to tear down a newer gesture.
        if (_destroying ||
            ownerClosing ||
            generation != _renderPumpGeneration ||
            hwnd == IntPtr.Zero ||
            hwnd != _hwnd ||
            GetCapture() != hwnd)
        {
            return false;
        }
        _pointerButtonMismatchStartedAtTimestamp = 0;
        _finalizing = false;
        return ReleaseCapture();
    }

    /// <summary>
    /// Explicitly retries a failed HWND attach. A failure never retries by
    /// itself; callers must opt in through this method.
    /// </summary>
    internal bool RetryAttach()
    {
        Dispatcher.VerifyAccess();
        if (!CanExplicitlyRetryAttach(
                _destroying,
                _attached,
                AttachFailed,
                _renderPumpSuspended,
                _hwnd != IntPtr.Zero))
        {
            return false;
        }

        AttachFailed = false;
        AttachmentFailureDetail = null;
        _renderPumpSuspended = false;
        _pendW = 0;
        _pendH = 0;
        _resizePendW = 0;
        _resizePendH = 0;
        _awaitingStableResizeAfterWindowInteraction = false;
        _redrawPending = true;
        if (_engine == IntPtr.Zero)
        {
            BeginNativeBootstrap(_renderPumpGeneration);
            return true;
        }
        QueueRenderPump();
        return true;
    }

    private IntPtr CallOriginalWindowProc(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam) =>
        _origProc != IntPtr.Zero
            ? CallWindowProcW(_origProc, hWnd, msg, wParam, lParam)
            : DefWindowProcW(hWnd, msg, wParam, lParam);

    // ビューポートのマウス: 左クリック=pick、右/中ドラッグ=pan、ホイール=zoom。
    private IntPtr ViewportWndProc(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam)
    {
        _wndProcDepth++;
        try
        {
          if (_destroying)
              return CallOriginalWindowProc(hWnd, msg, wParam, lParam);

          // --unattended is used for input-free screenshots and secondary-
          // monitor validation.  A child HWND can still receive mouse messages
          // even when its top-level WPF window has WS_EX_NOACTIVATE, so gate the
          // input at the native boundary before any pick, camera, gizmo, game
          // input, drag/drop, or SetCapture path can run.
          if (EditorInputGate.ShouldSuppressNativeMessage(
                  App.IsNonInteractiveLaunch, msg))
          {
              return IntPtr.Zero;
          }

          RouteWaterPointer(
              WaterPointerRoutingPolicy.ForWindowMessage(
                  msg,
                  _engine != IntPtr.Zero && !_destroying));
          RecoverStalePointerCaptureIfNeeded(msg);

          switch (msg)
          {
            case WM_RENDER_PUMP:
            {
                int token = unchecked((int)wParam.ToInt64());
                if (token != _renderPumpToken || !_renderPumpQueued)
                    return IntPtr.Zero;

                _renderPumpQueued = false;
                if (!IsContinuousRenderEligible())
                {
                    ArmDormantRenderTimer();
                    return IntPtr.Zero;
                }

                long renderAttemptStartedAt =
                    System.Diagnostics.Stopwatch.GetTimestamp();
                bool gpuBackpressure =
                    RenderOneFrame(out bool presented);
                double renderAttemptActiveCpuMilliseconds =
                    (System.Diagnostics.Stopwatch.GetTimestamp() -
                     renderAttemptStartedAt) *
                    1000.0 / System.Diagnostics.Stopwatch.Frequency;
                // Keep exactly one Win32 token in flight. Successful frames
                // and GPU-busy retries may continue directly only inside their
                // bounded epochs. Every deadline inserts a Dispatcher
                // checkpoint; real queued input can request one earlier.
                if (gpuBackpressure)
                {
                    _gpuBackpressureYieldCount++;
                    ScheduleNextRenderPumpAfterGpuBackpressure();
                }
                else
                {
                    CompleteGpuBackpressureEpoch(
                        readyAfterRetry: presented);
                    ScheduleNextRenderPumpAfterFrame(
                        presented,
                        renderAttemptActiveCpuMilliseconds);
                }
                return IntPtr.Zero;
            }

            case WM_CANCELMODE:
                CancelPointerInteraction();
                ResetGameInput();
                break;

            case WM_KILLFOCUS:
                ResetGameInput();
                break;

            case WM_NCHITTEST:
                return (IntPtr)HTCLIENT;   // STATIC を不透明にしてマウスを受け取る

            case WM_DROPFILES:
            {
                IntPtr hDrop = wParam;
                POINT dp = default;
                DragQueryPoint(hDrop, ref dp);                        // ドロップ点 (クライアント座標)
                uint n = DragQueryFileW(hDrop, 0xFFFFFFFF, null, 0);  // ファイル数
                for (uint i = 0; i < n; i++)
                {
                    var sb = new System.Text.StringBuilder(520);
                    DragQueryFileW(hDrop, i, sb, (uint)sb.Capacity);
                    AssetDropped?.Invoke(sb.ToString(), dp.X, dp.Y);
                }
                DragFinish(hDrop);
                return IntPtr.Zero;
            }

            case WM_KEYDOWN:
                // 描画モード中の Enter(0x0D)/Esc(0x1B) でポリゴン確定。
                if (PolyMode)
                {
                    long vk = wParam.ToInt64();
                    if (vk == 0x0D || vk == 0x1B) { PolyKeyFinalize?.Invoke(); break; }
                }
                break;

            case WM_LBUTTONDOWN:
                FeedMouseButton(0, 1);   // Play 中なら game へ転送 (Play 外は no-op)
                if (!CanRouteEditorViewportInteraction())
                    break;
                // 3D ビューポート: まず変形ギズモを掴めるか試し、掴めなければレイピック。
                if (_engine != IntPtr.Zero && EngineInterop.acs_editor_get_view3d(_engine) != 0)
                {
                    int gx = LoWord(lParam), gy = HiWord(lParam);
                    if (PolyMode)   // Ortho ポリゴン描画: クリックを z=0 平面へ逆射影して頂点を置く
                    {
                        EngineInterop.acs_editor_poly3d_add_point(_engine, gx, gy);
                        break;
                    }
                    bool gizmoAccepted =
                        EngineInterop.acs_editor_gizmo3d_begin(
                            _engine, gx, gy) != 0;
                    WaterPointerRoutingDecision waterPress =
                        WaterPointerRoutingPolicy.ForPress(
                            WaterPointerState(view3d: true),
                            gizmoAccepted);
                    if (gizmoAccepted)
                    {
                        EndWaterPointer();
                        _giz3dDragging = true; BeginPointerCapture(hWnd);
                    }
                    else
                    {
                        // Observe the existing selection gesture without
                        // taking capture or replacing the normal pick.
                        RouteWaterPointer(waterPress, gx, gy);
                        int p3 = EngineInterop.acs_editor_pick3d(_engine, gx, gy);
                        Picked?.Invoke(p3);
                    }
                    break;
                }
                // ポリゴン描画モード: クリックで点を置く (ピック/ギズモはしない)。
                if (_engine != IntPtr.Zero && PolyMode)
                {
                    EngineInterop.acs_editor_poly_add_point(_engine, LoWord(lParam), HiWord(lParam));
                    break;
                }
                // パン中/ドラッグ中はギズモ/ピックを始めない (capture とフラグの二重所有を防ぐ)。
                if (_engine != IntPtr.Zero && !_panning && !_gizmoDragging && !_marqueeDragging)
                {
                    int x = LoWord(lParam), y = HiWord(lParam);
                    // ダブルクリック検出: 直前クリックから DblClkTime 以内 & 近接 → ノードへフォーカス。
                    int tick = Environment.TickCount;
                    bool dbl = (uint)(tick - _lastClickTick) <= GetDoubleClickTime()
                               && Math.Abs(x - _lastClickX) <= 4 && Math.Abs(y - _lastClickY) <= 4;
                    _lastClickTick = tick; _lastClickX = x; _lastClickY = y;
                    if (dbl)
                    {
                        int picked = EngineInterop.acs_editor_pick(_engine, x, y);
                        if (picked >= 0)
                        {
                            EngineInterop.acs_editor_select(_engine, picked);
                            EngineInterop.acs_editor_camera_focus(_engine);
                            Picked?.Invoke(picked);
                        }
                        break;   // 2 回目のクリックでギズモ/マーキーを始めない
                    }
                    // まず選択ノードのギズモハンドルを掴めるか試す。掴めなければピック。
                    if (EngineInterop.acs_editor_gizmo_begin(_engine, x, y) != 0)
                    {
                        EndWaterPointer();
                        _gizmoDragging = true; BeginPointerCapture(hWnd);
                    }
                    else
                    {
                        // ピック: Ctrl+クリックで選択トグル、通常クリックで単一選択。
                        // 空クリックは通常なら全解除 (Ctrl+空は集合を保持)。選択集合は ABI が
                        // 真実点なので、C# 側は Picked を受けて ABI から読み直す。
                        bool ctrl = CtrlDown;
                        int id = EngineInterop.acs_editor_pick(_engine, x, y);
                        if (id >= 0)
                        {
                            if (ctrl) EngineInterop.acs_editor_select_toggle(_engine, id);
                            else      EngineInterop.acs_editor_select(_engine, id);
                            Picked?.Invoke(id);
                        }
                        else
                        {
                            // 空クリック → ラバーバンド選択を開始 (click か drag かは LBUTTONUP で判定)。
                            // additive (Ctrl) は gesture 開始時に latch する (mouse-up 時の再読みは
                            // drag 途中の Ctrl 離し/押しで意図が反転するため)。
                            _marqueeDragging = true;
                            _marqStartX = x; _marqStartY = y; _marqLastX = x; _marqLastY = y;
                            _marqAdditive = ctrl;
                            EndWaterPointer();
                            BeginPointerCapture(hWnd);
                        }
                    }
                }
                break;

            case WM_LBUTTONUP:
                FeedMouseButton(0, 0);
                // teardown は WM_CAPTURECHANGED に集約。ReleaseCapture が同期的にそれを送る。
                // _finalizing で「自前の解放」と「奪取」を区別し、奪取時は marquee をキャンセルする。
                if (_giz3dDragging) ReleaseCapture();   // 3D ギズモの確定は CAPTURECHANGED で
                if (_gizmoDragging || _marqueeDragging) { _finalizing = true; ReleaseCapture(); }
                break;

            case WM_RBUTTONDOWN:
                FeedMouseButton(1, 1);
                goto case WM_MBUTTONDOWN;
            case WM_MBUTTONDOWN:
                if (msg == WM_MBUTTONDOWN) FeedMouseButton(2, 1);
                if (!CanRouteEditorViewportInteraction())
                    break;
                // ギズモ/マーキー ドラッグ中はパンを始めない (gizmo/marquee/pan を相互排他に)。
                if (!_gizmoDragging && !_marqueeDragging)
                {
                    EndWaterPointer();
                    _panning = true;
                    _panMode = (msg == WM_MBUTTONDOWN) ? 1 : 0;   // 中ボタン=パン(平行移動) / 右ボタン=軌道(回転)
                    _lastX = LoWord(lParam); _lastY = HiWord(lParam); BeginPointerCapture(hWnd);
                }
                break;

            case WM_RBUTTONUP:
                FeedMouseButton(1, 0);
                goto case WM_MBUTTONUP;
            case WM_MBUTTONUP:
                if (msg == WM_MBUTTONUP) FeedMouseButton(2, 0);
                if (_panning) { _panning = false; ReleaseCapture(); }
                break;

            case WM_CAPTURECHANGED:
                _pointerButtonMismatchStartedAtTimestamp = 0;
                // capture 喪失/解放はすべてここで teardown する (LBUTTONUP も ReleaseCapture 経由で
                // 同期的にここへ来る)。これでドラッグ中にキャプチャを奪われても gizmo_end +
                // Inspector 更新が確実に走る。
                if (_giz3dDragging)
                {
                    bool wasDragging = _giz3dDragging;
                    _giz3dDragging = false;
                    if (_engine != IntPtr.Zero) EngineInterop.acs_editor_gizmo3d_end(_engine);
                    if (ShouldPublishGizmoTransformChange(
                            captureEnded: true,
                            wasDragging))
                    {
                        TransformChanged?.Invoke();
                    }
                }
                if (_gizmoDragging)
                {
                    _gizmoDragging = false;
                    if (_engine != IntPtr.Zero) EngineInterop.acs_editor_gizmo_end(_engine);
                    TransformChanged?.Invoke();
                }
                if (_marqueeDragging)
                {
                    _marqueeDragging = false;
                    if (_engine != IntPtr.Zero)
                    {
                        EngineInterop.acs_editor_set_marquee(_engine, 0, 0, 0, 0, 0);   // オーバーレイは常に消す
                        if (_finalizing)   // 自前の LBUTTONUP 由来の解放 (奪取ならコミットしない)
                        {
                            int dx = Math.Abs(_marqLastX - _marqStartX), dy = Math.Abs(_marqLastY - _marqStartY);
                            bool ctrl = _marqAdditive;   // gesture 開始時に latch した Ctrl
                            if (dx > MarqueeThreshold || dy > MarqueeThreshold)
                                EngineInterop.acs_editor_select_box(_engine, _marqStartX, _marqStartY,
                                                                    _marqLastX, _marqLastY, ctrl ? 1 : 0);
                            else if (!ctrl)
                                EngineInterop.acs_editor_select_none(_engine);   // 空クリック = 全解除
                            Picked?.Invoke(EngineInterop.acs_editor_selected(_engine));
                        }
                        // else: drag 中に capture を奪われた → 選択は確定せずキャンセル
                    }
                }
                _panning = false;
                _finalizing = false;
                break;

            case WM_MOUSEMOVE:
                EnsureMouseLeaveTracking(hWnd);
                if (_engine != IntPtr.Zero)
                {
                    int x = LoWord(lParam), y = HiWord(lParam);
                    if (CanRouteGameplayInput())
                        EngineInterop.acs_editor_logic_input_mouse_move(_engine, x, y);
                    if (!CanRouteEditorViewportInteraction())
                        break;
                    if (_giz3dDragging)
                    {
                        EngineInterop.acs_editor_gizmo3d_drag(_engine, x, y);
                        RequestRedraw();
                    }
                    else if (_gizmoDragging)
                    {
                        EngineInterop.acs_editor_gizmo_update(_engine, x, y);
                        RequestRedraw();
                    }
                    else if (_panning)
                    {
                        if (_panMode == 1) EngineInterop.acs_editor_camera_move(_engine, x - _lastX, y - _lastY);  // 中ドラッグ = パン
                        else               EngineInterop.acs_editor_camera_pan (_engine, x - _lastX, y - _lastY);  // 右ドラッグ = 軌道
                        _lastX = x; _lastY = y;
                        RequestRedraw();
                    }
                    else if (_marqueeDragging)
                    {
                        _marqLastX = x; _marqLastY = y;
                        EngineInterop.acs_editor_set_marquee(_engine, 1, _marqStartX, _marqStartY, x, y);
                    }
                    else
                    {
                        bool view3d =
                            EngineInterop.acs_editor_get_view3d(_engine) != 0;
                        WaterPointerRoutingDecision waterMove =
                            WaterPointerRoutingPolicy.ForMove(
                                WaterPointerState(view3d),
                                (wParam.ToInt64() & MK_LBUTTON) != 0);
                        if (waterMove.ShouldRoute)
                        {
                            RouteWaterPointer(waterMove, x, y);
                            RequestRedraw();
                        }
                    }
                }
                break;

            case WM_MOUSEWHEEL:
                if (CanRouteEditorViewportInteraction())
                {
                    int delta = HiWord(wParam);                       // 符号付きホイール量
                    var pt = new POINT { X = LoWord(lParam), Y = HiWord(lParam) };
                    ScreenToClient(hWnd, ref pt);                     // ホイールは screen 座標
                    float factor = delta > 0 ? 1.1f : 1.0f / 1.1f;
                    EngineInterop.acs_editor_camera_zoom(_engine, factor, pt.X, pt.Y);
                    RequestRedraw();
                }
                break;

            case WM_MOUSEHWHEEL:
                break;

            case WM_MOUSELEAVE:
                _trackingMouseLeave = false;
                break;
          }
          return CallOriginalWindowProc(hWnd, msg, wParam, lParam);
        }
        finally
        {
            _wndProcDepth--;
            CompleteDeferredDestroyIfReady();
        }
    }

    private static int LoWord(IntPtr v) => unchecked((short)(v.ToInt64() & 0xFFFF));
    private static int HiWord(IntPtr v) => unchecked((short)((v.ToInt64() >> 16) & 0xFFFF));

    // Play 中なら DLL へマウスボタンをフィードする (Play 外は editor_abi 側で no-op)。
    private void FeedMouseButton(int button, int down)
    {
        if (CanRouteGameplayInput())
            EngineInterop.acs_editor_logic_input_mouse_button(_engine, button, down);
    }

    private bool RenderOneFrame(out bool presented)
    {
        presented = false;
        Dispatcher.VerifyAccess();
        if (_destroying || _engine == IntPtr.Zero || _hwnd == IntPtr.Zero)
            return false;
        if (_frameActive)
        {
            _redrawPending = true;
            return false;
        }

        _frameActive = true;
        try
        {
        // Rendering remains continuous for play-mode animation. This flag records
        // WndProc redraw requests and, importantly, prevents them from invoking the
        // native renderer recursively.
        if (_redrawPending) _redrawPending = false;

        // スワップチェインを物理ピクセルで構成する。WM_* マウス座標も (PerMonitorV2 では)
        // 物理ピクセルなので、描画/ピック空間と入力空間が一致する。さらに ActualWidth は
        // DPI 変化で不変だが物理幅は変わるため、モニタ間 DPI 変化でも下の w!=_w が成立して
        // 再 attach/resize が走る。
        DpiScale dpi = VisualTreeHelper.GetDpi(this);
        uint w = _externalSurfaceParent != IntPtr.Zero
            ? Math.Max(1u, _externalSurfaceWidth)
            : (uint)Math.Max(1.0, ActualWidth * dpi.DpiScaleX);
        uint h = _externalSurfaceParent != IntPtr.Zero
            ? Math.Max(1u, _externalSurfaceHeight)
            : (uint)Math.Max(1.0, ActualHeight * dpi.DpiScaleY);

        if (ShouldAttemptAttach(_attached, AttachFailed))
        {
            // 起動直後のサイズが安定 (2 フレーム連続同値) するまで attach を待つ。確定前に attach すると
            // 直後のリサイズ連発で swapchain 再生成 + WaitIdle がフレームペーシングと競合し間欠クラッシュする。
            if (w != _pendW || h != _pendH)
            {
                _pendW = w;
                _pendH = h;
                return false;
            }
            long attachBegin = System.Diagnostics.Stopwatch.GetTimestamp();
            int attachResult;
            try
            {
                attachResult =
                    EngineInterop.acs_editor_attach(_engine, _hwnd, w, h);
            }
            finally
            {
                ObserveNativeCall("attach", attachBegin);
            }
            if (attachResult != 0)
            {
                if (_destroying) return false;
                _attached = true; _w = w; _h = h; AttachFailed = false;
                _awaitingStableResizeAfterWindowInteraction = false;
                _resizePendW = 0;
                _resizePendH = 0;
                bool hiddenStartupRenderingAllowedBeforeCallback =
                    _hiddenStartupRenderingAllowed;
                Attached?.Invoke();
                // OnEngineAttached pauses hidden submissions while project
                // settings are read. Do not let this already-active call
                // advance native startup once under the pre-settings defaults.
                if (!ShouldContinueRenderingAfterAttachCallback(
                        _destroying,
                        RenderPumpSuspended,
                        hiddenStartupRenderingAllowedBeforeCallback,
                        _hiddenStartupRenderingAllowed))
                {
                    return false;
                }
            }
            else
            {
                AttachFailed = true;
                AttachmentFailureDetail =
                    "Renderer attachment failed; automatic retry was stopped.";
                SuspendRenderPumpForStartupFailure();
                AttachmentFailed?.Invoke();
                return false;
            }
        }
        else if (!_attached)
        {
            // A failed attach is latched. Even if another wake source is added
            // later, only RetryAttach may clear the latch and call the ABI again.
            return false;
        }
        else if (w == _w && h == _h)
        {
            _awaitingStableResizeAfterWindowInteraction = false;
            _resizePendW = 0;
            _resizePendH = 0;
        }
        else
        {
            if (ShouldDeferFinalResize(
                    _awaitingStableResizeAfterWindowInteraction,
                    w,
                    h,
                    _resizePendW,
                    _resizePendH))
            {
                _resizePendW = w;
                _resizePendH = h;
                return false;
            }

            long resizeBegin = System.Diagnostics.Stopwatch.GetTimestamp();
            int resizeResult;
            try
            {
                resizeResult =
                    EngineInterop.acs_editor_resize(_engine, w, h);
            }
            finally
            {
                ObserveNativeCall("resize", resizeBegin);
            }
            ViewportResizeResultPolicy resizePolicy =
                ClassifyResizeResult(resizeResult);
            if (resizePolicy.CommitDimensions)
            {
                _w = w;
                _h = h;
                _awaitingStableResizeAfterWindowInteraction = false;
                _resizePendW = 0;
                _resizePendH = 0;
            }
            System.Diagnostics.Debug.Assert(
                resizePolicy.ContinueToRender);
        }
        if (_destroying) return false;

        // 経過秒 (dt) を計算してエンジンを 1 フレーム進める (アニメーションを描画)。
        double now = _clock.Elapsed.TotalSeconds;
        float dt = (float)Math.Clamp(
            now - _lastSec,
            0.0,
            MaximumNativeDeltaSeconds);
        int renderResult;
        long renderBegin = System.Diagnostics.Stopwatch.GetTimestamp();
        try
        {
            renderResult =
                EngineInterop.TryRenderEditorFrame(_engine, dt);
        }
        finally
        {
            ObserveNativeCall("render", renderBegin);
        }
        if (IsFatalRenderResult(renderResult))
        {
            _lastNativeCallKind = "render:fatal";
            SuspendRenderPumpForStartupFailure();
            RenderingFailed?.Invoke(
                "The native renderer rejected the cooperative frame contract; " +
                "automatic rendering was stopped.");
            return false;
        }

        // A backpressured try did not advance native simulation. Preserve its
        // elapsed time for the next submitted frame instead of making
        // Play/physics/water run in slow motion under GPU saturation.
        _lastSec = CommitRenderTimestamp(
            _lastSec,
            now,
            renderResult);
        presented = renderResult > 0;
        return ShouldYieldForGpuBackpressure(renderResult);
        }
        finally
        {
            _frameActive = false;
            CompleteDeferredDestroyIfReady();
        }
    }

    private void ObserveNativeCall(string kind, long startedAtTimestamp)
    {
        double elapsedMilliseconds =
            (System.Diagnostics.Stopwatch.GetTimestamp() -
             startedAtTimestamp) *
            1000.0 / System.Diagnostics.Stopwatch.Frequency;
        if (!double.IsFinite(elapsedMilliseconds) ||
            elapsedMilliseconds < 0.0)
        {
            return;
        }

        _nativeCallCount++;
        _lastNativeCallMilliseconds = elapsedMilliseconds;
        _maximumNativeCallMilliseconds = Math.Max(
            _maximumNativeCallMilliseconds,
            elapsedMilliseconds);
        _lastNativeCallKind = kind;
        if (elapsedMilliseconds >= SlowNativeCallThresholdMilliseconds)
            _slowNativeCallCount++;
    }

    internal ViewportNativeRenderDiagnostic GetNativeRenderDiagnostic() =>
        new(
            _nativeCallCount,
            _slowNativeCallCount,
            _gpuBackpressureYieldCount,
            _lastNativeCallMilliseconds,
            _maximumNativeCallMilliseconds,
            _lastNativeCallKind,
            _gpuBackpressureInputRetryTotal,
            _gpuBackpressureBackgroundFallbackCount,
            _gpuReadyAfterRetryCount,
            _renderFairnessYieldCount,
            _lastGpuBackpressureEpochMilliseconds,
            _maximumGpuBackpressureEpochMilliseconds,
            _peakPresentedRenderBurstFrames,
            _peakRenderBurstActiveCpuMilliseconds,
            _renderInputContinuationYieldCount,
            _renderMaintenanceYieldCount,
            _lastRenderContinuationQueueWaitMilliseconds,
            _maximumRenderContinuationQueueWaitMilliseconds,
            _lastRenderMaintenanceQueueWaitMilliseconds,
            _maximumRenderMaintenanceQueueWaitMilliseconds);

    internal void ResetNativeRenderDiagnostics()
    {
        Dispatcher.VerifyAccess();
        _nativeCallCount = 0;
        _slowNativeCallCount = 0;
        _gpuBackpressureYieldCount = 0;
        _gpuBackpressureInputRetryTotal = 0;
        _gpuBackpressureBackgroundFallbackCount = 0;
        _gpuReadyAfterRetryCount = 0;
        _renderFairnessYieldCount = 0;
        _lastGpuBackpressureEpochMilliseconds = 0.0;
        _maximumGpuBackpressureEpochMilliseconds = 0.0;
        _peakPresentedRenderBurstFrames = 0;
        _peakRenderBurstActiveCpuMilliseconds = 0.0;
        _renderInputContinuationYieldCount = 0;
        _renderMaintenanceYieldCount = 0;
        _lastRenderContinuationQueueWaitMilliseconds = 0.0;
        _maximumRenderContinuationQueueWaitMilliseconds = 0.0;
        _lastRenderMaintenanceQueueWaitMilliseconds = 0.0;
        _maximumRenderMaintenanceQueueWaitMilliseconds = 0.0;
        _lastNativeCallMilliseconds = 0.0;
        _maximumNativeCallMilliseconds = 0.0;
        _lastNativeCallKind = string.Empty;
    }
}
