// SPDX-License-Identifier: Apache-2.0
// Screen-Space Reflection (Phase 34e)
//
// 設計:
//   - シーン描画後の HDR scene color + depth-SRV を入力にして、各 pixel から
//     reflection ray を screen space で march
//   - 衝突したら scene_color を sample、ray 起点に reflection を加算
//   - 結果は別 HDR RT (= ssr_rt) に書き出し → caller が composite で本 HDR へ加算
//   - normal は FMotionVector パスが出力する normal G-buffer (RGBA16F world normal)
//     から sample する (Phase 34m)。旧 depth-derivative cross(ddx,ddy) は 2x2 quad
//     単位で faceted になり、曲面の反射ベクトルが段差状になってガビガビだった
//
// ray march は screen-space DDA (McGuire & Mara 2014、Phase 34n)。反射レイを
// screen 空間へ射影し 1 texel/step で行進する。world 空間固定ステップ march は
// screen 空間でサンプリングが疎になり、反射先がカメラ近傍/grazing だと 1 step が
// 多 pixel に飛んで反射像がレイ方向に伸びてスメアしていた — DDA で根本解決。
//
// Phase 34e-3: temporal accumulation。raw SSR を per-frame jitter 付きで撃ち、
// 履歴 (reproject + neighborhood clamp) と時間方向に平均することで、ray-march の
// silhouette ジャギーを均す。OutputTexture() は temporal 累積後を返す。
//
// glossy reflection (roughness 別 mip サンプル) は未対応 — roughness 依存の blend は
// FPbrShader 側 (Phase 34e-2fix) が担当。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"

namespace acs {

class FSsr {
public:
    FSsr() noexcept = default;
    ~FSsr() noexcept = default;

    FSsr(const FSsr&) = delete;
    FSsr& operator=(const FSsr&) = delete;

    // hdr_format: SSR 出力テクスチャのフォーマット (シーン HDR と同じ R16G16B16A16F 推奨)
    TResult<void> Init(IRhiDevice& device, EFormat hdr_format,
                       u32 width, u32 height) noexcept;
    void Shutdown() noexcept;

    TResult<void> Resize(u32 width, u32 height) noexcept;

    // SSR を計算する (raw march → temporal accumulation の 2 pass)。
    //   scene_color:    現フレームの HDR scene
    //   scene_depth:    現フレームの depth buffer (shader_visible_depth=true 必須)
    //   normal_gbuffer: FMotionVector パスの world-space normal G-buffer (RGBA16F)
    //   view_proj / inv_view_proj: 現フレームのカメラ行列 (row-major)
    //   prev_view_proj: 前フレームの view_proj (Phase 34e-3 temporal reproject 用)。
    //                   identity を渡すと reprojection 無効 (静的 accumulate)。
    //   eye: カメラワールド位置
    //   intensity: SSR 強度 (0..2)
    //   motion_texture: Phase 34p。非 null なら temporal pass が動く mesh の反射を
    //                   motion vector で reproject (null なら camera-only depth
    //                   reproject)。FMotionVector::OutputTexture() を渡す。
    //   hiz_texture: Phase 36-3a。非 null なら 1/8 解像度 coarse min-depth として
    //                ray march の skip-ahead に使う (FHiZ::Texture())。null なら
    //                旧 1 texel/step DDA (動作互換)。空中レイで 4-6 倍高速化。
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_color, IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                const FMat4& view_proj, const FMat4& inv_view_proj,
                const FMat4& prev_view_proj,
                FVec3 eye, f32 intensity = 0.6f,
                IRhiTexture* motion_texture = nullptr,
                IRhiTexture* hiz_texture    = nullptr) noexcept;

    // Phase 34e-3: temporal accumulation 後の history を返す (FPbrShader / overlay 用)。
    IRhiTexture* OutputTexture() const noexcept {
        return m_History[m_TemporalFrame == 0u ? 0u : ((m_TemporalFrame - 1u) & 1u)].Get();
    }
    IRhiTexture*  RawTexture()    const noexcept { return m_Output.Get(); }
    EFormat        OutputFormat() const noexcept { return m_HdrFormat; }

private:
    TResult<void> CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept;
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             m_Device = nullptr;
    u32                     m_Width  = 0;
    u32                     m_Height = 0;
    EFormat                  m_HdrFormat = EFormat::R16G16B16A16_Float;

    TUniquePtr<IRhiTexture>  m_Output;        // raw SSR (jitter 付き march)
    TUniquePtr<IRhiTexture>  m_History[2];    // Phase 34e-3: temporal accumulation ping-pong
    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiShader>   m_TemporalPs;   // Phase 34e-3
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiPipeline> m_TemporalPipeline;  // Phase 34e-3
    TUniquePtr<IRhiBuffer>   m_Cb;
    u32                     m_TemporalFrame = 0;
};

} // namespace acs
