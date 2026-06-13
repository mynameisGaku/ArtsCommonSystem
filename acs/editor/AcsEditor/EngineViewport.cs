using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace AcsEditor;

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
    [DllImport("user32.dll")] private static extern bool ScreenToClient(IntPtr hWnd, ref POINT pt);
    [DllImport("user32.dll")] private static extern IntPtr SetCapture(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern short GetKeyState(int nVirtKey);
    [DllImport("user32.dll")] private static extern uint GetDoubleClickTime();
    private const int VK_CONTROL = 0x11;
    private static bool CtrlDown => (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    [StructLayout(LayoutKind.Sequential)] private struct POINT { public int X; public int Y; }
    private delegate IntPtr WndProcDelegate(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    private const int GWLP_WNDPROC  = -4;
    private const int WM_KEYDOWN    = 0x0100;
    private const int WM_MOUSEMOVE  = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
                      WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
                      WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208,
                      WM_MOUSEWHEEL = 0x020A, WM_NCHITTEST = 0x0084,
                      WM_CAPTURECHANGED = 0x0215;
    private const int HTCLIENT = 1;

    private const int WS_CHILD = 0x40000000;
    private const int WS_VISIBLE = 0x10000000;
    private const int WS_CLIPCHILDREN = 0x02000000;
    private const int WS_CLIPSIBLINGS = 0x04000000;

    private IntPtr _hwnd;
    private IntPtr _engine;
    private bool _attached;
    private uint _w, _h;
    private readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
    private double _lastSec;

    private WndProcDelegate? _wndProc;   // GC で回収されないよう保持
    private IntPtr _origProc;            // 元の STATIC ウィンドウプロシージャ
    private bool _panning;
    private bool _gizmoDragging;         // 移動ギズモのドラッグ中
    private bool _giz3dDragging;         // 3D 変形ギズモのドラッグ中
    private int _lastX, _lastY;
    private bool _marqueeDragging;       // ラバーバンド (矩形) 選択のドラッグ中
    private int _marqStartX, _marqStartY, _marqLastX, _marqLastY;
    private bool _marqAdditive;          // gesture 開始時に latch した Ctrl (additive) 状態
    private bool _finalizing;            // 自前の ReleaseCapture か (capture 奪取と区別)
    private const int MarqueeThreshold = 3;   // これ未満の移動は drag でなく click 扱い
    private int _lastClickTick, _lastClickX, _lastClickY;   // ダブルクリック検出 (STATIC は WM_*DBLCLK を送らない)

    /// <summary>最後に試行した attach が失敗したか (UI のステータス表示用)。</summary>
    public bool AttachFailed { get; private set; }

    /// <summary>エンジンハンドル (シーン API 呼び出し用、未生成時 Zero)。</summary>
    public IntPtr Engine => _engine;

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

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        // 子ウィンドウ (予約クラス "STATIC")。DX12 スワップチェインの提示先。
        _hwnd = CreateWindowExW(0, "STATIC", string.Empty,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);

        _engine = EngineInterop.acs_editor_create();

        // STATIC のウィンドウプロシージャを差し替えてマウス入力 (pick / pan / zoom) を拾う。
        _wndProc = ViewportWndProc;
        _origProc = SetWindowLongPtrW(_hwnd, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(_wndProc));

        CompositionTarget.Rendering += OnFrame;
        return new HandleRef(this, _hwnd);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        CompositionTarget.Rendering -= OnFrame;
        if (_hwnd != IntPtr.Zero && _origProc != IntPtr.Zero)
            SetWindowLongPtrW(_hwnd, GWLP_WNDPROC, _origProc);   // proc を戻してから破棄
        _wndProc = null;
        if (_engine != IntPtr.Zero) { EngineInterop.acs_editor_destroy(_engine); _engine = IntPtr.Zero; }
        if (_hwnd != IntPtr.Zero) { DestroyWindow(_hwnd); _hwnd = IntPtr.Zero; }
        _attached = false;
    }

    // ビューポートのマウス: 左クリック=pick、右/中ドラッグ=pan、ホイール=zoom。
    private IntPtr ViewportWndProc(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam)
    {
        switch (msg)
        {
            case WM_NCHITTEST:
                return (IntPtr)HTCLIENT;   // STATIC を不透明にしてマウスを受け取る

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
                    if (EngineInterop.acs_editor_gizmo3d_begin(_engine, gx, gy) != 0)
                    {
                        _giz3dDragging = true; SetCapture(hWnd);
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
                        _gizmoDragging = true; SetCapture(hWnd);
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
                            SetCapture(hWnd);
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
                    _panning = true; _lastX = LoWord(lParam); _lastY = HiWord(lParam); SetCapture(hWnd);
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
                // capture 喪失/解放はすべてここで teardown する (LBUTTONUP も ReleaseCapture 経由で
                // 同期的にここへ来る)。これでドラッグ中にキャプチャを奪われても gizmo_end +
                // Inspector 更新が確実に走る。
                if (_giz3dDragging)
                {
                    _giz3dDragging = false;
                    if (_engine != IntPtr.Zero) EngineInterop.acs_editor_gizmo3d_end(_engine);
                    TransformChanged?.Invoke();
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
                        EngineInterop.acs_editor_render(_engine, 0f);
                        TransformChanged?.Invoke();
                    }
                    else if (_gizmoDragging)
                    {
                        EngineInterop.acs_editor_gizmo_update(_engine, x, y);
                        EngineInterop.acs_editor_render(_engine, 0f);   // 即時再描画 (操作追従、カクつき緩和)
                    }
                    else if (_panning)
                    {
                        EngineInterop.acs_editor_camera_pan(_engine, x - _lastX, y - _lastY);
                        _lastX = x; _lastY = y;
                        EngineInterop.acs_editor_render(_engine, 0f);   // 即時再描画でパンを滑らかに
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
                }
                break;
        }
        return CallWindowProcW(_origProc, hWnd, msg, wParam, lParam);
    }

    private static int LoWord(IntPtr v) => unchecked((short)(v.ToInt64() & 0xFFFF));
    private static int HiWord(IntPtr v) => unchecked((short)((v.ToInt64() >> 16) & 0xFFFF));

    // Play 中なら DLL へマウスボタンをフィードする (Play 外は editor_abi 側で no-op)。
    private void FeedMouseButton(int button, int down)
    {
        if (_engine != IntPtr.Zero) EngineInterop.acs_editor_logic_input_mouse_button(_engine, button, down);
    }

    private void OnFrame(object? sender, EventArgs e)
    {
        if (_engine == IntPtr.Zero || _hwnd == IntPtr.Zero) return;

        // スワップチェインを物理ピクセルで構成する。WM_* マウス座標も (PerMonitorV2 では)
        // 物理ピクセルなので、描画/ピック空間と入力空間が一致する。さらに ActualWidth は
        // DPI 変化で不変だが物理幅は変わるため、モニタ間 DPI 変化でも下の w!=_w が成立して
        // 再 attach/resize が走る。
        DpiScale dpi = VisualTreeHelper.GetDpi(this);
        uint w = (uint)Math.Max(1.0, ActualWidth  * dpi.DpiScaleX);
        uint h = (uint)Math.Max(1.0, ActualHeight * dpi.DpiScaleY);

        if (!_attached)
        {
            if (EngineInterop.acs_editor_attach(_engine, _hwnd, w, h) != 0)
            {
                _attached = true; _w = w; _h = h; AttachFailed = false;
                Attached?.Invoke();
            }
            else { AttachFailed = true; return; }
        }
        else if (w != _w || h != _h)
        {
            EngineInterop.acs_editor_resize(_engine, w, h);
            _w = w; _h = h;
        }

        // 経過秒 (dt) を計算してエンジンを 1 フレーム進める (アニメする 2D シーンを描画)。
        double now = _clock.Elapsed.TotalSeconds;
        float dt = (float)Math.Clamp(now - _lastSec, 0.0, 0.1);
        _lastSec = now;
        EngineInterop.acs_editor_render(_engine, dt);
    }
}
