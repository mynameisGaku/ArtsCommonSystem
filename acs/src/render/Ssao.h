// SPDX-License-Identifier: Apache-2.0
// Screen-Space Ambient Occlusion (Phase 34j)
//
// HBAO-lite: depth-only horizon-based AO。各 pixel から N 方向に screen-space で
// march して horizon angle を求め、ambient occlusion を出す。
//
// 出力は R8G8B8A8_UNorm の RT。.r = AO visibility、.g = contact shadow (Phase 34q、
// ともに 1=照明 / 0=遮蔽)。FPbrShader が .r を ambient、.g を direct light に乗算する。
//
// 制限:
//   - 法線は FMotionVector の normal G-buffer (world normal) を view 空間へ変換して
//     使う (Phase 34o、旧 depth 微分 cross(ddx,ddy) は faceted で AO がブロック状だった)
//   - 単純な uniform random direction、Halton 等の low-discrepancy 未使用
//   - 6 direction × 6 step = 36 sample / pixel (Phase 34j-3)
//   - Phase 34j-4: depth-aware bilateral blur pass を追加 (raw → blurred)。
//     OutputTexture() は blur 後を返す。
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

class FSsao {
public:
    FSsao() noexcept = default;
    ~FSsao() noexcept = default;

    FSsao(const FSsao&) = delete;
    FSsao& operator=(const FSsao&) = delete;

    TResult<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;
    void Shutdown() noexcept;
    TResult<void> Resize(u32 width, u32 height) noexcept;

    // SSAO (Phase 34j-6 から GTAO) を計算して内部 RT に書く。
    //   scene_depth: shader_visible_depth=true な depth buffer
    //   normal_gbuffer: FMotionVector の world-space normal G-buffer (RGBA16F)
    //   inv_view_proj: 現フレーム VP の逆 (depth+uv → world)
    //   view: world → view 変換 (GTAO の slice 計算は view 空間で行う)
    //   eye: カメラ world pos
    //   light_dir: dir light 0 への方向 (world、surface→light)。contact shadow march 方向。
    //   intensity: 遮蔽強度 (0=AO 無効、1=neutral、>1=過度に暗く)
    //   radius:    AO の最大半径 (world units、0.5 ~ 2.0 が典型)
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                const FMat4& view_proj,
                const FMat4& inv_view_proj,
                const FMat4& view,
                FVec3 eye, FVec3 light_dir,
                f32 intensity = 1.0f,
                f32 radius    = 0.5f) noexcept;

    // Phase 34j-4: blur 後の RT を返す (FPbrShader / overlay はこちらを読む)。
    IRhiTexture* OutputTexture() const noexcept { return _blur_output.Get(); }
    // raw (blur 前) が必要なデバッグ用途向け。
    IRhiTexture* RawTexture() const noexcept { return _output.Get(); }

private:
    TResult<void> CreateOutputRT(IRhiDevice& device, u32 w, u32 h) noexcept;
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             _device = nullptr;
    u32                     _width  = 0;
    u32                     _height = 0;

    TUniquePtr<IRhiTexture>  _output;       // SSAO raw
    TUniquePtr<IRhiTexture>  _blur_output;  // depth-aware bilateral blur 後 (Phase 34j-4)
    TUniquePtr<IRhiShader>   _vs;
    TUniquePtr<IRhiShader>   _ps;
    TUniquePtr<IRhiShader>   _blur_ps;      // Phase 34j-4 blur PS
    TUniquePtr<IRhiPipeline> _pipeline;
    TUniquePtr<IRhiPipeline> _blur_pipeline;// Phase 34j-4
    TUniquePtr<IRhiBuffer>   _cb;
};

} // namespace acs
