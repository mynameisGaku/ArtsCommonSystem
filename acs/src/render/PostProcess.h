// HDR ポストプロセス (Bloom + Tonemap) パイプライン
//
// 想定ワークフロー:
//   1) Renderer.BeginFrame() の直前で PostProcess.BeginScenePass()
//      → HDR R16G16B16A16_Float RT に切替
//   2) シーン (Sky, StandardShader, Particles 等) を HDR で描画
//   3) PostProcess.Render(cmd, swapchain_buffer)
//      → Bloom (extract → downsample → upsample) → Tonemap → Backbuffer
//   4) Renderer.EndFrame() で Present
//
// 設計上の選択:
//   - Bloom は 5 段の mip chain (1/2, 1/4, ... 1/32) で downsample → upsample
//   - Tonemap は ACES Filmic (Narkowicz 2016 近似) → 最後に sRGB ガンマ
//   - パイプラインは Diligent backend を前提（Dx12 raw は未対応）
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"

namespace acs {

class IRhiTexture;

// ポストプロセス効果のパラメータ
struct PostProcessParams {
    // Bloom
    bool  bloom_enabled    = true;
    f32   bloom_threshold  = 1.0f;     // この輝度を超えるピクセルを Bloom 元として抽出 (HDR scale)
    f32   bloom_intensity  = 0.6f;     // Bloom を加算する強度 (0..2)
    f32   bloom_radius     = 1.0f;     // upsample 時の半径スケール

    // Tonemap
    f32   exposure         = 1.0f;     // 露出 (1.0 = 中性)
    f32   gamma            = 2.2f;     // 出力ガンマ
    // 0=ACES Filmic (Narkowicz)、1=AgX (Sobotka)、2=Reinhard 拡張
    // AgX は彩度を控えめにする tonemap (UE5 デフォルトに近い neutral look)。
    // 既存サンプル互換のため初期値は ACES。
    i32   tonemap_kind     = 0;

    // Cinematic post-FX (Phase 34a)
    f32   vignette_intensity = 0.2f;   // 端の暗化強度 0..1
    f32   vignette_radius    = 0.5f;   // vignette が始まる半径 (0=中心、1=端)
    f32   chromatic_aberration = 0.002f; // RGB 半径方向の offset。0 で disable
    f32   grain_intensity    = 0.015f; // film grain 強度 0..0.1
    f32   grain_time         = 0.0f;   // procedural noise シード (Application から dt 累積)

    // SSR composite (Phase 34e、Ssr.OutputTexture() を直接 tonemap 直前に additive 加算)
    // null で SSR 無し。Bloom と並んで mix される。intensity は SSR shader 側で適用済 (二重なし)。
    IRhiTexture* ssr_texture = nullptr;
    f32          ssr_intensity = 1.0f; // tonemap 側で SSR の最終 mix 強度 (デフォルト 1.0)
};

class PostProcess {
public:
    PostProcess() noexcept = default;
    ~PostProcess() noexcept;

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // 初期化: HDR RT + Bloom mip chain + Tonemap pipeline を作成
    //   width / height: 出力解像度（通常はバックバッファサイズ）
    //   color_format  : 最終出力 (バックバッファ) のフォーマット
    Result<void> Init(IRhiDevice& device, u32 width, u32 height,
                       Format color_format) noexcept;

    void Shutdown() noexcept;

    // ウィンドウサイズ変更時に呼ぶ（HDR RT 等を再作成）
    Result<void> Resize(u32 width, u32 height) noexcept;

    // シーンが描かれる HDR RT を取得（Renderer がここに描画する）
    IRhiTexture* HdrRenderTarget() const noexcept { return _hdr_rt.Get(); }
    Format       HdrFormat()       const noexcept { return _hdr_format; }

    // Bloom + Tonemap を実行して swapchain buffer に書き出す
    //   cmd      : 既に Begin 済みの command list
    //   swapchain: 出力先（backbuffer をこのインスタンスから取り出す）
    //   buffer_index: AcquireNextImage の戻り値
    //   params   : 効果のパラメータ
    void Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index,
                const PostProcessParams& params) noexcept;

private:
    // mip chain 段数（1/2 から 1/32 までの 5 段）
    static constexpr u32 kBloomMips = 5;

    Result<void> CreateRenderTargets(IRhiDevice& device, u32 w, u32 h) noexcept;
    Result<void> CreatePipelines(IRhiDevice& device) noexcept;

    void Pass_Extract  (IRhiCommandList& cmd, const PostProcessParams& p) noexcept;
    void Pass_Downsample(IRhiCommandList& cmd, u32 from_mip) noexcept;
    void Pass_Upsample (IRhiCommandList& cmd, u32 to_mip, f32 radius) noexcept;
    void Pass_Tonemap  (IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buf_idx,
                        const PostProcessParams& p) noexcept;

    IRhiDevice* _device = nullptr;
    u32         _width  = 0;
    u32         _height = 0;
    Format      _color_format = Format::B8G8R8A8_UNorm;
    Format      _hdr_format   = Format::R16G16B16A16_Float;

    // メイン HDR RT (シーン描画先)
    UniquePtr<IRhiTexture> _hdr_rt;

    // Bloom mip chain (各段は HDR、解像度は半分ずつ)
    UniquePtr<IRhiTexture> _bloom_mips[kBloomMips];

    // パイプライン
    UniquePtr<IRhiShader>   _vs_fullscreen;     // 共通の全画面三角形 VS
    UniquePtr<IRhiShader>   _ps_extract;
    UniquePtr<IRhiShader>   _ps_downsample;
    UniquePtr<IRhiShader>   _ps_upsample;
    UniquePtr<IRhiShader>   _ps_tonemap;
    UniquePtr<IRhiPipeline> _pipe_extract;     // HDR → bloom_mips[0]
    UniquePtr<IRhiPipeline> _pipe_downsample;  // bloom_mips[i] → bloom_mips[i+1]
    UniquePtr<IRhiPipeline> _pipe_upsample;    // bloom_mips[i+1] + bloom_mips[i] → bloom_mips[i]
    UniquePtr<IRhiPipeline> _pipe_tonemap;     // HDR + bloom_mips[0] → backbuffer

    // 共通の動的 CB
    UniquePtr<IRhiBuffer>   _cb_post;          // 各パスのパラメータを統一して入れる
};

} // namespace acs
