// Screen-Space Reflection (Phase 34e)
//
// 設計:
//   - シーン描画後の HDR scene color + depth-SRV を入力にして、各 pixel から
//     reflection ray を screen space で march
//   - 衝突したら scene_color を sample、ray 起点に reflection を加算
//   - 結果は別 HDR RT (= ssr_rt) に書き出し → caller が composite で本 HDR へ加算
//   - normal は MotionVector パスが出力する normal G-buffer (RGBA16F world normal)
//     から sample する (Phase 34m)。旧 depth-derivative cross(ddx,ddy) は 2x2 quad
//     単位で faceted になり、曲面の反射ベクトルが段差状になってガビガビだった
//
// 48 step linear march + accelerating step + 8-step binary search refinement
// (Phase 34e-fix: 粗 march だけだと反射が滲むので交差点を二分探索で精密化)。
//
// Phase 34e-3: temporal accumulation。raw SSR を per-frame jitter 付きで撃ち、
// 履歴 (reproject + neighborhood clamp) と時間方向に平均することで、ray-march の
// silhouette ジャギーを均す。OutputTexture() は temporal 累積後を返す。
//
// glossy reflection (roughness 別 mip サンプル) は未対応 — roughness 依存の blend は
// PbrShader 側 (Phase 34e-2fix) が担当。
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

class Ssr {
public:
    Ssr() noexcept = default;
    ~Ssr() noexcept = default;

    Ssr(const Ssr&) = delete;
    Ssr& operator=(const Ssr&) = delete;

    // hdr_format: SSR 出力テクスチャのフォーマット (シーン HDR と同じ R16G16B16A16F 推奨)
    Result<void> Init(IRhiDevice& device, Format hdr_format,
                       u32 width, u32 height) noexcept;
    void Shutdown() noexcept;

    Result<void> Resize(u32 width, u32 height) noexcept;

    // SSR を計算する (raw march → temporal accumulation の 2 pass)。
    //   scene_color:    現フレームの HDR scene
    //   scene_depth:    現フレームの depth buffer (shader_visible_depth=true 必須)
    //   normal_gbuffer: MotionVector パスの world-space normal G-buffer (RGBA16F)
    //   view_proj / inv_view_proj: 現フレームのカメラ行列 (row-major)
    //   prev_view_proj: 前フレームの view_proj (Phase 34e-3 temporal reproject 用)。
    //                   identity を渡すと reprojection 無効 (静的 accumulate)。
    //   eye: カメラワールド位置
    //   intensity: SSR 強度 (0..2)
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_color, IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                const Mat4& view_proj, const Mat4& inv_view_proj,
                const Mat4& prev_view_proj,
                Vec3 eye, f32 intensity = 0.6f) noexcept;

    // Phase 34e-3: temporal accumulation 後の history を返す (PbrShader / overlay 用)。
    IRhiTexture* OutputTexture() const noexcept {
        return _history[_temporal_frame == 0u ? 0u : ((_temporal_frame - 1u) & 1u)].Get();
    }
    IRhiTexture*  RawTexture()    const noexcept { return _output.Get(); }
    Format        OutputFormat() const noexcept { return _hdr_format; }

private:
    Result<void> CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept;
    Result<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             _device = nullptr;
    u32                     _width  = 0;
    u32                     _height = 0;
    Format                  _hdr_format = Format::R16G16B16A16_Float;

    UniquePtr<IRhiTexture>  _output;        // raw SSR (jitter 付き march)
    UniquePtr<IRhiTexture>  _history[2];    // Phase 34e-3: temporal accumulation ping-pong
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiShader>   _temporal_ps;   // Phase 34e-3
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiPipeline> _temporal_pipeline;  // Phase 34e-3
    UniquePtr<IRhiBuffer>   _cb;
    u32                     _temporal_frame = 0;
};

} // namespace acs
