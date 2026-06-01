// SPDX-License-Identifier: Apache-2.0
// Win32 ウィンドウ（イベント駆動、初学者向け簡易 API）
//
// 使い方:
//   FWindowConfig cfg;
//   cfg.title  = L"MyGame";
//   cfg.width  = 1280;
//   cfg.height = 720;
//   auto wr = FWindow::Create(cfg);
//   if (wr.IsErr()) return -1;
//   FWindow& w = wr.Value();
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
struct FWindowConfig {
    const wchar_t* title       = L"ACS FWindow";
    u32            width       = 1280;
    u32            height      = 720;
    bool           resizable   = true;
    bool           fullscreen  = false;
    bool           vsync_hint  = true;
};

// イベントコールバック関数型（PollEvents 中に複数回呼ばれる）
using EventCallback = void (*)(void* user, const Event& e);

class FWindow {
public:
    FWindow() noexcept = default;
    ~FWindow() noexcept;

    FWindow(const FWindow&) = delete;
    FWindow& operator=(const FWindow&) = delete;
    FWindow(FWindow&& o) noexcept;
    FWindow& operator=(FWindow&& o) noexcept;

    // ウィンドウ生成（失敗時は Err）
    static TResult<FWindow> Create(const FWindowConfig& cfg) noexcept;

    // メッセージキューを処理（メインループで毎フレーム呼ぶ）
    void PollEvents() noexcept;

    // × ボタン or 閉じる要求が来たか
    bool ShouldClose() const noexcept { return m_ShouldClose; }

    // 閉じる要求を出す（ゲームコード側からウィンドウを閉じたいとき）
    void Close() noexcept { m_ShouldClose = true; }

    // クライアント領域のサイズ
    u32 Width()  const noexcept { return m_Width; }
    u32 Height() const noexcept { return m_Height; }

    // ネイティブ HWND を取得（Render モジュールが Swapchain 作成に使う）
    void* NativeHandle() const noexcept { return m_Hwnd; }

    // イベント通知先を登録（user は callback に渡される任意ポインタ）
    void SetEventCallback(EventCallback cb, void* user) noexcept;

    // タイトル変更（FPS 表示など）
    void SetTitle(const wchar_t* title) noexcept;

    // ボーダーレス全画面の ON/OFF。元のウィンドウ位置・スタイルを記憶して復元する。
    void SetFullscreen(bool on) noexcept;
    bool IsFullscreen() const noexcept { return m_Fullscreen; }

    // 内部用: WindowProc から呼ばれるイベント発行
    void DispatchEvent_Internal(const Event& e) noexcept;
    // 内部用: WindowProc から呼ばれるサイズ更新
    void UpdateSize_Internal(u32 w, u32 h) noexcept { m_Width = w; m_Height = h; }

private:
    void*           m_Hwnd          = nullptr;   // HWND
    u32             m_Width         = 0;
    u32             m_Height        = 0;
    bool            m_ShouldClose  = false;
    EventCallback   m_Callback      = nullptr;
    void*           m_CallbackUser = nullptr;
    // フルスクリーン状態。ムーブ (ctor / operator=) でも保存矩形/スタイルごと引き継ぐ。
    bool            m_Fullscreen    = false;
    i32             m_SavedStyle   = 0;          // 全画面化前の GWL_STYLE
    i32             m_SavedRect[4] {};           // 全画面化前の窓矩形 (l,t,r,b)
};

} // namespace acs
