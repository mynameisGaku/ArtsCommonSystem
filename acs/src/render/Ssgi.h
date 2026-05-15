// Screen-Space Global Illumination (Phase 33c)
//
// 各ピクセルから法線半球内に N 本のレイを screen-space で march し、ヒットした
// pixel の HDR 色を 1 バウンス indirect light として集める。SSAO の構造に色
// サンプリングを追加した発展版 (Aaltonen 2014 系の単純化版)。
//
// 入力: scene_color (HDR R16G16B16A16_Float)、scene_depth (shader-visible depth)
// 出力: ssgi_color (RGB)、PbrShader が ambient/indirect 項に加算
//
// 制限:
//   - depth-only normal 再構成 (G-buffer normal なし)、SSAO と同じ式
//   - 8 step / 4 ray = 32 sample/pixel (画質と速度のバランス)
//   - 反射的なシャープなパスは捨て、diffuse-ish な広い hemisphere に絞る
//   - 1 bounce のみ (Lumen の voxel cone tracing 等は未対応)
//   - blur 無し (PbrShader 側で linear sampling で smooth に補間する想定)
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

class Ssgi {
public:
    Ssgi() noexcept = default;
    ~Ssgi() noexcept = default;

    Ssgi(const Ssgi&) = delete;
    Ssgi& operator=(const Ssgi&) = delete;

    Result<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;
    void Shutdown() noexcept;
    Result<void> Resize(u32 width, u32 height) noexcept;

    // SSGI を計算して内部 RT に書く (raw → blur → temporal の 3 pass)。
    //   scene_color: 現在フレームの HDR scene RT
    //   scene_depth: shader-visible depth (SSR/SSAO と同じ)
    //   prev_view_proj: 前フレームの view_proj (Phase 33c-3 temporal reproject 用)。
    //                   identity を渡すと reprojection 無効 (= 静的 accumulate)。
    //   intensity:   indirect light の倍率 (0=無効、1=neutral、>1=強調)
    //   max_distance: ray march の世界距離上限 (世界座標、典型 5.0)
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_color,
                IRhiTexture& scene_depth,
                const Mat4& view_proj,
                const Mat4& inv_view_proj,
                const Mat4& prev_view_proj,
                Vec3 eye,
                f32 intensity   = 1.0f,
                f32 max_distance = 5.0f) noexcept;

    // Phase 33c-3: temporal accumulation 後の history RT を返す (= 直近の Render
    // が書き込んだ index)。PbrShader はこれを読む (blur + 時間積分でノイズ除去済)。
    IRhiTexture* OutputTexture() const noexcept {
        return _history[_temporal_frame == 0u ? 0u : ((_temporal_frame - 1u) & 1u)].Get();
    }
    IRhiTexture* RawTexture()    const noexcept { return _output.Get(); }

private:
    Result<void> CreateOutputRT(IRhiDevice& device, u32 w, u32 h) noexcept;
    Result<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             _device = nullptr;
    u32                     _width  = 0;
    u32                     _height = 0;

    UniquePtr<IRhiTexture>  _output;       // SSGI raw
    UniquePtr<IRhiTexture>  _blur_output;  // depth-aware bilateral blur 後 (Phase 33c-2)
    UniquePtr<IRhiTexture>  _history[2];   // temporal accumulation ping-pong (Phase 33c-3)
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiShader>   _blur_ps;      // Phase 33c-2
    UniquePtr<IRhiShader>   _temporal_ps;  // Phase 33c-3
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiPipeline> _blur_pipeline;    // Phase 33c-2
    UniquePtr<IRhiPipeline> _temporal_pipeline;// Phase 33c-3
    UniquePtr<IRhiBuffer>   _cb;
    u32                     _temporal_frame = 0;
};

} // namespace acs
