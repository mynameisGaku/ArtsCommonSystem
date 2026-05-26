// SPDX-License-Identifier: Apache-2.0
// Win32 ウィンドウ実装
#include "platform/Window.h"
#include "platform/InputCodes.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

#include <windowsx.h>

namespace acs {

void FWindow::DispatchEvent_Internal(const FEvent& e) noexcept {
    if (_callback) _callback(_callback_user, e);
}

namespace {

// VK_* (Win32 仮想キーコード) → EKey 変換
EKey VkToKey(WPARAM vk, LPARAM lParam) noexcept {
    bool extended = (lParam & (1 << 24)) != 0;  // 右側修飾キー判別用
    switch (vk) {
        case 'A': return EKey::A; case 'B': return EKey::B; case 'C': return EKey::C;
        case 'D': return EKey::D; case 'E': return EKey::E; case 'F': return EKey::F;
        case 'G': return EKey::G; case 'H': return EKey::H; case 'I': return EKey::I;
        case 'J': return EKey::J; case 'K': return EKey::K; case 'L': return EKey::L;
        case 'M': return EKey::M; case 'N': return EKey::N; case 'O': return EKey::O;
        case 'P': return EKey::P; case 'Q': return EKey::Q; case 'R': return EKey::R;
        case 'S': return EKey::S; case 'T': return EKey::T; case 'U': return EKey::U;
        case 'V': return EKey::V; case 'W': return EKey::W; case 'X': return EKey::X;
        case 'Y': return EKey::Y; case 'Z': return EKey::Z;
        case '0': return EKey::Num0; case '1': return EKey::Num1; case '2': return EKey::Num2;
        case '3': return EKey::Num3; case '4': return EKey::Num4; case '5': return EKey::Num5;
        case '6': return EKey::Num6; case '7': return EKey::Num7; case '8': return EKey::Num8;
        case '9': return EKey::Num9;
        case VK_F1: return EKey::F1;   case VK_F2: return EKey::F2;   case VK_F3: return EKey::F3;
        case VK_F4: return EKey::F4;   case VK_F5: return EKey::F5;   case VK_F6: return EKey::F6;
        case VK_F7: return EKey::F7;   case VK_F8: return EKey::F8;   case VK_F9: return EKey::F9;
        case VK_F10: return EKey::F10; case VK_F11: return EKey::F11; case VK_F12: return EKey::F12;
        case VK_SHIFT:   return extended ? EKey::RightShift : EKey::LeftShift;
        case VK_CONTROL: return extended ? EKey::RightCtrl  : EKey::LeftCtrl;
        case VK_MENU:    return extended ? EKey::RightAlt   : EKey::LeftAlt;
        case VK_LWIN:    return EKey::LeftSuper;
        case VK_RWIN:    return EKey::RightSuper;
        case VK_UP:    return EKey::Up;
        case VK_DOWN:  return EKey::Down;
        case VK_LEFT:  return EKey::Left;
        case VK_RIGHT: return EKey::Right;
        case VK_SPACE:    return EKey::Space;
        case VK_RETURN:   return EKey::Enter;
        case VK_TAB:      return EKey::Tab;
        case VK_BACK:     return EKey::Backspace;
        case VK_ESCAPE:   return EKey::Escape;
        case VK_INSERT:   return EKey::Insert;
        case VK_DELETE:   return EKey::Delete;
        case VK_HOME:     return EKey::Home;
        case VK_END:      return EKey::End;
        case VK_PRIOR:    return EKey::PageUp;
        case VK_NEXT:     return EKey::PageDown;
        case VK_CAPITAL:  return EKey::CapsLock;
        case VK_NUMLOCK:  return EKey::NumLock;
        case VK_SCROLL:   return EKey::ScrollLock;
        case VK_OEM_MINUS: return EKey::Minus;
        case VK_OEM_PLUS:  return EKey::Equal;
        case VK_OEM_4:     return EKey::LeftBracket;
        case VK_OEM_6:     return EKey::RightBracket;
        case VK_OEM_5:     return EKey::Backslash;
        case VK_OEM_1:     return EKey::Semicolon;
        case VK_OEM_7:     return EKey::Apostrophe;
        case VK_OEM_COMMA: return EKey::Comma;
        case VK_OEM_PERIOD:return EKey::Period;
        case VK_OEM_2:     return EKey::Slash;
        case VK_OEM_3:     return EKey::Grave;
        case VK_NUMPAD0: return EKey::KP0; case VK_NUMPAD1: return EKey::KP1;
        case VK_NUMPAD2: return EKey::KP2; case VK_NUMPAD3: return EKey::KP3;
        case VK_NUMPAD4: return EKey::KP4; case VK_NUMPAD5: return EKey::KP5;
        case VK_NUMPAD6: return EKey::KP6; case VK_NUMPAD7: return EKey::KP7;
        case VK_NUMPAD8: return EKey::KP8; case VK_NUMPAD9: return EKey::KP9;
        case VK_ADD:      return EKey::KPAdd;
        case VK_SUBTRACT: return EKey::KPSubtract;
        case VK_MULTIPLY: return EKey::KPMultiply;
        case VK_DIVIDE:   return EKey::KPDivide;
        case VK_DECIMAL:  return EKey::KPDecimal;
        default:          return EKey::Unknown;
    }
}

// HWND → FWindow* 紐付け用キー
constexpr const wchar_t* kPropKey = L"ACS_WINDOW_PTR";

FWindow* GetWindowFromHwnd(HWND hwnd) noexcept {
    return static_cast<FWindow*>(::GetPropW(hwnd, kPropKey));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    FWindow* w = GetWindowFromHwnd(hwnd);
    if (!w) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CLOSE: {
            // × ボタン押下 → ShouldClose を立ててアプリに通知
            FEvent e{}; e.type = EventType::WindowClose;
            w->Close();
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_SIZE: {
            u32 width  = static_cast<u32>(LOWORD(lp));
            u32 height = static_cast<u32>(HIWORD(lp));
            w->UpdateSize_Internal(width, height);
            FEvent e{}; e.type = EventType::WindowResize;
            e.resize.width = width; e.resize.height = height;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            FEvent e{};
            e.type = (msg == WM_SETFOCUS) ? EventType::WindowFocus : EventType::WindowLostFocus;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            bool repeat = (lp & (1 << 30)) != 0;
            FEvent e{};
            e.type = repeat ? EventType::KeyRepeat : EventType::KeyPressed;
            e.key.key = VkToKey(wp, lp);
            e.key.repeat = repeat;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            FEvent e{}; e.type = EventType::KeyReleased;
            e.key.key = VkToKey(wp, lp);
            e.key.repeat = false;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP: {
            bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
            EMouseButton b = EMouseButton::Left;
            if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) b = EMouseButton::Right;
            if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) b = EMouseButton::Middle;
            FEvent e{};
            e.type = down ? EventType::MouseButtonPressed : EventType::MouseButtonReleased;
            e.mouse_button.button = b;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_MOUSEMOVE: {
            FEvent e{}; e.type = EventType::MouseMoved;
            e.mouse_move.x = static_cast<f32>(GET_X_LPARAM(lp));
            e.mouse_move.y = static_cast<f32>(GET_Y_LPARAM(lp));
            e.mouse_move.dx = 0;  // 差分は FInput::Update 側で計算
            e.mouse_move.dy = 0;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            FEvent e{}; e.type = EventType::MouseScrolled;
            e.mouse_scroll.x = 0;
            e.mouse_scroll.y = static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_CHAR: {
            // 制御文字はスキップ
            if (wp >= 32) {
                FEvent e{}; e.type = EventType::CharInput;
                e.char_input.codepoint = static_cast<u32>(wp);
                w->DispatchEvent_Internal(e);
            }
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool g_class_registered = false;
constexpr const wchar_t* kClassName = L"ACSWindow";

// ウィンドウクラス登録（初回のみ）
bool EnsureWindowClass() noexcept {
    if (g_class_registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) return false;
    g_class_registered = true;
    return true;
}

} // namespace

FWindow::~FWindow() noexcept {
    if (_hwnd) {
        ::RemovePropW(static_cast<HWND>(_hwnd), kPropKey);
        ::DestroyWindow(static_cast<HWND>(_hwnd));
    }
}

FWindow::FWindow(FWindow&& o) noexcept
    : _hwnd(o._hwnd), _width(o._width), _height(o._height),
      _should_close(o._should_close),
      _callback(o._callback), _callback_user(o._callback_user) {
    if (_hwnd) {
        // HWND に紐付くポインタを更新
        ::SetPropW(static_cast<HWND>(_hwnd), kPropKey, this);
    }
    o._hwnd = nullptr;
    o._width = 0;
    o._height = 0;
    o._should_close = false;
    o._callback = nullptr;
    o._callback_user = nullptr;
}

FWindow& FWindow::operator=(FWindow&& o) noexcept {
    if (this == &o) return *this;
    if (_hwnd) {
        ::RemovePropW(static_cast<HWND>(_hwnd), kPropKey);
        ::DestroyWindow(static_cast<HWND>(_hwnd));
    }
    _hwnd = o._hwnd;
    _width = o._width;
    _height = o._height;
    _should_close = o._should_close;
    _callback = o._callback;
    _callback_user = o._callback_user;
    if (_hwnd) ::SetPropW(static_cast<HWND>(_hwnd), kPropKey, this);
    o._hwnd = nullptr;
    o._width = 0;
    o._height = 0;
    o._should_close = false;
    o._callback = nullptr;
    o._callback_user = nullptr;
    return *this;
}

TResult<FWindow> FWindow::Create(const FWindowConfig& cfg) noexcept {
    if (!EnsureWindowClass()) {
        return ACS_ERR_OS(OS, 10, "RegisterClassExW failed", ::GetLastError());
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    // resizable=false ならサイズ変更不可スタイルへ
    if (!cfg.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

    // クライアント領域が cfg.width × cfg.height になるよう外接矩形を計算
    RECT rect{ 0, 0, static_cast<LONG>(cfg.width), static_cast<LONG>(cfg.height) };
    ::AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = ::CreateWindowExW(
        0, kClassName, cfg.title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        return ACS_ERR_OS(OS, 11, "CreateWindowExW failed", ::GetLastError());
    }

    FWindow w;
    w._hwnd = hwnd;
    w._width = cfg.width;
    w._height = cfg.height;
    w._should_close = false;

    // ムーブで返した後の安定アドレスに紐付けるため、いったん仮で登録 → ムーブ後に再登録
    FWindow result = Move(w);
    ::SetPropW(hwnd, kPropKey, &result);
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    return TResult<FWindow>(OkInit, Move(result));
}

void FWindow::PollEvents() noexcept {
    MSG msg{};
    // PeekMessage でノンブロッキング処理（メインスレッドを止めない）
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

void FWindow::SetEventCallback(EventCallback cb, void* user) noexcept {
    _callback = cb;
    _callback_user = user;
}

void FWindow::SetTitle(const wchar_t* title) noexcept {
    if (_hwnd) ::SetWindowTextW(static_cast<HWND>(_hwnd), title);
}

void FWindow::SetFullscreen(bool on) noexcept {
    if (!_hwnd || on == _fullscreen) return;
    HWND hwnd = static_cast<HWND>(_hwnd);
    if (on) {
        // 現在の窓矩形・スタイルを記憶
        RECT r{};
        ::GetWindowRect(hwnd, &r);
        _saved_rect[0] = r.left;  _saved_rect[1] = r.top;
        _saved_rect[2] = r.right; _saved_rect[3] = r.bottom;
        _saved_style = static_cast<i32>(::GetWindowLongW(hwnd, GWL_STYLE));
        // 現在のモニタ全体を覆うボーダーレス窓へ
        HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        ::GetMonitorInfoW(mon, &mi);
        ::SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        ::SetWindowPos(hwnd, HWND_TOP,
                       mi.rcMonitor.left, mi.rcMonitor.top,
                       mi.rcMonitor.right  - mi.rcMonitor.left,
                       mi.rcMonitor.bottom - mi.rcMonitor.top,
                       SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        // 記憶した窓に戻す
        ::SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(_saved_style));
        ::SetWindowPos(hwnd, HWND_TOP,
                       _saved_rect[0], _saved_rect[1],
                       _saved_rect[2] - _saved_rect[0],
                       _saved_rect[3] - _saved_rect[1],
                       SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    _fullscreen = on;
}

} // namespace acs
