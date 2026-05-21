// SPDX-License-Identifier: Apache-2.0
// Win32 ウィンドウ実装
#include "platform/Window.h"
#include "platform/InputCodes.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

#include <windowsx.h>

namespace acs {

void Window::DispatchEvent_Internal(const Event& e) noexcept {
    if (_callback) _callback(_callback_user, e);
}

namespace {

// VK_* (Win32 仮想キーコード) → Key 変換
Key VkToKey(WPARAM vk, LPARAM lParam) noexcept {
    bool extended = (lParam & (1 << 24)) != 0;  // 右側修飾キー判別用
    switch (vk) {
        case 'A': return Key::A; case 'B': return Key::B; case 'C': return Key::C;
        case 'D': return Key::D; case 'E': return Key::E; case 'F': return Key::F;
        case 'G': return Key::G; case 'H': return Key::H; case 'I': return Key::I;
        case 'J': return Key::J; case 'K': return Key::K; case 'L': return Key::L;
        case 'M': return Key::M; case 'N': return Key::N; case 'O': return Key::O;
        case 'P': return Key::P; case 'Q': return Key::Q; case 'R': return Key::R;
        case 'S': return Key::S; case 'T': return Key::T; case 'U': return Key::U;
        case 'V': return Key::V; case 'W': return Key::W; case 'X': return Key::X;
        case 'Y': return Key::Y; case 'Z': return Key::Z;
        case '0': return Key::Num0; case '1': return Key::Num1; case '2': return Key::Num2;
        case '3': return Key::Num3; case '4': return Key::Num4; case '5': return Key::Num5;
        case '6': return Key::Num6; case '7': return Key::Num7; case '8': return Key::Num8;
        case '9': return Key::Num9;
        case VK_F1: return Key::F1;   case VK_F2: return Key::F2;   case VK_F3: return Key::F3;
        case VK_F4: return Key::F4;   case VK_F5: return Key::F5;   case VK_F6: return Key::F6;
        case VK_F7: return Key::F7;   case VK_F8: return Key::F8;   case VK_F9: return Key::F9;
        case VK_F10: return Key::F10; case VK_F11: return Key::F11; case VK_F12: return Key::F12;
        case VK_SHIFT:   return extended ? Key::RightShift : Key::LeftShift;
        case VK_CONTROL: return extended ? Key::RightCtrl  : Key::LeftCtrl;
        case VK_MENU:    return extended ? Key::RightAlt   : Key::LeftAlt;
        case VK_LWIN:    return Key::LeftSuper;
        case VK_RWIN:    return Key::RightSuper;
        case VK_UP:    return Key::Up;
        case VK_DOWN:  return Key::Down;
        case VK_LEFT:  return Key::Left;
        case VK_RIGHT: return Key::Right;
        case VK_SPACE:    return Key::Space;
        case VK_RETURN:   return Key::Enter;
        case VK_TAB:      return Key::Tab;
        case VK_BACK:     return Key::Backspace;
        case VK_ESCAPE:   return Key::Escape;
        case VK_INSERT:   return Key::Insert;
        case VK_DELETE:   return Key::Delete;
        case VK_HOME:     return Key::Home;
        case VK_END:      return Key::End;
        case VK_PRIOR:    return Key::PageUp;
        case VK_NEXT:     return Key::PageDown;
        case VK_CAPITAL:  return Key::CapsLock;
        case VK_NUMLOCK:  return Key::NumLock;
        case VK_SCROLL:   return Key::ScrollLock;
        case VK_OEM_MINUS: return Key::Minus;
        case VK_OEM_PLUS:  return Key::Equal;
        case VK_OEM_4:     return Key::LeftBracket;
        case VK_OEM_6:     return Key::RightBracket;
        case VK_OEM_5:     return Key::Backslash;
        case VK_OEM_1:     return Key::Semicolon;
        case VK_OEM_7:     return Key::Apostrophe;
        case VK_OEM_COMMA: return Key::Comma;
        case VK_OEM_PERIOD:return Key::Period;
        case VK_OEM_2:     return Key::Slash;
        case VK_OEM_3:     return Key::Grave;
        case VK_NUMPAD0: return Key::KP0; case VK_NUMPAD1: return Key::KP1;
        case VK_NUMPAD2: return Key::KP2; case VK_NUMPAD3: return Key::KP3;
        case VK_NUMPAD4: return Key::KP4; case VK_NUMPAD5: return Key::KP5;
        case VK_NUMPAD6: return Key::KP6; case VK_NUMPAD7: return Key::KP7;
        case VK_NUMPAD8: return Key::KP8; case VK_NUMPAD9: return Key::KP9;
        case VK_ADD:      return Key::KPAdd;
        case VK_SUBTRACT: return Key::KPSubtract;
        case VK_MULTIPLY: return Key::KPMultiply;
        case VK_DIVIDE:   return Key::KPDivide;
        case VK_DECIMAL:  return Key::KPDecimal;
        default:          return Key::Unknown;
    }
}

// HWND → Window* 紐付け用キー
constexpr const wchar_t* kPropKey = L"ACS_WINDOW_PTR";

Window* GetWindowFromHwnd(HWND hwnd) noexcept {
    return static_cast<Window*>(::GetPropW(hwnd, kPropKey));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    Window* w = GetWindowFromHwnd(hwnd);
    if (!w) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CLOSE: {
            // × ボタン押下 → ShouldClose を立ててアプリに通知
            Event e{}; e.type = EventType::WindowClose;
            w->Close();
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_SIZE: {
            u32 width  = static_cast<u32>(LOWORD(lp));
            u32 height = static_cast<u32>(HIWORD(lp));
            w->UpdateSize_Internal(width, height);
            Event e{}; e.type = EventType::WindowResize;
            e.resize.width = width; e.resize.height = height;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            Event e{};
            e.type = (msg == WM_SETFOCUS) ? EventType::WindowFocus : EventType::WindowLostFocus;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            bool repeat = (lp & (1 << 30)) != 0;
            Event e{};
            e.type = repeat ? EventType::KeyRepeat : EventType::KeyPressed;
            e.key.key = VkToKey(wp, lp);
            e.key.repeat = repeat;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            Event e{}; e.type = EventType::KeyReleased;
            e.key.key = VkToKey(wp, lp);
            e.key.repeat = false;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP: {
            bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
            MouseButton b = MouseButton::Left;
            if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) b = MouseButton::Right;
            if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) b = MouseButton::Middle;
            Event e{};
            e.type = down ? EventType::MouseButtonPressed : EventType::MouseButtonReleased;
            e.mouse_button.button = b;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_MOUSEMOVE: {
            Event e{}; e.type = EventType::MouseMoved;
            e.mouse_move.x = static_cast<f32>(GET_X_LPARAM(lp));
            e.mouse_move.y = static_cast<f32>(GET_Y_LPARAM(lp));
            e.mouse_move.dx = 0;  // 差分は Input::Update 側で計算
            e.mouse_move.dy = 0;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            Event e{}; e.type = EventType::MouseScrolled;
            e.mouse_scroll.x = 0;
            e.mouse_scroll.y = static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            w->DispatchEvent_Internal(e);
            return 0;
        }
        case WM_CHAR: {
            // 制御文字はスキップ
            if (wp >= 32) {
                Event e{}; e.type = EventType::CharInput;
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

Window::~Window() noexcept {
    if (_hwnd) {
        ::RemovePropW(static_cast<HWND>(_hwnd), kPropKey);
        ::DestroyWindow(static_cast<HWND>(_hwnd));
    }
}

Window::Window(Window&& o) noexcept
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

Window& Window::operator=(Window&& o) noexcept {
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

Result<Window> Window::Create(const WindowConfig& cfg) noexcept {
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

    Window w;
    w._hwnd = hwnd;
    w._width = cfg.width;
    w._height = cfg.height;
    w._should_close = false;

    // ムーブで返した後の安定アドレスに紐付けるため、いったん仮で登録 → ムーブ後に再登録
    Window result = Move(w);
    ::SetPropW(hwnd, kPropKey, &result);
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    return Result<Window>(OkInit, Move(result));
}

void Window::PollEvents() noexcept {
    MSG msg{};
    // PeekMessage でノンブロッキング処理（メインスレッドを止めない）
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

void Window::SetEventCallback(EventCallback cb, void* user) noexcept {
    _callback = cb;
    _callback_user = user;
}

void Window::SetTitle(const wchar_t* title) noexcept {
    if (_hwnd) ::SetWindowTextW(static_cast<HWND>(_hwnd), title);
}

void Window::SetFullscreen(bool on) noexcept {
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
