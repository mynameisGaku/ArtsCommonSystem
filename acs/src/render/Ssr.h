// Screen-Space Reflection (Phase 34e)
//
// 設計:
//   - シーン描画後の HDR scene color + depth-SRV を入力にして、各 pixel から
//     reflection ray を screen space で march
//   - 衝突したら scene_color を sample、ray 起点に reflection を加算
//   - 結果は別 HDR RT (= ssr_rt) に書き出し → caller が composite で本 HDR へ加算
//   - normal は depth derivatives (ddx/ddy of reconstructed world pos) から得る
//     (G-buffer normal は Phase 34d-2 待ち、これで暫定運用)
//
// 簡略: 32 step linear march、accelerating step、binary search 無し。
// glossy reflection (mip サンプル) も無し。Phase 35+ で hierarchical-Z + roughness
// に拡張可能。
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

    // SSR を内部スクラッチに描画する。caller は OutputTexture() を composite で
    // additive blend 加算する想定。
    //   scene_color: 現フレームの HDR scene
    //   scene_depth: 現フレームの depth buffer (shader_visible_depth=true 必須)
    //   view_proj / inv_view_proj: カメラ行列 (row-major)
    //   eye: カメラワールド位置
    //   intensity: SSR 強度 (0..2、デフォルト 0.6)
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_color, IRhiTexture& scene_depth,
                const Mat4& view_proj, const Mat4& inv_view_proj,
                Vec3 eye, f32 intensity = 0.6f) noexcept;

    IRhiTexture* OutputTexture() const noexcept { return _output.Get(); }
    Format        OutputFormat() const noexcept { return _hdr_format; }

private:
    Result<void> CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept;
    Result<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             _device = nullptr;
    u32                     _width  = 0;
    u32                     _height = 0;
    Format                  _hdr_format = Format::R16G16B16A16_Float;

    UniquePtr<IRhiTexture>  _output;
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiBuffer>   _cb;
};

} // namespace acs
