// 高レベル Renderer（初学者向け、ウィンドウへ簡単に描画する API）
//
// 使い方:
//   Renderer rdr;
//   rdr.Init(window);
//   while (!window.ShouldClose()) {
//       window.PollEvents();
//       rdr.BeginFrame({0.1f, 0.2f, 0.3f, 1.0f});  // 背景色を渡してクリア
//       // ここで描画コマンドを追加（v1 では Clear のみ）
//       rdr.EndFrame();
//   }
//   rdr.Shutdown();
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiCommandList.h"

namespace acs {

class Window;

class Renderer {
public:
    Renderer() noexcept = default;
    ~Renderer() noexcept;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ウィンドウに紐付けて初期化（DX12 Device + Swapchain + CommandList を作成）
    Result<void> Init(Window& w, bool enable_debug = false) noexcept;

    // 全リソースを解放
    void Shutdown() noexcept;

    // フレーム開始（クリア色で塗りつぶし）
    void BeginFrame(const ClearColor& clear) noexcept;

    // フレーム終了（GPU に投入 + Present）
    void EndFrame() noexcept;

    // ウィンドウサイズ変更時に呼ぶ（イベントハンドラから）
    void OnResize(u32 width, u32 height) noexcept;

    IRhiDevice*     Device()      const noexcept { return _device.Get(); }
    IRhiSwapchain*  Swapchain()   const noexcept { return _swapchain.Get(); }
    IRhiCommandList* CommandList() const noexcept { return _cmd.Get(); }

private:
    UniquePtr<IRhiDevice>      _device;
    UniquePtr<IRhiSwapchain>   _swapchain;
    UniquePtr<IRhiCommandList> _cmd;
    u32                         _current_buffer = 0;
    bool                        _frame_open     = false;
};

} // namespace acs
