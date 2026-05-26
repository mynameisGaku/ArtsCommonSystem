// SPDX-License-Identifier: Apache-2.0
// Hi-Z (Hierarchical-Z) coarse min-depth buffer (Phase 36-3a)
//
// scene_depth から 1/8 解像度の "min depth" RT を一発で焼く。各 pixel は元の
// 8x8 ブロック中の 最近接 (= camera から最も近い) NDC depth を持つ。SSR 等の
// screen-space ray march で「ray より遠くに surface が無い」と分かった場合に
// 複数 texel を一気にスキップする (skip-ahead) のに使う。
//
// 設計上の選択:
//   ・mip_levels=1 の単独 RT を採用 (true Hi-Z mip chain は Diligent の
//     whole-resource transition との相性が悪いため、同一テクスチャ内 mip 間
//     read-write を避ける)
//   ・EFormat は R16G16_Float (=BRDF LUT と同じ)。.r に min depth、.g は未使用。
//     16-bit half は [0,1] NDC depth に対し ~0.1% 精度 → skip 用途十分。
//   ・sky pixel (depth >= 0.9999) は無視 — 屋外シーンで「ground - sky」の
//     skip が大きく走るのは安全 (空には反射先が無い)
//
// 使い方 (SSR 統合):
//   FHiZ hiz;
//   hiz.Init(*dev, w, h);
//   // 毎フレーム main pass の depth が完成したあと、SSR 前に:
//   hiz.Build(*dev, *cl, scene_depth);
//   ssr.Render(..., hiz.Texture());     // SSR shader が hiz_min を読む
//
// 上位の SSR shader 側で skip-ahead する具体実装 (Ssr.cpp) と一体で機能する。
// Hi-Z を渡さない場合 (= 旧挙動) も FSsr で OK (nullptr fallback)。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"
#include "render/RhiTypes.h"

namespace acs {

class FHiZ {
public:
    FHiZ() noexcept = default;
    ~FHiZ() noexcept = default;

    FHiZ(const FHiZ&) = delete;
    FHiZ& operator=(const FHiZ&) = delete;

    // src_width / src_height は scene_depth の解像度。Hi-Z は内部で
    // ceil(src_w / 8) x ceil(src_h / 8) サイズで確保される。
    TResult<void> Init(IRhiDevice& device, u32 src_width, u32 src_height) noexcept;
    void Shutdown() noexcept;

    TResult<void> Resize(u32 src_width, u32 src_height) noexcept;

    // scene_depth (shader_visible_depth=true の D32_Float) から coarse min を焼く。
    void Build(IRhiDevice& device, IRhiCommandList& cl,
               IRhiTexture& scene_depth) noexcept;

    // 1/8 解像度の min-depth RT (R16G16_Float、.r=min depth)、SSR の
    // skip-ahead で sample する。
    IRhiTexture* Texture() const noexcept { return _hiz.Get(); }
    u32 SrcWidth()  const noexcept { return _src_w; }
    u32 SrcHeight() const noexcept { return _src_h; }
    u32 Width()     const noexcept { return _hiz_w; }
    u32 Height()    const noexcept { return _hiz_h; }
    static constexpr u32 kBlockSize = 8;

private:
    TResult<void> CreateRT(IRhiDevice& device, u32 src_w, u32 src_h) noexcept;
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice* _device = nullptr;
    u32 _src_w = 0, _src_h = 0;
    u32 _hiz_w = 0, _hiz_h = 0;

    TUniquePtr<IRhiTexture>  _hiz;       // R32_Float, 1 mip, src/8
    TUniquePtr<IRhiShader>   _vs;
    TUniquePtr<IRhiShader>   _ps;
    TUniquePtr<IRhiPipeline> _pipeline;
    TUniquePtr<IRhiBuffer>   _cb;
};

} // namespace acs
