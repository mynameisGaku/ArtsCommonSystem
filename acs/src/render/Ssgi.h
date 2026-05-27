// SPDX-License-Identifier: Apache-2.0
// Screen-Space Global Illumination (Phase 33c)
//
// 各ピクセルから法線半球内に N 本のレイを screen-space で march し、ヒットした
// pixel の HDR 色を 1 バウンス indirect light として集める。SSAO の構造に色
// サンプリングを追加した発展版 (Aaltonen 2014 系の単純化版)。
//
// 入力: scene_color (HDR R16G16B16A16_Float)、scene_depth (shader-visible depth)
// 出力: ssgi_color (RGB)、FPbrShader が ambient/indirect 項に加算
//
// 制限:
//   - 法線は FMotionVector の normal G-buffer (world normal) から sample (Phase 34o、
//     旧 depth 微分 cross(ddx,ddy) は faceted で hemisphere ray がブロック状だった)
//   - 8 step / 4 ray = 32 sample/pixel (画質と速度のバランス)
//   - 反射的なシャープなパスは捨て、diffuse-ish な広い hemisphere に絞る
//   - 1 bounce のみ (Lumen の voxel cone tracing 等は未対応)
//   - blur 無し (FPbrShader 側で linear sampling で smooth に補間する想定)
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

class FSsgi {
public:
    FSsgi() noexcept = default;
    ~FSsgi() noexcept = default;

    FSsgi(const FSsgi&) = delete;
    FSsgi& operator=(const FSsgi&) = delete;

    TResult<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;
    void Shutdown() noexcept;
    TResult<void> Resize(u32 width, u32 height) noexcept;

    // SSGI を計算して内部 RT に書く (raw → blur → temporal の 3 pass)。
    //   scene_color: 現在フレームの HDR scene RT
    //   scene_depth: shader-visible depth (SSR/SSAO と同じ)
    //   normal_gbuffer: FMotionVector の world-space normal G-buffer (RGBA16F)
    //   prev_view_proj: 前フレームの view_proj (Phase 33c-3 temporal reproject 用)。
    //                   identity を渡すと reprojection 無効 (= 静的 accumulate)。
    //   intensity:   indirect light の倍率 (0=無効、1=neutral、>1=強調)
    //   max_distance: ray march の世界距離上限 (世界座標、典型 5.0)
    //   motion_texture: Phase 34f-3。非 null なら temporal pass が depth reprojection
    //                   ではなくこの motion vector で history を引く (動く mesh も
    //                   ghost せず追従)。FMotionVector::OutputTexture() を渡す。
    //                   null なら従来の camera-only depth reprojection。
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_color,
                IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                const FMat4& view_proj,
                const FMat4& inv_view_proj,
                const FMat4& prev_view_proj,
                FVec3 eye,
                f32 intensity   = 1.0f,
                f32 max_distance = 5.0f,
                IRhiTexture* motion_texture = nullptr) noexcept;

    // Phase 33c-3: temporal accumulation 後の history RT を返す (= 直近の Render
    // が書き込んだ index)。FPbrShader はこれを読む (blur + 時間積分でノイズ除去済)。
    IRhiTexture* OutputTexture() const noexcept {
        return m_History[m_TemporalFrame == 0u ? 0u : ((m_TemporalFrame - 1u) & 1u)].Get();
    }
    IRhiTexture* RawTexture()    const noexcept { return m_Output.Get(); }

private:
    TResult<void> CreateOutputRT(IRhiDevice& device, u32 w, u32 h) noexcept;
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             m_Device = nullptr;
    u32                     m_Width  = 0;
    u32                     m_Height = 0;

    TUniquePtr<IRhiTexture>  m_Output;       // SSGI raw
    TUniquePtr<IRhiTexture>  m_BlurOutput;  // depth-aware bilateral blur 後 (Phase 33c-2)
    TUniquePtr<IRhiTexture>  m_History[2];   // temporal accumulation ping-pong (Phase 33c-3)
    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiShader>   m_BlurPs;      // Phase 33c-2
    TUniquePtr<IRhiShader>   m_TemporalPs;  // Phase 33c-3
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiPipeline> m_BlurPipeline;    // Phase 33c-2
    TUniquePtr<IRhiPipeline> m_TemporalPipeline;// Phase 33c-3
    TUniquePtr<IRhiBuffer>   m_Cb;
    u32                     m_TemporalFrame = 0;
};

} // namespace acs
