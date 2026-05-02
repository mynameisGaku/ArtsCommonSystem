// Win32 ウィンドウ（イベント駆動、初学者向け簡易 API）
//
// 使い方:
//   WindowConfig cfg;
//   cfg.title  = L"MyGame";
//   cfg.width  = 1280;
//   cfg.height = 720;
//   auto wr = Window::Create(cfg);
//   if (wr.IsErr()) return -1;
//   Window& w = wr.Value();
//
//   while (!w.ShouldClose()) {
//       w.PollEvents();
//       // ゲーム処理
//   }
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "platform/Event.h"

namespace acs {

// ウィンドウ作成オプション
struct WindowConfig {
    const wchar_t* title       = L"ACS Window";
    u32            width       = 1280;
    u32            height      = 720;
    bool           resizable   = true;
    bool           fullscreen  = false;
    bool           vsync_hint  = true;
};

// イベントコールバック関数型（PollEvents 中に複数回呼ばれる）
using EventCallback = void (*)(void* user, const Event& e);

class Window {
public:
    Window() noexcept = default;
    ~Window() noexcept;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& o) noexcept;
    Window& operator=(Window&& o) noexcept;

    // ウィンドウ生成（失敗時は Err）
    static Result<Window> Create(const WindowConfig& cfg) noexcept;

    // メッセージキューを処理（メインループで毎フレーム呼ぶ）
    void PollEvents() noexcept;

    // × ボタン or 閉じる要求が来たか
    bool ShouldClose() const noexcept { return _should_close; }

    // 閉じる要求を出す（ゲームコード側からウィンドウを閉じたいとき）
    void Close() noexcept { _should_close = true; }

    // クライアント領域のサイズ
    u32 Width()  const noexcept { return _width; }
    u32 Height() const noexcept { return _height; }

    // ネイティブ HWND を取得（Render モジュールが Swapchain 作成に使う）
    void* NativeHandle() const noexcept { return _hwnd; }

    // イベント通知先を登録（user は callback に渡される任意ポインタ）
    void SetEventCallback(EventCallback cb, void* user) noexcept;

    // タイトル変更（FPS 表示など）
    void SetTitle(const wchar_t* title) noexcept;

    // 内部用: WindowProc から呼ばれるイベント発行
    void DispatchEvent_Internal(const Event& e) noexcept;
    // 内部用: WindowProc から呼ばれるサイズ更新
    void UpdateSize_Internal(u32 w, u32 h) noexcept { _width = w; _height = h; }

private:
    void*           _hwnd          = nullptr;   // HWND
    u32             _width         = 0;
    u32             _height        = 0;
    bool            _should_close  = false;
    EventCallback   _callback      = nullptr;
    void*           _callback_user = nullptr;
};

} // namespace acs
