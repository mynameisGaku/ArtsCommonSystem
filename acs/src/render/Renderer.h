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
#include "render/IRhiTexture.h"

namespace acs {

class Window;

class Renderer {
public:
    Renderer() noexcept = default;
    ~Renderer() noexcept;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ウィンドウに紐付けて初期化（DX12 Device + Swapchain + CommandList を作成）
    // enable_depth=true なら深度バッファ (D32_Float) を自動で作成する
    Result<void> Init(Window& w, bool enable_debug = false, bool enable_depth = true) noexcept;

    // 全リソースを解放
    void Shutdown() noexcept;

    // フレーム開始（クリア色で塗りつぶし、深度は 1.0 でクリア）
    void BeginFrame(const ClearColor& clear) noexcept;

    // フレーム終了（GPU に投入 + Present）
    void EndFrame() noexcept;

    // ウィンドウサイズ変更時に呼ぶ（イベントハンドラから）
    void OnResize(u32 width, u32 height) noexcept;

    IRhiDevice*     Device()      const noexcept { return _device.Get(); }
    IRhiSwapchain*  Swapchain()   const noexcept { return _swapchain.Get(); }
    IRhiCommandList* CommandList() const noexcept { return _cmd.Get(); }
    IRhiTexture*    DepthBuffer() const noexcept { return _depth.Get(); }

    // 描画ターゲットのフォーマット（パイプライン作成時に必要）
    Format          ColorFormat() const noexcept { return _color_format; }
    Format          DepthFormat() const noexcept { return _depth_format; }

private:
    Result<void> RebuildDepth(u32 w, u32 h) noexcept;

    UniquePtr<IRhiDevice>      _device;
    UniquePtr<IRhiSwapchain>   _swapchain;
    UniquePtr<IRhiCommandList> _cmd;
    UniquePtr<IRhiTexture>     _depth;
    Format                      _color_format  = Format::B8G8R8A8_UNorm;
    Format                      _depth_format  = Format::D32_Float;
    u32                         _current_buffer = 0;
    bool                        _enable_depth   = true;
    bool                        _frame_open     = false;
};

} // namespace acs
