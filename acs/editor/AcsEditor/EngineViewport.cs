using System;
using System.Runtime.InteropServices;
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
    [DllImport("user32.dll")] private static extern short GetKeyState(int nVirtKey);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int nVirtKey);
    [DllImport("user32.dll")] private static extern uint GetDoubleClickTime();
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
    private delegate IntPtr WndProcDelegate(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    private const int GWLP_WNDPROC  = -4;
    private const int WM_CANCELMODE = 0x001F;
    private const int WM_KEYDOWN    = 0x0100;
    private const int WM_MOUSEMOVE  = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
                      WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
                      WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208,
                      WM_MOUSEWHEEL = 0x020A, WM_MOUSEHWHEEL = 0x020E,
                      WM_NCHITTEST = 0x0084,
                      WM_CAPTURECHANGED = 0x0215, WM_DROPFILES = 0x0233;
    private const int WM_RENDER_PUMP = 0x8000 + 0x5A1; // private WM_APP message
    private const int HTCLIENT = 1;

    private const int WS_CHILD = 0x40000000;
    private const int WS_VISIBLE = 0x10000000;
    private const int WS_CLIPCHILDREN = 0x02000000;
    private const int WS_CLIPSIBLINGS = 0x04000000;

    private IntPtr _hwnd;
    private IntPtr _engine;
    private bool _attached;
    private uint _w, _h;
    private uint _pendW, _pendH;   // attach 前の «サイズ安定待ち» 用 (起動直後のリサイズ連発を回避)
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
    private int _renderBurstFrames;
    private long _renderBurstStartedAtTimestamp;

    // Native rendering must not be driven by CompositionTarget.Rendering:
    // that event is deliberately synchronized to WPF/DWM composition and
    // therefore caps the engine to the monitor cadence (or a divisor of it).
    // A single private Win32 message is kept in flight. Direct messages run in
    // bounded bursts so WPF's Dispatcher is guaranteed a Background turn for
    // startup, timers, commands and asset work without paying a Dispatcher hop
    // for every native frame.
    internal const int MaxDirectRenderBurstFrames = 8;
    internal const double MaxDirectRenderBurstMilliseconds = 8.0;
    internal const double PointerCaptureRecoveryGraceMilliseconds = 100.0;
    internal const int PointerButtonLeftMask = 1 << 0;
    internal const int PointerButtonRightMask = 1 << 1;
    internal const int PointerButtonMiddleMask = 1 << 2;
    internal const DispatcherPriority RenderFairnessPriority =
        DispatcherPriority.Background;
    internal const DispatcherPriority DormantWakePriority =
        DispatcherPriority.Background;

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

    internal static bool ShouldRecoverRenderPumpFromComposition(
        bool frameActive,
        bool eligible,
        bool queued,
        bool queuedPumpStale) =>
        !frameActive && eligible && (!queued || queuedPumpStale);

    internal static bool ShouldAttemptAttach(bool attached, bool attachFailed) =>
        !attached && !attachFailed;

    internal static bool IsAnyRenderPumpSuspensionActive(
        bool startupFailureSuspended,
        bool windowInteractionPaused) =>
        startupFailureSuspended || windowInteractionPaused;

    internal static bool ShouldYieldRenderBurst(
        int completedFrames,
        double elapsedMilliseconds) =>
        completedFrames >= MaxDirectRenderBurstFrames ||
        elapsedMilliseconds >= MaxDirectRenderBurstMilliseconds;

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
        double mismatchAgeMilliseconds) =>
        viewportOwnsCapture &&
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
    private long _pointerButtonMismatchStartedAtTimestamp;
    private const int MarqueeThreshold = 3;   // これ未満の移動は drag でなく click 扱い
    private int _lastClickTick, _lastClickX, _lastClickY;   // ダブルクリック検出 (STATIC は WM_*DBLCLK を送らない)

    /// <summary>最後に試行した attach が失敗したか (UI のステータス表示用)。</summary>
    public bool AttachFailed { get; private set; }

    /// <summary>HWND への attach が失敗し、自動再試行を停止したときに 1 度発火する。</summary>
    public event Action? AttachmentFailed;

    /// <summary>エンジンハンドル (シーン API 呼び出し用、未生成時 Zero)。</summary>
    public IntPtr Engine => _destroying ? IntPtr.Zero : _engine;

    /// <summary>ポリゴン描画モード中か (左クリックで点を置く)。MainWindow が制御。</summary>
    public bool PolyMode { get; set; }

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
        _destroying = false;
        _destroyDeferred = false;
        _frameActive = false;
        _wndProcDepth = 0;
        _redrawPending = true;
        _renderPumpQueued = false;
        _renderPumpGeneration++;
        _renderPumpToken++;
        _renderPumpQueuedAt = 0;
        _renderPumpSuspended = false;
        _windowInteractionPaused = false;
        _renderFairnessYieldQueued = false;
        _pointerButtonMismatchStartedAtTimestamp = 0;
        ResetRenderBurst();
        AttachFailed = false;
        _dormantRenderTimer?.Stop();

        // 子ウィンドウ (予約クラス "STATIC")。DX12 スワップチェインの提示先。
        _hwnd = CreateWindowExW(0, "STATIC", string.Empty,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);

        _engine = EngineInterop.acs_editor_create();

        // STATIC のウィンドウプロシージャを差し替えてマウス入力 (pick / pan / zoom) を拾う。
        _wndProc = ViewportWndProc;
        _origProc = SetWindowLongPtrW(_hwnd, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(_wndProc));
        DragAcceptFiles(_hwnd, !App.IsNonInteractiveLaunch);

        // WPF's compositor callback is a bootstrap/watchdog only. It never
        // paces an already-running native pump, so monitor refresh cannot cap
        // engine frames. Keeping this wake source makes initial attach robust
        // while the HwndHost is transitioning into the visible tree.
        CompositionTarget.Rendering += OnCompositionWake;
        // BuildWindowCore can run before WPF publishes IsVisible/ActualWidth.
        // The first private message is therefore posted only at Loaded
        // priority. Posting during BuildWindowCore can succeed before WPF has
        // finished parenting the HWND and leave a permanently latched message.
        int loadGeneration = _renderPumpGeneration;
        _ = Dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                if (loadGeneration == _renderPumpGeneration)
                    QueueRenderPump();
            }));
        return new HandleRef(this, _hwnd);
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
        _hwnd = IntPtr.Zero;
        _attached = false;
        _windowInteractionPaused = false;
        _panning = false;
        _gizmoDragging = false;
        _giz3dDragging = false;
        _marqueeDragging = false;
        _finalizing = false;
        _pointerButtonMismatchStartedAtTimestamp = 0;

        if (engine != IntPtr.Zero) EngineInterop.acs_editor_destroy(engine);
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
        return ShouldRenderContinuously(
            _destroying,
            RenderPumpSuspended,
            _engine != IntPtr.Zero && _hwnd != IntPtr.Zero,
            _hwnd != IntPtr.Zero && IsWindowVisible(_hwnd),
            minimized,
            ActualWidth,
            ActualHeight);
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
        QueueRenderPump();
    }

    private void QueueRenderPump()
    {
        Dispatcher.VerifyAccess();
        if (_renderPumpQueued || _renderFairnessYieldQueued || _frameActive ||
            _destroying ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return;
        }

        if (!IsContinuousRenderEligible())
        {
            ResetRenderBurst();
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

    private void ScheduleNextRenderPumpAfterFrame()
    {
        Dispatcher.VerifyAccess();
        if (_frameActive || _destroying || RenderPumpSuspended ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return;
        }

        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        if (_renderBurstStartedAtTimestamp == 0)
            _renderBurstStartedAtTimestamp = now;
        _renderBurstFrames++;
        double elapsedMilliseconds =
            (now - _renderBurstStartedAtTimestamp) * 1000.0 /
            System.Diagnostics.Stopwatch.Frequency;

        if (ShouldYieldRenderBurst(_renderBurstFrames, elapsedMilliseconds))
        {
            QueueRenderFairnessYield();
            return;
        }

        QueueRenderPump();
    }

    private void QueueRenderFairnessYield()
    {
        Dispatcher.VerifyAccess();
        if (_renderFairnessYieldQueued || _destroying || RenderPumpSuspended ||
            Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return;
        }

        _renderFairnessYieldQueued = true;
        int generation = _renderPumpGeneration;
        _ = Dispatcher.BeginInvoke(
            RenderFairnessPriority,
            new Action(() =>
            {
                if (generation != _renderPumpGeneration)
                    return;

                _renderFairnessYieldQueued = false;
                ResetRenderBurst();
                QueueRenderPump();
            }));
    }

    private void ResetRenderBurst()
    {
        _renderBurstFrames = 0;
        _renderBurstStartedAtTimestamp = 0;
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
        ResetRenderBurst();
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
        CancelPointerInteraction();
        StopRenderPump();
    }

    /// <summary>
    /// Resumes after WM_EXITSIZEMOVE. The first frame observes the final WPF
    /// size and performs at most one native resize before rendering.
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
        _pointerButtonMismatchStartedAtTimestamp = 0;
        if (_hwnd != IntPtr.Zero && GetCapture() == _hwnd)
            ReleaseCapture();
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

    private void RecoverStalePointerCaptureIfNeeded(int currentMessage)
    {
        int activeButtonMask = ActivePointerButtonMask(
            _gizmoDragging,
            _giz3dDragging,
            _marqueeDragging,
            _panning,
            _panMode);

        // The matching button-up is the commit path for gizmos and marquee
        // selection. It must run before any recovery decision, even when the
        // physical button is already up and a render token was queued first.
        if (_hwnd == IntPtr.Zero || activeButtonMask == 0 ||
            IsInitiatingPointerButtonUpMessage(currentMessage, activeButtonMask))
        {
            _pointerButtonMismatchStartedAtTimestamp = 0;
            return;
        }

        bool ownsCapture = GetCapture() == _hwnd;
        int physicallyDownButtonMask = PhysicalPointerButtonMask(activeButtonMask);
        bool mismatch = ownsCapture &&
            !_windowInteractionPaused &&
            !_finalizing &&
            (activeButtonMask & physicallyDownButtonMask) == 0;
        if (!mismatch)
        {
            _pointerButtonMismatchStartedAtTimestamp = 0;
            return;
        }

        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        if (_pointerButtonMismatchStartedAtTimestamp <= 0)
        {
            _pointerButtonMismatchStartedAtTimestamp = Math.Max(1L, now);
            return;
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
                mismatchAgeMilliseconds))
        {
            return;
        }

        _pointerButtonMismatchStartedAtTimestamp = 0;
        _finalizing = false;
        ReleaseCapture();
    }

    /// <summary>
    /// Explicitly retries a failed HWND attach. A failure never retries by
    /// itself; callers must opt in through this method.
    /// </summary>
    internal bool RetryAttach()
    {
        Dispatcher.VerifyAccess();
        if (_destroying || _attached || !AttachFailed ||
            _engine == IntPtr.Zero || _hwnd == IntPtr.Zero)
        {
            return false;
        }

        AttachFailed = false;
        _renderPumpSuspended = false;
        _pendW = 0;
        _pendH = 0;
        _redrawPending = true;
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

                if (_renderBurstFrames == 0 || _renderBurstStartedAtTimestamp == 0)
                    _renderBurstStartedAtTimestamp =
                        System.Diagnostics.Stopwatch.GetTimestamp();
                RenderOneFrame();
                // Keep exactly one Win32 token in flight, but end every bounded
                // burst with a real Dispatcher.Background turn. This avoids
                // both the old per-frame Dispatcher cap and Win32-pump
                // starvation of startup/timers/commands.
                ScheduleNextRenderPumpAfterFrame();
                return IntPtr.Zero;
            }

            case WM_CANCELMODE:
                CancelPointerInteraction();
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
                // 3D ビューポート: まず変形ギズモを掴めるか試し、掴めなければレイピック。
                if (_engine != IntPtr.Zero && EngineInterop.acs_editor_get_view3d(_engine) != 0)
                {
                    int gx = LoWord(lParam), gy = HiWord(lParam);
                    if (PolyMode)   // Ortho ポリゴン描画: クリックを z=0 平面へ逆射影して頂点を置く
                    {
                        EngineInterop.acs_editor_poly3d_add_point(_engine, gx, gy);
                        break;
                    }
                    if (EngineInterop.acs_editor_gizmo3d_begin(_engine, gx, gy) != 0)
                    {
                        _giz3dDragging = true; BeginPointerCapture(hWnd);
                    }
                    else
                    {
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
                // ギズモ/マーキー ドラッグ中はパンを始めない (gizmo/marquee/pan を相互排他に)。
                if (!_gizmoDragging && !_marqueeDragging)
                {
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
                if (_engine != IntPtr.Zero)
                {
                    int x = LoWord(lParam), y = HiWord(lParam);
                    EngineInterop.acs_editor_logic_input_mouse_move(_engine, x, y);   // Play 中なら game へ (外は no-op)
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
                }
                break;

            case WM_MOUSEWHEEL:
                if (_engine != IntPtr.Zero)
                {
                    int delta = HiWord(wParam);                       // 符号付きホイール量
                    var pt = new POINT { X = LoWord(lParam), Y = HiWord(lParam) };
                    ScreenToClient(hWnd, ref pt);                     // ホイールは screen 座標
                    float factor = delta > 0 ? 1.1f : 1.0f / 1.1f;
                    EngineInterop.acs_editor_camera_zoom(_engine, factor, pt.X, pt.Y);
                    RequestRedraw();
                }
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
        if (_engine != IntPtr.Zero) EngineInterop.acs_editor_logic_input_mouse_button(_engine, button, down);
    }

    private void RenderOneFrame()
    {
        Dispatcher.VerifyAccess();
        if (_destroying || _engine == IntPtr.Zero || _hwnd == IntPtr.Zero) return;
        if (_frameActive)
        {
            _redrawPending = true;
            return;
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
        uint w = (uint)Math.Max(1.0, ActualWidth  * dpi.DpiScaleX);
        uint h = (uint)Math.Max(1.0, ActualHeight * dpi.DpiScaleY);

        if (ShouldAttemptAttach(_attached, AttachFailed))
        {
            // 起動直後のサイズが安定 (2 フレーム連続同値) するまで attach を待つ。確定前に attach すると
            // 直後のリサイズ連発で swapchain 再生成 + WaitIdle がフレームペーシングと競合し間欠クラッシュする。
            if (w != _pendW || h != _pendH) { _pendW = w; _pendH = h; return; }
            if (EngineInterop.acs_editor_attach(_engine, _hwnd, w, h) != 0)
            {
                if (_destroying) return;
                _attached = true; _w = w; _h = h; AttachFailed = false;
                Attached?.Invoke();
                if (_destroying) return;
            }
            else
            {
                AttachFailed = true;
                SuspendRenderPumpForStartupFailure();
                AttachmentFailed?.Invoke();
                return;
            }
        }
        else if (!_attached)
        {
            // A failed attach is latched. Even if another wake source is added
            // later, only RetryAttach may clear the latch and call the ABI again.
            return;
        }
        else if (w != _w || h != _h)
        {
            EngineInterop.acs_editor_resize(_engine, w, h);
            _w = w; _h = h;
        }
        if (_destroying) return;

        // 経過秒 (dt) を計算してエンジンを 1 フレーム進める (アニメーションを描画)。
        double now = _clock.Elapsed.TotalSeconds;
        float dt = (float)Math.Clamp(now - _lastSec, 0.0, 0.1);
        _lastSec = now;
        EngineInterop.acs_editor_render(_engine, dt);
        }
        finally
        {
            _frameActive = false;
            CompleteDeferredDestroyIfReady();
        }
    }
}
