// SPDX-License-Identifier: Apache-2.0
// 高レベル FRenderer（ウィンドウへの描画ループを統括する司令塔）
//
// 使い方 (典型的):
//   FRenderer rdr;
//   rdr.Init(window);
//   while (!window.ShouldClose()) {
//       window.PollEvents();
//       rdr.BeginFrame({0.1f, 0.2f, 0.3f, 1.0f});
//       // BeginFrame 後は GetCommandList() でコマンドを積める。
//       // 高レベルヘルパ:
//       //   FStandardShader  — Lambert+Blinn-Phong + シャドウマップ
//       //   FSkinnedShader   — GPU スキニング (BoneCB + BLENDINDICES/WEIGHT)
//       //   FSky             — 手続き生成スカイ (Day/Sunset/Night プリセット)
//       //   FSpriteBatch     — 2D スプライト + フォント
//       //   FFont            — TTrueType -> アトラス -> FSpriteBatch
//       //   FParticleSystem  — 簡易 GPU パーティクル
//       //   FShadowMap       — depth-only パス
//       //   FPostProcess     — HDR + Bloom + ACES Tonemap (Diligent backend 専用)
//       rdr.EndFrame();
//   }
//   rdr.Shutdown();
//
// HDR ポストプロセス経路を組むときは FApplication::OnCustomFrame() を override
// して、自分で BeginRenderToTexture(HDR_RT) → 描画 → FPostProcess.Render() →
// Present までを直接書く (HelloBloom 参照)。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"

namespace acs {

class FWindow;

class FRenderer {
public:
    FRenderer() noexcept = default;
    ~FRenderer() noexcept;

    FRenderer(const FRenderer&) = delete;
    FRenderer& operator=(const FRenderer&) = delete;

    // ウィンドウに紐付けて初期化（DX12 Device + Swapchain + CommandList を作成）
    // enable_depth=true なら深度バッファ (D32_Float) を自動で作成する
    TResult<void> Init(FWindow& w, bool enable_debug = false, bool enable_depth = true) noexcept;

    // 全リソースを解放
    void Shutdown() noexcept;

    // フレーム開始（クリア色で塗りつぶし、深度は 1.0 でクリア）
    void BeginFrame(const FClearColor& clear) noexcept;

    // フレーム終了（GPU に投入 + Present）
    void EndFrame() noexcept;

    // ウィンドウサイズ変更時に呼ぶ（イベントハンドラから）
    void OnResize(u32 width, u32 height) noexcept;

    IRhiDevice*     Device()      const noexcept { return _device.Get(); }
    IRhiSwapchain*  Swapchain()   const noexcept { return _swapchain.Get(); }
    IRhiCommandList* CommandList() const noexcept { return _cmd.Get(); }
    IRhiTexture*    DepthBuffer() const noexcept { return _depth.Get(); }

    // 描画ターゲットのフォーマット（パイプライン作成時に必要）
    EFormat          ColorFormat() const noexcept { return _color_format; }
    EFormat          DepthFormat() const noexcept { return _depth_format; }

private:
    TResult<void> RebuildDepth(u32 w, u32 h) noexcept;

    TUniquePtr<IRhiDevice>      _device;
    TUniquePtr<IRhiSwapchain>   _swapchain;
    TUniquePtr<IRhiCommandList> _cmd;
    TUniquePtr<IRhiTexture>     _depth;
    EFormat                      _color_format  = EFormat::B8G8R8A8_UNorm;
    EFormat                      _depth_format  = EFormat::D32_Float;
    u32                         _current_buffer = 0;
    bool                        _enable_depth   = true;
    bool                        _frame_open     = false;
};

} // namespace acs
