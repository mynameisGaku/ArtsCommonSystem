// SPDX-License-Identifier: Apache-2.0
// HDR ポストプロセス (Bloom + Tonemap) パイプライン
//
// 想定ワークフロー:
//   1) FRenderer.BeginFrame() の直前で FPostProcess.BeginScenePass()
//      → HDR R16G16B16A16_Float RT に切替
//   2) シーン (FSky, FStandardShader, Particles 等) を HDR で描画
//   3) FPostProcess.Render(cmd, swapchain_buffer)
//      → Bloom (extract → downsample → upsample) → Tonemap → Backbuffer
//   4) FRenderer.EndFrame() で Present
//
// 設計上の選択:
//   - Bloom は 5 段の mip chain (1/2, 1/4, ... 1/32) で downsample → upsample
//   - Tonemap は ACES Filmic (Narkowicz 2016 近似) → 最後に sRGB ガンマ
//   - パイプラインは Diligent backend を前提（Dx12 raw は未対応）
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
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
struct FPostProcessParams {
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
    f32   grain_time         = 0.0f;   // procedural noise シード (FApplication から dt 累積)

    // SSR composite (Phase 34e、FSsr.OutputTexture() を直接 tonemap 直前に additive 加算)
    // null で SSR 無し。Bloom と並んで mix される。intensity は SSR shader 側で適用済 (二重なし)。
    IRhiTexture* ssr_texture = nullptr;
    f32          ssr_intensity = 1.0f; // tonemap 側で SSR の最終 mix 強度 (デフォルト 1.0)

    // SSAO の composite は FPbrShader 側に移動 (Phase 34j-2)。
    // 理由: tonemap PSO の slot 3 に新規 SRV を追加すると Diligent の combined
    // sampler binding が期待通り動かず、sample が常に 0 を返す現象に当たった
    // (詳細は Phase 34j-fix の commit メッセージ参照)。
    // FPbrShader はすでに 6 slot で安定運用しており、slot 6 に SSAO を追加する
    // 方が確実に動作する。HelloIbl から FPbrShader::SetSsao() で渡す。

    // FColor grading (Phase 34h、ASC-CDL 風)。tonemap 後 / gamma 前に適用。
    f32  cg_saturation   = 1.0f;       // 0=モノクロ、1=neutral、>1=彩度ブースト
    f32  cg_contrast     = 1.0f;       // 0..2、中心 0.5 を pivot とした contrast curve
    f32  cg_temperature  = 0.0f;       // -1=cool/blue、0=neutral、+1=warm/orange
    f32  cg_tint         = 0.0f;       // -1=green、0=neutral、+1=magenta
    FVec3 cg_lift         = FVec3{0, 0, 0};   // shadow 域の色 offset (足し算)
    FVec3 cg_gain         = FVec3{1, 1, 1};   // highlight 域の色 multiplier

    // CAS Sharpening (Phase 34i、AMD FSR 簡略版)。FColor grading 後 / gamma 前に適用。
    // 0 = disable、0.3 = subtle、0.6 = neutral、1.0 = strong。負値は不可。
    f32  cas_strength    = 0.0f;

    // TAA (Phase 34f、Temporal Anti-Aliasing)。Halton jitter を FCamera で applied
    // した上で、history と neighborhood-clamp blend して resolve する。
    // Phase 34f-2: 任意で depth_texture + view_proj + prev_view_proj を渡すと
    // camera motion 由来の history reprojection を行う (動く mesh は対象外)。
    //   false: 無効、tonemap は直接 HDR RT を読む
    //   true : Pass_TaaResolve を実行、tonemap は resolved RT を読む
    bool taa_enabled = false;
    // history 重み (現フレームに何%取り込むか)。0.1 = 10% current + 90% history が
    // 標準。値が小さいほどスムージング強い (motion ghost も増える)。
    f32  taa_blend_factor = 0.1f;
    // TAA reprojection 用 (Phase 34f-2)。null なら motion=0 (= 静的 reprojection 無し、
    // カメラ動かしたとき ghost する)。set すると camera motion 由来の motion vec で
    // history を offset sample する。
    IRhiTexture* taa_depth_texture = nullptr;
    FMat4         taa_view_proj_no_jitter{};      // Halton 適用前の view_proj
    FMat4         taa_prev_view_proj_no_jitter{}; // 前フレームの view_proj (Halton 適用前)
    // Phase 34f-3: 動的 mesh 対応の motion vector テクスチャ (FMotionVector モジュール)。
    // 非 null なら TAA は depth reprojection の代わりにこの texture で history を
    // 引く。motion vector は camera 動き + object 動きの両方を含むので、動く mesh の
    // ghost / trail が消える。null なら従来の depth reprojection にフォールバック。
    // depth slot (t2) を再利用して bind するため TAA resolve PSO の slot 数は不変。
    IRhiTexture* taa_motion_texture = nullptr;

    // Auto-exposure (Phase 34k-2): GPU でシーンの平均輝度を測定し、露出を自動算出する。
    // Phase 34k の CPU eye-adaptation を「実測輝度ベース」へ置き換える本実装。
    //   false: 従来どおり exposure をそのまま使う (既存サンプル互換)。
    //   true : luma reduction → 露出順応 → ExposureApply の 3 pass を内部で実行。
    //          露出はシーン輝度から自動算出され、exposure は「自動露出にさらに掛ける
    //          手動補正 (EV compensation)」として働く (中性 = 1.0)。
    bool auto_exposure_enabled = false;
    f32  auto_exposure_key     = 0.5f;    // 露出後の目標平均輝度。大きいほど明るく写る
    f32  auto_exposure_min     = 0.05f;   // 自動露出の下限 (明所での白飛びを防ぐ)
    f32  auto_exposure_max     = 12.0f;   // 自動露出の上限 (暗所での黒つぶれを防ぐ)
    f32  auto_exposure_speed   = 1.8f;    // eye adaptation 速度 (/秒、大きいほど速く順応)
    f32  delta_time            = 0.0166f; // 露出順応の時間補間に使うフレーム時間
};

class FPostProcess {
public:
    FPostProcess() noexcept = default;
    ~FPostProcess() noexcept;

    FPostProcess(const FPostProcess&) = delete;
    FPostProcess& operator=(const FPostProcess&) = delete;

    // 初期化: HDR RT + Bloom mip chain + Tonemap pipeline を作成
    //   width / height: 出力解像度（通常はバックバッファサイズ）
    //   color_format  : 最終出力 (バックバッファ) のフォーマット
    TResult<void> Init(IRhiDevice& device, u32 width, u32 height,
                       EFormat color_format) noexcept;

    void Shutdown() noexcept;

    // ウィンドウサイズ変更時に呼ぶ（HDR RT 等を再作成）
    TResult<void> Resize(u32 width, u32 height) noexcept;

    // シーンが描かれる HDR RT を取得（FRenderer がここに描画する）
    IRhiTexture* HdrRenderTarget() const noexcept { return _hdr_rt.Get(); }
    EFormat       HdrFormat()       const noexcept { return _hdr_format; }

    // Bloom + Tonemap を実行して swapchain buffer に書き出す
    //   cmd      : 既に Begin 済みの command list
    //   swapchain: 出力先（backbuffer をこのインスタンスから取り出す）
    //   buffer_index: AcquireNextImage の戻り値
    //   params   : 効果のパラメータ
    void Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index,
                const FPostProcessParams& params) noexcept;

private:
    // mip chain 段数（1/2 から 1/32 までの 5 段）
    // Phase 36-1: Downsample を真の Jimenez 13-tap に直しただけで「がびがび」は
    // 解消したので mip 段数は 5 段に維持 (6 段化は upsample additive pass が
    // 1 回増える分 bloom 強度が ~25% lift して halo が過剰になる)。
    static constexpr u32 kBloomMips = 5;

    TResult<void> CreateRenderTargets(IRhiDevice& device, u32 w, u32 h) noexcept;
    TResult<void> CreatePipelines(IRhiDevice& device) noexcept;

    void Pass_Extract  (IRhiCommandList& cmd, const FPostProcessParams& p) noexcept;
    void Pass_Downsample(IRhiCommandList& cmd, u32 from_mip) noexcept;
    void Pass_Upsample (IRhiCommandList& cmd, u32 to_mip, f32 radius) noexcept;
    void Pass_TaaResolve(IRhiCommandList& cmd, const FPostProcessParams& p) noexcept;
    void Pass_Tonemap  (IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buf_idx,
                        const FPostProcessParams& p) noexcept;

    // Auto-exposure (Phase 34k-2)
    void Pass_LumaReduce  (IRhiCommandList& cmd) noexcept;
    void Pass_ExposureAdapt(IRhiCommandList& cmd, const FPostProcessParams& p) noexcept;
    void Pass_ExposureApply(IRhiCommandList& cmd) noexcept;
    // 下流パス (TAA / Bloom / Tonemap) が読むシーン texture。auto-exposure 有効なら
    // 露出適用後の _exposed_rt、そうでなければ raw _hdr_rt。
    IRhiTexture* SceneInput(const FPostProcessParams& p) const noexcept;

    IRhiDevice* _device = nullptr;
    u32         _width  = 0;
    u32         _height = 0;
    EFormat      _color_format = EFormat::B8G8R8A8_UNorm;
    EFormat      _hdr_format   = EFormat::R16G16B16A16_Float;

    // メイン HDR RT (シーン描画先)
    TUniquePtr<IRhiTexture> _hdr_rt;

    // Bloom mip chain (各段は HDR、解像度は半分ずつ)
    TUniquePtr<IRhiTexture> _bloom_mips[kBloomMips];

    // パイプライン
    TUniquePtr<IRhiShader>   _vs_fullscreen;     // 共通の全画面三角形 VS
    TUniquePtr<IRhiShader>   _ps_extract;
    TUniquePtr<IRhiShader>   _ps_downsample;
    TUniquePtr<IRhiShader>   _ps_upsample;
    TUniquePtr<IRhiShader>   _ps_tonemap;
    TUniquePtr<IRhiPipeline> _pipe_extract;     // HDR → bloom_mips[0]
    TUniquePtr<IRhiPipeline> _pipe_downsample;  // bloom_mips[i] → bloom_mips[i+1]
    TUniquePtr<IRhiPipeline> _pipe_upsample;    // bloom_mips[i+1] + bloom_mips[i] → bloom_mips[i]
    TUniquePtr<IRhiPipeline> _pipe_tonemap;     // HDR + bloom_mips[0] → backbuffer

    // 共通の動的 CB
    TUniquePtr<IRhiBuffer>   _cb_post;          // 各パスのパラメータを統一して入れる

    // TAA (Phase 34f): ping-pong history RT + resolve pipeline。
    // _taa[N%2] が current frame の resolved 出力、_taa[(N+1)%2] が previous frame の
    // resolved 出力 (= history input)。frame index で role を swap する。
    TUniquePtr<IRhiTexture>  _taa[2];
    TUniquePtr<IRhiShader>   _ps_taa_resolve;
    TUniquePtr<IRhiPipeline> _pipe_taa_resolve;
    u32                     _taa_frame = 0;
    TUniquePtr<IRhiBuffer>   _cb_taa_reproj;     // Phase 34f-2: separate b1 で行列 2 枚
    TUniquePtr<IRhiTexture>  _taa_depth_fb;      // depth 未指定時の fallback (1x1)

    // Auto-exposure (Phase 34k-2): GPU luminance 測定 + eye adaptation。
    // _luma_mips は _hdr_rt の 1/2 から 1x1 まで縮約する mip chain。最深段 (1x1) に
    // シーン平均 log2 輝度が入る。_exposure[2] は順応済み露出を保持する 1x1 ping-pong
    // (frame N が _exposure[N%2] に書き、_exposure[(N+1)%2] = 前フレーム値を読む)。
    // _exposed_rt は _hdr_rt に自動露出を掛けた結果で、下流パスはこれを読む。
    static constexpr u32 kMaxLumaMips = 13;     // 4K (3840) でも floor(log2)+1 に収まる
    TUniquePtr<IRhiTexture>  _luma_mips[kMaxLumaMips];
    u32                     _luma_mip_count = 0;
    TUniquePtr<IRhiTexture>  _exposure[2];       // 1x1、順応済み露出 (ping-pong)
    TUniquePtr<IRhiTexture>  _exposed_rt;        // _hdr_rt * auto exposure
    TUniquePtr<IRhiShader>   _ps_luma_extract;   // HDR → log2 輝度 (downsample 兼)
    TUniquePtr<IRhiShader>   _ps_luma_down;      // log2 輝度の box average
    TUniquePtr<IRhiShader>   _ps_exposure;       // 露出順応 (eye adaptation)
    TUniquePtr<IRhiShader>   _ps_expose_apply;   // _hdr_rt * 露出 → _exposed_rt
    TUniquePtr<IRhiPipeline> _pipe_luma_extract;
    TUniquePtr<IRhiPipeline> _pipe_luma_down;
    TUniquePtr<IRhiPipeline> _pipe_exposure;
    TUniquePtr<IRhiPipeline> _pipe_expose_apply;
    TUniquePtr<IRhiBuffer>   _cb_auto;           // auto-exposure 用パラメータ CB
    u32                     _auto_frame = 0;    // 露出 ping-pong / cold-start 判定用
    EFormat                  _luma_format = EFormat::R16G16_Float;
};

} // namespace acs
